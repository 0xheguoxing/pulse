// wsl_path.h — recognise paths into a WSL distribution (\\wsl$\..., \\wsl.localhost\...).
//
// Touching such a path starts the distribution's VM, and an open handle on it
// keeps `wsl --shutdown` waiting (#360), so Pulse never reaches them in the
// background: no reachability probes, warm-up listings or network index.
#pragma once

#include <string_view>

namespace pulse::fs {

inline bool IsWslPath(std::wstring_view path) {
    if (path.size() > 8 && (path.substr(0, 8) == L"\\\\?\\UNC\\" || path.substr(0, 8) == L"\\\\?\\unc\\"))
        path.remove_prefix(8);
    else if (path.size() > 2 && (path[0] == L'\\' || path[0] == L'/') && (path[1] == L'\\' || path[1] == L'/'))
        path.remove_prefix(2);
    else
        return false;
    const size_t end = path.find_first_of(L"\\/");
    const std::wstring_view server = path.substr(0, end);
    const auto equals = [&](std::wstring_view name) {
        if (server.size() != name.size()) return false;
        for (size_t i = 0; i < name.size(); ++i) {
            wchar_t c = server[i];
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
            if (c != name[i]) return false;
        }
        return true;
    };
    return equals(L"wsl$") || equals(L"wsl.localhost");
}

} // namespace pulse::fs
