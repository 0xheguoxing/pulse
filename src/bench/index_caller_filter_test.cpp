// #66: the index host serves names only from folders the caller can list.
// A second caller is simulated with a restricted copy of our own token (the
// user SID and Administrators made deny-only), so one isolated fixture host
// answers both an allowed and a restricted client.
#include "../index/index_protocol.h"
#include "../index/folder_size_protocol.h"
#include "../ipc/protocol.h"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")

using namespace pulse::index;
using pulse::ipc::MsgHeader;
using pulse::ipc::PayloadReader;
using pulse::ipc::PayloadWriter;
using pulse::ipc::PipeRead;
using pulse::ipc::PipeWrite;
namespace fs = std::filesystem;

namespace {

int failures = 0;

void Check(bool ok, const char* label) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
    if (!ok) ++failures;
}

std::wstring Sibling(const wchar_t* name) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    wchar_t* slash = wcsrchr(path, L'\\');
    return slash ? std::wstring(path, slash + 1) + name : name;
}

std::wstring UserSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> buffer(bytes);
    std::wstring result;
    LPWSTR text = nullptr;
    if (GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes) &&
        ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) {
        result = text;
        LocalFree(text);
    }
    CloseHandle(token);
    return result;
}

// Same user, but the user SID and Administrators only deny: only the
// Authenticated Users / Everyone style grants still apply.
HANDLE RestrictedImpersonationToken() {
    HANDLE self = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_IMPERSONATE, &self))
        return nullptr;
    DWORD bytes = 0;
    GetTokenInformation(self, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> user(bytes);
    BYTE admins[SECURITY_MAX_SID_SIZE]{};
    DWORD admins_size = sizeof(admins);
    HANDLE restricted = nullptr, impersonation = nullptr;
    if (GetTokenInformation(self, TokenUser, user.data(), bytes, &bytes) &&
        CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admins, &admins_size)) {
        SID_AND_ATTRIBUTES disable[2]{};
        disable[0].Sid = reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;
        disable[1].Sid = admins;
        if (CreateRestrictedToken(self, 0, 2, disable, 0, nullptr, 0, nullptr, &restricted))
            DuplicateToken(restricted, SecurityImpersonation, &impersonation);
    }
    if (restricted) CloseHandle(restricted);
    CloseHandle(self);
    return impersonation;
}

bool SetDacl(const fs::path& path, const std::wstring& sddl, bool is_protected) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr))
        return false;
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
    const SECURITY_INFORMATION info = DACL_SECURITY_INFORMATION |
        (is_protected ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION);
    const DWORD error = SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT, info,
                                              nullptr, nullptr, dacl, nullptr);
    LocalFree(sd);
    return error == ERROR_SUCCESS;
}

bool CanList(HANDLE token, const fs::path& dir) {
    if (token && !SetThreadToken(nullptr, token)) return false;
    const HANDLE h = CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (token) RevertToSelf();
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

// The pipe server sees the token the thread presents when the pipe is opened.
HANDLE Connect(const std::wstring& pipe_name, HANDLE as_token, DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        if (as_token && !SetThreadToken(nullptr, as_token)) return INVALID_HANDLE_VALUE;
        // Static tracking: without SQOS the server would follow later writes,
        // which are made after RevertToSelf.
        HANDLE pipe = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  as_token ? SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION : 0, nullptr);
        const DWORD error = GetLastError();
        if (as_token) RevertToSelf();
        if (pipe != INVALID_HANDLE_VALUE) return pipe;
        if (GetTickCount64() >= deadline) {
            std::cout << "[INFO] pipe connect error " << error << std::endl;
            return INVALID_HANDLE_VALUE;
        }
        if (error == ERROR_PIPE_BUSY) WaitNamedPipeW(pipe_name.c_str(), 50);
        else Sleep(20);
    }
}

bool SendFrame(HANDLE pipe, uint32_t type, uint32_t id, const std::vector<uint8_t>& payload) {
    const MsgHeader header = MakeIndexHdr(type, id, static_cast<uint32_t>(payload.size()));
    return PipeWrite(pipe, reinterpret_cast<const uint8_t*>(&header), sizeof(header)) &&
        (payload.empty() || PipeWrite(pipe, payload.data(), static_cast<DWORD>(payload.size())));
}

bool ReadReply(HANDLE pipe, uint32_t type, uint32_t id, std::vector<uint8_t>& payload, DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return false;
        if (available < sizeof(MsgHeader)) { Sleep(5); continue; }
        MsgHeader header{};
        if (!PipeRead(pipe, reinterpret_cast<uint8_t*>(&header), sizeof(header)) ||
            header.magic != kIndexMagic || header.payload_size > kIndexMaxPayload)
            return false;
        payload.resize(header.payload_size);
        if (!payload.empty() && !PipeRead(pipe, payload.data(), header.payload_size)) return false;
        if (header.type == type && header.request_id == id) return true;
    }
    return false;
}

struct Names { bool ok = false; uint32_t total = 0; std::set<std::wstring> names; };

Names Search(HANDLE pipe, uint32_t id, const std::wstring& needle) {
    PayloadWriter w;
    w.PutU32(1u);  // ranked
    w.PutU32(static_cast<uint32_t>(ResultSort::Index));
    w.PutU32(48);
    w.PutU32(0);
    w.PutString(needle);
    w.PutString(L"");
    Names out;
    std::vector<uint8_t> reply;
    if (!SendFrame(pipe, REQ_IDX_SEARCH, id, w.data()) || !ReadReply(pipe, RSP_IDX_SEARCH, id, reply, 15000))
        return out;
    PayloadReader r(reply.data(), reply.size());
    uint32_t hits = 0;
    if (!r.GetU32(out.total) || !r.GetU32(hits)) return out;
    for (uint32_t i = 0; i < hits; ++i) {
        std::wstring path, name;
        uint32_t dir = 0, a = 0, b = 0, c = 0, d = 0;
        if (!r.GetString(path) || !r.GetString(name) || !r.GetU32(dir) || !r.GetU32(a) ||
            !r.GetU32(b) || !r.GetU32(c) || !r.GetU32(d))
            return out;
        out.names.insert(name);
    }
    out.ok = true;
    return out;
}

// Ranked pages over many unchecked folders arrive twice: the final page with
// a total of confirmed names, then the exact total (same request id).
std::vector<Names> SearchReplies(HANDLE pipe, uint32_t id, const std::wstring& needle, size_t count) {
    PayloadWriter w;
    w.PutU32(1u);  // ranked
    w.PutU32(static_cast<uint32_t>(ResultSort::Index));
    w.PutU32(48);
    w.PutU32(0);
    w.PutString(needle);
    w.PutString(L"");
    std::vector<Names> out;
    if (!SendFrame(pipe, REQ_IDX_SEARCH, id, w.data())) return out;
    std::vector<uint8_t> reply;
    while (out.size() < count && ReadReply(pipe, RSP_IDX_SEARCH, id, reply, out.empty() ? 15000 : 10000)) {
        Names page;
        PayloadReader r(reply.data(), reply.size());
        uint32_t hits = 0;
        if (!r.GetU32(page.total) || !r.GetU32(hits)) break;
        bool parsed = true;
        for (uint32_t i = 0; i < hits && parsed; ++i) {
            std::wstring path, name;
            uint32_t dir = 0, a = 0, b = 0, c = 0, d = 0;
            parsed = r.GetString(path) && r.GetString(name) && r.GetU32(dir) && r.GetU32(a) &&
                r.GetU32(b) && r.GetU32(c) && r.GetU32(d);
            if (parsed) page.names.insert(name);
        }
        page.ok = parsed;
        out.push_back(std::move(page));
    }
    return out;
}

bool FolderSizes(HANDLE pipe, uint32_t id, const std::vector<std::wstring>& paths,
                 std::vector<IndexedFolderSize>& values) {
    PayloadWriter w;
    w.PutU32(1);
    w.PutU32(static_cast<uint32_t>(paths.size()));
    for (const auto& path : paths) w.PutString(path);
    std::vector<uint8_t> reply;
    if (!SendFrame(pipe, kFolderSizeRequest, id, w.data()) ||
        !ReadReply(pipe, kFolderSizeResponse, id, reply, 5000))
        return false;
    PayloadReader r(reply.data(), reply.size());
    return ReadFolderSizes(r, paths.size(), values);
}

void Print(const char* label, const Names& n) {
    std::wcout << L"[INFO] " << label << L" total=" << n.total << L" names=";
    for (const auto& name : n.names) std::wcout << name << L' ';
    std::wcout << std::endl;
}

// Opt-in timing: index_caller_filter_test.exe --perf <dir> [0|1]. A fresh
// connection has an empty folder cache, so its first broad query pays for
// every distinct parent folder once.
int Perf(const std::wstring& dir, bool filter) {
    const auto base = fs::absolute(fs::path(L"../bench_data") /
        (L"caller-filter-perf-" + std::to_wstring(GetCurrentProcessId())));
    fs::create_directories(base / L"profile");
    fs::create_directories(base / L"cache");
    SetEnvironmentVariableW(L"LOCALAPPDATA", (base / L"profile").c_str());
    SetEnvironmentVariableW(L"PULSE_INDEX_TEST_CALLER_FILTER", filter ? L"1" : L"0");
    const std::wstring token = L"cfp" + std::to_wstring(GetCurrentProcessId());
    const std::wstring pipe_name = L"\\\\.\\pipe\\PulseIndex.Test." + token;
    const std::wstring exe = Sibling(L"Pulse.Index.exe");
    std::wstring command = L"\"" + exe + L"\" --test-host " + token + L" \"" + dir + L"\" \"" +
        (base / L"cache").wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION host{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &startup, &host))
        return 1;
    CloseHandle(host.hThread);
    HANDLE probe = Connect(pipe_name, nullptr, 15000);
    uint32_t id = 1, last = 0;
    int stable = 0;
    const ULONGLONG until = GetTickCount64() + 90000;
    while (probe != INVALID_HANDLE_VALUE && GetTickCount64() < until && stable < 3) {
        const Names n = Search(probe, id++, L"e");
        stable = n.ok && n.total && n.total == last ? stable + 1 : 0;
        last = n.total;
        Sleep(1000);
    }
    std::cout << "[INFO] perf filter=" << filter << " matches=" << last << std::endl;
    HANDLE fresh = Connect(pipe_name, nullptr, 5000);
    for (const wchar_t* needle : {L"e", L"e", L"a", L"dll"}) {
        const ULONGLONG started = GetTickCount64();
        const Names n = Search(fresh, id++, needle);
        std::wcout << L"[INFO] perf needle=" << needle << L" total=" << n.total << L" ms="
                   << GetTickCount64() - started << std::endl;
    }
    SendFrame(probe, REQ_IDX_TEST_SHUTDOWN, id++, {});
    if (WaitForSingleObject(host.hProcess, 5000) != WAIT_OBJECT_0) TerminateProcess(host.hProcess, 1);
    CloseHandle(host.hProcess);
    std::error_code ec;
    fs::remove_all(base, ec);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 3 && std::wstring_view(argv[1]) == L"--perf")
        return Perf(argv[2], argc < 4 || std::wstring_view(argv[3]) != L"0");
    const auto base = fs::absolute(fs::path(L"../bench_data") /
        (L"caller-filter-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    const auto root = base / L"files", profile = base / L"profile", cache = base / L"name-index";
    const auto open = root / L"open", secret = root / L"secret", inner = secret / L"inner";
    fs::create_directories(open);
    fs::create_directories(inner);
    fs::create_directories(secret / L"pulsecf_subdir");
    fs::create_directories(profile);
    fs::create_directories(cache);
    for (const auto& file : {root / L"pulsecf_top.txt", open / L"pulsecf_open.txt",
                             secret / L"pulsecf_secret.txt", inner / L"pulsecf_inner.txt"})
        std::ofstream(file) << "pulse caller filter";
    // Top-first pages: many listable folders, and the best-ranked name (an
    // exact match) in the hidden one.
    constexpr int kSpread = 600;
    for (int i = 0; i < kSpread; ++i) {
        const auto folder = root / L"spread" / (L"d" + std::to_wstring(i));
        fs::create_directories(folder);
        std::ofstream(folder / (L"pulsetf_" + std::to_wstring(i) + L".txt")) << "x";
    }
    std::ofstream(secret / L"pulsetf") << "x";

    // Root: owner/SYSTEM/admins full, Authenticated Users may read. "secret"
    // drops the Authenticated Users grant; "inner" gets it back explicitly,
    // so the restricted caller can list inner but not secret itself.
    const std::wstring sid = UserSid();
    const std::wstring owners = L"(A;OICI;FA;;;" + sid + L")(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
    const bool acl = !sid.empty() &&
        SetDacl(root, L"D:P" + owners + L"(A;OICI;0x1200a9;;;AU)", true) &&
        SetDacl(secret, L"D:P" + owners, true) &&
        SetDacl(inner, L"D:(A;OICI;0x1200a9;;;AU)", false);
    Check(acl, "fixture ACLs applied");
    HANDLE restricted = RestrictedImpersonationToken();
    Check(restricted != nullptr, "restricted caller token created");
    Check(CanList(nullptr, secret), "fixture: host user can list secret");
    Check(restricted && CanList(restricted, root) && CanList(restricted, open) && CanList(restricted, inner),
          "fixture: restricted caller can list root, open and secret\\inner");
    Check(restricted && !CanList(restricted, secret), "fixture: restricted caller cannot list secret");

    SetEnvironmentVariableW(L"LOCALAPPDATA", profile.c_str());
    SetEnvironmentVariableW(L"PULSE_INDEX_TEST_CALLER_FILTER", L"1");
    const std::wstring token = L"cf" + std::to_wstring(GetCurrentProcessId());
    const std::wstring pipe_name = L"\\\\.\\pipe\\PulseIndex.Test." + token;
    const std::wstring exe = Sibling(L"Pulse.Index.exe");
    std::wstring command = L"\"" + exe + L"\" --test-host " + token + L" \"" + root.wstring() + L"\" \"" +
        cache.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION host{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &startup, &host)) {
        std::cout << "[FAIL] launch host error=" << GetLastError() << std::endl;
        return 1;
    }
    CloseHandle(host.hThread);

    HANDLE mine = Connect(pipe_name, nullptr, 15000);
    HANDLE other = Connect(pipe_name, restricted, 15000);
    Check(mine != INVALID_HANDLE_VALUE && other != INVALID_HANDLE_VALUE, "both callers connected");

    const std::set<std::wstring> all = {L"pulsecf_top.txt", L"pulsecf_open.txt", L"pulsecf_secret.txt",
                                        L"pulsecf_subdir", L"pulsecf_inner.txt"};
    const std::set<std::wstring> listable = {L"pulsecf_top.txt", L"pulsecf_open.txt", L"pulsecf_inner.txt"};
    uint32_t id = 1;
    Names own;
    const ULONGLONG until = GetTickCount64() + 20000;
    while (mine != INVALID_HANDLE_VALUE && GetTickCount64() < until) {
        own = Search(mine, id++, L"pulsecf");
        if (own.ok && own.names == all) break;
        Sleep(100);
    }
    Print("own", own);
    Check(own.ok && own.names == all && own.total == 5, "host user sees every fixture name");

    if (other != INVALID_HANDLE_VALUE) {
        const ULONGLONG started = GetTickCount64();
        const Names first = Search(other, id++, L"pulsecf");
        const ULONGLONG elapsed = GetTickCount64() - started;
        Print("restricted", first);
        std::cout << "[INFO] restricted first query ms=" << elapsed << std::endl;
        Check(first.ok && first.names == listable && first.total == 3,
              "restricted caller sees only names in folders it can list");
        const Names again = Search(other, id++, L"pulsecf");
        Check(again.ok && again.names == listable && again.total == 3, "cached checks give the same page");
        const Names direct = Search(other, id++, L"pulsecf_secret");
        Check(direct.ok && direct.names.empty() && direct.total == 0, "exact name in a hidden folder is not found");
        const Names deep = Search(other, id++, L"pulsecf_inner");
        Check(deep.ok && deep.names.size() == 1 && deep.total == 1, "listable folder below a hidden one still works");
    }
    // Ranked page over 600 unchecked folders: the restricted caller gets the
    // page at once (no hidden name, a partial total), then the exact total.
    Names spread_own;
    const ULONGLONG spread_until = GetTickCount64() + 30000;
    while (mine != INVALID_HANDLE_VALUE && GetTickCount64() < spread_until) {
        spread_own = Search(mine, id++, L"pulsetf");
        if (spread_own.ok && spread_own.total == kSpread + 1) break;
        Sleep(200);
    }
    Check(spread_own.ok && spread_own.total == kSpread + 1 && spread_own.names.contains(L"pulsetf"),
          "owner sees every spread name, the hidden exact match first");
    if (other != INVALID_HANDLE_VALUE) {
        const ULONGLONG started = GetTickCount64();
        const auto pages = SearchReplies(other, id++, L"pulsetf", 2);
        std::cout << "[INFO] spread replies=" << pages.size() << " ms=" << GetTickCount64() - started;
        for (const auto& page : pages) std::cout << " total=" << page.total << " hits=" << page.names.size();
        std::cout << std::endl;
        Check(pages.size() == 2, "spread: page first, exact total after");
        if (pages.size() == 2) {
            Check(pages[0].ok && pages[0].names.size() == 48 && !pages[0].names.contains(L"pulsetf") &&
                  pages[0].total >= 48 && pages[0].total < static_cast<uint32_t>(kSpread),
                  "spread: first page is full, hides the secret name, counts confirmed names only");
            Check(pages[1].ok && pages[1].total == static_cast<uint32_t>(kSpread) && pages[1].names == pages[0].names,
                  "spread: exact total follows with the same page");
        }
        const Names again = Search(other, id++, L"pulsetf");
        Check(again.ok && again.total == static_cast<uint32_t>(kSpread) && !again.names.contains(L"pulsetf"),
              "spread: checked folders answer the next query at once");
    }
    // Shared engine cache must not leak the restricted view to the owner.
    const Names own_again = Search(mine, id++, L"pulsecf");
    Check(own_again.ok && own_again.names == all, "owner results unaffected after restricted queries");

    std::vector<IndexedFolderSize> sizes;
    const std::vector<std::wstring> folders = {open.wstring(), secret.wstring()};
    const bool own_sizes = FolderSizes(mine, id++, folders, sizes);
    const bool own_secret = own_sizes && sizes.size() == 2 && sizes[1].available;
    std::cout << "[INFO] owner folder sizes ok=" << own_sizes << " open=" << (own_sizes && sizes[0].available)
              << " secret=" << own_secret << std::endl;
    const bool other_sizes = other != INVALID_HANDLE_VALUE && FolderSizes(other, id++, folders, sizes);
    Check(other_sizes && sizes.size() == 2 && !sizes[1].available, "restricted caller gets no size for secret");
    if (own_secret) Check(other_sizes && sizes.size() == 2 && sizes[0].available, "restricted caller keeps size for open");
    else std::cout << "[INFO] fixture host has no folder sizes; size filter check is one-sided" << std::endl;

    SendFrame(mine, REQ_IDX_TEST_SHUTDOWN, id++, {});
    if (WaitForSingleObject(host.hProcess, 5000) != WAIT_OBJECT_0) TerminateProcess(host.hProcess, 1);
    CloseHandle(host.hProcess);
    if (mine != INVALID_HANDLE_VALUE) CloseHandle(mine);
    if (other != INVALID_HANDLE_VALUE) CloseHandle(other);
    if (restricted) CloseHandle(restricted);
    std::error_code ec;
    fs::remove_all(base, ec);
    std::cout << (failures ? "FAILED " : "ALL PASS ") << failures << std::endl;
    return failures ? 1 : 0;
}
