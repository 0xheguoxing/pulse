#include "ai_settings.h"
#include "../common/json_value.h"
#include "../common/utf8_file.h"
#include <windows.h>

namespace pulse::app::ai {
namespace {

constexpr size_t kMaxClients = 64, kMaxRules = 256, kMaxName = 128;

std::wstring LocalAppData() {
    wchar_t root[32768];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", root, static_cast<DWORD>(std::size(root)));
    return n && n < std::size(root) ? std::wstring(root, n) : std::wstring{};
}

const char* GrantName(ClientGrant grant) {
    switch (grant) {
    case ClientGrant::Allowed: return "allowed";
    case ClientGrant::Denied: return "denied";
    default: return "ask";   // Session approvals are not remembered
    }
}

ClientGrant GrantFrom(const std::string& name) {
    if (name == "allowed") return ClientGrant::Allowed;
    if (name == "denied") return ClientGrant::Denied;
    return ClientGrant::Ask;
}

// Absolute, "." and ".." resolved lexically, no trailing separator (except a
// drive root). Empty for relative or unusable paths.
std::wstring NormalizeFolder(std::wstring_view folder) {
    if (folder.size() < 3 || folder.size() > 32000) return {};
    std::wstring in(folder);
    for (auto& c : in) if (c == L'/') c = L'\\';
    const bool drive = in.size() >= 3 && in[1] == L':' && in[2] == L'\\';
    const bool unc = in.rfind(L"\\\\", 0) == 0 && in.rfind(L"\\\\?\\", 0) != 0 && in.rfind(L"\\\\.\\", 0) != 0;
    if (!drive && !unc) return {};
    std::wstring out(32768, L'\0');
    const DWORD n = GetFullPathNameW(in.c_str(), static_cast<DWORD>(out.size()), out.data(), nullptr);
    if (!n || n >= out.size()) return {};
    out.resize(n);
    while (out.size() > 3 && out.back() == L'\\') out.pop_back();
    return out;
}

bool SameText(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() &&
           CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

} // namespace

std::wstring AiDirectory() {
    const std::wstring root = LocalAppData();
    return root.empty() ? std::wstring{} : root + L"\\Pulse\\ai";
}

std::wstring SettingsPath() {
    const std::wstring dir = AiDirectory();
    return dir.empty() ? std::wstring{} : dir + L"\\ai.json";
}

std::wstring ActivityDirectory() {
    const std::wstring dir = AiDirectory();
    return dir.empty() ? std::wstring{} : dir + L"\\activity";
}

Settings ParseSettings(std::string_view utf8, bool* malformed) {
    Settings s;
    json::Value root;
    if (!json::Parse(utf8, root) || !root.is_object()) {
        if (malformed) *malformed = true;
        return s;
    }
    if (malformed) *malformed = false;
    s.mcp_enabled = root.get_bool("mcp_enabled", false);
    if (const auto* days = root.find("log_days")) {
        int64_t n = 0;
        if (days->get_int(n, 1, 365)) s.log_days = static_cast<int>(n);
    }
    if (const auto* clients = root.find("clients"); clients && clients->is_array()) {
        for (const auto& c : clients->items()) {
            if (s.clients.size() >= kMaxClients) break;
            const std::wstring name = json::Wide(c.get_string("name"));
            if (name.empty() || name.size() > kMaxName) continue;
            ClientRecord record{name, GrantFrom(c.get_string("grant")), 0};
            int64_t last = 0;
            if (const auto* v = c.find("last_call"); v && v->get_int(last, 0)) record.last_call = static_cast<uint64_t>(last);
            s.clients.push_back(std::move(record));
        }
    }
    if (const auto* allow = root.find("allow"); allow && allow->is_array()) {
        for (const auto& a : allow->items()) {
            if (s.allow.size() >= kMaxRules) break;
            AllowRule rule{json::Wide(a.get_string("client")), a.get_string("tool"),
                           NormalizeFolder(json::Wide(a.get_string("folder")))};
            if (rule.client.empty() || rule.client.size() > kMaxName || rule.tool.empty() || rule.tool.size() > kMaxName ||
                rule.folder.empty())
                continue;
            s.allow.push_back(std::move(rule));
        }
    }
    if (const auto* a = root.find("assistant"); a && a->is_object()) {
        s.assistant.floating = a->get_string("dock") == "floating";
        const std::string model = a->get_string("model");
        if (!model.empty() && model.size() <= kMaxName) s.assistant.model = model;
        if (const auto* effort = a->find("effort")) {
            int64_t n = 0;
            if (effort->get_int(n, 0, 3)) s.assistant.effort = static_cast<int>(n);
        }
        const std::string base_url = a->get_string("base_url");
        if (base_url.size() <= 512 && (base_url.starts_with("https://") || base_url.starts_with("http://")))
            s.assistant.base_url = base_url;
    }
    return s;
}

Settings LoadSettings(bool* malformed) {
    if (malformed) *malformed = false;
    const std::wstring path = SettingsPath();
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
    std::wstring text;
    if (!ReadUtf8File(path, text)) {
        if (malformed) *malformed = true;
        return {};
    }
    return ParseSettings(json::Utf8(text), malformed);
}

std::string SerializeSettings(const Settings& s) {
    json::Value root = json::Value::Object();
    root.set("mcp_enabled", s.mcp_enabled);
    root.set("log_days", s.log_days);
    json::Value clients = json::Value::Array();
    for (const auto& c : s.clients) {
        json::Value v = json::Value::Object();
        v.set("name", json::Utf8(c.name));
        v.set("grant", GrantName(c.grant));
        v.set("last_call", c.last_call);
        clients.push(std::move(v));
    }
    root.set("clients", std::move(clients));
    json::Value allow = json::Value::Array();
    for (const auto& a : s.allow) {
        json::Value v = json::Value::Object();
        v.set("client", json::Utf8(a.client));
        v.set("tool", a.tool);
        v.set("folder", json::Utf8(a.folder));
        allow.push(std::move(v));
    }
    root.set("allow", std::move(allow));
    json::Value assistant = json::Value::Object();
    assistant.set("dock", s.assistant.floating ? "floating" : "docked");
    assistant.set("model", s.assistant.model);
    assistant.set("effort", s.assistant.effort);
    if (!s.assistant.base_url.empty()) assistant.set("base_url", s.assistant.base_url);
    root.set("assistant", std::move(assistant));
    return json::Serialize(root) + "\n";
}

bool SaveSettings(const Settings& settings) {
    const std::wstring dir = AiDirectory();
    if (dir.empty()) { SetLastError(ERROR_PATH_NOT_FOUND); return false; }
    const std::wstring pulse_dir = dir.substr(0, dir.find_last_of(L'\\'));
    CreateDirectoryW(pulse_dir.c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    return WriteUtf8FileAtomic(SettingsPath(), json::Wide(SerializeSettings(settings)));
}

bool IsAlwaysAllowed(const Settings& settings, PermissionClass cls, std::wstring_view client, std::string_view tool,
                     std::wstring_view folder) {
    if (cls != PermissionClass::Create || client.empty() || tool.empty()) return false;
    const std::wstring target = NormalizeFolder(folder);
    if (target.empty()) return false;
    for (const auto& rule : settings.allow) {
        if (rule.tool != tool || !SameText(rule.client, client)) continue;
        if (SameText(rule.folder, target)) return true;
        const bool root = rule.folder.size() == 3;   // "C:\" already ends with the separator
        const size_t prefix = rule.folder.size() + (root ? 0 : 1);
        if (target.size() > prefix && SameText(std::wstring_view(target).substr(0, rule.folder.size()), rule.folder) &&
            (root || target[rule.folder.size()] == L'\\'))
            return true;
    }
    return false;
}

} // namespace pulse::app::ai
