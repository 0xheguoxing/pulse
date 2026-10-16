// pack_catalog_tool.cpp - pulse_pack_catalog, a build-time host tool.
//
// Packs the content enhancement pack (Pulse.Content.exe, Pulse.Document.exe)
// as LZMS release assets and writes the catalog pulse.exe compiles in. Every
// file is pinned by size and SHA-256 before and after compression, like the
// preview pack catalog scripts/package_preview_packs.py generates, so the
// download needs no signature or manifest of its own.
//
//   pulse_pack_catalog --key content --symbol Content --version 1.0.57
//       --channel windows --base-url https://.../v1.0.57/ --out-dir DIR
//       --header generated/content_pack_catalog.h FILE...
//
// Writes DIR/<key>-<channel>-<file>.lzms, DIR/<key>-pack-assets.json and the
// header. The build runs it again whenever one of the files changes, which is
// also what a release does after signing them.
#include <windows.h>
#include <bcrypt.h>
#include <compressapi.h>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kMaximumFileBytes = 256ull * 1024 * 1024;  // pack_installer's limit

struct Options {
    std::wstring key, symbol, version, channel, base_url, out_dir, header;
    std::vector<std::wstring> files;
};

struct Entry {
    std::wstring name, asset;
    uint64_t size = 0, packed_size = 0;
    std::string sha256, packed_sha256;
};

int Fail(const wchar_t* what, const std::wstring& detail = {}) {
    fwprintf(stderr, L"pulse_pack_catalog: %ls%ls%ls (error %lu)\n", what, detail.empty() ? L"" : L": ",
             detail.c_str(), GetLastError());
    return 1;
}

bool SafeComponent(const std::wstring& text) {
    if (text.empty() || text == L"." || text == L"..") return false;
    for (wchar_t c : text) {
        const bool ok = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                        c == L'.' || c == L'-' || c == L'_';
        if (!ok) return false;
    }
    return true;
}

bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 &&
              static_cast<uint64_t>(size.QuadPart) <= kMaximumFileBytes;
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size();
    }
    CloseHandle(file);
    return ok;
}

bool WriteFileBytes(const std::wstring& path, const void* data, size_t size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(file, data, static_cast<DWORD>(size), &wrote, nullptr) && wrote == size;
    CloseHandle(file);
    return ok;
}

std::string Sha256(const std::vector<uint8_t>& data) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    uint8_t digest[32]{};
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0)) &&
              BCRYPT_SUCCESS(BCryptHashData(hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0)) &&
              BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return {};
    static const char kHex[] = "0123456789abcdef";
    std::string text;
    for (uint8_t b : digest) { text += kHex[b >> 4]; text += kHex[b & 15]; }
    return text;
}

// LZMS through the Compression API, checked by decompressing it again.
bool Pack(const std::vector<uint8_t>& data, std::vector<uint8_t>& packed) {
    COMPRESSOR_HANDLE compressor = nullptr;
    if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &compressor)) return false;
    SIZE_T needed = 0;
    Compress(compressor, data.data(), data.size(), nullptr, 0, &needed);
    bool ok = needed > 0 && needed <= kMaximumFileBytes;
    if (ok) {
        packed.resize(needed);
        ok = Compress(compressor, data.data(), data.size(), packed.data(), packed.size(), &needed) != FALSE;
        packed.resize(needed);
    }
    CloseCompressor(compressor);
    if (!ok) return false;
    DECOMPRESSOR_HANDLE decompressor = nullptr;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &decompressor)) return false;
    std::vector<uint8_t> round(data.size());
    SIZE_T written = 0;
    ok = Decompress(decompressor, packed.data(), packed.size(), round.data(), round.size(), &written) &&
         written == data.size() && round == data;
    CloseDecompressor(decompressor);
    return ok;
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring FileName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool Parse(int argc, wchar_t** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        std::wstring* target = a == L"--key" ? &o.key : a == L"--symbol" ? &o.symbol : a == L"--version" ? &o.version
            : a == L"--channel" ? &o.channel : a == L"--base-url" ? &o.base_url : a == L"--out-dir" ? &o.out_dir
            : a == L"--header" ? &o.header : nullptr;
        if (target) {
            if (++i >= argc) return false;
            *target = argv[i];
        } else if (a.rfind(L"--", 0) == 0) {
            return false;
        } else {
            o.files.push_back(a);
        }
    }
    return SafeComponent(o.key) && SafeComponent(o.symbol) && SafeComponent(o.version) && SafeComponent(o.channel) &&
           o.base_url.rfind(L"https://", 0) == 0 && o.base_url.back() == L'/' && !o.out_dir.empty() &&
           !o.header.empty() && !o.files.empty();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Options o;
    if (!Parse(argc, argv, o)) {
        fwprintf(stderr, L"usage: pulse_pack_catalog --key K --symbol S --version V --channel C "
                         L"--base-url https://.../ --out-dir DIR --header FILE FILE...\n");
        return 2;
    }
    CreateDirectoryW(o.out_dir.c_str(), nullptr);
    std::vector<Entry> entries;
    for (const auto& path : o.files) {
        Entry e;
        e.name = FileName(path);
        if (!SafeComponent(e.name)) return Fail(L"unsafe file name", e.name);
        e.asset = o.key + L"-" + o.channel + L"-" + e.name + L".lzms";
        std::vector<uint8_t> data, packed;
        if (!ReadFileBytes(path, data)) return Fail(L"cannot read", path);
        if (!Pack(data, packed)) return Fail(L"LZMS failed", path);
        e.size = data.size();
        e.packed_size = packed.size();
        e.sha256 = Sha256(data);
        e.packed_sha256 = Sha256(packed);
        if (e.sha256.empty() || e.packed_sha256.empty()) return Fail(L"SHA-256 failed", path);
        if (!WriteFileBytes(o.out_dir + L"\\" + e.asset, packed.data(), packed.size()))
            return Fail(L"cannot write", o.out_dir + L"\\" + e.asset);
        entries.push_back(std::move(e));
    }

    const std::string key = Utf8(o.key), symbol = Utf8(o.symbol), version = Utf8(o.version);
    std::string h = "// Generated by pulse_pack_catalog from the files of this build. Do not edit.\n"
                    "// Include pack_catalog.h first; this only defines the release.\n#pragma once\n\n"
                    "namespace pulse::app {\n\n";
    h += "inline constexpr PackFile k" + symbol + "PackFiles[] = {\n";
    for (const auto& e : entries) {
        h += "    {L\"" + Utf8(e.name) + "\", " + std::to_string(e.size) + "ull, L\"" + e.sha256 + "\", " +
             std::to_string(e.packed_size) + "ull, L\"" + e.packed_sha256 + "\", L\"" + Utf8(e.asset) + "\"},\n";
    }
    h += "};\ninline constexpr PackRelease k" + symbol + "PackRelease{L\"" + key + "\", L\"" + version + "\", L\"" +
         Utf8(o.base_url) + "\", k" + symbol + "PackFiles, std::size(k" + symbol + "PackFiles)};\n\n"
         "} // namespace pulse::app\n";
    if (!WriteFileBytes(o.header, h.data(), h.size())) return Fail(L"cannot write", o.header);

    std::string j = "{\n  \"key\": \"" + key + "\",\n  \"version\": \"" + version + "\",\n  \"channel\": \"" +
                    Utf8(o.channel) + "\",\n  \"base_url\": \"" + Utf8(o.base_url) + "\",\n  \"files\": [\n";
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        j += "    {\"name\": \"" + Utf8(e.name) + "\", \"size\": " + std::to_string(e.size) + ", \"sha256\": \"" +
             e.sha256 + "\", \"asset\": \"" + Utf8(e.asset) + "\", \"packed_size\": " + std::to_string(e.packed_size) +
             ", \"packed_sha256\": \"" + e.packed_sha256 + "\"}" + (i + 1 < entries.size() ? ",\n" : "\n");
    }
    j += "  ]\n}\n";
    const std::wstring manifest = o.out_dir + L"\\" + o.key + L"-pack-assets.json";
    if (!WriteFileBytes(manifest, j.data(), j.size())) return Fail(L"cannot write", manifest);
    uint64_t total = 0;
    for (const auto& e : entries) total += e.packed_size;
    wprintf(L"%ls pack %ls (%ls): %zu files, %llu download bytes\n", o.key.c_str(), o.version.c_str(),
            o.channel.c_str(), entries.size(), static_cast<unsigned long long>(total));
    return 0;
}
