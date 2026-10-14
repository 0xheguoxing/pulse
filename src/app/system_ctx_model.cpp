#include "system_ctx_model.h"

#include "../common/localization.h"
#include "../ipc/ctx_menu_util.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <unordered_map>

namespace pulse::app {
namespace {

using sysctx::Action;
using sysctx::Item;
using sysctx::Kind;
using sysctx::Op;
using sysctx::OpResult;
using sysctx::Root;

const wchar_t* KindGlyph(Kind kind) {
    switch (kind) {
    case Kind::ShellEx: return L"\xEA86";   // puzzle: shell extension DLL
    case Kind::DragDrop: return L"\xE8B0";
    case Kind::ShellNew: return L"\xE710";
    case Kind::SendTo: return L"\xE724";
    case Kind::OpenWith: return L"\xE7AC";
    default: return L"\xE756";
    }
}

bool Toggleable(const Item& item) {
    return (item.kind == Kind::Verb || item.kind == Kind::ShellEx || item.kind == Kind::DragDrop) &&
           !item.is_protected && !item.trusted_installer && item.root != Root::File;
}

std::wstring Expand(const std::wstring& s) {
    if (s.find(L'%') == std::wstring::npos) return s;
    wchar_t buffer[MAX_PATH * 2];
    const DWORD n = ExpandEnvironmentStringsW(s.c_str(), buffer, ARRAYSIZE(buffer));
    return n > 0 && n <= ARRAYSIZE(buffer) ? std::wstring(buffer) : s;
}

bool HasExtension(const std::wstring& path, std::wstring_view ext) {
    if (path.size() <= ext.size()) return false;
    return _wcsicmp(path.c_str() + path.size() - ext.size(), std::wstring(ext).c_str()) == 0;
}

// An .exe/.ico the shell icon cache can draw: the Icon value first ("x.exe,0"),
// then the program itself. DLL resources keep the kind glyph.
std::wstring IconFile(const Item& item) {
    auto usable = [](std::wstring path) -> std::wstring {
        path = Expand(path);
        if (!(HasExtension(path, L".exe") || HasExtension(path, L".ico"))) return {};
        const DWORD attrs = GetFileAttributesW(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY) ? path : std::wstring();
    };
    if (!item.icon.empty()) {
        std::wstring icon = item.icon;
        if (!icon.empty() && icon[0] == L'"') {
            const size_t close = icon.find(L'"', 1);
            icon = icon.substr(1, close == std::wstring::npos ? std::wstring::npos : close - 1);
        } else if (const size_t comma = icon.rfind(L','); comma != std::wstring::npos) {
            icon = icon.substr(0, comma);
        }
        if (auto path = usable(icon); !path.empty()) return path;
    }
    if (item.kind == Kind::SendTo) return {};
    return item.file.empty() ? std::wstring() : usable(item.file);
}

std::wstring Timestamp(bool file_name) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t buffer[40];
    swprintf_s(buffer, file_name ? L"%04u%02u%02u-%02u%02u%02u" : L"%04u-%02u-%02uT%02u:%02u:%02u", t.wYear,
               t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buffer;
}

std::string Utf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

bool WriteBytes(const std::wstring& path, const void* data, size_t size, bool append) {
    HANDLE f = CreateFileW(path.c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = size == 0 || (WriteFile(f, data, static_cast<DWORD>(size), &written, nullptr) && written == size);
    CloseHandle(f);
    return ok;
}

std::wstring JsonEscape(const std::wstring& s) {
    std::wstring out;
    for (const wchar_t c : s) {
        if (c == L'"' || c == L'\\') { out += L'\\'; out += c; }
        else if (c < 0x20) { wchar_t b[8]; swprintf_s(b, L"\\u%04x", static_cast<unsigned>(c)); out += b; }
        else out += c;
    }
    return out;
}

std::wstring HelperPath() {
    wchar_t module[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
    if (n == 0 || n >= ARRAYSIZE(module)) return {};
    std::wstring path(module, n);
    const size_t slash = path.rfind(L'\\');
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1) + L"pulse_integration.exe";
}


// "CompanyName" / "FileDescription" from the version resource.
std::wstring VersionString(const std::vector<BYTE>& block, const wchar_t* name) {
    struct LangCodePage { WORD language; WORD codepage; };
    LangCodePage* translate = nullptr;
    UINT bytes = 0;
    std::vector<std::wstring> queries;
    if (VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translate), &bytes) &&
        translate && bytes >= sizeof(LangCodePage)) {
        wchar_t q[96];
        swprintf_s(q, L"\\StringFileInfo\\%04x%04x\\%s", translate[0].language, translate[0].codepage, name);
        queries.emplace_back(q);
    }
    queries.push_back(std::wstring(L"\\StringFileInfo\\040904b0\\") + name);
    queries.push_back(std::wstring(L"\\StringFileInfo\\080404b0\\") + name);
    for (const auto& q : queries) {
        wchar_t* value = nullptr;
        UINT len = 0;
        if (VerQueryValueW(block.data(), q.c_str(), reinterpret_cast<void**>(&value), &len) && value && len > 1 &&
            value[0]) {
            std::wstring s(value);
            while (!s.empty() && (s.back() == L' ' || s.back() == L'\0')) s.pop_back();
            return s;
        }
    }
    return {};
}

void ReadVersionInfo(const std::wstring& path, std::wstring& company, std::wstring& description) {
    DWORD unused = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &unused);
    if (size == 0) return;
    std::vector<BYTE> block(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, block.data())) return;
    company = VersionString(block, L"CompanyName");
    description = VersionString(block, L"FileDescription");
}

bool ContainsNoCase(const std::wstring& text, std::wstring_view needle) {
    if (needle.empty() || text.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= text.size(); ++i)
        if (_wcsnicmp(text.c_str() + i, needle.data(), needle.size()) == 0) return true;
    return false;
}

std::wstring FileName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool IsWindowsFile(const std::wstring& path) {
    static const std::wstring windows = [] {
        wchar_t dir[MAX_PATH];
        const UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
        return n > 0 && n < MAX_PATH ? std::wstring(dir, n) + L"\\" : std::wstring(L"C:\\Windows\\");
    }();
    return path.size() > windows.size() && _wcsnicmp(path.c_str(), windows.c_str(), windows.size()) == 0;
}

bool IsMicrosoft(const std::wstring& publisher, const std::wstring& file) {
    return ContainsNoCase(publisher, L"Microsoft") || (publisher.empty() && !file.empty() && IsWindowsFile(file));
}

const wchar_t* KindLabel(Kind kind) {
    switch (kind) {
    case Kind::ShellEx: return l10n::Pick(L"Shell 扩展", L"Shell extension");
    case Kind::DragDrop: return l10n::Pick(L"拖放扩展", L"Drag-and-drop handler");
    case Kind::ShellNew: return l10n::Pick(L"新建模板", L"New item template");
    case Kind::SendTo: return l10n::Pick(L"发送到目标", L"Send to target");
    case Kind::OpenWith: return l10n::Pick(L"打开方式程序", L"Open with program");
    default: return l10n::Pick(L"静态动词", L"Static verb");
    }
}

std::wstring MethodText(const Item& item, bool toggleable) {
    if (item.trusted_installer)
        return l10n::Pick(L"这个键归 TrustedInstaller 所有，Pulse 不会去改它。",
                          L"This key is owned by TrustedInstaller; Pulse leaves it alone.");
    if (item.is_protected)
        return l10n::Pick(L"这是 Windows 的基础命令，关掉会让资源管理器不好用，所以不提供开关。",
                          L"A basic Windows command; switching it off breaks File Explorer, so there is no switch.");
    if (!toggleable)
        return l10n::Pick(L"新建、发送到、打开方式的开关会在后续版本提供，现在可以查看和定位。",
                          L"Switches for New, Send to and Open with come in a later version; for now you can inspect them.");
    if (item.kind == Kind::Verb)
        return l10n::Pick(L"关闭时 Pulse 会在这个键里写入 HideBasedOnVelocityId = 0x639bc8、LegacyDisable、"
                          L"ProgrammaticAccessOnly（与 ContextMenuManager 相同），打开时删掉这些值。",
                          L"Switching it off writes HideBasedOnVelocityId = 0x639bc8, LegacyDisable and "
                          L"ProgrammaticAccessOnly into this key (like ContextMenuManager); switching it on removes them.");
    return l10n::Pick(L"关闭时 Pulse 把这个键移到同级的 “-” 分组下（与 ContextMenuManager 相同），打开时移回来。",
                      L"Switching it off moves this key under the sibling \"-\" group (like ContextMenuManager); "
                      L"switching it on moves it back.");
}

std::wstring SceneKeys(const sysctx::Scene& scene) {
    switch (scene.mode) {
    case sysctx::SceneMode::ShellNew: return L"HKCR\\.ext\\[ProgID\\]ShellNew";
    case sysctx::SceneMode::SendTo: return L"%APPDATA%\\Microsoft\\Windows\\SendTo";
    case sysctx::SceneMode::OpenWith: return L"HKCR\\Applications\\*.exe";
    case sysctx::SceneMode::DragDrop: return L"HKCR\\…\\shellex\\DragDropHandlers";
    default: break;
    }
    std::wstring out;
    for (const auto& key : scene.class_keys) {
        if (!out.empty()) out += L" · ";
        out += L"HKCR\\" + key;
    }
    return out;
}

// The key ContextMenuPrefs files the entry under in Pulse's own menu: shell
// extensions by CLSID, static verbs by their menu text (as Pulse sees it).
std::wstring PulseKey(const Item& item) {
    if (item.kind == Kind::ShellEx) return item.clsid.empty() ? std::wstring() : ipc::HandlerCatalogKey(item.clsid);
    if (item.kind == Kind::Verb && !item.display.empty()) return ipc::CatalogKey(item.display, false);
    return {};
}

} // namespace

SystemCtxModel::SystemCtxModel() {
    // Flat scene ids; the scene list (nav_) groups them like the prototype.
    scene_ids_ = {L"file", L"allobj", L"folder", L"dirbg", L"desk", L"drive", L"pc", L"bin", L"lnk", L"exe",
                  L"shellnew", L"sendto", L"openwith", L"dragdrop"};
    static const wchar_t* const kExts[] = {L".txt", L".png", L".jpg", L".mp4", L".pdf", L".docx", L".zip"};
    static const wchar_t* const kPerceived[] = {L"image", L"video", L"audio", L"text"};
    const int first_ext = static_cast<int>(scene_ids_.size());
    for (const auto* ext : kExts) scene_ids_.push_back(std::wstring(L"ext:") + ext);
    const int first_perc = static_cast<int>(scene_ids_.size());
    for (const auto* type : kPerceived) scene_ids_.push_back(std::wstring(L"perc:") + type);

    nav_ = {
        {L"文件", L"Files", L"\xE7C3", 0, {0}, {}},
        {L"所有对象", L"All objects", L"\xE8C8", 0, {1}, {}},
        {L"文件夹", L"Folders", L"\xE838", 0, {2}, {}},
        {L"目录背景", L"Folder background", L"\xE8B7", 0, {3}, {}},
        {L"桌面背景", L"Desktop", L"\xE7F4", 0, {4}, {}},
        {L"磁盘", L"Drives", L"\xEDA2", 0, {5}, {}},
        {L"此电脑", L"This PC", L"\xE977", 0, {6}, {}},
        {L"回收站", L"Recycle Bin", L"\xE74D", 0, {7}, {}},
        {L"快捷方式 .lnk", L"Shortcuts .lnk", L"\xE71B", 1, {8}, {}},
        {L"程序 .exe", L"Programs .exe", L"\xE756", 1, {9}, {}},
        {L"按扩展名", L"By extension", L"\xE8EC", 1, {}, {}},
        {L"按感知类型", L"By perceived type", L"\xE890", 1, {}, {}},
        {L"新建", L"New", L"\xE710", 2, {10}, {}},
        {L"发送到", L"Send to", L"\xE724", 2, {11}, {}},
        {L"打开方式", L"Open with", L"\xE7AC", 2, {12}, {}},
        {L"拖放菜单", L"Right-drag", L"\xE8B0", 2, {13}, {}},
    };
    for (int i = 0; i < static_cast<int>(std::size(kExts)); ++i) {
        nav_[10].scenes.push_back(first_ext + i);
        nav_[10].subs.emplace_back(kExts[i]);
    }
    static const wchar_t* const kPercZh[] = {L"图片", L"视频", L"音频", L"文本"};
    static const wchar_t* const kPercEn[] = {L"Images", L"Videos", L"Audio", L"Text"};
    for (int i = 0; i < static_cast<int>(std::size(kPerceived)); ++i) {
        nav_[11].scenes.push_back(first_perc + i);
        nav_[11].subs.emplace_back(l10n::Pick(kPercZh[i], kPercEn[i]));
    }
    sub_index_.assign(nav_.size(), 0);

    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local)
        data_dir_ = std::wstring(local) + L"\\Pulse\\SystemContextMenu";
    CoTaskMemFree(local);
}

SystemCtxModel::~SystemCtxModel() {
    if (worker_.joinable()) worker_.join();
}

void SystemCtxModel::SetNotifyWindow(HWND hwnd) {
    std::lock_guard lock(mutex_);
    notify_ = hwnd;
}

void SystemCtxModel::SetDataDirectoryForTesting(std::wstring directory) {
    std::lock_guard lock(mutex_);
    data_dir_ = std::move(directory);
}

std::wstring SystemCtxModel::DataDirectory() const {
    std::lock_guard lock(mutex_);
    return data_dir_;
}

std::wstring SystemCtxModel::BackupDirectory() const {
    std::lock_guard lock(mutex_);
    return data_dir_.empty() ? std::wstring() : data_dir_ + L"\\Backups";
}

std::wstring SystemCtxModel::OpKey(Root root, Kind kind, const std::wstring& key_path) {
    std::wstring key = std::to_wstring(static_cast<int>(root)) + L"|" + std::to_wstring(static_cast<int>(kind)) + L"|";
    for (const wchar_t c : key_path) key += static_cast<wchar_t>(std::towlower(c));
    return key;
}

std::wstring SystemCtxModel::PendingKey(const Op& op) {
    std::wstring key = OpKey(op.root, op.kind, op.key_path);
    if (op.action == Action::Position) key += L"|pos";
    return key;
}

void SystemCtxModel::EnsureLoaded() {
    {
        std::lock_guard lock(mutex_);
        if (loaded_ || state_ != ui::SysCtxView::State::Idle) return;
        loaded_ = true;
    }
    StartJob(ui::SysCtxView::State::Loading, {}, 0, false);
}

void SystemCtxModel::Refresh() {
    StartJob(ui::SysCtxView::State::Loading, {}, 0, false);
}

void SystemCtxModel::StartJob(ui::SysCtxView::State state, std::vector<Op> ops, size_t pop_undo, bool record_undo) {
    std::lock_guard lock(mutex_);
    if (state_ == ui::SysCtxView::State::Loading || state_ == ui::SysCtxView::State::Applying) return;
    if (worker_.joinable()) worker_.join();  // finished: it cleared the state before exiting
    state_ = state;
    dirty_ = true;
    worker_ = std::thread([this, ops = std::move(ops), pop_undo, record_undo]() mutable {
        RunJob(std::move(ops), pop_undo, record_undo);
    });
}

SystemCtxModel::ScanResult SystemCtxModel::Scan() const {
    const sysctx::Roots roots;
    ScanResult out;
    out.entries.resize(scene_ids_.size());
    out.keys.resize(scene_ids_.size());
    std::unordered_map<std::wstring, std::pair<std::wstring, std::wstring>> versions;  // file -> company, description
    for (size_t i = 0; i < scene_ids_.size(); ++i) {
        const std::wstring& id = scene_ids_[i];
        sysctx::Scene scene;
        if (id.rfind(L"ext:", 0) == 0) {
            scene = sysctx::ExtensionScene(roots, std::wstring_view(id).substr(4));
        } else if (id.rfind(L"perc:", 0) == 0) {
            scene = sysctx::PerceivedScene(std::wstring_view(id).substr(5));
        } else {
            for (const auto& builtin : sysctx::BuiltinScenes())
                if (builtin.id == id) scene = builtin;
        }
        out.keys[i] = SceneKeys(scene);
        if (scene.id.empty()) continue;
        for (auto& item : sysctx::Enumerate(roots, scene)) {
            Entry e;
            e.icon = IconFile(item);
            if (!item.file.empty() && !item.missing) {
                auto it = versions.find(item.file);
                if (it == versions.end()) {
                    std::pair<std::wstring, std::wstring> v;
                    ReadVersionInfo(item.file, v.first, v.second);
                    it = versions.emplace(item.file, std::move(v)).first;
                }
                e.publisher = it->second.first;
                e.description = it->second.second;
            }
            e.item = std::move(item);
            out.entries[i].push_back(std::move(e));
        }
    }
    return out;
}
std::wstring SystemCtxModel::WriteBackup(const std::vector<Op>& ops) const {
    const std::wstring dir = BackupDirectory();
    if (dir.empty() || SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr) > ERROR_ALREADY_EXISTS) return {};
    // regedit reads REGEDIT5 files as UTF-16LE with a BOM.
    const std::wstring text = L"\xFEFF" + sysctx::ExportReg(sysctx::Roots{}, ops);
    std::wstring path = dir + L"\\" + Timestamp(true) + L".reg";
    for (int n = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && n < 100; ++n)
        path = dir + L"\\" + Timestamp(true) + L"-" + std::to_wstring(n) + L".reg";
    return WriteBytes(path, text.data(), text.size() * sizeof(wchar_t), false) ? FileName(path) : std::wstring();
}

void SystemCtxModel::AppendLog(const std::vector<Op>& ops, const std::vector<OpResult>& results) const {
    std::wstring dir;
    {
        std::lock_guard lock(mutex_);
        dir = data_dir_;
    }
    if (dir.empty() || SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr) > ERROR_ALREADY_EXISTS) return;
    std::wstring lines;
    const std::wstring time = Timestamp(false);
    static constexpr const wchar_t* kKinds[] = {L"verb", L"shellex", L"dragdrop", L"shellnew", L"sendto", L"openwith"};
    for (size_t i = 0; i < ops.size(); ++i) {
        const Op& op = ops[i];
        lines += L"{\"time\":\"" + time + L"\",\"action\":\"" +
                 (op.action == Action::Disable  ? L"disable"
                  : op.action == Action::Enable ? L"enable"
                  : op.place == sysctx::Place::Top    ? L"position-top"
                  : op.place == sysctx::Place::Bottom ? L"position-bottom"
                                                      : L"position-default") +
                 L"\",\"kind\":\"" +
                 kKinds[static_cast<int>(op.kind)] + L"\",\"root\":\"" + (op.root == Root::Machine ? L"HKLM" : L"HKCU") +
                 L"\",\"key\":\"" + JsonEscape(op.key_path) + L"\",\"result\":\"" +
                 (i < results.size() ? sysctx::ResultName(results[i]) : L"skipped") + L"\"}\n";
    }
    const std::string bytes = Utf8(lines);
    WriteBytes(dir + L"\\ops.jsonl", bytes.data(), bytes.size(), true);
}

bool SystemCtxModel::RunElevated(const std::vector<Op>& ops, std::vector<OpResult>& results,
                                 std::wstring& error) const {
    results.assign(ops.size(), OpResult::Failed);
    const std::wstring helper = HelperPath();
    if (helper.empty() || GetFileAttributesW(helper.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = l10n::Pick(L"找不到 pulse_integration.exe", L"pulse_integration.exe is missing");
        return false;
    }
    wchar_t temp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp)) return false;
    const std::wstring stem = std::wstring(temp) + L"pulse_sysctx_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                              std::to_wstring(GetTickCount64());
    const std::wstring batch = stem + L".txt", result = stem + L".result";
    const std::string bytes = Utf8(sysctx::SerializeBatch(ops));
    if (!WriteBytes(batch, bytes.data(), bytes.size(), false)) return false;
    DeleteFileW(result.c_str());
    const std::wstring params = L"--system-ctx-apply \"" + batch + L"\" --result \"" + result + L"\"";
    HWND owner = nullptr;
    {
        std::lock_guard lock(mutex_);
        owner = notify_;
    }
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.hwnd = owner;
    info.lpVerb = L"runas";
    info.lpFile = helper.c_str();
    info.lpParameters = params.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess) {
        const DWORD e = GetLastError();
        DeleteFileW(batch.c_str());
        error = e == ERROR_CANCELLED ? l10n::Pick(L"已取消：没有获得管理员权限，未做任何更改。",
                                                  L"Cancelled: no administrator permission, nothing changed.")
                                     : l10n::Pick(L"无法启动管理员助手。", L"Couldn't start the administrator helper.");
        return false;
    }
    WaitForSingleObject(info.hProcess, INFINITE);
    CloseHandle(info.hProcess);
    std::wstring text;
    std::vector<OpResult> parsed;
    {
        HANDLE f = CreateFileW(result.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            char buffer[16384];
            DWORD read = 0;
            if (ReadFile(f, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
                const int n = MultiByteToWideChar(CP_UTF8, 0, buffer, static_cast<int>(read), nullptr, 0);
                text.resize(static_cast<size_t>(n));
                MultiByteToWideChar(CP_UTF8, 0, buffer, static_cast<int>(read), text.data(), n);
            }
            CloseHandle(f);
        }
    }
    DeleteFileW(batch.c_str());
    DeleteFileW(result.c_str());
    if (!sysctx::ParseResults(text, parsed) || parsed.size() != ops.size()) {
        error = l10n::Pick(L"管理员助手没有返回结果。", L"The administrator helper returned no result.");
        return false;
    }
    results = std::move(parsed);
    return true;
}

void SystemCtxModel::RunJob(std::vector<Op> ops, size_t pop_undo, bool record_undo) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::wstring message;
    bool error = false;
    std::vector<Op> done;
    std::wstring backup;  // 概览 · 最近的改动 names the .reg file
    bool cancelled = false;
    if (!ops.empty()) {
        const sysctx::Roots roots;
        // Undo steps arrive planned (Inverse keeps the recorded markers);
        // re-planning them would lose e.g. a cleared CommandFlags bit.
        for (Op& op : ops)
            if (!op.planned) sysctx::PlanOp(roots, op);
        backup = WriteBackup(ops);
        std::vector<Op> user, machine;
        for (const Op& op : ops) (op.root == Root::Machine ? machine : user).push_back(op);
        std::vector<OpResult> user_results, machine_results;
        for (const Op& op : user) user_results.push_back(sysctx::Apply(roots, op));
        if (!user.empty()) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        std::wstring elevated_error;
        if (!machine.empty() && !RunElevated(machine, machine_results, elevated_error)) {
            cancelled = true;
            message = elevated_error;
            error = true;
        }
        AppendLog(user, user_results);
        if (!cancelled) AppendLog(machine, machine_results);
        int failed = 0;
        auto collect = [&](const std::vector<Op>& list, const std::vector<OpResult>& results) {
            for (size_t i = 0; i < list.size() && i < results.size(); ++i) {
                if (results[i] == OpResult::Ok) done.push_back(list[i]);
                else ++failed;
            }
        };
        collect(user, user_results);
        if (!cancelled) collect(machine, machine_results);
        if (!cancelled || !done.empty()) {
            if (failed > 0) {
                message = l10n::Pick(L"有 ", L"") + std::to_wstring(failed) +
                          l10n::Pick(L" 项没有改成（可能被系统保护或已被其他程序改动），其余已完成。",
                                     L" change(s) failed (protected, or changed by another program); the rest are done.");
                error = true;
            } else if (!done.empty() && message.empty()) {
                message = (record_undo ? l10n::Pick(L"已更改 ", L"Changed ") : l10n::Pick(L"已撤销 ", L"Undid ")) +
                          std::to_wstring(done.size()) +
                          l10n::Pick(L" 项，改动前的注册表已备份。", L" entr(ies); the registry was backed up first.");
            }
        }
    }
    ScanResult scanned = Scan();
    HWND notify = nullptr;
    {
        std::lock_guard lock(mutex_);
        entries_ = std::move(scanned.entries);
        scene_keys_ = std::move(scanned.keys);
        if (!ops.empty()) {
            // Queued entries that went through (or failed for good) leave the
            // queue; a cancelled UAC prompt keeps them for another try.
            for (const Op& op : ops) {
                if (op.root != Root::Machine || !cancelled) pending_.erase(PendingKey(op));
            }
            if (record_undo && !done.empty()) {
                undo_.push_back({done});
                const std::wstring time = Timestamp(false);  // yyyy-mm-ddThh:mm:ss
                Change change;
                change.title = ChangeTitleLocked(done);
                change.caption = time.size() >= 16 ? time.substr(11, 5) : time;
                if (!backup.empty()) change.caption += L" · " + backup;
                changes_.push_back(std::move(change));
            }
            // Undo keeps its step until the reversal actually ran.
            if (!record_undo && !cancelled) {
                for (size_t n = 0; n < pop_undo && !undo_.empty(); ++n) {
                    undo_.pop_back();
                    // 概览 · 最近的改动: the newest change still standing is the one reversed.
                    const auto reversed = std::find_if(changes_.rbegin(), changes_.rend(),
                                                       [](const Change& c) { return !c.undone; });
                    if (reversed != changes_.rend()) reversed->undone = true;
                }
            }
            message_ = message;
            message_error_ = error;
        }
        state_ = ui::SysCtxView::State::Ready;
        dirty_ = true;
        notify = notify_;
    }
    if (SUCCEEDED(com)) CoUninitialize();
    // No lock from here on: StartJob joins this thread while holding it.
    if (notify) InvalidateRect(notify, nullptr, FALSE);
}

int SystemCtxModel::CurrentSceneLocked() const {
    const Nav& nav = nav_[static_cast<size_t>(nav_index_)];
    if (nav.scenes.empty()) return -1;
    const int sub = std::clamp(sub_index_[static_cast<size_t>(nav_index_)], 0, static_cast<int>(nav.scenes.size()) - 1);
    return nav.scenes[static_cast<size_t>(sub)];
}

std::shared_ptr<const ui::SysCtxView> SystemCtxModel::View() {
    std::lock_guard lock(mutex_);
    if (!dirty_ && view_) return view_;
    auto view = std::make_shared<ui::SysCtxView>();
    view->state = state_;
    view->scene = nav_index_;
    view->filter = filter_;
    view->side_tab = side_tab_;
    view->undo_steps = static_cast<int>(undo_.size());
    view->message = message_;
    view->message_error = message_error_;
    for (const auto& [key, op] : pending_) if (op.root == Root::Machine) ++view->pending;
    auto pending_op = [&](const Item& item) -> const Op* {
        const auto it = pending_.find(OpKey(item.root, item.kind, item.key_path));
        return it == pending_.end() ? nullptr : &it->second;
    };
    auto pending_pos = [&](const Item& item) -> const Op* {
        const auto it = pending_.find(OpKey(item.root, item.kind, item.key_path) + L"|pos");
        return it == pending_.end() ? nullptr : &it->second;
    };
    auto effective_on = [&](const Item& item) {
        const Op* op = pending_op(item);
        return op ? op->action == Action::Enable : item.enabled;
    };
    auto zone_of = [](sysctx::Place place) {
        return place == sysctx::Place::Top ? ui::kSysCtxZoneTop
             : place == sysctx::Place::Bottom ? ui::kSysCtxZoneBottom : ui::kSysCtxZoneDefault;
    };
    auto effective_zone = [&](const Item& item) {
        if (item.kind != Kind::Verb) return static_cast<int>(ui::kSysCtxZoneDefault);
        const Op* op = pending_pos(item);
        return static_cast<int>(zone_of(op ? op->place : item.position));
    };
    auto default_hidden = [&](const Entry& e) {
        return !e.item.enabled && !e.item.dash_marker && e.item.root == Root::Machine &&
               (e.item.kind == Kind::Verb) && IsMicrosoft(e.publisher, e.item.file.empty() ? L"C:\\Windows\\x" : e.item.file);
    };
    auto third_party = [&](const Entry& e) {
        return !e.publisher.empty() ? !ContainsNoCase(e.publisher, L"Microsoft")
                                    : (!e.item.file.empty() && !IsWindowsFile(e.item.file)) || e.item.missing;
    };

    // Scene list: counts over every sub scene.
    std::vector<bool> scene_pending(scene_ids_.size(), false);
    for (size_t s = 0; s < entries_.size(); ++s)
        for (const Entry& e : entries_[s])
            if (pending_op(e.item) || pending_pos(e.item)) scene_pending[s] = true;
    for (const Nav& nav : nav_) {
        ui::SysCtxSceneView scene;
        scene.label = l10n::Pick(nav.zh, nav.en);
        scene.glyph = nav.glyph;
        scene.group = nav.group;
        for (const int s : nav.scenes) {
            if (s >= static_cast<int>(entries_.size())) continue;
            for (const Entry& e : entries_[static_cast<size_t>(s)]) {
                ++scene.count;
                if (!effective_on(e.item)) ++scene.disabled;
            }
            scene.pending = scene.pending || scene_pending[static_cast<size_t>(s)];
        }
        view->scenes.push_back(std::move(scene));
    }
    // Totals: each scene id once.
    for (const auto& list : entries_) {
        for (const Entry& e : list) {
            ++view->total;
            view->disabled += effective_on(e.item) ? 0 : 1;
            view->missing += e.item.missing ? 1 : 0;
        }
    }

    const Nav& nav = nav_[static_cast<size_t>(nav_index_)];
    view->scene_title = l10n::Pick(nav.zh, nav.en);
    view->subs = nav.subs;
    view->sub = sub_index_[static_cast<size_t>(nav_index_)];
    const int s = CurrentSceneLocked();
    if (s >= 0 && s < static_cast<int>(scene_keys_.size())) view->scene_keys = scene_keys_[static_cast<size_t>(s)];

    row_map_.clear();
    preview_map_.clear();
    selected_row_ = -1;
    view->drag_item = drag_item_;
    view->drag_live = drag_live_;
    view->drag_y = drag_y_;
    view->drag_grab = drag_grab_;
    if (s >= 0 && s < static_cast<int>(entries_.size())) {
        const auto& list = entries_[static_cast<size_t>(s)];
        for (size_t i = 0; i < list.size(); ++i) {
            const Entry& e = list[i];
            const Item& item = e.item;
            const Op* toggle = pending_op(item);
            const Op* moving = pending_pos(item);
            const Op* pending = toggle ? toggle : moving;
            const bool on = effective_on(item);
            const int zone = effective_zone(item);
            const bool hidden_by_default = default_hidden(e);
            const bool third = third_party(e);
            const bool matches[ui::kSysCtxFilterCount] = {true, !on, item.root == Root::Machine, third, item.missing,
                                                          hidden_by_default};
            for (int f = 0; f < ui::kSysCtxFilterCount; ++f) view->filter_counts[static_cast<size_t>(f)] += matches[f] ? 1 : 0;
            const std::wstring key = OpKey(item.root, item.kind, item.key_path);

            std::wstring title = item.display.empty() ? item.key_name : item.display;
            // A bare handler key ("AABdzCtx") says less than the DLL's own description.
            if ((item.kind == Kind::ShellEx || item.kind == Kind::DragDrop) && !e.description.empty() &&
                (title == item.key_name || (!title.empty() && title[0] == L'{')))
                title = e.description;
            // Keyboard accelerators ("在此处打开命令窗口(&W)") are noise here.
            title.erase(std::remove(title.begin(), title.end(), L'&'), title.end());

            // Live preview: the menu after the queued changes; what is off
            // sits on the 已隐藏 shelf under it.
            {
                ui::SysCtxPreviewItem p;
                p.title = title;
                p.icon_path = e.icon;
                p.glyph = KindGlyph(item.kind);
                p.change = toggle ? (on ? 2 : 1) : 0;
                p.extended = item.extended;
                p.selected = key == selected_;
                p.hidden = !on;
                p.zone = zone;
                p.rank = item.kind != Kind::Verb ? 2 : zone == ui::kSysCtxZoneTop ? 0 : zone == ui::kSysCtxZoneBottom ? 3 : 1;
                p.moved = moving ? 1 + zone : 0;
                p.draggable = Toggleable(item) && state_ == ui::SysCtxView::State::Ready;
                p.movable = p.draggable && item.kind == Kind::Verb;
                view->preview.push_back(std::move(p));
                preview_map_.emplace_back(s, i);
            }
            if (!matches[filter_]) continue;

            ui::SysCtxRowView row;
            row.title = std::move(title);
            const std::wstring target = !item.file.empty() ? item.file : !item.command.empty() ? item.command : item.clsid;
            if (item.missing) {
                row.subtitle = l10n::Pick(L"程序不存在 · ", L"Program missing · ") +
                               (!item.command.empty() ? item.command : target);
            } else if (!e.publisher.empty() && !item.file.empty()) {
                row.subtitle = e.publisher + L" · " + FileName(item.file);
            } else if (!item.file.empty()) {
                row.subtitle = FileName(item.file);
            } else if (item.kind == Kind::SendTo) {
                row.subtitle = FileName(item.key_path);
            } else {
                row.subtitle = KindLabel(item.kind);
                if (!item.clsid.empty()) row.subtitle += L" · " + item.clsid;
            }
            if (item.kind == Kind::ShellEx && item.key_name != row.title) row.subtitle = item.key_name + L" · " + row.subtitle;
            row.icon_path = e.icon;
            row.glyph = KindGlyph(item.kind);
            row.on = on;
            row.pending = pending != nullptr;
            row.machine = item.root == Root::Machine;
            row.missing = item.missing;
            row.locked = item.is_protected || item.trusted_installer;
            row.system_owned = item.trusted_installer;
            row.read_only = !row.locked && !Toggleable(item);
            row.extended = item.extended;
            row.default_hidden = hidden_by_default;
            row.third_party = third;
            row.selected = key == selected_;
            row.place = zone;
            if (const std::wstring pulse_key = PulseKey(item); !pulse_key.empty())
                row.pulse = std::binary_search(pulse_hidden_.begin(), pulse_hidden_.end(), pulse_key) ? 2 : 1;
            row.kind_label = std::wstring(KindLabel(item.kind)) + L" · " +
                             (item.root == Root::File ? l10n::Pick(L"当前用户的文件夹", L"your SendTo folder")
                              : row.machine          ? l10n::Pick(L"所有用户", L"all users")
                                                     : l10n::Pick(L"当前用户", L"current user"));
            row.registry = item.root == Root::File ? item.key_path
                           : (row.machine ? L"HKLM\\Software\\Classes\\" : L"HKCU\\Software\\Classes\\") + item.key_path;
            row.file = !item.file.empty() ? item.file : !item.command.empty() ? item.command : item.clsid;
            row.publisher = e.publisher;
            row.method = MethodText(item, Toggleable(item));
            if (row.selected) {
                selected_row_ = static_cast<int>(view->rows.size());
                view->selected = selected_row_;
            }
            view->rows.push_back(std::move(row));
            row_map_.emplace_back(s, i);
        }
    }

    // 概览: ContextMenuManager marks, Pulse's handler timings, this session's changes.
    auto entry_title = [](const Entry& e) {
        std::wstring title = e.item.display.empty() ? e.item.key_name : e.item.display;
        if ((e.item.kind == Kind::ShellEx || e.item.kind == Kind::DragDrop) && !e.description.empty() &&
            (title == e.item.key_name || (!title.empty() && title[0] == L'{')))
            title = e.description;
        title.erase(std::remove(title.begin(), title.end(), L'&'), title.end());
        return title;
    };
    ui::SysCtxOverview& ov = view->overview;
    ov.shown = overview_;
    mark_map_.clear();
    slow_map_.clear();
    for (size_t sc = 0; sc < entries_.size(); ++sc) {
        for (size_t i = 0; i < entries_[sc].size(); ++i) {
            const Entry& e = entries_[sc][i];
            if (!e.item.dash_marker || effective_on(e.item)) continue;
            ++ov.marks;
            if (ov.mark_rows.size() >= 12) continue;  // the scenes list the rest
            ui::SysCtxMarkRow mark;
            mark.title = entry_title(e);
            mark.registry = (e.item.root == Root::Machine ? L"HKLM\\Software\\Classes\\" : L"HKCU\\Software\\Classes\\") +
                            e.item.key_path;
            mark.icon_path = e.icon;
            mark.glyph = KindGlyph(e.item.kind);
            ov.mark_rows.push_back(std::move(mark));
            mark_map_.emplace_back(static_cast<int>(sc), i);
        }
    }
    // Each measured CLSID once; the first scene registering it names it.
    struct Ranked {
        ui::SysCtxSlowRow row;
        std::pair<int, size_t> entry{-1, 0};
    };
    std::vector<Ranked> ranked;
    for (const auto& timing : timings_) {
        if (timing.clsid.empty()) continue;
        Ranked r;
        r.row.clsid = timing.clsid;
        r.row.title = timing.clsid;
        r.row.glyph = KindGlyph(Kind::ShellEx);
        r.row.ms = timing.ms;
        r.row.pulse_skipped = timing.pulse_skipped;
        for (size_t sc = 0; sc < entries_.size() && r.entry.first < 0; ++sc) {
            for (size_t i = 0; i < entries_[sc].size(); ++i) {
                const Entry& e = entries_[sc][i];
                if (e.item.clsid.size() != timing.clsid.size() || !ContainsNoCase(e.item.clsid, timing.clsid)) continue;
                r.entry = {static_cast<int>(sc), i};
                r.row.title = entry_title(e);
                r.row.publisher = !e.publisher.empty() ? e.publisher : FileName(e.item.file);
                r.row.icon_path = e.icon;
                r.row.glyph = KindGlyph(e.item.kind);
                r.row.on = effective_on(e.item);
                r.row.pending = pending_op(e.item) != nullptr;
                r.row.cmm = e.item.dash_marker;
                r.row.switchable = Toggleable(e.item) && state_ == ui::SysCtxView::State::Ready;
                break;
            }
        }
        if (r.entry.first < 0) r.row.publisher = l10n::Pick(L"不在已扫描的场景里", L"Not in a scanned scene");
        if (r.row.ms >= ui::kSysCtxSlowMs) ++ov.slow;
        ranked.push_back(std::move(r));
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const Ranked& a, const Ranked& b) { return a.row.ms > b.row.ms; });
    if (ranked.size() > 6) ranked.resize(6);
    for (Ranked& r : ranked) {
        ov.slow_rows.push_back(std::move(r.row));
        slow_map_.push_back(r.entry);
    }
    for (auto it = changes_.rbegin(); it != changes_.rend(); ++it) {
        if (!it->undone) ++ov.changes;
        if (ov.recent.size() < 4) ov.recent.push_back({it->title, it->caption, it->undone});
    }

    view_ = std::move(view);
    dirty_ = false;
    return view_;
}

void SystemCtxModel::SelectScene(int scene) {
    std::lock_guard lock(mutex_);
    if (scene < 0 || scene >= static_cast<int>(nav_.size())) return;
    if (scene != nav_index_) selected_.clear();
    nav_index_ = scene;
    overview_ = false;
    dirty_ = true;
}

void SystemCtxModel::SelectSub(int sub) {
    std::lock_guard lock(mutex_);
    const Nav& nav = nav_[static_cast<size_t>(nav_index_)];
    if (sub < 0 || sub >= static_cast<int>(nav.scenes.size())) return;
    sub_index_[static_cast<size_t>(nav_index_)] = sub;
    selected_.clear();
    dirty_ = true;
}

void SystemCtxModel::SetFilter(int filter) {
    std::lock_guard lock(mutex_);
    if (filter < 0 || filter >= ui::kSysCtxFilterCount) return;
    filter_ = filter;
    dirty_ = true;
}

void SystemCtxModel::SelectRow(int row) {
    std::lock_guard lock(mutex_);
    const Entry* e = RowEntryLocked(row);
    if (!e) return;
    const std::wstring key = OpKey(e->item.root, e->item.kind, e->item.key_path);
    selected_ = key == selected_ ? std::wstring() : key;
    if (!selected_.empty()) side_tab_ = 1;
    dirty_ = true;
}

void SystemCtxModel::SetSideTab(int tab) {
    std::lock_guard lock(mutex_);
    side_tab_ = tab == 1 ? 1 : 0;
    dirty_ = true;
}

void SystemCtxModel::BeginPreviewDrag(int item, float y, float grab) {
    std::lock_guard lock(mutex_);
    if (item < 0 || item >= static_cast<int>(preview_map_.size())) return;
    drag_item_ = item;
    drag_live_ = false;
    drag_start_ = drag_y_ = y;
    drag_grab_ = grab;
}

bool SystemCtxModel::PreviewDragMove(float y) {
    std::lock_guard lock(mutex_);
    if (drag_item_ < 0) return false;
    if (!drag_live_ && std::fabs(y - drag_start_) < 3.0f) return false;
    drag_live_ = true;
    drag_y_ = y;
    dirty_ = true;
    return true;
}

int SystemCtxModel::PreviewDragItem() const {
    std::lock_guard lock(mutex_);
    return drag_item_;
}

bool SystemCtxModel::PreviewDragLive() const {
    std::lock_guard lock(mutex_);
    return drag_item_ >= 0 && drag_live_;
}

void SystemCtxModel::EndPreviewDrag() {
    std::lock_guard lock(mutex_);
    if (drag_item_ < 0) return;
    drag_item_ = -1;
    drag_live_ = false;
    dirty_ = true;
}

void SystemCtxModel::SelectPreview(int item) {
    std::lock_guard lock(mutex_);
    if (item < 0 || item >= static_cast<int>(preview_map_.size())) return;
    const auto [s, i] = preview_map_[static_cast<size_t>(item)];
    if (s < 0 || s >= static_cast<int>(entries_.size()) || i >= entries_[static_cast<size_t>(s)].size()) return;
    const Item& it = entries_[static_cast<size_t>(s)][i].item;
    const std::wstring key = OpKey(it.root, it.kind, it.key_path);
    selected_ = key == selected_ ? std::wstring() : key;
    dirty_ = true;
}

void SystemCtxModel::DropPreview(int index, int zone) {
    std::vector<Op> run_now;
    {
        std::lock_guard lock(mutex_);
        if (state_ != ui::SysCtxView::State::Ready) return;
        if (index < 0 || index >= static_cast<int>(preview_map_.size())) return;
        if (zone < ui::kSysCtxZoneTop || zone > ui::kSysCtxZoneHidden) return;
        const auto [s, i] = preview_map_[static_cast<size_t>(index)];
        if (s < 0 || s >= static_cast<int>(entries_.size()) || i >= entries_[static_cast<size_t>(s)].size()) return;
        const Item item = entries_[static_cast<size_t>(s)][i].item;
        if (!Toggleable(item)) return;
        const std::wstring key = OpKey(item.root, item.kind, item.key_path);
        const auto toggle = pending_.find(key);
        const bool on = toggle != pending_.end() ? toggle->second.action == Action::Enable : item.enabled;
        // `restores`: the op only returns the entry to its scanned state, so
        // the queued op is dropped instead (per-user entries never queue).
        auto queue = [&](const std::wstring& k, const Op& op, bool restores) {
            if (restores) {
                pending_.erase(k);
            } else if (op.root == Root::Machine) {
                pending_[k] = op;
            } else {
                run_now.push_back(op);
            }
        };
        if (zone == ui::kSysCtxZoneHidden) {
            if (on) queue(key, sysctx::MakeOp(item, Action::Disable), !item.enabled);
        } else {
            if (!on) queue(key, sysctx::MakeOp(item, Action::Enable), item.enabled);
            if (item.kind == Kind::Verb) {
                const sysctx::Place want = zone == ui::kSysCtxZoneTop      ? sysctx::Place::Top
                                         : zone == ui::kSysCtxZoneBottom   ? sysctx::Place::Bottom
                                                                           : sysctx::Place::Default;
                const auto moving = pending_.find(key + L"|pos");
                const sysctx::Place now = moving != pending_.end() ? moving->second.place : item.position;
                if (want != now) queue(key + L"|pos", sysctx::MakePositionOp(item, want), want == item.position);
            }
        }
        message_.clear();
        dirty_ = true;
    }
    if (!run_now.empty()) StartJob(ui::SysCtxView::State::Applying, std::move(run_now), 0, true);
}

const SystemCtxModel::Entry* SystemCtxModel::RowEntryLocked(int row) const {
    if (row < 0) row = selected_row_;
    if (row < 0 || row >= static_cast<int>(row_map_.size())) return nullptr;
    const auto [s, i] = row_map_[static_cast<size_t>(row)];
    if (s < 0 || s >= static_cast<int>(entries_.size()) || i >= entries_[static_cast<size_t>(s)].size()) return nullptr;
    return &entries_[static_cast<size_t>(s)][i];
}

bool SystemCtxModel::ToggleEntryLocked(const Entry& e, Op& op) {
    if (state_ != ui::SysCtxView::State::Ready || !Toggleable(e.item)) return false;
    const std::wstring key = OpKey(e.item.root, e.item.kind, e.item.key_path);
    if (pending_.erase(key)) {
        dirty_ = true;
        return false;  // flipped back before it was applied
    }
    op = sysctx::MakeOp(e.item, e.item.enabled ? Action::Disable : Action::Enable);
    if (op.root == Root::Machine) {
        pending_[key] = op;
        message_.clear();
        dirty_ = true;
        return false;
    }
    return true;
}

void SystemCtxModel::ToggleRow(int row) {
    Op op;
    {
        std::lock_guard lock(mutex_);
        const Entry* e = RowEntryLocked(row);
        if (!e || !ToggleEntryLocked(*e, op)) return;
    }
    StartJob(ui::SysCtxView::State::Applying, {op}, 0, true);
}

void SystemCtxModel::ToggleMapped(const std::vector<std::pair<int, size_t>>& map, int index) {
    Op op;
    {
        std::lock_guard lock(mutex_);
        if (index < 0 || index >= static_cast<int>(map.size())) return;
        const auto [s, i] = map[static_cast<size_t>(index)];
        if (s < 0 || s >= static_cast<int>(entries_.size()) || i >= entries_[static_cast<size_t>(s)].size()) return;
        if (!ToggleEntryLocked(entries_[static_cast<size_t>(s)][i], op)) return;
    }
    StartJob(ui::SysCtxView::State::Applying, {op}, 0, true);
}

void SystemCtxModel::ToggleSlowRow(int index) { ToggleMapped(slow_map_, index); }

void SystemCtxModel::RestoreMarkRow(int index) { ToggleMapped(mark_map_, index); }

std::wstring SystemCtxModel::SlowRowClsid(int index) {
    std::lock_guard lock(mutex_);
    if (!view_ || index < 0 || index >= static_cast<int>(view_->overview.slow_rows.size())) return {};
    return view_->overview.slow_rows[static_cast<size_t>(index)].clsid;
}

void SystemCtxModel::ShowOverview() {
    std::lock_guard lock(mutex_);
    overview_ = true;
    dirty_ = true;
}

void SystemCtxModel::SetHandlerTimings(std::vector<ui::SysCtxHandlerTiming> timings) {
    std::lock_guard lock(mutex_);
    if (timings.size() == timings_.size() &&
        std::equal(timings.begin(), timings.end(), timings_.begin(), [](const auto& a, const auto& b) {
            return a.ms == b.ms && a.pulse_skipped == b.pulse_skipped && a.clsid == b.clsid;
        }))
        return;
    timings_ = std::move(timings);
    dirty_ = true;
}

void SystemCtxModel::SetPulseHidden(std::vector<std::wstring> keys) {
    std::lock_guard lock(mutex_);
    if (keys == pulse_hidden_) return;
    pulse_hidden_ = std::move(keys);
    dirty_ = true;
}

std::wstring SystemCtxModel::ChangeTitleLocked(const std::vector<Op>& ops) const {
    if (ops.empty()) return {};
    const Op& first = ops.front();
    // The scan after the batch lists the entry where the op left it.
    const std::wstring after = sysctx::KeyPathAfter(first);
    std::wstring name;
    for (const auto& list : entries_) {
        for (const Entry& e : list) {
            if (e.item.root != first.root || e.item.kind != first.kind ||
                (e.item.key_path != after && e.item.key_path != first.key_path))
                continue;
            name = e.item.display.empty() ? e.item.key_name : e.item.display;
            break;
        }
        if (!name.empty()) break;
    }
    if (name.empty()) name = FileName(first.key_path);
    name.erase(std::remove(name.begin(), name.end(), L'&'), name.end());
    std::wstring title = first.action == Action::Disable ? l10n::Pick(L"禁用", L"Turned off ")
                         : first.action == Action::Enable ? l10n::Pick(L"启用", L"Turned on ")
                                                          : l10n::Pick(L"移动", L"Moved ");
    title += l10n::Pick(L"「", L"\"") + name + l10n::Pick(L"」", L"\"");
    if (ops.size() > 1) {
        const bool chinese = std::wstring_view(l10n::Pick(L"zh", L"en")) == L"zh";
        title += chinese ? L"等 " + std::to_wstring(ops.size()) + L" 项"
                         : L" + " + std::to_wstring(ops.size() - 1) + L" more";
    }
    return title;
}

void SystemCtxModel::ApplyPending() {
    std::vector<Op> ops;
    {
        std::lock_guard lock(mutex_);
        if (state_ != ui::SysCtxView::State::Ready) return;
        for (const auto& [key, op] : pending_) ops.push_back(op);
    }
    if (!ops.empty()) StartJob(ui::SysCtxView::State::Applying, std::move(ops), 0, true);
}

void SystemCtxModel::DiscardPending() {
    std::lock_guard lock(mutex_);
    pending_.clear();
    dirty_ = true;
}

void SystemCtxModel::Undo() {
    std::vector<Op> ops;
    {
        std::lock_guard lock(mutex_);
        if (state_ != ui::SysCtxView::State::Ready || undo_.empty()) return;
        const auto& last = undo_.back().ops;
        for (auto it = last.rbegin(); it != last.rend(); ++it) ops.push_back(sysctx::Inverse(*it));
    }
    StartJob(ui::SysCtxView::State::Applying, std::move(ops), 1, false);
}

void SystemCtxModel::RestoreAll() {
    std::vector<Op> ops;
    size_t steps = 0;
    {
        std::lock_guard lock(mutex_);
        if (state_ != ui::SysCtxView::State::Ready || undo_.empty()) return;
        // Newest batch first, each in reverse, so every key ends where it began.
        for (auto batch = undo_.rbegin(); batch != undo_.rend(); ++batch)
            for (auto it = batch->ops.rbegin(); it != batch->ops.rend(); ++it) ops.push_back(sysctx::Inverse(*it));
        steps = undo_.size();
    }
    StartJob(ui::SysCtxView::State::Applying, std::move(ops), steps, false);
}

std::wstring SystemCtxModel::RowRegistryPath(int row) {
    std::lock_guard lock(mutex_);
    const Entry* e = RowEntryLocked(row);
    if (!e || e->item.root == Root::File) return {};
    return (e->item.root == Root::Machine ? L"HKEY_LOCAL_MACHINE" : L"HKEY_CURRENT_USER") +
           std::wstring(L"\\Software\\Classes\\") + e->item.key_path;
}

std::wstring SystemCtxModel::RowFile(int row) {
    std::lock_guard lock(mutex_);
    const Entry* e = RowEntryLocked(row);
    if (!e) return {};
    if (e->item.root == Root::File) return e->item.key_path;
    return e->item.file;
}

std::wstring SystemCtxModel::RowPulseKey(int row) {
    std::lock_guard lock(mutex_);
    const Entry* e = RowEntryLocked(row);
    return e ? PulseKey(e->item) : std::wstring();
}

} // namespace pulse::app
