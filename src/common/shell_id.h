// shell_id.h - ids for shell items with no file system path (a phone's
// storages, folders and files).
//
// Their parsing names (SIGDN_DESKTOPABSOLUTEPARSING) do not parse back below
// the device: SHParseDisplayName gives E_INVALIDARG for an MTP folder or file
// (a user's Xiaomi phone, Windows 11 25H2), so every copy, paste, open and
// context menu of such an item failed with 参数错误. Pulse carries the item's
// ID list instead, as Explorer does on the clipboard and in Quick access:
// "::pidl:" + the absolute ID list in hex. Anything else parses as before.
// Header-only: pulse, pulse_shell and the preview host share it.
#pragma once
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <cstring>
#include <string>
#include <string_view>

namespace pulse::shell_id {

inline constexpr std::wstring_view kPidlPrefix = L"::pidl:";

inline bool IsPidlId(std::wstring_view id) {
    return id.size() > kPidlPrefix.size() && id.substr(0, kPidlPrefix.size()) == kPidlPrefix;
}

inline std::wstring EncodePidl(PCIDLIST_ABSOLUTE pidl) {
    if (!pidl) return {};
    static constexpr wchar_t kHex[] = L"0123456789ABCDEF";
    const UINT size = ILGetSize(pidl);
    const auto* bytes = reinterpret_cast<const BYTE*>(pidl);
    std::wstring out(kPidlPrefix);
    out.reserve(kPidlPrefix.size() + static_cast<size_t>(size) * 2);
    for (UINT i = 0; i < size; ++i) {
        out += kHex[bytes[i] >> 4];
        out += kHex[bytes[i] & 0x0F];
    }
    return out;
}

// The id of a namespace item: its ID list. Empty when the shell gives none.
inline std::wstring FromItem(IShellItem* item) {
    if (!item) return {};
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHGetIDListFromObject(item, &pidl)) || !pidl) return {};
    std::wstring id = EncodePidl(pidl);
    CoTaskMemFree(pidl);
    return id;
}

namespace detail {
inline int HexValue(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    return -1;
}
} // namespace detail

// id -> absolute ID list (CoTaskMemFree it). A "::pidl:" id is decoded and
// checked (every SHITEMID inside the buffer, a zero terminator at its end);
// any other id is a parsing name.
inline HRESULT ParseId(const std::wstring& id, PIDLIST_ABSOLUTE* out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!IsPidlId(id)) return SHParseDisplayName(id.c_str(), nullptr, out, 0, nullptr);
    const size_t digits = id.size() - kPidlPrefix.size();
    if (digits < 4 || digits % 2 != 0 || digits / 2 > 0xFFFFFF) return E_INVALIDARG;
    const size_t size = digits / 2;
    auto* bytes = static_cast<BYTE*>(CoTaskMemAlloc(size));
    if (!bytes) return E_OUTOFMEMORY;
    for (size_t i = 0; i < size; ++i) {
        const int high = detail::HexValue(id[kPidlPrefix.size() + i * 2]);
        const int low = detail::HexValue(id[kPidlPrefix.size() + i * 2 + 1]);
        if (high < 0 || low < 0) {
            CoTaskMemFree(bytes);
            return E_INVALIDARG;
        }
        bytes[i] = static_cast<BYTE>((high << 4) | low);
    }
    size_t offset = 0;
    bool valid = false;
    while (offset + sizeof(USHORT) <= size) {
        USHORT cb = 0;
        std::memcpy(&cb, bytes + offset, sizeof(cb));
        if (cb == 0) {
            valid = offset + sizeof(USHORT) == size;
            break;
        }
        if (cb < sizeof(USHORT) || cb > size - offset) break;
        offset += cb;
    }
    if (!valid) {
        CoTaskMemFree(bytes);
        return E_INVALIDARG;
    }
    *out = reinterpret_cast<PIDLIST_ABSOLUTE>(bytes);
    return S_OK;
}

inline HRESULT CreateItem(const std::wstring& id, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!IsPidlId(id)) return SHCreateItemFromParsingName(id.c_str(), nullptr, riid, out);
    PIDLIST_ABSOLUTE pidl = nullptr;
    HRESULT hr = ParseId(id, &pidl);
    if (SUCCEEDED(hr)) hr = SHCreateItemFromIDList(pidl, riid, out);
    CoTaskMemFree(pidl);
    return hr;
}

} // namespace pulse::shell_id
