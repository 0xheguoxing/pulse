// ai_model_list.cpp - The connected service's models (ai_model_list.h).
#include "ai_model_list.h"

#include "ai_model_key.h"
#include "ai_model_stream.h"
#include "ai_settings.h"
#include "../common/json_value.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwctype>
#include <memory>
#include <thread>

namespace pulse::app::ai {
namespace {

constexpr UINT_PTR kRefreshTimer = 0x4155;            // 'AU'
constexpr uint64_t kDay = 24ull * 3600 * 10000000;    // FILETIME units
constexpr uint64_t kRetryMs = 10 * 60 * 1000;
constexpr size_t kMaxModels = 500;
constexpr size_t kMaxId = 200;
constexpr char kOfficialUrl[] = "https://api.deepseek.com";
const std::vector<std::string> kOfficial = {"deepseek-flash", "deepseek-v4-pro"};

struct Cache {
    bool loaded = false;
    std::wstring url;     // normalized
    uint64_t at = 0;      // FILETIME of the fetch
    std::vector<std::string> ids;
};
Cache g_cache;

struct Job {
    std::wstring url;     // normalized
    ModelProbe result;
    std::atomic<bool> done{false};
};
std::shared_ptr<Job> g_job;
std::wstring g_tried_url;
uint64_t g_tried_at = 0;  // GetTickCount64 of a failed or running try

uint64_t Now() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// "HTTPS://Api.DeepSeek.com/" and "https://api.deepseek.com" are one endpoint.
std::wstring Normalize(const std::wstring& url) {
    size_t a = 0, b = url.size();
    while (a < b && std::iswspace(url[a])) ++a;
    while (b > a && (std::iswspace(url[b - 1]) || url[b - 1] == L'/')) --b;
    std::wstring out = url.substr(a, b - a);
    for (wchar_t& ch : out) ch = static_cast<wchar_t>(std::towlower(ch));
    return out;
}

// An id the cache file (one per line) and the UI can carry.
bool UsableId(const std::string& id) {
    if (id.empty() || id.size() > kMaxId) return false;
    return std::none_of(id.begin(), id.end(), [](char ch) { return static_cast<unsigned char>(ch) < 0x20; });
}

std::wstring CachePath() {
    const std::wstring dir = AiDirectory();
    return dir.empty() ? std::wstring() : dir + L"\\models.txt";
}

// models.txt: "pulse-models 1", the endpoint, the fetch time, then one id a line.
void Load() {
    if (g_cache.loaded) return;
    g_cache.loaded = true;
    const std::wstring path = CachePath();
    if (path.empty()) return;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    std::string data;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 256 * 1024) {
        data.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) || read != data.size())
            data.clear();
    }
    CloseHandle(file);
    std::vector<std::string> lines;
    size_t at = 0;
    while (at < data.size()) {
        size_t end = data.find('\n', at);
        if (end == std::string::npos) end = data.size();
        std::string line = data.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        at = end + 1;
    }
    if (lines.size() < 4 || lines[0] != "pulse-models 1") return;
    Cache c;
    c.loaded = true;
    c.url = Normalize(json::Wide(lines[1]));
    try {
        c.at = std::stoull(lines[2]);
    } catch (...) {
        return;
    }
    for (size_t i = 3; i < lines.size() && c.ids.size() < kMaxModels; ++i)
        if (UsableId(lines[i])) c.ids.push_back(lines[i]);
    if (!c.url.empty() && !c.ids.empty()) g_cache = std::move(c);
}

void Save() {
    const std::wstring path = CachePath();
    if (path.empty()) return;
    CreateDirectoryW(AiDirectory().c_str(), nullptr);
    std::string data = "pulse-models 1\n" + json::Utf8(g_cache.url) + "\n" + std::to_string(g_cache.at) + "\n";
    for (const std::string& id : g_cache.ids) data += id + "\n";
    const std::wstring temp = path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    const bool ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                    written == data.size();
    CloseHandle(file);
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(temp.c_str());
}

void CALLBACK RefreshTimer(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    if (!g_job) {
        KillTimer(hwnd, id);
        return;
    }
    if (!g_job->done.load(std::memory_order_acquire)) return;
    KillTimer(hwnd, id);
    const std::shared_ptr<Job> job = std::move(g_job);
    if (job->result.http_status == 200 && !job->result.models.empty()) {
        StoreListedModels(job->url, job->result.models);
        g_tried_url.clear();
        if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
    }
}

} // namespace

const std::vector<std::string>& ListedModels(const std::wstring& base_url) {
    static const std::vector<std::string> kNone;
    Load();
    const std::wstring url = Normalize(base_url);
    if (!g_cache.ids.empty() && g_cache.url == url) return g_cache.ids;
    return url == Normalize(json::Wide(kOfficialUrl)) ? kOfficial : kNone;
}

void StoreListedModels(const std::wstring& base_url, const std::vector<std::string>& ids) {
    std::vector<std::string> usable;
    for (const std::string& id : ids) {
        if (usable.size() >= kMaxModels) break;
        if (UsableId(id) && std::find(usable.begin(), usable.end(), id) == usable.end()) usable.push_back(id);
    }
    const std::wstring url = Normalize(base_url);
    if (usable.empty() || url.empty()) return;
    Load();
    g_cache.url = url;
    g_cache.at = Now();
    g_cache.ids = std::move(usable);
    Save();
}

void RefreshListedModels(HWND hwnd, const std::wstring& base_url) {
    if (g_job) return;
    Load();
    const std::wstring url = Normalize(base_url);
    if (url.empty()) return;
    const uint64_t now = Now();
    if (g_cache.url == url && !g_cache.ids.empty() && now >= g_cache.at && now - g_cache.at < kDay) return;
    const uint64_t tick = GetTickCount64();
    if (g_tried_url == url && tick - g_tried_at < kRetryMs) return;
    ModelEndpoint endpoint;
    endpoint.base_url = base_url;
    if (!LoadModelKey(endpoint.key) || endpoint.key.empty()) return;
    g_tried_url = url;
    g_tried_at = tick;
    auto job = std::make_shared<Job>();
    job->url = url;
    g_job = job;
    std::thread([job, endpoint = std::move(endpoint)]() mutable {
        job->result = ProbeModelService(std::move(endpoint));   // wipes the key
        job->done.store(true, std::memory_order_release);
    }).detach();
    if (hwnd) SetTimer(hwnd, kRefreshTimer, 100, RefreshTimer);
}

} // namespace pulse::app::ai
