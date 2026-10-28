#include "archive_pack.h"
#include "../common/ffmpeg_tool.h"
#include "../common/preview_packs.h"
#include <algorithm>
#include <charconv>
#include <limits>
#include <atomic>
#include <thread>

namespace pulse::preview {
namespace {
constexpr size_t kOutputLimit = 8u * 1024u * 1024u;
constexpr size_t kEntryLimit = 20000;
constexpr DWORD kTimeoutMs = 2500;

bool Fail(std::wstring* error, const wchar_t* reason) {
    if (error) *error = reason;
    return false;
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

// Fail closed if the handle allowlist or Job limits cannot be established.
// One pipe merges both streams so neither can deadlock on a full buffer.
bool Run(const std::wstring& tool, const std::wstring& arguments,
    std::string& output, DWORD& code, std::wstring* error) {
    const bool trace = GetEnvironmentVariableW(L"PULSE_ARCHIVE_TRACE", nullptr, 0) != 0;
    const ULONGLONG trace_start = GetTickCount64();
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle read, write, input, job, process, thread;
    if (!CreatePipe(&read.value, &write.value, &security, 65536) ||
        !SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0))
        return Fail(error, L"archive-pack-pipe");
    input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, 0, nullptr);
    if (input.value == INVALID_HANDLE_VALUE) return Fail(error, L"archive-pack-input");
    job.value = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
        JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_PROCESS_MEMORY |
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.ProcessMemoryLimit = 512u * 1024u * 1024u;
    if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
        &limits, sizeof(limits))) return Fail(error, L"archive-pack-job");
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> storage(size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size))
        return Fail(error, L"archive-pack-handles");
    HANDLE handles[] = {input.value, write.value};
    const bool attributes_ok = UpdateProcThreadAttribute(attributes, 0,
        PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr) != FALSE;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.value;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = write.value;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION info{};
    std::wstring command = ffmpeg::QuoteArgument(tool) + L" " + arguments;
    const std::wstring directory = packs::DirectoryOf(tool);
    const bool started = attributes_ok && CreateProcessW(tool.c_str(), command.data(),
        nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED |
        CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, directory.c_str(),
        &startup.StartupInfo, &info);
    DeleteProcThreadAttributeList(attributes);
    if (!started) return Fail(error, L"archive-pack-launch");
    process.value = info.hProcess;
    thread.value = info.hThread;
    CloseHandle(write.value); write.value = nullptr;
    const BOOL assigned = AssignProcessToJobObject(job.value, process.value);
    const DWORD resumed = assigned ? ResumeThread(thread.value) : DWORD(-1);
    if (trace) std::fprintf(stderr, "archive trace pid=%lu launch_ms=%llu assigned=%d resume=%lu\n", info.dwProcessId,
        static_cast<unsigned long long>(GetTickCount64() - trace_start), assigned, resumed);
    if (!assigned || resumed == DWORD(-1)) {
        TerminateProcess(process.value, ERROR_CANCELLED);
        WaitForSingleObject(process.value, 1000);
        return Fail(error, L"archive-pack-job");
    }
    std::atomic<const wchar_t*> failure{nullptr};
    std::atomic<size_t> received{0};
    // A blocking reader wakes on bytes, whereas polling the process handle
    // throttles bursts of output and can consume the entire preview deadline.
    std::thread reader([&]() {
        char buffer[65536];
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(read.value, buffer, sizeof(buffer), &got, nullptr)) {
                const DWORD read_error = GetLastError();
                if (read_error != ERROR_BROKEN_PIPE && read_error != ERROR_OPERATION_ABORTED) {
                    const wchar_t* expected = nullptr;
                    failure.compare_exchange_strong(expected, L"archive-pack-read");
                }
                break;
            }
            if (!got) break;
            received.fetch_add(got);
            if (output.size() + got > kOutputLimit) {
                failure.store(L"archive-pack-output-limit");
                TerminateJobObject(job.value, ERROR_CANCELLED);
                break;
            }
            output.append(buffer, got);
        }
    });
    const DWORD waited = WaitForSingleObject(process.value, kTimeoutMs);
    if (trace) std::fprintf(stderr, "archive trace wait=%lu elapsed_ms=%llu received=%llu\n", waited,
        static_cast<unsigned long long>(GetTickCount64() - trace_start), static_cast<unsigned long long>(received.load()));
    if (waited != WAIT_OBJECT_0) {
        const wchar_t* expected = nullptr;
        failure.compare_exchange_strong(expected, L"archive-pack-timeout");
        const BOOL terminated = TerminateJobObject(job.value, ERROR_CANCELLED);
        const DWORD terminate_error = terminated ? 0 : GetLastError();
        const DWORD killed_wait = WaitForSingleObject(process.value, 1000);
        DWORD killed_exit = 0;
        GetExitCodeProcess(process.value, &killed_exit);
        if (trace) std::fprintf(stderr, "archive trace terminate=%d error=%lu killed_wait=%lu exit=%lu\n",
            terminated, terminate_error, killed_wait, killed_exit);
        CancelSynchronousIo(reader.native_handle());
    }
    reader.join();
    if (failure.load()) return Fail(error, failure.load());
    if (WaitForSingleObject(process.value, 100) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.value, &code)) return Fail(error, L"archive-pack-exit");
    return true;
}

bool Wide(std::string_view input, std::wstring& out) {
    if (input.empty()) { out.clear(); return true; }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0);
    if (count <= 0) return false;
    out.resize(count);
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), out.data(), count) == count;
}
bool Number(std::string_view input, uint64_t& number) {
    const auto result = std::from_chars(input.data(), input.data() + input.size(), number);
    return result.ec == std::errc{} && result.ptr == input.data() + input.size();
}
}

bool IsArchivePackExtension(std::wstring_view extension) {
    constexpr std::wstring_view extensions[] = {L".7z", L".rar", L".zip", L".zipx", L".001",
        L".cab", L".wim", L".swm", L".esd", L".vhd", L".vhdx", L".vdi", L".vmdk",
        L".dmg", L".xar", L".rpm", L".deb", L".arj", L".lzh", L".lha", L".chm",
        L".iso", L".udf", L".tar", L".gz", L".xz", L".bz2", L".zst"};
    return std::find(std::begin(extensions), std::end(extensions), extension) != std::end(extensions);
}

bool ParseArchivePackListing(std::string_view output, DWORD exit_code,
    std::wstring& text, std::wstring* error) {
    text.clear();
    if (error) error->clear();
    if (exit_code != 0) return Fail(error, L"archive-pack-tool-error");
    if (output.size() > kOutputLimit) return Fail(error, L"archive-pack-output-limit");
    if (output.empty() || output.back() != '\n') return Fail(error, L"archive-pack-truncated");
    std::vector<ArchiveListingEntry> entries;
    ArchiveListingEntry entry;
    bool records = false, has_path = false, has_size = false, incomplete = false;
    std::wstring format;
    auto flush = [&]() {
        if (!has_path) return true;
        if (!has_size && !entry.dir) return false;
        if (entries.size() >= kEntryLimit) { incomplete = true; }
        else entries.push_back(std::move(entry));
        entry = {};
        has_path = has_size = false;
        return true;
    };
    while (!output.empty()) {
        const size_t newline = output.find('\n');
        std::string_view line = output.substr(0, newline);
        output.remove_prefix(newline + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line == "----------") { records = true; continue; }
        if (line.empty()) { if (records && !flush()) return Fail(error, L"archive-pack-record"); continue; }
        // 7-Zip prints "Key = " with an empty value for unknown properties
        // (WIM/XAR/HFS directories have no Size); treat those as absent.
        if (line.size() >= 2 && line.substr(line.size() - 2) == " =") continue;
        const size_t equal = line.find(" = ");
        if (equal == std::string_view::npos) {
            if (records) return Fail(error, L"archive-pack-record");
            continue;
        }
        const auto key = line.substr(0, equal), value = line.substr(equal + 3);
        if (value.empty()) continue;
        if (!records) {
            if (key == "Type") {
                if (!Wide(value, format) || format.size() > 32 || format.find_first_of(L"\t\r\n") != std::wstring::npos)
                    return Fail(error, L"archive-pack-format");
            }
            if (key == "Volumes" || key == "Volume") {
                uint64_t volumes = 0;
                if (!Number(value, volumes)) return Fail(error, L"archive-pack-volume");
                if (volumes > 1) incomplete = true;
            }
            if ((key == "Type" && value == "Split") || (key == "Multivolume" && value == "+")) incomplete = true;
            if (key == "ERROR" || key == "Errors" || key == "Warnings") return Fail(error, L"archive-pack-tool-error");
            continue;
        }
        if (key == "Path") {
            if (has_path || value.empty() || value.size() > 32768 || !Wide(value, entry.path))
                return Fail(error, L"archive-pack-path");
            has_path = true;
        } else if (key == "Size") {
            if (has_size || !Number(value, entry.size)) return Fail(error, L"archive-pack-size");
            has_size = true;
        } else if (key == "Folder") entry.dir = value == "+";
        else if (key == "Attributes" && value.find('D') != std::string_view::npos) entry.dir = true;
        else if (key == "Mode" && value.front() == 'd') entry.dir = true;  // XAR / tar-style modes
        else if (key == "Encrypted" && value == "+") { entry.encrypted = true; incomplete = true; }
        else if ((key == "Split Before" || key == "Split After") && value == "+") incomplete = true;
        else if (key == "Modified" && value.size() >= 19) {
            unsigned y=0, m=0, d=0, h=0, min=0, s=0;
            const std::string date(value);
            if (sscanf_s(date.c_str(), "%u-%u-%u %u:%u:%u", &y, &m, &d, &h, &min, &s) == 6 && y <= 9999) {
                entry.time = {static_cast<WORD>(y), static_cast<WORD>(m), 0, static_cast<WORD>(d),
                    static_cast<WORD>(h), static_cast<WORD>(min), static_cast<WORD>(s), 0};
                FILETIME ft{};
                entry.has_time = SystemTimeToFileTime(&entry.time, &ft) != FALSE;
            }
        }
    }
    if (!records || format.empty() || !flush()) return Fail(error, L"archive-pack-record");
    text = SerializeArchiveEntries(std::move(entries), format + L" (7-Zip)", incomplete);
    return true;
}

bool ArchivePackAvailable() {
    const std::wstring tool = packs::PackToolPath(packs::PackId::Archives, L"7z.exe");
    return !tool.empty() && packs::IsRegularFile(packs::DirectoryOf(tool) + L"\\7z.dll");
}

bool ArchivePackListingWithTool(const std::wstring& tool, const std::wstring& path,
    std::wstring& text, uint32_t& bytes_read, std::wstring* error) {
    text.clear();
    bytes_read = 0; // Child archive I/O is not measured by the host.
    if (!packs::IsRegularFile(tool) || !packs::IsRegularFile(packs::DirectoryOf(tool) + L"\\7z.dll"))
        return Fail(error, L"archive-pack-unavailable");
    std::string output;
    DWORD code = 0;
    // Explicit nonempty password avoids a prompt, even for encrypted headers.
    // No extraction or archive-test command is used on the preview path.
    if (!Run(tool, L"l -slt -sccUTF-8 -bd -pPulsePreviewNoPassword -- " +
        ffmpeg::QuoteArgument(path), output, code, error)) return false;
    return ParseArchivePackListing(output, code, text, error);
}

bool ArchivePackListing(const std::wstring& path, std::wstring& text,
    uint32_t& bytes_read, std::wstring* error) {
    return ArchivePackListingWithTool(packs::PackToolPath(packs::PackId::Archives, L"7z.exe"),
        path, text, bytes_read, error);
}
}
