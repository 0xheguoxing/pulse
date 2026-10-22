#include "shortcut_launch.h"

#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <vector>

namespace pulse::ops {
namespace {

std::wstring Expand(const wchar_t* text) {
    if (!text || !*text) return {};
    std::vector<wchar_t> buffer(32768);
    const DWORD length = ExpandEnvironmentStringsW(text, buffer.data(), static_cast<DWORD>(buffer.size()));
    return length > 0 && length <= buffer.size() ? std::wstring(buffer.data()) : std::wstring(text);
}

std::wstring_view Trim(std::wstring_view text) {
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t')) text.remove_suffix(1);
    return text;
}

} // namespace

bool ReadShortcutLaunch(const std::wstring& lnk_path, ShortcutLaunch& out) {
    out = {};
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
        FAILED(link.As(&file)) || FAILED(file->Load(lnk_path.c_str(), STGM_READ)))
        return false;
    std::vector<wchar_t> text(32768);
    if (SUCCEEDED(link->GetArguments(text.data(), static_cast<int>(text.size()))))
        out.arguments = Expand(text.data());
    std::fill(text.begin(), text.end(), L'\0');
    if (SUCCEEDED(link->GetWorkingDirectory(text.data(), static_cast<int>(text.size()))))
        out.directory = Expand(text.data());
    Microsoft::WRL::ComPtr<IShellLinkDataList> data;
    DWORD flags = 0;
    out.run_as = SUCCEEDED(link.As(&data)) && SUCCEEDED(data->GetFlags(&flags)) &&
                 (flags & SLDF_RUNAS_USER) != 0;
    return true;
}

std::wstring CombineShortcutArguments(std::wstring_view own, std::wstring_view dropped) {
    own = Trim(own);
    dropped = Trim(dropped);
    std::wstring combined(own);
    if (!combined.empty() && !dropped.empty()) combined += L' ';
    combined += dropped;
    return combined;
}

} // namespace pulse::ops
