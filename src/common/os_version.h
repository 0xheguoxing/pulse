// os_version.h — the real Windows build number, immune to compatibility layers.
//
// RtlGetVersion and GetVersionEx report the version in the process's PEB,
// which a compatibility layer rewrites: "以兼容模式运行这个程序" (Properties >
// 兼容性), the compatibility troubleshooter, or a __COMPAT_LAYER variable
// inherited from whatever launched us. A Windows 11 24H2 machine (26100) then
// reports 7601 or 9200, and a Windows 10 gate wrongly refuses it. The kernel's
// own numbers are not rewritten. Header-only so the standalone setup can use it.
#pragma once

#include <windows.h>
#include <cstdint>

namespace pulse::os {

// What RtlGetVersion reports: this process's (possibly shimmed) view. 0 when
// the call is unavailable.
inline uint32_t ReportedWindowsBuild() noexcept {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto fn = ntdll ? reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))) : nullptr;
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    return fn && fn(&info) == 0 ? info.dwBuildNumber : 0;
}

// The build the kernel runs, whatever compatibility layer the process has.
// 0 when no source is available (never seen in practice).
inline uint32_t TrueWindowsBuild() noexcept {
    uint32_t build = 0;
    // ntdll's own numbers (high nibble = free/checked marker), not the PEB's.
    using NumbersFn = void(WINAPI*)(DWORD*, DWORD*, DWORD*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto numbers = ntdll ? reinterpret_cast<NumbersFn>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetNtVersionNumbers"))) : nullptr;
    if (numbers) {
        DWORD major = 0, minor = 0, raw = 0;
        numbers(&major, &minor, &raw);
        build = raw & 0xFFFF;
    }
    // KUSER_SHARED_DATA, mapped read-only at 0x7FFE0000 in every process:
    // NtMajorVersion at 0x26C; NtBuildNumber at 0x260 exists from Windows 10
    // on (earlier systems keep other data there, hence the major check).
    const auto* shared = reinterpret_cast<const volatile uint8_t*>(static_cast<uintptr_t>(0x7FFE0000));
    const uint32_t shared_major = *reinterpret_cast<const volatile ULONG*>(shared + 0x26C);
    if (shared_major >= 10) {
        const uint32_t shared_build = *reinterpret_cast<const volatile ULONG*>(shared + 0x260) & 0xFFFF;
        if (shared_build > build) build = shared_build;
    }
    const uint32_t reported = ReportedWindowsBuild();
    return reported > build ? reported : build;
}

}  // namespace pulse::os
