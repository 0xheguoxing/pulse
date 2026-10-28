// cloud_preview_test - the preview host never reads a cloud placeholder's
// content: no decode, no text, no properties, no folder thumbnail; a grid tile
// may only come from Explorer's thumbnail cache. Drives Pulse.Preview.exe over
// its real pipe with placeholder attributes on ordinary files (the host acts on
// the attributes the listing sends, so any decoder that read the file would
// show up as a picture or text here).
#include "../ipc/preview_protocol.h"
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, const char* label) { std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n'; failures += !ok; }

constexpr DWORD kRecallOnData = 0x00400000;
constexpr DWORD kRecallOnOpen = 0x00040000;

struct Reply {
    pulse::ipc::PreviewResponse head{};
    std::wstring text, error;
    bool mapped = false;
};

struct Host {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION child{};
    ~Host() { Stop(); }
    void Stop() {
        if (pipe != INVALID_HANDLE_VALUE) { CloseHandle(pipe); pipe = INVALID_HANDLE_VALUE; }
        if (child.hProcess) {
            if (WaitForSingleObject(child.hProcess, 2000) == WAIT_TIMEOUT) TerminateProcess(child.hProcess, 91);
            CloseHandle(child.hProcess); CloseHandle(child.hThread); child = {};
        }
    }
    bool Start(const std::filesystem::path& logs) {
        wchar_t self[32768]{}; GetModuleFileNameW(nullptr, self, 32768);
        const auto exe = std::filesystem::path(self).parent_path() / L"Pulse.Preview.exe";
        auto command = L"\"" + exe.wstring() + L"\" " + std::to_wstring(GetCurrentProcessId()) + L" \"\" \"" + logs.wstring() + L"\"";
        STARTUPINFOW start{sizeof(start)};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &start, &child)) return false;
        const auto name = pulse::ipc::PreviewPipeName(GetCurrentProcessId());
        const auto end = GetTickCount64() + 5000;
        while (GetTickCount64() < end) {
            pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) {
                ULONG server = 0; return GetNamedPipeServerProcessId(pipe, &server) && server == child.dwProcessId;
            }
            if (WaitForSingleObject(child.hProcess, 10) == WAIT_OBJECT_0) return false;
        }
        return false;
    }
    bool Read(void* output, DWORD size, ULONGLONG end) {
        auto* at = static_cast<unsigned char*>(output);
        while (size) {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || GetTickCount64() >= end) return false;
            if (!available) { Sleep(1); continue; }
            DWORD got = 0;
            if (!ReadFile(pipe, at, (std::min)(available, size), &got, nullptr) || !got) return false;
            size -= got; at += got;
        }
        return true;
    }
    // false = transport failure, or a properties payload (which this bench
    // never expects for a placeholder) the connection cannot skip.
    bool Request(const std::wstring& path, uint32_t id, pulse::ipc::PreviewRequestKind kind, DWORD attrs,
                 uint32_t flags, uint32_t pixels, Reply& reply) {
        pulse::ipc::PreviewRequest request{};
        request.request_id = id; request.generation = id; request.kind = kind;
        request.path_chars = static_cast<uint32_t>(path.size());
        request.attrs = attrs; request.flags = flags; request.pixel_size = pixels;
        if (!pulse::ipc::WriteAll(pipe, &request, sizeof(request)) ||
            !pulse::ipc::WriteAll(pipe, path.data(), request.path_chars * static_cast<DWORD>(sizeof(wchar_t)))) return false;
        const auto end = GetTickCount64() + 8000;
        reply = {};
        auto& r = reply.head;
        if (!Read(&r, sizeof(r), end) || r.magic != pulse::ipc::kPreviewMagic || r.request_id != id ||
            r.mapping_chars > 512 || r.text_chars > (1u << 20) || r.error_chars > 65536 || r.property_count) return false;
        std::wstring name(r.mapping_chars, L'\0');
        reply.text.assign(r.text_chars, L'\0');
        reply.error.assign(r.error_chars, L'\0');
        if (!Read(name.data(), r.mapping_chars * static_cast<DWORD>(sizeof(wchar_t)), end) ||
            !Read(reply.text.data(), r.text_chars * static_cast<DWORD>(sizeof(wchar_t)), end) ||
            !Read(reply.error.data(), r.error_chars * static_cast<DWORD>(sizeof(wchar_t)), end)) return false;
        if (!name.empty()) {
            if (HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str())) {
                void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, size_t(r.stride) * r.height);
                reply.mapped = view != nullptr;
                if (view) UnmapViewOfFile(view);
                CloseHandle(mapping);
            }
            unsigned char ack = 1;
            if (!pulse::ipc::WriteAll(pipe, &ack, 1)) return false;
        }
        return true;
    }
};

void WriteBmp(const std::filesystem::path& file) {
    const int w = 64, h = 64, row = w * 3, bytes = row * h;
    std::vector<unsigned char> data(54 + bytes, 0);
    auto put32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) data[at + i] = static_cast<unsigned char>(v >> (8 * i)); };
    data[0] = 'B'; data[1] = 'M'; put32(2, static_cast<uint32_t>(data.size())); put32(10, 54);
    put32(14, 40); put32(18, w); put32(22, h); data[26] = 1; data[28] = 24; put32(34, bytes);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            unsigned char* p = &data[54 + y * row + x * 3];
            p[0] = static_cast<unsigned char>(x * 4); p[1] = static_cast<unsigned char>(y * 4); p[2] = 200;
        }
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

// Puts the file's thumbnail into Explorer's cache, as browsing it there would.
bool PrimeThumbnailCache(const std::wstring& path) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    IShellItem* item = nullptr;
    IThumbnailCache* cache = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) &&
        SUCCEEDED(CoCreateInstance(CLSID_LocalThumbnailCache, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&cache)))) {
        for (UINT size : {96u, 256u}) {
            ISharedBitmap* shared = nullptr;
            if (SUCCEEDED(cache->GetThumbnail(item, size, WTS_EXTRACT, &shared, nullptr, nullptr)) && shared) {
                ok = true;
                shared->Release();
            }
        }
    }
    if (cache) cache->Release();
    if (item) item->Release();
    if (SUCCEEDED(com)) CoUninitialize();
    return ok;
}
} // namespace

int wmain() {
    using pulse::ipc::PreviewRequestKind;
    const auto root = std::filesystem::absolute(L"bench_data"); std::filesystem::create_directories(root);
    const auto folder = root / (L"cloud-preview-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!CreateDirectoryW(folder.c_str(), nullptr)) return 2;
    const auto picture = folder / L"picture.bmp", fresh = folder / L"never-seen.bmp", note = folder / L"note.txt";
    const auto sub = folder / L"album";
    WriteBmp(picture);
    WriteBmp(fresh);
    { std::ofstream out(note); out << "cloud text that must stay unread"; }
    CreateDirectoryW(sub.c_str(), nullptr);
    WriteBmp(sub / L"inside.bmp");
    const DWORD placeholder = FILE_ATTRIBUTE_ARCHIVE | kRecallOnData;
    const DWORD offline = FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_OFFLINE;

    {
        Host host;
        Check(host.Start(folder / L"logs"), "start an isolated preview host");
        if (host.pipe != INVALID_HANDLE_VALUE) {
            Reply r;
            uint32_t id = 1;
            Check(host.Request(picture.wstring(), id++, PreviewRequestKind::Content, FILE_ATTRIBUTE_ARCHIVE, 0, 512, r) &&
                      r.head.status == 0 && r.mapped,
                  "control: a local picture previews");
            Check(host.Request(picture.wstring(), id++, PreviewRequestKind::Content, placeholder, 0, 512, r) &&
                      r.head.status != 0 && !r.mapped && r.error == L"offline-placeholder",
                  "placeholder picture: no decode, reported as online-only");
            Check(host.Request(note.wstring(), id++, PreviewRequestKind::Content, placeholder, 0, 512, r) &&
                      r.text.empty() && !r.mapped && r.error == L"offline-placeholder",
                  "placeholder text: nothing read");
            Check(host.Request(note.wstring(), id++, PreviewRequestKind::Content, offline, 0, 512, r) &&
                      r.text.empty() && r.error == L"offline-placeholder",
                  "OFFLINE-only file counts as a placeholder too");
            Check(host.Request(note.wstring(), id++, PreviewRequestKind::Properties, placeholder, 0, 0, r) &&
                      r.head.property_count == 0,
                  "placeholder properties: none read");
            Check(host.Request(sub.wstring(), id++, PreviewRequestKind::Content, FILE_ATTRIBUTE_DIRECTORY | kRecallOnOpen,
                               pulse::ipc::kPreviewRequestFlagFolderThumbnail, 256, r) &&
                      r.head.status != 0 && !r.mapped,
                  "online-only folder: no folder thumbnail (its listing is not fetched)");
            Check(host.Request(fresh.wstring(), id++, PreviewRequestKind::Content, placeholder,
                               pulse::ipc::kPreviewRequestFlagGrid, 96, r) &&
                      r.head.status != 0 && !r.mapped,
                  "placeholder grid tile without a cached thumbnail: type icon");
            const bool primed = PrimeThumbnailCache(picture.wstring());
            if (primed) {
                Check(host.Request(picture.wstring(), id++, PreviewRequestKind::Content, placeholder,
                                   pulse::ipc::kPreviewRequestFlagGrid, 96, r) &&
                          r.head.status == 0 && r.mapped,
                      "placeholder grid tile: Explorer's cached thumbnail is shown");
            } else {
                Check(true, "cached-thumbnail case skipped (no thumbnail provider for .bmp here)");
            }
            Check(host.Request(note.wstring(), id++, PreviewRequestKind::Content, FILE_ATTRIBUTE_ARCHIVE, 0, 512, r) &&
                      r.head.status == 0 && !r.text.empty(),
                  "connection stays aligned; a local text file still reads");
        }
        host.Stop();
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
    Check(!std::filesystem::exists(folder), "fixture removed");
    std::cout << (failures ? "FAILED" : "OK") << " (" << failures << " failures)\n";
    return failures ? 1 : 0;
}
