#pragma once
#include "app_navigation.h"
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>

namespace pulse {
// UI refreshes enqueue fixed metadata only. A bounded background sink owns disk IO.
// `stem` names the per-process file under the logs folder; `dir_env` redirects
// that folder (tests).
class SearchRefreshLog {
public:
    SearchRefreshLog() : SearchRefreshLog(L"content-session-", L"PULSE_CONTENT_TIMING_DIR") {}
    SearchRefreshLog(std::wstring stem, std::wstring dir_env)
        : stem_(std::move(stem)), dir_env_(std::move(dir_env)), worker_([this] { Run(); }) {}
    ~SearchRefreshLog() {
        { std::lock_guard lock(mutex_); stopping_ = true; }
        wake_.notify_one();
        worker_.join();
    }
    void Push(std::string record) {
        { std::lock_guard lock(mutex_);
          if (records_.size() == 128) records_.pop_front();
          records_.push_back(std::move(record)); }
        wake_.notify_one();
    }
private:
    static std::wstring Environment(const wchar_t* name) {
        std::wstring value(32768, L'\0');
        const DWORD count = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
        if (!count || count >= value.size()) return {};
        value.resize(count);
        return value;
    }
    static void Write(const std::wstring& path, const std::string& record) {
        if (path.empty()) return;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER size{}, end{};
        if (!GetFileSizeEx(file, &size)) { CloseHandle(file); return; }
        if (size.QuadPart + static_cast<LONGLONG>(record.size()) > 1024 * 1024) {
            CloseHandle(file);
            if (!MoveFileExW(path.c_str(), (path + L".1").c_str(), MOVEFILE_REPLACE_EXISTING)) return;
            file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return;
        }
        if (SetFilePointerEx(file, end, nullptr, FILE_END)) {
            DWORD written = 0;
            WriteFile(file, record.data(), static_cast<DWORD>(record.size()), &written, nullptr);
        }
        CloseHandle(file);
    }
    void Run() noexcept {
        std::wstring path;
        try {
            auto directory = Environment(dir_env_.c_str());
            if (directory.empty()) {
                directory = Environment(L"LOCALAPPDATA");
                if (!directory.empty()) directory += L"\\Pulse\\logs";
            }
            if (!directory.empty()) {
                std::error_code error;
                std::filesystem::create_directories(directory, error);
                if (!error) path = (std::filesystem::path(directory) /
                    (stem_ + std::to_wstring(GetCurrentProcessId()) + L".jsonl")).wstring();
            }
        } catch (...) {}
        for (;;) {
            std::string record;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return stopping_ || !records_.empty(); });
                if (records_.empty()) return;
                record = std::move(records_.front());
                records_.pop_front();
            }
            try { Write(path, record); } catch (...) {}
        }
    }
    const std::wstring stem_;
    const std::wstring dir_env_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::string> records_;
    bool stopping_ = false;
    std::thread worker_;
};

inline void LogSearchRefresh(RefreshReason reason, bool retained, uint64_t session,
                             uint64_t generation, bool scanning) noexcept {
    try {
        const char* source = "background";
        switch (reason) {
        case RefreshReason::Explicit: source = "explicit"; break;
        case RefreshReason::ShellNotification: source = "shell_notification"; break;
        case RefreshReason::OperationCompleted: source = "operation_completed"; break;
        case RefreshReason::FileChange: source = "file_change"; break;
        default: break;
        }
        SYSTEMTIME utc{}; GetSystemTime(&utc);
        char timestamp[40]{};
        sprintf_s(timestamp, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", utc.wYear, utc.wMonth, utc.wDay,
            utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds);
        const std::string record = "{\"event\":\"refresh\",\"utc\":\"" + std::string(timestamp) +
            "\",\"pid\":" + std::to_string(GetCurrentProcessId()) + ",\"tick_ms\":" + std::to_string(GetTickCount64()) +
            ",\"reason\":\"" + source + "\",\"action\":\"" + (retained ? "retain_subscription" : "restart") +
            "\",\"session\":" + std::to_string(session) + ",\"generation\":" + std::to_string(generation) +
            ",\"scanning\":" + (scanning ? "true" : "false") + "}\n";
        static SearchRefreshLog sink;
        sink.Push(record);
    } catch (...) {}
}
} // namespace pulse
