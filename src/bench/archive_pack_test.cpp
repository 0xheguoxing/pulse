#include "../preview_host/archive_pack.h"
#include <cstdio>
#include <string>

namespace {
int failed = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failed;
}
}
int wmain(int argc, wchar_t** argv) {
    using namespace pulse::preview;
    if (argc > 2 && std::wstring(argv[1]) == L"l") {
        const std::wstring mode = argv[argc - 1];
        if (mode == L"test-helper-timeout") { Sleep(10000); return 0; }
        if (mode == L"test-helper-output") {
            const std::string block(16384, 'x');
            for (int i = 0; i < 600; ++i) std::fwrite(block.data(), 1, block.size(), stdout);
            return 0;
        }
        return 2;
    }
    std::wstring text, error;
    const std::string prefix = "7-Zip\n\n--\nPath = archive.7z\nType = 7z\n\n----------\n";
    const std::string file = "Path = folder/hello.txt\nSize = 7\nEncrypted = -\nModified = 2026-01-01 01:02:03\n\n";
    Check(ParseArchivePackListing(prefix + file, 0, text, &error) &&
        text.find(L"\t0\n") != std::wstring::npos && text.find(L"1\t-\t7\t-\t2026-01-01 01:02\thello.txt") != std::wstring::npos,
        "tree and metadata");
    Check(ParseArchivePackListing(prefix, 0, text, &error), "empty archive");
    Check(!ParseArchivePackListing(prefix + file, 2, text, &error) && text.empty(), "nonzero exit rejects partial list");
    Check(!ParseArchivePackListing(prefix + "Path = bad\nSize = -1\n\n", 0, text, &error), "negative size rejected");
    Check(!ParseArchivePackListing(prefix + "Path = bad\nSize = 18446744073709551616\n\n", 0, text, &error), "size overflow rejected");
    Check(!ParseArchivePackListing(prefix + "Path = bad\n", 0, text, &error), "missing size rejected");
    Check(ParseArchivePackListing("Type = wim\nComment = \n----------\nPath = data\nFolder = +\nSize = \nPacked Size = \n\n" + file,
        0, text, &error) && text.find(L"hello.txt") != std::wstring::npos, "empty WIM directory values ignored");
    Check(ParseArchivePackListing("Type = Xar\n----------\nPath = data\nSize =\nMode = drwxr-xr-x\nType = directory\n\n" + file,
        0, text, &error), "XAR directory mode accepted");
    Check(!ParseArchivePackListing(prefix + "Path = bad\nSize = 1", 0, text, &error), "truncated output rejected");
    Check(!ParseArchivePackListing(prefix + "Path = bad\nPath = other\nSize = 1\n\n", 0, text, &error), "duplicate path rejected");
    Check(!ParseArchivePackListing(prefix + "Path = bad\xff\nSize = 1\n\n", 0, text, &error), "invalid UTF8 rejected");
    Check(ParseArchivePackListing(prefix + "Path = secret\nSize = 1\nEncrypted = +\n\n", 0, text, &error) &&
        text.find(L"\t1\n") != std::wstring::npos && text.find(L"\te\t") != std::wstring::npos, "encrypted list incomplete");
    Check(ParseArchivePackListing("Type = Split\nVolumes = 2\n----------\n" + file, 0, text, &error) &&
        text.find(L"\t1\n") != std::wstring::npos, "split list incomplete");
    Check(ParseArchivePackListing("Type = WIM\nVolume = 1\nVolumes = 1\nMultivolume = -\n----------\n" + file,
        0, text, &error) && text.find(L"\t0\n") != std::wstring::npos, "single-volume WIM complete");
    Check(ParseArchivePackListing("Type = WIM\nVolume = 1\nVolumes = 2\nMultivolume = +\n----------\n" + file,
        0, text, &error) && text.find(L"\t1\n") != std::wstring::npos, "multiple-volume WIM incomplete");
    Check(ParseArchivePackListing(prefix + "Path = hello.txt\nSize = 7\nSplit Before = -\nSplit After = -\nVolume Index = 0\n\n",
        0, text, &error) && text.find(L"\t0\n") != std::wstring::npos, "negative split flags remain complete");
    Check(ParseArchivePackListing(prefix + "Path = hello.txt\nSize = 7\nSplit Before = +\nSplit After = -\n\n",
        0, text, &error) && text.find(L"\t1\n") != std::wstring::npos, "positive split flag incomplete");
    std::string large = prefix;
    for (int i = 0; i < 20001; ++i) large += "Path = f" + std::to_string(i) + "\nSize = 1\n\n";
    Check(ParseArchivePackListing(large, 0, text, &error) && text.find(L"\t1\n") != std::wstring::npos,
        "entry budget incomplete");
    Check(!ParseArchivePackListing(std::string(8u * 1024u * 1024u + 1u, '\n'), 0, text, &error), "output budget rejected");
    Check(IsArchivePackExtension(L".001") && !IsArchivePackExtension(L".002"), "only first numbered volume entry point");
    if (argc == 2 && std::wstring(argv[1]) == L"--process-limits") {
        wchar_t self[32768]{};
        GetModuleFileNameW(nullptr, self, 32768);
        CreateDirectoryW(L"bench_data", nullptr);
        const std::wstring directory = L"bench_data\\archive-pack-process-" + std::to_wstring(GetCurrentProcessId());
        Check(CreateDirectoryW(directory.c_str(), nullptr) != FALSE, "isolated process fixture directory");
        wchar_t absolute[32768]{};
        GetFullPathNameW(directory.c_str(), 32768, absolute, nullptr);
        const std::wstring tool = std::wstring(absolute) + L"\\7z.exe";
        const std::wstring dll = std::wstring(absolute) + L"\\7z.dll";
        Check(CopyFileW(self, tool.c_str(), TRUE) != FALSE, "isolated process fixture executable");
        HANDLE placeholder = CreateFileW(dll.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(placeholder != INVALID_HANDLE_VALUE, "isolated process fixture companion");
        if (placeholder != INVALID_HANDLE_VALUE) CloseHandle(placeholder);
        uint32_t bytes = 0;
        const ULONGLONG started = GetTickCount64();
        Check(!ArchivePackListingWithTool(tool, L"test-helper-timeout", text, bytes, &error) &&
            error == L"archive-pack-timeout" && GetTickCount64() - started < 4500, "hung child killed at deadline");
        const bool flood_ok = ArchivePackListingWithTool(tool, L"test-helper-output", text, bytes, &error);
        if (flood_ok || error != L"archive-pack-output-limit") std::wprintf(L"flood result: %s\n", error.c_str());
        Check(!flood_ok && error == L"archive-pack-output-limit", "flooding child killed at output budget");
        const bool deleted_tool = DeleteFileW(tool.c_str()) != FALSE;
        if (!deleted_tool) std::printf("fixture executable delete error: %lu\n", GetLastError());
        const bool deleted_dll = DeleteFileW(dll.c_str()) != FALSE;
        if (!deleted_dll) std::printf("fixture companion delete error: %lu\n", GetLastError());
        const bool deleted_directory = RemoveDirectoryW(absolute) != FALSE;
        if (!deleted_directory) std::printf("fixture directory delete error: %lu\n", GetLastError());
        Check(deleted_tool && deleted_dll && deleted_directory,
            "children exited and fixtures cleaned");
    }
    if (argc == 4) {
        uint32_t bytes = 123;
        const bool ok = ArchivePackListingWithTool(argv[1], argv[2], text, bytes, &error);
        const std::wstring mode = argv[3];
        if (mode == L"fail") Check(!ok && text.empty(), "real tool error does not expose complete list");
        else Check(ok && text.find(L"hello.txt") != std::wstring::npos &&
            text.find(mode == L"incomplete" ? L"\t1\n" : L"\t0\n") != std::wstring::npos,
            "real tool bounded listing");
        if (!ok) std::wprintf(L"tool result: %s\n", error.c_str());
        Check(bytes == 0, "unmeasured child I/O not fabricated");
    }
    return failed ? 1 : 0;
}
