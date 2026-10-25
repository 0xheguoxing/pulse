// ai_cleanup_scan.cpp - see ai_cleanup_scan.h.
#include "ai_cleanup_scan.h"

#include "../common/path_utils.h"
#include "../fs/fs_enum.h"
#include "../fs/fs_recycle.h"
#include "../index/index_feed.h"
#include "../ops/elevated_delete.h"

#include <shlobj.h>
#include <knownfolders.h>

#include <algorithm>
#include <cwctype>
#include <functional>
#include <map>
#include <unordered_set>

namespace pulse::app::ai {
namespace {

constexpr uint64_t kDay = 24ull * 3600ull * 10000000ull;   // FILETIME units

uint64_t Ticks(const FILETIME& ft) { return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }

uint64_t NowTicks() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    return Ticks(ft);
}

uint64_t SizeOf(const WIN32_FIND_DATAW& d) {
    return (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
}

std::wstring Known(const KNOWNFOLDERID& id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &p)) && p) out = p;
    CoTaskMemFree(p);
    return out;
}

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    return dir.back() == L'\\' ? dir + name : dir + L'\\' + name;
}

std::wstring Lower(std::wstring s) {
    for (wchar_t& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
    return s;
}

bool Under(const std::wstring& path, const std::wstring& root) {
    if (root.empty() || path.size() < root.size()) return false;
    if (!pulse::path::EqualInsensitive(std::wstring_view(path).substr(0, root.size()), root)) return false;
    return path.size() == root.size() || path[root.size()] == L'\\' || root.back() == L'\\';
}

bool OnDrive(const std::wstring& path, wchar_t drive) {
    return !drive || (path.size() >= 2 && path[1] == L':' && std::towupper(path[0]) == std::towupper(drive));
}

// One directory's entries (no . / ..). False when it cannot be listed.
bool List(const std::wstring& dir, const std::function<void(const WIN32_FIND_DATAW&)>& each) {
    WIN32_FIND_DATAW d{};
    HANDLE h = FindFirstFileExW(Join(dir, L"*").c_str(), FindExInfoBasic, &d, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (d.cFileName[0] == L'.' && (!d.cFileName[1] || (d.cFileName[1] == L'.' && !d.cFileName[2]))) continue;
        each(d);
    } while (FindNextFileW(h, &d));
    FindClose(h);
    return true;
}

// Folder sizes from the index, in batches. Missing answers stay unset.
std::map<std::wstring, uint64_t> IndexSizes(const std::vector<std::wstring>& paths) {
    std::map<std::wstring, uint64_t> out;
    if (paths.empty()) return out;
    HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    {
        index::IndexFeedConnection conn(cancel);
        for (size_t at = 0; at < paths.size(); at += index::kFolderSizeBatch) {
            std::vector<std::wstring> batch(paths.begin() + static_cast<ptrdiff_t>(at),
                                            paths.begin() + static_cast<ptrdiff_t>((std::min)(paths.size(), at + index::kFolderSizeBatch)));
            std::vector<index::IndexedFolderSize> values;
            if (!conn.FolderSizes(batch, values)) break;
            for (size_t i = 0; i < batch.size() && i < values.size(); ++i)
                if (values[i].available) out[Lower(batch[i])] = values[i].bytes;
        }
    }
    if (cancel) CloseHandle(cancel);
    return out;
}

bool Find(const std::map<std::wstring, uint64_t>& sizes, const std::wstring& path, uint64_t& bytes) {
    const auto it = sizes.find(Lower(path));
    if (it == sizes.end()) return false;
    bytes = it->second;
    return true;
}

}  // namespace

SpaceRoots CurrentSpaceRoots() {
    SpaceRoots r;
    wchar_t over[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"PULSE_AI_SPACE_ROOT", over, MAX_PATH);
    if (n && n < MAX_PATH) {
        const std::wstring root = over;
        r.fixture = true;
        r.profile = root;
        r.downloads = Join(root, L"Downloads");
        r.desktop = Join(root, L"Desktop");
        r.videos = Join(root, L"Videos");
        r.temp = Join(root, L"Temp");
        r.local_appdata = Join(root, L"AppData\\Local");
        r.roaming_appdata = Join(root, L"AppData\\Roaming");
        return r;
    }
    r.profile = Known(FOLDERID_Profile);
    r.downloads = Known(FOLDERID_Downloads);
    r.desktop = Known(FOLDERID_Desktop);
    r.videos = Known(FOLDERID_Videos);
    r.local_appdata = Known(FOLDERID_LocalAppData);
    r.roaming_appdata = Known(FOLDERID_RoamingAppData);
    wchar_t tmp[MAX_PATH + 1]{};
    if (const DWORD t = GetTempPathW(MAX_PATH + 1, tmp); t && t <= MAX_PATH) {
        r.temp = tmp;
        while (r.temp.size() > 3 && r.temp.back() == L'\\') r.temp.pop_back();
        wchar_t longer[MAX_PATH]{};
        if (const DWORD l = GetLongPathNameW(r.temp.c_str(), longer, MAX_PATH); l && l < MAX_PATH) r.temp = longer;
    }
    return r;
}

// ------------------------------------------------------------- disk usage --

bool MeasureDisk(wchar_t drive, DiskUsage& out, const std::atomic<bool>& cancel) {
    const uint64_t t0 = GetTickCount64();
    out = {};
    out.drive = static_cast<wchar_t>(std::towupper(drive));
    const std::wstring root = std::wstring(1, out.drive) + L":\\";
    const UINT type = GetDriveTypeW(root.c_str());
    if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) return false;
    ULARGE_INTEGER avail{}, total{}, free{};
    if (!GetDiskFreeSpaceExW(root.c_str(), &avail, &total, &free)) return false;
    out.total = total.QuadPart;
    out.free = free.QuadPart;
    const uint64_t used = out.total - (std::min)(out.free, out.total);

    const SpaceRoots roots = CurrentSpaceRoots();
    const std::wstring windows = Known(FOLDERID_Windows);
    const std::wstring pf = Known(FOLDERID_ProgramFiles), pfx = Known(FOLDERID_ProgramFilesX86),
                       pd = Known(FOLDERID_ProgramData);
    const std::wstring users_dir = Join(root, L"Users");
    const std::wstring profile = OnDrive(roots.profile, out.drive) ? roots.profile : std::wstring{};
    if (!profile.empty()) {
        const size_t slash = profile.find_last_of(L'\\');
        out.user_name = slash == std::wstring::npos ? profile : profile.substr(slash + 1);
    }

    // What to ask the index: the root's folders, the profile's folders and
    // the two AppData folders' children.
    struct RootItem { std::wstring path; uint64_t bytes; bool dir; };
    std::vector<RootItem> root_items;
    List(root, [&](const WIN32_FIND_DATAW& d) {
        const bool dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return;   // Documents and Settings
        root_items.push_back({Join(root, d.cFileName), dir ? 0 : SizeOf(d), dir});
    });
    std::vector<std::wstring> ask;
    for (const RootItem& it : root_items)
        if (it.dir) ask.push_back(it.path);
    std::vector<std::wstring> profile_dirs, local_kids, roaming_kids;
    const std::wstring appdata = profile.empty() ? std::wstring{} : Join(profile, L"AppData");
    if (!profile.empty()) {
        ask.push_back(profile);
        List(profile, [&](const WIN32_FIND_DATAW& d) {
            if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return;
            const std::wstring p = Join(profile, d.cFileName);
            if (pulse::path::EqualInsensitive(p, appdata)) return;
            profile_dirs.push_back(p);
            ask.push_back(p);
        });
        for (const auto& [base, kids] : {std::pair<const std::wstring*, std::vector<std::wstring>*>{&roots.local_appdata, &local_kids},
                                         {&roots.roaming_appdata, &roaming_kids}}) {
            if (base->empty() || !Under(*base, profile)) continue;
            ask.push_back(*base);
            List(*base, [&](const WIN32_FIND_DATAW& d) {
                if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return;
                kids->push_back(Join(*base, d.cFileName));
                ask.push_back(kids->back());
            });
        }
    }
    if (cancel.load()) return false;
    const std::map<std::wstring, uint64_t> sizes = IndexSizes(ask);
    out.indexed = !sizes.empty();

    std::vector<DiskTop> cands;
    const auto lower_leaf = [](const std::wstring& p) {
        const size_t s = p.find_last_of(L'\\');
        return Lower(s == std::wstring::npos ? p : p.substr(s + 1));
    };
    for (RootItem& it : root_items) {
        DiskTop t;
        t.path = it.path;
        const std::wstring leaf = lower_leaf(it.path);
        if (it.dir) {
            if (!Find(sizes, it.path, it.bytes)) { ++out.unindexed; continue; }
            if (pulse::path::EqualInsensitive(it.path, users_dir)) { out.users += it.bytes; continue; }
            if (leaf == L"$recycle.bin") continue;   // counted as the bin
            const bool sys = pulse::path::EqualInsensitive(it.path, windows) || leaf == L"system volume information" ||
                             leaf == L"recovery" || leaf == L"$winreagent" || leaf == L"$windows.~bt" || leaf == L"$windows.~ws";
            const bool prog = pulse::path::EqualInsensitive(it.path, pf) || pulse::path::EqualInsensitive(it.path, pfx) ||
                              pulse::path::EqualInsensitive(it.path, pd);
            t.place = sys ? DiskPlace::System : prog ? DiskPlace::Programs : DiskPlace::Other;
            if (sys) out.system += it.bytes;
            if (prog) out.programs += it.bytes;
        } else {
            t.is_file = true;
            if (leaf == L"hiberfil.sys") t.place = DiskPlace::Hibernate;
            else if (leaf == L"pagefile.sys" || leaf == L"swapfile.sys") t.place = DiskPlace::Pagefile;
            else continue;
            out.system += it.bytes;
        }
        t.bytes = it.bytes;
        cands.push_back(std::move(t));
    }
    if (!profile.empty()) {
        Find(sizes, profile, out.profile);
        for (const std::wstring& p : profile_dirs) {
            DiskTop t;
            if (!Find(sizes, p, t.bytes)) continue;
            t.path = p;
            t.place = DiskPlace::Profile;
            cands.push_back(std::move(t));
        }
        for (const auto& [base, kids] : {std::pair<const std::wstring*, const std::vector<std::wstring>*>{&roots.local_appdata, &local_kids},
                                         {&roots.roaming_appdata, &roaming_kids}}) {
            DiskTop t;
            if (base->empty() || !Find(sizes, *base, t.bytes)) continue;
            t.path = *base;
            t.place = DiskPlace::AppData;
            for (const std::wstring& k : *kids) {
                uint64_t b = 0;
                if (Find(sizes, k, b)) t.parts.push_back({k.substr(k.find_last_of(L'\\') + 1), b});
            }
            std::sort(t.parts.begin(), t.parts.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            if (t.parts.size() > 3) t.parts.resize(3);
            cands.push_back(std::move(t));
        }
    }
    const uint64_t known = out.system + out.programs + out.users;
    out.other = used > known ? used - known : 0;
    std::sort(cands.begin(), cands.end(), [](const DiskTop& a, const DiskTop& b) { return a.bytes > b.bytes; });
    for (DiskTop& t : cands) {
        if (out.top.size() >= 5 || t.bytes == 0) break;
        out.top.push_back(std::move(t));
    }
    const BinState bin = ReadBin(out.drive);
    out.recycle_bytes = bin.used_bytes;
    out.recycle_items = bin.items;
    out.ms = GetTickCount64() - t0;
    return true;
}

// ---------------------------------------------------------------- cleanup --
namespace {

// Squashed for comparison: lower case letters, digits and CJK only.
std::wstring Squash(const std::wstring& s) {
    std::wstring out;
    for (wchar_t ch : s) {
        if (std::iswalnum(ch)) out += static_cast<wchar_t>(std::towlower(ch));
        else if (ch >= 0x2E80) out += ch;
    }
    return out;
}

struct Program { std::wstring name, squashed, bare; };

std::vector<Program> InstalledPrograms() {
    std::vector<Program> out;
    std::unordered_set<std::wstring> seen;
    const wchar_t* key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    for (HKEY hive : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
            HKEY h = nullptr;
            if (RegOpenKeyExW(hive, key, 0, KEY_READ | view, &h) != ERROR_SUCCESS) continue;
            wchar_t sub[256];
            for (DWORD i = 0;; ++i) {
                DWORD len = 256;
                if (RegEnumKeyExW(h, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                wchar_t name[260]{};
                DWORD bytes = sizeof(name);
                if (RegGetValueW(h, sub, L"DisplayName", RRF_RT_REG_SZ, nullptr, name, &bytes) != ERROR_SUCCESS) continue;
                Program p;
                p.name = name;
                p.squashed = Squash(p.name);
                if (p.squashed.size() < 2 || !seen.insert(p.squashed).second) continue;
                p.bare = p.squashed;
                for (const wchar_t* vendor : {L"microsoft", L"google", L"mozilla", L"adobe", L"tencent", L"jetbrains", L"the"}) {
                    const std::wstring v = vendor;
                    if (p.bare.rfind(v, 0) == 0 && p.bare.size() > v.size() + 2) { p.bare = p.bare.substr(v.size()); break; }
                }
                out.push_back(std::move(p));
            }
            RegCloseKey(h);
        }
    }
    return out;
}

// "DockerDesktopInstaller" -> docker desktop installer; "python-3.13.0-amd64"
// -> python 3 13 0 amd 64.
std::vector<std::wstring> Words(const std::wstring& stem) {
    std::vector<std::wstring> out;
    std::wstring cur;
    const auto flush = [&] { if (!cur.empty()) out.push_back(Lower(cur)); cur.clear(); };
    for (size_t i = 0; i < stem.size(); ++i) {
        const wchar_t ch = stem[i];
        if (!std::iswalnum(ch) && ch < 0x2E80) { flush(); continue; }
        if (!cur.empty()) {
            const wchar_t prev = cur.back();
            const bool digit = std::iswdigit(ch) != 0, prev_digit = std::iswdigit(prev) != 0;
            const bool camel = std::iswupper(ch) && std::iswlower(prev);
            const bool acronym_end = std::iswupper(ch) && std::iswupper(prev) && i + 1 < stem.size() && std::iswlower(stem[i + 1]);
            if (digit != prev_digit || camel || acronym_end) flush();
        }
        cur += ch;
    }
    flush();
    return out;
}

bool StopWord(const std::wstring& w) {
    static const wchar_t* stop[] = {L"setup", L"installer", L"install", L"x", L"v", L"amd", L"arm", L"win", L"windows",
                                    L"user", L"full", L"offline", L"online", L"web", L"latest", L"release", L"stable",
                                    L"bit", L"msi", L"exe", L"x86", L"x64", L"en", L"us", L"zh", L"cn", L"stub", L"pro",
                                    L"plus", L"proplus", L"retail", L"build", L"update"};
    if (std::all_of(w.begin(), w.end(), [](wchar_t ch) { return std::iswdigit(ch) != 0; })) return true;
    for (const wchar_t* s : stop)
        if (w == s) return true;
    return false;
}

// The installed program an installer file belongs to, or nullptr.
const Program* InstalledFor(const std::wstring& name, const std::vector<Program>& programs) {
    const size_t dot = name.find_last_of(L'.');
    const std::wstring ext = dot == std::wstring::npos ? L"" : Lower(name.substr(dot + 1));
    const std::wstring stem = dot == std::wstring::npos ? name : name.substr(0, dot);
    const std::wstring lower = Lower(stem);
    if (lower.find(L"portable") != std::wstring::npos || lower.find(L"uninst") != std::wstring::npos) return nullptr;
    const bool package = ext == L"msi" || ext == L"msix" || ext == L"msixbundle" || ext == L"appx" || ext == L"iso";
    bool versioned = false;
    for (size_t i = 1; i + 1 < stem.size(); ++i)
        if (stem[i] == L'.' && std::iswdigit(stem[i - 1]) && std::iswdigit(stem[i + 1])) versioned = true;
    const bool setup_word = lower.find(L"setup") != std::wstring::npos || lower.find(L"install") != std::wstring::npos;
    const bool arch = lower.find(L"x64") != std::wstring::npos || lower.find(L"amd64") != std::wstring::npos ||
                      lower.find(L"win64") != std::wstring::npos || lower.find(L"x86") != std::wstring::npos;
    if (ext == L"exe" ? !(setup_word || versioned || arch) : !package) return nullptr;

    std::vector<std::wstring> keys;
    const std::wstring sq = Squash(stem);
    static const std::pair<const wchar_t*, const wchar_t*> alias[] = {
        {L"vscode", L"visualstudiocode"}, {L"7z", L"7zip"}, {L"wechat", L"wechat"}, {L"wechat", L"微信"},
        {L"weixin", L"微信"}, {L"chromesetup", L"chrome"}, {L"googlechrome", L"chrome"}, {L"node", L"nodejs"},
        {L"office", L"office"}, {L"office", L"365"}, {L"npp", L"notepad"}, {L"qqmusic", L"qq音乐"},
        {L"dingtalk", L"钉钉"}, {L"feishu", L"飞书"}, {L"lark", L"飞书"}, {L"baidunetdisk", L"百度网盘"}};
    for (const auto& [from, to] : alias)
        if (sq.rfind(from, 0) == 0) keys.push_back(to);
    if (keys.empty()) {
        std::vector<std::wstring> words;
        for (std::wstring& w : Words(stem))
            if (!StopWord(w)) words.push_back(std::move(w));
        if (words.empty()) return nullptr;
        std::wstring joined;
        for (const std::wstring& w : words) joined += w;
        if (joined.size() >= 3) keys.push_back(joined);
        static const wchar_t* vendors[] = {L"microsoft", L"google", L"mozilla", L"adobe", L"tencent", L"jetbrains"};
        size_t first = 0;
        for (const wchar_t* v : vendors)
            if (words[0] == v && words.size() > 1) first = 1;
        if (words[first].size() >= 3) keys.push_back(words[first]);
    }
    const Program* best = nullptr;
    for (const std::wstring& k : keys) {
        for (const Program& p : programs) {
            const bool hit = p.squashed.rfind(k, 0) == 0 || p.bare.rfind(k, 0) == 0 ||
                             (k.size() >= 2 && k[0] >= 0x2E80 && p.squashed.find(k) != std::wstring::npos);
            if (hit && (!best || p.name.size() < best->name.size())) best = &p;
        }
        if (best) break;
    }
    return best;
}

// Zip central directory: entry count and total uncompressed bytes.
bool ZipTotals(const std::wstring& path, uint64_t& files, uint64_t& bytes) {
    files = bytes = 0;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(h, &size) && size.QuadPart >= 22) {
        const DWORD tail = static_cast<DWORD>((std::min<LONGLONG>)(size.QuadPart, 65557));
        std::vector<uint8_t> buf(tail);
        LARGE_INTEGER at{};
        at.QuadPart = size.QuadPart - tail;
        DWORD read = 0;
        if (SetFilePointerEx(h, at, nullptr, FILE_BEGIN) && ReadFile(h, buf.data(), tail, &read, nullptr) && read == tail) {
            for (size_t i = tail - 22 + 1; i-- > 0;) {
                if (buf[i] == 0x50 && buf[i + 1] == 0x4B && buf[i + 2] == 0x05 && buf[i + 3] == 0x06) {
                    uint32_t cd_size = 0, cd_off = 0;
                    uint16_t count = 0;
                    memcpy(&count, &buf[i + 10], 2);
                    memcpy(&cd_size, &buf[i + 12], 4);
                    memcpy(&cd_off, &buf[i + 16], 4);
                    if (count == 0xFFFF || cd_off == 0xFFFFFFFFu || cd_size > 64u * 1024 * 1024) break;   // zip64: not judged
                    std::vector<uint8_t> cd(cd_size);
                    LARGE_INTEGER co{};
                    co.QuadPart = cd_off;
                    DWORD got = 0;
                    if (!SetFilePointerEx(h, co, nullptr, FILE_BEGIN) || !ReadFile(h, cd.data(), cd_size, &got, nullptr) || got != cd_size) break;
                    size_t p = 0;
                    ok = true;
                    for (uint16_t e = 0; e < count; ++e) {
                        if (p + 46 > cd.size() || cd[p] != 0x50 || cd[p + 1] != 0x4B || cd[p + 2] != 0x01 || cd[p + 3] != 0x02) { ok = false; break; }
                        uint32_t usize = 0;
                        uint16_t nlen = 0, xlen = 0, clen = 0;
                        memcpy(&usize, &cd[p + 24], 4);
                        memcpy(&nlen, &cd[p + 28], 2);
                        memcpy(&xlen, &cd[p + 30], 2);
                        memcpy(&clen, &cd[p + 32], 2);
                        if (usize == 0xFFFFFFFFu) { ok = false; break; }
                        const bool dir = nlen && p + 46 + nlen <= cd.size() && (cd[p + 46 + nlen - 1] == '/' || cd[p + 46 + nlen - 1] == '\\');
                        if (!dir) { ++files; bytes += usize; }
                        p += 46u + nlen + xlen + clen;
                    }
                    break;
                }
            }
        }
    }
    CloseHandle(h);
    return ok;
}

struct Walk {
    uint64_t files = 0, bytes = 0, newest = 0;
    bool busy = false, capped = false;
};

// Everything under a folder, newest change and whether a file is in use.
void WalkTree(const std::wstring& dir, Walk& w, bool probe, const std::atomic<bool>& cancel, int depth = 0) {
    if (cancel.load() || w.capped || w.busy || depth > 24) return;
    List(dir, [&](const WIN32_FIND_DATAW& d) {
        if (w.capped || w.busy) return;
        if (w.files > 20000) { w.capped = true; return; }
        const std::wstring p = Join(dir, d.cFileName);
        w.newest = (std::max)({w.newest, Ticks(d.ftLastWriteTime), Ticks(d.ftCreationTime)});
        if (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { w.busy = true; return; }   // never follow links
        if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { WalkTree(p, w, probe, cancel, depth + 1); return; }
        ++w.files;
        w.bytes += SizeOf(d);
        if (probe) {
            HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) w.busy = true;
            else CloseHandle(h);
        }
    });
}

bool SameContent(const std::wstring& a, const std::wstring& b, const std::atomic<bool>& cancel) {
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE ha = CreateFileW(a.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (ha == INVALID_HANDLE_VALUE) return false;
    HANDLE hb = CreateFileW(b.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hb == INVALID_HANDLE_VALUE) { CloseHandle(ha); return false; }
    constexpr DWORD kChunk = 1u << 20;
    std::vector<uint8_t> ba(kChunk), bb(kChunk);
    bool same = true;
    for (;;) {
        DWORD ra = 0, rb = 0;
        if (cancel.load() || !ReadFile(ha, ba.data(), kChunk, &ra, nullptr) || !ReadFile(hb, bb.data(), kChunk, &rb, nullptr) ||
            ra != rb) {
            same = false;
            break;
        }
        if (ra == 0) break;
        if (memcmp(ba.data(), bb.data(), ra) != 0) {
            same = false;
            break;
        }
    }
    CloseHandle(ha);
    CloseHandle(hb);
    return same;
}

// "IMG_2041 (1).jpg" / "report - 副本.pdf" / "a - Copy.txt": the original's
// name, or empty.
std::wstring OriginalName(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    std::wstring stem = dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot);
    const std::wstring ext = dot == std::wstring::npos || dot == 0 ? L"" : name.substr(dot);
    std::wstring base;
    if (stem.size() > 3 && stem.back() == L')') {
        const size_t open = stem.find_last_of(L'(');
        if (open != std::wstring::npos && open + 2 < stem.size() &&
            std::all_of(stem.begin() + static_cast<ptrdiff_t>(open + 1), stem.end() - 1, [](wchar_t ch) { return std::iswdigit(ch) != 0; })) {
            base = stem.substr(0, open);
            while (!base.empty() && base.back() == L' ') base.pop_back();
        }
    }
    for (const wchar_t* suffix : {L" - \u526F\u672C", L" - Copy", L" - \u590D\u5236", L"\u526F\u672C", L" copy"}) {
        const std::wstring sf = suffix;
        if (base.empty() && stem.size() > sf.size() && Lower(stem.substr(stem.size() - sf.size())) == Lower(sf))
            base = stem.substr(0, stem.size() - sf.size());
    }
    return base.empty() ? std::wstring{} : base + ext;
}

bool IsVideo(const std::wstring& lower_ext) {
    for (const wchar_t* e : {L"mp4", L"mkv", L"mov", L"avi", L"wmv", L"m4v", L"ts", L"m2ts", L"flv", L"webm", L"mpg", L"mpeg"})
        if (lower_ext == e) return true;
    return false;
}

std::wstring ExtOf(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? std::wstring{} : Lower(name.substr(dot + 1));
}

}  // namespace

void FindCleanup(wchar_t drive, CleanupScan& out, CleanProgress& progress, const std::atomic<bool>& cancel) {
    const uint64_t t0 = GetTickCount64();
    out = {};
    const SpaceRoots roots = CurrentSpaceRoots();
    const uint64_t now = NowTicks();
    std::vector<CleanCategory> cats(static_cast<size_t>(CleanKind::Count));
    for (size_t i = 0; i < cats.size(); ++i) cats[i].kind = static_cast<CleanKind>(i);
    cats[static_cast<size_t>(CleanKind::OldVideo)].default_on = false;
    const auto add = [&](CleanKind k, CleanItem it) {
        if (!OnDrive(it.path, drive)) return;
        CleanCategory& c = cats[static_cast<size_t>(k)];
        c.bytes += it.bytes;
        c.files += it.files;
        c.items.push_back(std::move(it));
    };
    // Protected: never offered even when a user folder points there.
    std::vector<std::wstring> guard;
    for (const KNOWNFOLDERID* id : {&FOLDERID_Windows, &FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86, &FOLDERID_ProgramData})
        if (std::wstring g = Known(*id); !g.empty()) guard.push_back(std::move(g));
    const auto guarded = [&](const std::wstring& p) {
        return std::any_of(guard.begin(), guard.end(), [&](const std::wstring& g) { return Under(p, g); });
    };

    // The top of Downloads (and one level down, where browsers sometimes
    // sort), files only.
    struct File { std::wstring dir, name; WIN32_FIND_DATAW d; };
    std::vector<File> downloads;
    if (!roots.downloads.empty() && !guarded(roots.downloads)) {
        List(roots.downloads, [&](const WIN32_FIND_DATAW& d) {
            if (d.dwFileAttributes & (FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_REPARSE_POINT)) return;
            if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) downloads.push_back({roots.downloads, d.cFileName, d});
        });
    }

    // 0. Installers whose program is installed.
    progress.stage = 0;
    progress.fraction = 0.05f;
    {
        const std::vector<Program> programs = InstalledPrograms();
        for (const File& f : downloads) {
            if (cancel.load()) return;
            const Program* p = InstalledFor(f.name, programs);
            if (!p) continue;
            CleanItem it;
            it.path = Join(f.dir, f.name);
            it.bytes = SizeOf(f.d);
            it.mtime = f.d.ftLastWriteTime;
            it.arg = p->name;
            add(CleanKind::Installer, std::move(it));
        }
    }
    // 1. Archives with their extracted folder beside them.
    progress.stage = 1;
    progress.fraction = 0.25f;
    for (const File& f : downloads) {
        if (cancel.load()) return;
        const std::wstring ext = ExtOf(f.name);
        if (ext != L"zip" && ext != L"7z" && ext != L"rar") continue;
        const std::wstring stem = f.name.substr(0, f.name.size() - ext.size() - 1);
        std::wstring folder = Join(f.dir, stem);
        DWORD a = GetFileAttributesW(folder.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            const std::wstring orig = OriginalName(stem + L".x");   // "pack (1).zip" -> "pack"
            if (orig.empty()) continue;
            folder = Join(f.dir, orig.substr(0, orig.size() - 2));
            a = GetFileAttributesW(folder.c_str());
            if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) continue;
        }
        if (a & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        Walk w;
        WalkTree(folder, w, false, cancel);
        if (w.capped || w.files == 0) continue;
        const uint64_t bytes = SizeOf(f.d);
        uint64_t zf = 0, zb = 0;
        bool verified = false;
        if (ext == L"zip" && ZipTotals(Join(f.dir, f.name), zf, zb)) {
            if (w.files < zf || w.bytes + w.bytes / 50 < zb) continue;   // the folder lacks what the zip holds
            verified = true;
        } else if (w.bytes < bytes) {
            continue;   // smaller than the archive: probably not all of it
        }
        CleanItem it;
        it.path = Join(f.dir, f.name);
        it.bytes = bytes;
        it.mtime = f.d.ftLastWriteTime;
        it.arg = folder.substr(folder.find_last_of(L'\\') + 1);
        it.verified = verified;
        add(CleanKind::Extracted, std::move(it));
    }
    // 2. (1)(2) copies identical to the original beside them.
    progress.stage = 2;
    progress.fraction = 0.45f;
    for (const std::wstring& dir : {roots.downloads, roots.desktop}) {
        if (dir.empty() || guarded(dir)) continue;
        std::vector<File> files;
        if (dir == roots.downloads) files = downloads;
        else List(dir, [&](const WIN32_FIND_DATAW& d) {
                if (!(d.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_REPARSE_POINT)))
                    files.push_back({dir, d.cFileName, d});
            });
        std::map<std::wstring, const File*> by_name;
        for (const File& f : files) by_name[Lower(f.name)] = &f;
        for (const File& f : files) {
            if (cancel.load()) return;
            const std::wstring orig = OriginalName(f.name);
            if (orig.empty()) continue;
            const auto o = by_name.find(Lower(orig));
            if (o == by_name.end() || SizeOf(o->second->d) != SizeOf(f.d) || SizeOf(f.d) == 0) continue;
            if (!SameContent(Join(dir, f.name), Join(dir, o->second->name), cancel)) continue;
            CleanItem it;
            it.path = Join(dir, f.name);
            it.bytes = SizeOf(f.d);
            it.mtime = f.d.ftLastWriteTime;
            it.arg = o->second->name;
            add(CleanKind::Copy, std::move(it));
        }
    }
    // 3. Temp: untouched for 7 days and not in use.
    progress.stage = 3;
    progress.fraction = 0.65f;
    if (!roots.temp.empty() && !guarded(roots.temp)) {
        std::vector<WIN32_FIND_DATAW> top;
        List(roots.temp, [&](const WIN32_FIND_DATAW& d) { top.push_back(d); });
        size_t done = 0;
        for (const WIN32_FIND_DATAW& d : top) {
            if (cancel.load()) return;
            progress.fraction = 0.65f + 0.2f * static_cast<float>(++done) / static_cast<float>((std::max<size_t>)(top.size(), 1));
            if (d.dwFileAttributes & (FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            const std::wstring p = Join(roots.temp, d.cFileName);
            uint64_t newest = (std::max)(Ticks(d.ftLastWriteTime), Ticks(d.ftCreationTime));
            CleanItem it;
            it.path = p;
            it.mtime = d.ftLastWriteTime;
            if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                Walk w;
                WalkTree(p, w, true, cancel);
                if (w.busy || w.capped) continue;
                newest = (std::max)(newest, w.newest);
                it.dir = true;
                it.files = w.files;
                it.bytes = w.bytes;
            } else {
                HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h == INVALID_HANDLE_VALUE) continue;   // in use (or not ours)
                CloseHandle(h);
                it.bytes = SizeOf(d);
            }
            if (now < newest || now - newest < 7 * kDay) continue;
            it.days = static_cast<uint32_t>((now - newest) / kDay);
            if (it.bytes == 0 && it.files == 0) it.files = 1;
            add(CleanKind::Temp, std::move(it));
        }
    }
    // 4. Videos of 1 GB or more not opened for a year (offered unchecked).
    progress.stage = 4;
    progress.fraction = 0.88f;
    {
        std::unordered_set<std::wstring> seen;
        std::function<void(const std::wstring&, int)> walk = [&](const std::wstring& dir, int depth) {
            if (depth > 6 || cancel.load() || seen.size() > 50000) return;
            List(dir, [&](const WIN32_FIND_DATAW& d) {
                if (d.dwFileAttributes & (FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_REPARSE_POINT)) return;
                const std::wstring p = Join(dir, d.cFileName);
                if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { walk(p, depth + 1); return; }
                if (SizeOf(d) < (1ull << 30) || !IsVideo(ExtOf(d.cFileName)) || !seen.insert(Lower(p)).second) return;
                const uint64_t last = (std::max)(Ticks(d.ftLastWriteTime), Ticks(d.ftLastAccessTime));
                if (now < last || now - last < 365 * kDay) return;
                CleanItem it;
                it.path = p;
                it.bytes = SizeOf(d);
                it.mtime = d.ftLastWriteTime;
                it.days = static_cast<uint32_t>((now - last) / kDay);
                add(CleanKind::OldVideo, std::move(it));
            });
        };
        for (const std::wstring& dir : {roots.videos, roots.downloads, roots.desktop})
            if (!dir.empty() && !guarded(dir)) walk(dir, 0);
    }
    progress.fraction = 1.0f;
    for (CleanCategory& c : cats) {
        if (c.items.empty()) continue;
        std::sort(c.items.begin(), c.items.end(), [](const CleanItem& a, const CleanItem& b) { return a.bytes > b.bytes; });
        out.cats.push_back(std::move(c));
    }
    out.ms = GetTickCount64() - t0;
}

// -------------------------------------------------------------------- bin --

BinState ReadBin(wchar_t drive) {
    BinState b;
    const std::wstring root = std::wstring(1, static_cast<wchar_t>(std::towupper(drive))) + L":\\";
    DWORD err = 0;
    b.available = ops::RecycleBinAvailable(root, &err) && !err;
    const ops::RecycleBinLimits limits = ops::ReadRecycleBinLimits(root, &err);
    b.max_bytes = limits.disabled ? 0 : limits.max_bytes;
    std::vector<fs::DirEntry> items;
    fs::EnumerateRecycleBinAtRoot(root + L"$Recycle.Bin", items);
    for (const fs::DirEntry& e : items) b.used_bytes += e.size;
    b.items = items.size();
    return b;
}

std::vector<BinHit> FindInBin(const std::vector<std::wstring>& originals, FILETIME since) {
    std::vector<BinHit> out;
    std::unordered_set<std::wstring> want;
    std::unordered_set<wchar_t> drives;
    for (const std::wstring& p : originals) {
        want.insert(Lower(fs::NormalizePath(p)));
        if (p.size() >= 2 && p[1] == L':') drives.insert(static_cast<wchar_t>(std::towupper(p[0])));
    }
    // $I times are written as the item goes in; allow the clock a moment.
    const uint64_t from = Ticks(since) > 20000000ull ? Ticks(since) - 20000000ull : 0;
    for (wchar_t d : drives) {
        std::vector<fs::DirEntry> items;
        fs::EnumerateRecycleBinAtRoot(std::wstring(1, d) + L":\\$Recycle.Bin", items);
        for (const fs::DirEntry& e : items) {
            if (Ticks(e.mtime) < from || !want.count(Lower(e.full_path))) continue;
            BinHit h;
            h.original = e.full_path;
            h.payload = e.recycle_path;
            h.index = fs::RecycleIndexPath(e.recycle_path);
            h.bytes = e.size;
            if (!h.payload.empty() && !h.index.empty()) out.push_back(std::move(h));
        }
    }
    return out;
}

namespace {
struct AiList {
    std::wstring title;
    std::vector<AiListRow> rows;
    std::wstring column;
};
std::map<uint32_t, AiList>& AiLists() {
    static std::map<uint32_t, AiList> lists;
    return lists;
}
}  // namespace

void PutAiList(uint32_t id, std::wstring title, std::vector<AiListRow> rows, std::wstring column) {
    auto& lists = AiLists();
    // A long session keeps only the latest few.
    while (lists.size() >= 16 && !lists.contains(id)) lists.erase(lists.begin());
    lists[id] = {std::move(title), std::move(rows), std::move(column)};
}

std::wstring AiListColumn(uint32_t id) {
    const auto& lists = AiLists();
    const auto it = lists.find(id);
    return it == lists.end() ? std::wstring() : it->second.column;
}

bool GetAiList(uint32_t id, std::wstring& title, std::vector<AiListRow>& rows) {
    const auto& lists = AiLists();
    const auto it = lists.find(id);
    if (it == lists.end()) return false;
    title = it->second.title;
    rows = it->second.rows;
    return true;
}

}  // namespace pulse::app::ai
