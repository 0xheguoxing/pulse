#include "ai_tools_fs.h"
#include "zip_compress.h"
#include <objbase.h>
#include <propsys.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <memory>

namespace pulse::app::ai {
namespace {

constexpr size_t kListCap = 100000;      // items enumerated per listing at most
constexpr size_t kChildCountCap = 10000; // a folder's children counted at most
constexpr DWORD kFolderAttribute = FILE_ATTRIBUTE_DIRECTORY;

ToolOutcome Invalid(const std::string& message) { return ToolOutcome::Fail("invalid_args", message); }

std::string U8(const std::wstring& s) { return json::Utf8(s); }

bool IsSep(wchar_t c) { return c == L'\\' || c == L'/'; }

bool StartsWithI(const std::wstring& s, std::wstring_view prefix) {
    return s.size() >= prefix.size() && _wcsnicmp(s.c_str(), prefix.data(), prefix.size()) == 0;
}

bool EqualI(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

// `path` is `folder` or inside it.
bool Within(const std::wstring& path, const std::wstring& folder) {
    if (folder.empty() || !StartsWithI(path, folder)) return false;
    if (path.size() == folder.size()) return true;
    return folder.back() == L'\\' || path[folder.size()] == L'\\';
}

// Win32 error of a path lookup -> tool error.
ToolOutcome PathFailure(DWORD error, const std::wstring& path) {
    const std::string where = "\"" + U8(path) + "\"";
    switch (error) {
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
            return ToolOutcome::Fail("access_denied", "Access to " + where + " is denied.");
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_NET_NAME:
        case ERROR_INVALID_NAME:
        case ERROR_INVALID_DRIVE:
        case ERROR_NOT_READY:
        case ERROR_DIRECTORY:
            return ToolOutcome::Fail("not_found", where + " does not exist.");
        default:
            return ToolOutcome::Fail("internal", "Could not read " + where + " (Windows error " +
                                                     std::to_string(error) + ").", true);
    }
}

bool Attributes(const std::wstring& path, WIN32_FILE_ATTRIBUTE_DATA& data, DWORD& error) {
    if (GetFileAttributesExW(Win32Path(path).c_str(), GetFileExInfoStandard, &data)) return true;
    error = GetLastError();
    return false;
}

uint64_t SizeOf(DWORD high, DWORD low) { return (static_cast<uint64_t>(high) << 32) | low; }

int CompareNames(const std::wstring& a, const std::wstring& b) {
    const int r = CompareStringEx(LOCALE_NAME_INVARIANT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, a.c_str(),
                                  static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), nullptr,
                                  nullptr, 0);
    if (r == CSTR_LESS_THAN) return -1;
    if (r == CSTR_GREATER_THAN) return 1;
    return a.compare(b) < 0 ? -1 : (a == b ? 0 : 1);   // deterministic for names equal but for case
}

json::Value AttributeList(DWORD attributes) {
    static constexpr struct { DWORD bit; const char* name; } kBits[] = {
        {FILE_ATTRIBUTE_READONLY, "readonly"},   {FILE_ATTRIBUTE_HIDDEN, "hidden"},
        {FILE_ATTRIBUTE_SYSTEM, "system"},       {FILE_ATTRIBUTE_ARCHIVE, "archive"},
        {FILE_ATTRIBUTE_COMPRESSED, "compressed"}, {FILE_ATTRIBUTE_ENCRYPTED, "encrypted"},
        {FILE_ATTRIBUTE_OFFLINE, "offline"},     {FILE_ATTRIBUTE_REPARSE_POINT, "link"},
        {FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS, "cloud"},
    };
    json::Value list = json::Value::Array();
    for (const auto& bit : kBits)
        if (attributes & bit.bit) list.push(bit.name);
    return list;
}

class ComScope {
public:
    ComScope() : hr_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ~ComScope() { if (SUCCEEDED(hr_)) CoUninitialize(); }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
private:
    HRESULT hr_;
};

// Shell property keys, local like wall_aspects.cpp (propkey.h needs INITGUID).
constexpr GUID kImageFmt = {0x6444048F, 0x4C8B, 0x11D1, {0x8B, 0x70, 0x08, 0x00, 0x36, 0xB1, 0x1A, 0x03}};
constexpr GUID kMediaFmt = {0x64440490, 0x4C8B, 0x11D1, {0x8B, 0x70, 0x08, 0x00, 0x36, 0xB1, 0x1A, 0x03}};
constexpr GUID kVideoFmt = {0x64440491, 0x4C8B, 0x11D1, {0x8B, 0x70, 0x08, 0x00, 0x36, 0xB1, 0x1A, 0x03}};
constexpr GUID kPhotoFmt = {0x14B81DA1, 0x0135, 0x4D31, {0x96, 0xD9, 0x6C, 0xBF, 0xC9, 0x67, 0x1A, 0x99}};
constexpr GUID kSummaryFmt = {0xF29F85E0, 0x4FF9, 0x1068, {0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9}};

class Properties {
public:
    explicit Properties(const std::wstring& path) {
        if (FAILED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT | GPS_BESTEFFORT,
                                                     IID_PPV_ARGS(&store_))))
            store_ = nullptr;
    }
    ~Properties() { if (store_) store_->Release(); }
    Properties(const Properties&) = delete;
    Properties& operator=(const Properties&) = delete;
    explicit operator bool() const noexcept { return store_ != nullptr; }

    uint64_t Number(const GUID& fmt, DWORD pid) const {
        PROPVARIANT v;
        PropVariantInit(&v);
        uint64_t out = 0;
        if (SUCCEEDED(store_->GetValue(PROPERTYKEY{fmt, pid}, &v))) {
            switch (v.vt) {
                case VT_UI2: out = v.uiVal; break;
                case VT_UI4: out = v.ulVal; break;
                case VT_UI8: out = v.uhVal.QuadPart; break;
                case VT_I4: out = v.lVal > 0 ? static_cast<uint64_t>(v.lVal) : 0; break;
                case VT_I8: out = v.hVal.QuadPart > 0 ? static_cast<uint64_t>(v.hVal.QuadPart) : 0; break;
                case VT_FILETIME: out = SizeOf(v.filetime.dwHighDateTime, v.filetime.dwLowDateTime); break;
                default: break;
            }
        }
        PropVariantClear(&v);
        return out;
    }
    std::wstring Text(const GUID& fmt, DWORD pid) const {
        PROPVARIANT v;
        PropVariantInit(&v);
        std::wstring out;
        if (SUCCEEDED(store_->GetValue(PROPERTYKEY{fmt, pid}, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
            out = v.pwszVal;
        PropVariantClear(&v);
        while (!out.empty() && (out.back() == L' ' || out.back() == L'\0')) out.pop_back();
        return out.size() > 256 ? out.substr(0, 256) : out;
    }

private:
    IPropertyStore* store_ = nullptr;
};

void AddShellProperties(const std::wstring& path, json::Value& item) {
    const Properties props(path);
    if (!props) return;
    auto pair = [&](const char* key, const GUID& fmt) {
        const uint64_t w = props.Number(fmt, 3), h = props.Number(fmt, 4);
        if (!w || !h) return;
        json::Value size = json::Value::Object();
        size.set("width", w);
        size.set("height", h);
        item.set(key, std::move(size));
    };
    pair("image", kImageFmt);
    pair("video", kVideoFmt);
    if (const uint64_t duration = props.Number(kMediaFmt, 3))   // 100 ns units
        item.set("duration_seconds", static_cast<double>(duration / 100000) / 100.0);
    if (const uint64_t pages = props.Number(kSummaryFmt, 14)) item.set("pages", pages);
    const std::wstring make = props.Text(kPhotoFmt, 271), model = props.Text(kPhotoFmt, 272);
    if (!make.empty() || !model.empty()) {
        json::Value camera = json::Value::Object();
        if (!make.empty()) camera.set("make", U8(make));
        if (!model.empty()) camera.set("model", U8(model));
        item.set("camera", std::move(camera));
    }
    if (const uint64_t taken = props.Number(kPhotoFmt, 36867); taken) item.set("taken", IsoTime(taken));
}

bool IsProgram(const std::wstring& path) {
    const size_t dot = path.rfind(L'.');
    if (dot == std::wstring::npos) return false;
    const std::wstring ext = path.substr(dot);
    for (const wchar_t* known : {L".exe", L".dll", L".sys", L".ocx", L".cpl", L".scr"})
        if (EqualI(ext, known)) return true;
    return false;
}

void AddVersion(const std::wstring& path, json::Value& item) {
    DWORD ignored = 0;
    const std::wstring file = Win32Path(path);
    const DWORD size = GetFileVersionInfoSizeExW(FILE_VER_GET_NEUTRAL, file.c_str(), &ignored);
    if (size == 0 || size > (16u << 20)) return;
    std::vector<BYTE> block(size);
    if (!GetFileVersionInfoExW(FILE_VER_GET_NEUTRAL, file.c_str(), 0, size, block.data())) return;
    json::Value version = json::Value::Object();
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT length = 0;
    if (VerQueryValueW(block.data(), L"\\", reinterpret_cast<void**>(&fixed), &length) && fixed &&
        length >= sizeof(VS_FIXEDFILEINFO) && fixed->dwSignature == 0xFEEF04BD) {
        char text[64];
        std::snprintf(text, sizeof(text), "%u.%u.%u.%u", HIWORD(fixed->dwFileVersionMS),
                      LOWORD(fixed->dwFileVersionMS), HIWORD(fixed->dwFileVersionLS), LOWORD(fixed->dwFileVersionLS));
        version.set("file", text);
    }
    struct Translation { WORD language, codepage; }* translations = nullptr;
    if (VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations),
                       &length) && translations && length >= sizeof(Translation)) {
        wchar_t prefix[64];
        swprintf_s(prefix, L"\\StringFileInfo\\%04x%04x\\", translations[0].language, translations[0].codepage);
        for (const auto& [key, name] : {std::pair{L"ProductName", "product_name"},
                                        std::pair{L"ProductVersion", "product_version"},
                                        std::pair{L"CompanyName", "company"},
                                        std::pair{L"FileDescription", "description"}}) {
            wchar_t* value = nullptr;
            UINT chars = 0;
            const std::wstring query = std::wstring(prefix) + key;
            if (VerQueryValueW(block.data(), query.c_str(), reinterpret_cast<void**>(&value), &chars) && value &&
                chars > 1) {
                std::wstring text(value, wcsnlen(value, chars));
                if (text.size() > 256) text.resize(256);
                if (!text.empty()) version.set(name, U8(text));
            }
        }
    }
    if (version.size()) item.set("version", std::move(version));
}

// Direct children, up to the cap.
size_t CountChildren(const std::wstring& folder, bool& more) {
    more = false;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW(Win32Path(folder + (folder.back() == L'\\' ? L"*" : L"\\*")).c_str(),
                                   FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return 0;
    size_t n = 0;
    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        if (++n >= kChildCountCap) {
            more = true;
            break;
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return n;
}

bool StringArray(const json::Value& args, const char* name, size_t maximum, std::vector<const json::Value*>& out,
                 ToolOutcome& error) {
    const json::Value* list = args.find(name);
    if (!list || !list->is_array() || list->size() == 0 || list->size() > maximum) {
        error = Invalid(std::string(name) + " must be an array of 1 to " + std::to_string(maximum) + " paths");
        return false;
    }
    for (const json::Value& v : list->items()) out.push_back(&v);
    return true;
}

bool ReservedDeviceName(const std::wstring& name) {
    std::wstring stem = name.substr(0, name.find(L'.'));
    for (auto& c : stem) c = static_cast<wchar_t>(std::towupper(c));
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL") return true;
    return stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) && stem[3] >= L'1' &&
           stem[3] <= L'9';
}

} // namespace

std::wstring Win32Path(const std::wstring& path) {
    if (path.size() < 248 || StartsWithI(path, L"\\\\?\\")) return path;
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

std::wstring ParentFolder(const std::wstring& path) {
    const size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) return {};
    if (path.size() == 3 && path[1] == L':') return {};                // C:\ itself
    std::wstring parent = path.substr(0, slash);
    if (parent.size() == 2 && parent[1] == L':') parent += L'\\';     // C:\x -> C:\  .
    if (parent.size() >= 2 && parent[0] == L'\\' && parent[1] == L'\\' &&
        std::count(parent.begin() + 2, parent.end(), L'\\') < 1)
        return {};                                                     // \\server\share has none
    return parent;
}

bool CheckPathArg(const json::Value& value, const char* name, std::wstring& out, ToolOutcome& error) {
    const std::string field = std::string(name);
    if (!value.is_string() || value.as_string().empty()) {
        error = Invalid(field + " must be a non-empty path");
        return false;
    }
    std::wstring raw = json::Wide(value.as_string());
    if (raw.empty() || raw.size() > 32000) {
        error = Invalid(field + " is not a valid path");
        return false;
    }
    for (auto& c : raw) if (c == L'/') c = L'\\';
    // Explicit long-path forms of ordinary paths are accepted; device and
    // object-manager forms are not.
    if (StartsWithI(raw, L"\\\\?\\UNC\\")) raw = L"\\\\" + raw.substr(8);
    else if (StartsWithI(raw, L"\\\\?\\") && raw.size() >= 7 && raw[5] == L':') raw = raw.substr(4);
    const bool drive = raw.size() >= 3 && std::iswalpha(raw[0]) && raw[1] == L':' && raw[2] == L'\\';
    const bool unc = raw.size() >= 5 && raw[0] == L'\\' && raw[1] == L'\\' && raw[2] != L'?' && raw[2] != L'.' &&
                     raw[2] != L'\\';
    if (!drive && !unc) {
        error = Invalid(field + " must be an absolute path such as C:\\Users\\me\\Documents or \\\\server\\share");
        return false;
    }
    for (size_t i = 0; i < raw.size(); ++i) {
        const wchar_t c = raw[i];
        if (c < 0x20 || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|' ||
            (c == L':' && i != 1)) {
            error = Invalid(field + " contains characters a path cannot have");
            return false;
        }
    }
    std::wstring full(raw.size() + 16, L'\0');
    DWORD n = GetFullPathNameW(raw.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
    if (n >= full.size()) {
        full.resize(n + 1);
        n = GetFullPathNameW(raw.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
    }
    if (n == 0 || n >= full.size()) {
        error = Invalid(field + " is not a valid path");
        return false;
    }
    full.resize(n);
    if (unc) {
        // \\server\share at least, both parts present.
        const size_t server_end = full.find(L'\\', 2);
        if (server_end == std::wstring::npos || server_end == 2 || server_end + 1 >= full.size()) {
            error = Invalid(field + " must name a share, such as \\\\server\\share");
            return false;
        }
    }
    while (full.size() > 3 && full.back() == L'\\') full.pop_back();
    out = std::move(full);
    return true;
}

bool IsSystemFolder(const std::wstring& path) {
    static const std::vector<std::wstring> roots = [] {
        std::vector<std::wstring> list;
        for (const KNOWNFOLDERID& id : {FOLDERID_Windows, FOLDERID_ProgramFiles, FOLDERID_ProgramFilesX86,
                                        FOLDERID_ProgramData}) {
            PWSTR found = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &found)) && found && *found)
                list.emplace_back(found);
            CoTaskMemFree(found);
        }
        return list;
    }();
    for (const std::wstring& root : roots)
        if (Within(path, root)) return true;
    // X:\$Recycle.Bin, X:\System Volume Information
    if (path.size() >= 3 && path[1] == L':') {
        const std::wstring rest = path.substr(3);
        for (const wchar_t* name : {L"$Recycle.Bin", L"System Volume Information"}) {
            const size_t n = wcslen(name);
            if (rest.size() >= n && _wcsnicmp(rest.c_str(), name, n) == 0 && (rest.size() == n || rest[n] == L'\\'))
                return true;
        }
    }
    return false;
}

std::string IsoTime(uint64_t filetime) {
    FILETIME ft{static_cast<DWORD>(filetime & 0xFFFFFFFFu), static_cast<DWORD>(filetime >> 32)};
    return IsoTime(ft);
}

std::string IsoTime(const FILETIME& time) {
    if (time.dwHighDateTime == 0 && time.dwLowDateTime == 0) return {};
    SYSTEMTIME st{};
    if (!FileTimeToSystemTime(&time, &st)) return {};
    char text[32];
    std::snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour,
                  st.wMinute, st.wSecond);
    return text;
}

ToolOutcome ListFolder(const json::Value& args, const std::atomic<bool>& cancelled) {
    std::wstring folder;
    ToolOutcome error;
    const json::Value* path_arg = args.find("path");
    if (!CheckPathArg(path_arg ? *path_arg : json::Value(), "path", folder, error)) return error;
    int64_t limit = 100;
    if (const json::Value* v = args.find("limit"); v && !v->get_int(limit, 1, 500))
        return Invalid("limit must be an integer from 1 to 500");
    size_t offset = 0;
    if (const json::Value* v = args.find("cursor")) {
        const std::string c = v->is_string() ? v->as_string() : std::string();
        if (c.empty() || c.size() > 9 ||
            !std::all_of(c.begin(), c.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
            return Invalid("cursor must be the next_cursor of an earlier page");
        offset = static_cast<size_t>(std::stoul(c));
    }
    WIN32_FILE_ATTRIBUTE_DATA data{};
    DWORD win_error = 0;
    if (!Attributes(folder, data, win_error)) return PathFailure(win_error, folder);
    if (!(data.dwFileAttributes & kFolderAttribute)) return Invalid("path is a file, not a folder");

    struct Entry {
        std::wstring name;
        bool dir;
        bool hidden;
        uint64_t size;
        FILETIME modified;
    };
    std::vector<Entry> entries;
    bool partial = false;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileExW(Win32Path(folder + (folder.back() == L'\\' ? L"*" : L"\\*")).c_str(),
                                   FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND && e != ERROR_NO_MORE_FILES) return PathFailure(e, folder);
    } else {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            const DWORD a = fd.dwFileAttributes;
            if ((a & FILE_ATTRIBUTE_HIDDEN) && (a & FILE_ATTRIBUTE_SYSTEM)) continue;   // protected OS files
            entries.push_back({fd.cFileName, (a & kFolderAttribute) != 0, (a & FILE_ATTRIBUTE_HIDDEN) != 0,
                               SizeOf(fd.nFileSizeHigh, fd.nFileSizeLow), fd.ftLastWriteTime});
            if (entries.size() >= kListCap) {
                partial = true;
                break;
            }
            if ((entries.size() & 1023) == 0 && cancelled) break;
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    if (cancelled) return ToolOutcome::Fail("cancelled", "The call was cancelled.");
    if (offset > entries.size()) return Invalid("cursor is past the end of the folder");
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.dir != b.dir) return a.dir;
        return CompareNames(a.name, b.name) < 0;
    });
    ToolOutcome out;
    json::Value result = json::Value::Object();
    result.set("path", U8(folder));
    result.set("total", static_cast<uint64_t>(entries.size()));
    json::Value items = json::Value::Array();
    const size_t end = std::min(entries.size(), offset + static_cast<size_t>(limit));
    for (size_t i = offset; i < end; ++i) {
        const Entry& e = entries[i];
        json::Value item = json::Value::Object();
        item.set("name", U8(e.name));
        item.set("type", e.dir ? "folder" : "file");
        if (!e.dir) item.set("size", e.size);
        if (const std::string t = IsoTime(e.modified); !t.empty()) item.set("modified", t);
        if (e.hidden) item.set("hidden", true);
        items.push(std::move(item));
    }
    result.set("items", std::move(items));
    if (end < entries.size()) result.set("next_cursor", std::to_string(end));
    if (partial) result.set("partial", true);   // more than kListCap items: the rest is not listed
    out.data = std::move(result);
    return out;
}

ToolOutcome GetProperties(const json::Value& args, const std::atomic<bool>& cancelled) {
    std::vector<const json::Value*> paths;
    ToolOutcome error;
    if (!StringArray(args, "paths", 50, paths, error)) return error;
    const ComScope com;
    json::Value items = json::Value::Array();
    for (const json::Value* arg : paths) {
        if (cancelled) return ToolOutcome::Fail("cancelled", "The call was cancelled.");
        json::Value item = json::Value::Object();
        std::wstring path;
        ToolOutcome refused;
        if (!CheckPathArg(*arg, "path", path, refused)) {
            item.set("path", arg->as_string());
            item.set("error", refused.error.code + ": " + refused.error.message);
            items.push(std::move(item));
            continue;
        }
        item.set("path", U8(path));
        WIN32_FILE_ATTRIBUTE_DATA data{};
        DWORD win_error = 0;
        if (!Attributes(path, data, win_error)) {
            const ToolOutcome failure = PathFailure(win_error, path);
            item.set("error", failure.error.code + ": " + failure.error.message);
            items.push(std::move(item));
            continue;
        }
        const bool dir = (data.dwFileAttributes & kFolderAttribute) != 0;
        const size_t slash = path.find_last_of(L'\\');
        item.set("name", U8(slash == std::wstring::npos || slash + 1 == path.size() ? path : path.substr(slash + 1)));
        item.set("type", dir ? "folder" : "file");
        if (!dir) item.set("size", SizeOf(data.nFileSizeHigh, data.nFileSizeLow));
        if (const std::string t = IsoTime(data.ftCreationTime); !t.empty()) item.set("created", t);
        if (const std::string t = IsoTime(data.ftLastWriteTime); !t.empty()) item.set("modified", t);
        if (const std::string t = IsoTime(data.ftLastAccessTime); !t.empty()) item.set("accessed", t);
        if (json::Value list = AttributeList(data.dwFileAttributes); list.size()) item.set("attributes", std::move(list));
        if (dir) {
            bool more = false;
            item.set("items", static_cast<uint64_t>(CountChildren(path, more)));
            if (more) item.set("items_more", true);
        } else if (!(data.dwFileAttributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS))) {
            // Cloud placeholders are not opened: reading them would download them.
            AddShellProperties(path, item);
            if (IsProgram(path)) AddVersion(path, item);
        }
        items.push(std::move(item));
    }
    ToolOutcome out;
    out.data = json::Value::Object();
    out.data.set("items", std::move(items));
    return out;
}

bool PlanSearch(const json::Value& args, SearchPlan& plan, ToolOutcome& error) {
    plan = {};
    const std::string query = args.get_string("query");
    const std::wstring needle = json::Wide(query);
    if (needle.empty() || needle.size() > 256 || needle.find_first_of(L"\r\n") != std::wstring::npos) {
        error = Invalid("query must be 1 to 256 characters on one line");
        return false;
    }
    plan.needle = needle;
    if (const json::Value* folder = args.find("folder")) {
        if (!CheckPathArg(*folder, "folder", plan.folder, error)) return false;
    }
    if (const json::Value* type = args.find("type")) {
        const std::string& t = type->as_string();
        if (t == "folder") plan.folders_only = true;
        else if (t == "file") plan.needle += L" file:";
        else if (t == "image" || t == "video" || t == "audio" || t == "document" || t == "archive")
            plan.needle += L" type:" + json::Wide(t);
        else {
            error = Invalid("type must be one of: file, folder, image, video, audio, document, archive");
            return false;
        }
    }
    if (const json::Value* after = args.find("modified_after")) {
        const std::string& d = after->as_string();
        const bool shaped = d.size() == 10 && d[4] == '-' && d[7] == '-' &&
            std::all_of(d.begin(), d.end(), [](char c) { return (c >= '0' && c <= '9') || c == '-'; });
        const int month = shaped ? std::atoi(d.substr(5, 2).c_str()) : 0;
        const int day = shaped ? std::atoi(d.substr(8, 2).c_str()) : 0;
        if (!shaped || month < 1 || month > 12 || day < 1 || day > 31) {
            error = Invalid("modified_after must be a date as YYYY-MM-DD");
            return false;
        }
        plan.needle += L" dm:" + json::Wide(d) + L"..2999-12-31";
    }
    int64_t limit = 50;
    if (const json::Value* v = args.find("limit"); v && !v->get_int(limit, 1, 200)) {
        error = Invalid("limit must be an integer from 1 to 200");
        return false;
    }
    plan.limit = static_cast<size_t>(limit);
    return true;
}

json::Value SearchResultJson(const std::vector<FoundItem>& items, size_t total) {
    json::Value result = json::Value::Object();
    json::Value list = json::Value::Array();
    for (const FoundItem& found : items) {
        json::Value item = json::Value::Object();
        item.set("path", U8(found.path));
        item.set("name", U8(found.name));
        item.set("type", found.is_dir ? "folder" : "file");
        if (!found.is_dir) item.set("size", found.size);
        if (const std::string t = IsoTime(found.modified); !t.empty()) item.set("modified", t);
        list.push(std::move(item));
    }
    result.set("total", static_cast<uint64_t>(std::max(total, items.size())));
    result.set("items", std::move(list));
    return result;
}

std::wstring ZipTargetFolder(const json::Value& args) {
    std::wstring folder;
    ToolOutcome ignored;
    if (const json::Value* dest = args.find("dest_folder"))
        return CheckPathArg(*dest, "dest_folder", folder, ignored) ? folder : std::wstring();
    const json::Value* paths = args.find("paths");
    if (!paths || !paths->is_array() || paths->size() == 0) return {};
    std::wstring first;
    return CheckPathArg(paths->at(0), "paths", first, ignored) ? ParentFolder(first) : std::wstring();
}

bool PlanZip(const json::Value& args, ZipPlan& plan, ToolOutcome& error) {
    plan = {};
    if (!ZipCompressAvailable()) {
        error = ToolOutcome::Fail("unsupported", "Creating zip archives needs Windows 10 version 1803 or later.");
        return false;
    }
    std::vector<const json::Value*> paths;
    if (!StringArray(args, "paths", 1000, paths, error)) return false;
    std::wstring parent;
    std::vector<bool> folders;
    for (const json::Value* arg : paths) {
        std::wstring path;
        if (!CheckPathArg(*arg, "paths", path, error)) return false;
        const std::wstring here = ParentFolder(path);
        if (here.empty()) {
            error = Invalid("a drive or share root cannot be packed; pass the items inside it");
            return false;
        }
        if (parent.empty()) parent = here;
        else if (!EqualI(parent, here)) {
            error = Invalid("all paths must be in the same folder");
            return false;
        }
        WIN32_FILE_ATTRIBUTE_DATA data{};
        DWORD win_error = 0;
        if (!Attributes(path, data, win_error)) {
            error = PathFailure(win_error, path);
            return false;
        }
        for (const std::wstring& seen : plan.sources)
            if (EqualI(seen, path)) {
                error = Invalid("each path may appear only once");
                return false;
            }
        folders.push_back((data.dwFileAttributes & kFolderAttribute) != 0);
        plan.sources.push_back(std::move(path));
    }
    plan.folder = parent;
    if (const json::Value* dest = args.find("dest_folder")) {
        if (!CheckPathArg(*dest, "dest_folder", plan.folder, error)) return false;
        WIN32_FILE_ATTRIBUTE_DATA data{};
        DWORD win_error = 0;
        if (!Attributes(plan.folder, data, win_error)) {
            error = PathFailure(win_error, plan.folder);
            return false;
        }
        if (!(data.dwFileAttributes & kFolderAttribute)) {
            error = Invalid("dest_folder is a file, not a folder");
            return false;
        }
    }
    for (size_t i = 0; i < plan.sources.size(); ++i)
        if (folders[i] && Within(plan.folder, plan.sources[i])) {
            error = Invalid("dest_folder must not be inside a folder being packed");
            return false;
        }
    if (IsSystemFolder(plan.folder)) {
        error = ToolOutcome::Fail("system_folder", "Pulse does not let AI clients create files in system folders.");
        return false;
    }
    const std::wstring folder_slash = plan.folder.back() == L'\\' ? plan.folder : plan.folder + L'\\';
    std::wstring name;
    if (const json::Value* name_arg = args.find("name")) {
        name = json::Wide(name_arg->as_string());
        while (!name.empty() && name.front() == L' ') name.erase(name.begin());
        while (!name.empty() && (name.back() == L' ' || name.back() == L'.')) name.pop_back();
        if (name.size() >= 4 && EqualI(name.substr(name.size() - 4), L".zip")) name.resize(name.size() - 4);
        while (!name.empty() && (name.back() == L' ' || name.back() == L'.')) name.pop_back();
        const bool bad_char = std::any_of(name.begin(), name.end(), [](wchar_t c) {
            return c < 0x20 || wcschr(L"\\/:*?\"<>|", c) != nullptr;
        });
        if (name.empty() || bad_char || name.size() > 240 || ReservedDeviceName(name)) {
            error = Invalid("name must be a plain file name (no folders or \\ / : * ? \" < > |)");
            return false;
        }
    } else {
        // As Pulse names it (zip_compress.h): one item gives its name without
        // the extension, several the name of their folder, also when the
        // archive goes to another folder.
        const std::wstring& named = plan.sources.size() == 1 ? plan.sources.front() : parent;
        const size_t slash = named.find_last_of(L'\\');
        name = slash == std::wstring::npos || slash + 1 >= named.size() ? std::wstring() : named.substr(slash + 1);
        if (plan.sources.size() == 1 && !folders.front()) {
            const size_t dot = name.find_last_of(L'.');
            if (dot != std::wstring::npos && dot > 0) name.resize(dot);
        }
        if (!name.empty() && name.back() == L':') name.clear();
        if (name.empty()) name = L"Archive";
    }
    plan.zip_path = folder_slash + name + L".zip";
    for (int n = 2; GetFileAttributesW(Win32Path(plan.zip_path).c_str()) != INVALID_FILE_ATTRIBUTES; ++n) {
        if (n >= 1000) {
            error = ToolOutcome::Fail("internal", "No free name for the archive.");
            return false;
        }
        plan.zip_path = folder_slash + name + L" (" + std::to_wstring(n) + L").zip";
    }
    return true;
}

ToolOutcome RunZip(const ZipPlan& plan, const std::atomic<bool>& cancelled) {
    const ZipCompressResult result = CompressToZip(plan.sources, plan.zip_path, &cancelled, {});
    if (result.cancelled || cancelled) return ToolOutcome::Fail("cancelled", "The call was cancelled; no archive was left.");
    if (!result.ok)
        return ToolOutcome::Fail("internal", "Could not create the archive" +
                                                 (result.error.empty() ? std::string(".") : ": " + U8(result.error)));
    ToolOutcome out;
    out.data = json::Value::Object();
    out.data.set("path", U8(plan.zip_path));
    WIN32_FILE_ATTRIBUTE_DATA data{};
    DWORD ignored = 0;
    if (Attributes(plan.zip_path, data, ignored)) out.data.set("size", SizeOf(data.nFileSizeHigh, data.nFileSizeLow));
    out.data.set("files", result.files);
    out.changed_files = true;
    return out;
}

} // namespace pulse::app::ai
