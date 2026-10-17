// zip_compress.cpp - see zip_compress.h.
#include "zip_compress.h"

#include <windows.h>

#include <algorithm>
#include <memory>
#include <type_traits>

namespace pulse::app {
namespace {

struct archive;
struct archive_entry;

constexpr int kArchiveOk = 0;
constexpr int kArchiveWarn = -20;
constexpr unsigned kIfReg = 0100000;
constexpr unsigned kIfDir = 0040000;

struct WriterApi {
    HMODULE module = nullptr;
    archive* (*write_new)() = nullptr;
    int (*set_format_zip)(archive*) = nullptr;
    int (*set_options)(archive*, const char*) = nullptr;
    int (*open_filename_w)(archive*, const wchar_t*) = nullptr;
    int (*write_header)(archive*, archive_entry*) = nullptr;
    int64_t (*write_data)(archive*, const void*, size_t) = nullptr;  // la_ssize_t on x64
    int (*finish_entry)(archive*) = nullptr;
    int (*write_close)(archive*) = nullptr;
    int (*write_free)(archive*) = nullptr;
    const char* (*error_string)(archive*) = nullptr;
    archive_entry* (*entry_new)() = nullptr;
    void (*entry_free)(archive_entry*) = nullptr;
    void (*copy_pathname_w)(archive_entry*, const wchar_t*) = nullptr;
    void (*set_size)(archive_entry*, int64_t) = nullptr;
    void (*set_filetype)(archive_entry*, unsigned) = nullptr;
    void (*set_perm)(archive_entry*, unsigned short) = nullptr;
    void (*set_mtime)(archive_entry*, int64_t, long) = nullptr;
    bool ready = false;

    WriterApi() {
        module = LoadLibraryExW(L"archiveint.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return;
        auto load = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(module, name));
            return fn != nullptr;
        };
        ready = load(write_new, "archive_write_new") &&
            load(set_format_zip, "archive_write_set_format_zip") &&
            load(set_options, "archive_write_set_options") &&
            load(open_filename_w, "archive_write_open_filename_w") &&
            load(write_header, "archive_write_header") &&
            load(write_data, "archive_write_data") &&
            load(finish_entry, "archive_write_finish_entry") &&
            load(write_close, "archive_write_close") &&
            load(write_free, "archive_write_free") &&
            load(error_string, "archive_error_string") &&
            load(entry_new, "archive_entry_new") &&
            load(entry_free, "archive_entry_free") &&
            load(copy_pathname_w, "archive_entry_copy_pathname_w") &&
            load(set_size, "archive_entry_set_size") &&
            load(set_filetype, "archive_entry_set_filetype") &&
            load(set_perm, "archive_entry_set_perm") &&
            load(set_mtime, "archive_entry_set_mtime");
    }
};

const WriterApi& Api() {
    static const WriterApi api;
    return api;
}

struct Item {
    std::wstring path;   // on disk
    std::wstring name;   // in the archive, '/' separated
    bool dir = false;
    uint64_t size = 0;
    FILETIME mtime{};
};

std::wstring Leaf(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? p : p.substr(slash + 1);
}

// Extended-length form for deep trees; the archive names stay relative.
std::wstring LongPath(const std::wstring& path) {
    if (path.size() < 240 || path.rfind(L"\\\\?\\", 0) == 0) return path;
    if (path.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

void Collect(const std::wstring& path, const std::wstring& name, const WIN32_FILE_ATTRIBUTE_DATA& data,
             std::vector<Item>& out, uint64_t& total, const std::atomic<bool>* cancel) {
    Item item;
    item.path = path;
    item.name = name;
    item.dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    item.size = item.dir ? 0 : (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    item.mtime = data.ftLastWriteTime;
    out.push_back(item);
    total += item.size;
    // A junction or a directory symlink goes in as an empty folder: following
    // it could loop or pull in a whole other tree.
    if (!item.dir || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileExW(LongPath(path + L"\\*").c_str(), FindExInfoBasic, &fd,
                                   FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (cancel && cancel->load()) break;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        WIN32_FILE_ATTRIBUTE_DATA child{};
        child.dwFileAttributes = fd.dwFileAttributes;
        child.ftLastWriteTime = fd.ftLastWriteTime;
        child.nFileSizeHigh = fd.nFileSizeHigh;
        child.nFileSizeLow = fd.nFileSizeLow;
        Collect(path + L"\\" + fd.cFileName, name + L"/" + fd.cFileName, child, out, total, cancel);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

std::wstring Widen(const char* text) {
    if (!text || !*text) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), n);
    return out;
}

std::wstring SystemMessage(DWORD code) {
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring out = text ? text : L"";
    if (text) LocalFree(text);
    while (!out.empty() && (out.back() == L'\n' || out.back() == L'\r' || out.back() == L' ')) out.pop_back();
    return out;
}

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(LongPath(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

} // namespace

bool ZipCompressAvailable() {
    return Api().ready;
}

std::wstring ZipPathFor(const std::vector<std::wstring>& sources, const std::wstring& dir) {
    std::wstring stem;
    if (sources.size() == 1) {
        stem = Leaf(sources.front());
        WIN32_FILE_ATTRIBUTE_DATA data{};
        const bool folder = GetFileAttributesExW(LongPath(sources.front()).c_str(), GetFileExInfoStandard, &data) &&
            (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        const size_t dot = stem.find_last_of(L'.');
        if (!folder && dot != std::wstring::npos && dot > 0) stem.resize(dot);
    } else {
        stem = Leaf(dir);
        if (stem.empty() || stem.back() == L':' || stem.back() == L'\\') stem = L"Archive";
    }
    if (stem.empty()) stem = L"Archive";
    std::wstring base = dir;
    if (!base.empty() && base.back() != L'\\') base += L'\\';
    std::wstring path = base + stem + L".zip";
    for (int n = 2; Exists(path) && n < 10000; ++n) path = base + stem + L" (" + std::to_wstring(n) + L").zip";
    return path;
}

ZipCompressResult CompressToZip(const std::vector<std::wstring>& sources, const std::wstring& zip_path,
                                const std::atomic<bool>* cancel,
                                const std::function<void(uint64_t, uint64_t)>& progress) {
    ZipCompressResult result;
    const WriterApi& api = Api();
    if (!api.ready) {
        result.error = L"archiveint.dll";
        return result;
    }
    std::vector<Item> items;
    uint64_t total = 0;
    for (const auto& source : sources) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(LongPath(source).c_str(), GetFileExInfoStandard, &data)) {
            result.error = Leaf(source) + L": " + SystemMessage(GetLastError());
            return result;
        }
        std::wstring path = source;
        while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
        Collect(path, Leaf(path), data, items, total, cancel);
    }
    if (cancel && cancel->load()) {
        result.cancelled = true;
        return result;
    }

    const size_t slash = zip_path.find_last_of(L"\\/");
    const std::wstring temp = slash == std::wstring::npos ? zip_path + L".pulse-tmp"
        : zip_path.substr(0, slash + 1) + L"." + zip_path.substr(slash + 1) + L".pulse-tmp";
    archive* a = api.write_new();
    if (!a) {
        result.error = L"archive_write_new";
        return result;
    }
    const auto fail = [&](std::wstring message) {
        result.error = std::move(message);
        api.write_free(a);   // closes the file
        a = nullptr;
        DeleteFileW(LongPath(temp).c_str());
        return result;
    };
    api.set_format_zip(a);
    // UTF-8 names, flagged as such: Explorer, 7-Zip and macOS all read them.
    api.set_options(a, "hdrcharset=UTF-8");
    if (api.open_filename_w(a, LongPath(temp).c_str()) != kArchiveOk)
        return fail(Widen(api.error_string(a)));
    SetFileAttributesW(LongPath(temp).c_str(), FILE_ATTRIBUTE_HIDDEN);

    std::unique_ptr<char[]> buffer(new char[256 * 1024]);
    uint64_t done = 0;
    if (progress) progress(0, total);
    for (const Item& item : items) {
        if (cancel && cancel->load()) {
            result.cancelled = true;
            return fail({});
        }
        HANDLE file = INVALID_HANDLE_VALUE;
        if (!item.dir) {
            file = CreateFileW(LongPath(item.path).c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (file == INVALID_HANDLE_VALUE) return fail(item.name + L": " + SystemMessage(GetLastError()));
        }
        archive_entry* entry = api.entry_new();
        api.copy_pathname_w(entry, item.name.c_str());
        api.set_filetype(entry, item.dir ? kIfDir : kIfReg);
        api.set_perm(entry, static_cast<unsigned short>(item.dir ? 0755 : 0644));
        api.set_size(entry, static_cast<int64_t>(item.size));
        ULARGE_INTEGER t{};
        t.LowPart = item.mtime.dwLowDateTime;
        t.HighPart = item.mtime.dwHighDateTime;
        if (t.QuadPart > 116444736000000000ull) {
            const uint64_t unix100 = t.QuadPart - 116444736000000000ull;
            api.set_mtime(entry, static_cast<int64_t>(unix100 / 10000000ull),
                          static_cast<long>((unix100 % 10000000ull) * 100));
        }
        const int header = api.write_header(a, entry);
        api.entry_free(entry);
        if (header != kArchiveOk && header != kArchiveWarn) {
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            return fail(item.name + L": " + Widen(api.error_string(a)));
        }
        if (file != INVALID_HANDLE_VALUE) {
            uint64_t left = item.size;
            DWORD got = 0;
            bool ok = true;
            while (left > 0 && ReadFile(file, buffer.get(), 256 * 1024, &got, nullptr) && got > 0) {
                const DWORD take = static_cast<DWORD>(std::min<uint64_t>(got, left));
                if (api.write_data(a, buffer.get(), take) < 0) {
                    ok = false;
                    break;
                }
                left -= take;
                done += take;
                if (progress) progress(done, total);
                if (cancel && cancel->load()) break;
            }
            CloseHandle(file);
            if (!ok) return fail(item.name + L": " + Widen(api.error_string(a)));
            if (cancel && cancel->load()) {
                result.cancelled = true;
                return fail({});
            }
            ++result.files;
        }
        api.finish_entry(a);
    }
    if (api.write_close(a) != kArchiveOk) return fail(Widen(api.error_string(a)));
    api.write_free(a);
    a = nullptr;
    SetFileAttributesW(LongPath(temp).c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!MoveFileExW(LongPath(temp).c_str(), LongPath(zip_path).c_str(), MOVEFILE_COPY_ALLOWED)) {
        const DWORD error = GetLastError();
        DeleteFileW(LongPath(temp).c_str());
        result.error = SystemMessage(error);
        return result;
    }
    result.ok = true;
    result.bytes = done;
    return result;
}

} // namespace pulse::app
