// tiff_reduce_test.cpp - the parallel TIFF reduction used for large
// thumbnails (src/preview_host/tiff_reduce.cpp) against WIC's full decode,
// box-averaged by the same factor.
#include "../preview_host/tiff_reduce.h"
#include "tiff_reduce_fixtures.inc"
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;
using pulse::preview::ReduceTiffFile;
using pulse::preview::ReduceTiffMemory;
using pulse::preview::TiffReduced;
using pulse::preview::TiffReduceStatus;

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& name, const std::string& detail = {}) {
    std::printf("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : " - ", detail.c_str());
    if (!ok) ++g_failures;
}

// Rounding differs by one level at most: WIC turns 16-bit samples into
// 8 bits and premultiplies with its own rounding.
constexpr int kMaxDifference = 2;
constexpr uint32_t kMinEdge = 8;  // 49x33 -> factor 6, 9x6

struct Fixture {
    const char* name;
    const uint8_t* data;
    size_t size;
    uint32_t channels;  // what ReduceTiff should produce
};

bool DecodeWithWic(const uint8_t* data, size_t size, uint32_t channels, UINT& width, UINT& height,
                   std::vector<uint8_t>& pixels) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    const GUID format = channels == 1 ? GUID_WICPixelFormat8bppGray
        : channels == 3 ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppPBGRA;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(size))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), format, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&width, &height))) return false;
    pixels.resize(size_t{width} * height * channels);
    return SUCCEEDED(converter->CopyPixels(nullptr, width * channels, static_cast<UINT>(pixels.size()),
                                           pixels.data()));
}

void TestFixture(const Fixture& f) {
    const std::string name = f.name;
    TiffReduced one, four;
    const TiffReduceStatus status = ReduceTiffMemory(f.data, f.size, kMinEdge, 1, one);
    Check(status == TiffReduceStatus::Made, name + " reduces");
    if (status != TiffReduceStatus::Made) return;
    Check(ReduceTiffMemory(f.data, f.size, kMinEdge, 4, four) == TiffReduceStatus::Made &&
          four.pixels == one.pixels, name + " gives the same picture on 1 and 4 threads");

    UINT width = 0, height = 0;
    std::vector<uint8_t> full;
    Check(DecodeWithWic(f.data, f.size, f.channels, width, height, full), name + " WIC reference");
    if (full.empty()) return;
    const uint32_t k = one.factor;
    char detail[200];
    std::snprintf(detail, sizeof(detail), "source %ux%u factor %u -> %ux%u ch=%u (WIC %ux%u)",
                  one.source_width, one.source_height, k, one.width, one.height, one.channels, width, height);
    Check(one.source_width == width && one.source_height == height && k == (std::max)(width, height) / kMinEdge &&
          one.width == (width + k - 1) / k && one.height == (height + k - 1) / k && one.channels == f.channels,
          name + " geometry", detail);

    int worst = 0;
    double total = 0;
    size_t count = 0;
    for (uint32_t oy = 0; oy < one.height; ++oy) {
        for (uint32_t ox = 0; ox < one.width; ++ox) {
            for (uint32_t c = 0; c < f.channels; ++c) {
                uint64_t sum = 0, n = 0;
                for (uint32_t y = oy * k; y < (std::min)(height, (oy + 1) * k); ++y)
                    for (uint32_t x = ox * k; x < (std::min)(width, (ox + 1) * k); ++x, ++n)
                        sum += full[(size_t{y} * width + x) * f.channels + c];
                const int expected = static_cast<int>((sum + n / 2) / n);
                const int got = one.pixels[size_t{oy} * one.stride + size_t{ox} * f.channels + c];
                worst = (std::max)(worst, std::abs(got - expected));
                total += std::abs(got - expected);
                ++count;
            }
        }
    }
    std::snprintf(detail, sizeof(detail), "mean %.2f, max %d (limit %d)", total / count, worst, kMaxDifference);
    Check(worst <= kMaxDifference, name + " matches WIC's box means", detail);
}

void TestFileReader(const Fixture& f) {
    wchar_t dir[MAX_PATH]{}, path[MAX_PATH]{};
    GetTempPathW(MAX_PATH, dir);
    GetTempFileNameW(dir, L"ptr", 0, path);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    const bool wrote = file != INVALID_HANDLE_VALUE &&
        WriteFile(file, f.data, static_cast<DWORD>(f.size), &written, nullptr) && written == f.size;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    TiffReduced from_file, from_memory;
    const bool ok = wrote && ReduceTiffFile(path, kMinEdge, from_file) &&
        ReduceTiffMemory(f.data, f.size, kMinEdge, 1, from_memory) == TiffReduceStatus::Made &&
        from_file.pixels == from_memory.pixels;
    DeleteFileW(path);
    Check(ok, "file reader matches the in-memory decode");
}

void TestRejections() {
    TiffReduced out;
    Check(ReduceTiffMemory(kTiff_rgb8_deflate_head, sizeof(kTiff_rgb8_deflate_head), kMinEdge, 1, out) ==
          TiffReduceStatus::Unsupported, "Deflate is left to WIC");
    Check(ReduceTiffMemory(kTiff_rgb8_bigtiff_head, sizeof(kTiff_rgb8_bigtiff_head), kMinEdge, 1, out) ==
          TiffReduceStatus::Unsupported, "BigTIFF is left to WIC");
    Check(ReduceTiffMemory(kTiff_rgb8_lzw_strips, sizeof(kTiff_rgb8_lzw_strips), 40, 1, out) ==
          TiffReduceStatus::Unsupported, "a factor below 2 is left to WIC");
    Check(ReduceTiffMemory(kTiff_rgb8_lzw_strips, sizeof(kTiff_rgb8_lzw_strips) / 2, kMinEdge, 1, out) !=
          TiffReduceStatus::Made, "strips past the end of a cut file are rejected");
    const uint8_t jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0, 0, 0, 0};
    Check(ReduceTiffMemory(jpeg, sizeof(jpeg), kMinEdge, 1, out) == TiffReduceStatus::Unsupported,
          "non-TIFF bytes are Unsupported");
    // KwKwK and the 9 -> 10 -> ... bit switches: 40000 equal bytes as
    // codes 'a', 258 ("aa"), 259 ("aaa"), ..., each one byte longer, the
    // worst case for the decoder's table (every code is the one not yet in it).
    constexpr size_t kRun = 40000;
    std::vector<uint8_t> lzw;
    {
        uint64_t buffer = 0;
        int bits = 0, width = 9, next = 258;
        bool first = true;
        auto put = [&](int code) {
            buffer = (buffer << width) | static_cast<uint64_t>(code);
            bits += width;
            while (bits >= 8) { lzw.push_back(static_cast<uint8_t>(buffer >> (bits - 8))); bits -= 8; }
            // Mirror the decoder: no entry for the first code after a clear.
            if (code == 256) { width = 9; next = 258; first = true; return; }
            if (first) { first = false; return; }
            ++next;
            if (next >= (1 << width) - 1 && width < 12) ++width;
        };
        put(256);
        size_t emitted = 0;
        int length = 1, code = 'a';
        while (emitted + length <= kRun && next < 4094) {
            put(code);
            emitted += length;
            code = length == 1 ? 258 : code + 1;
            ++length;
        }
        while (emitted < kRun) { put('a'); ++emitted; }
        put(257);
        if (bits) lzw.push_back(static_cast<uint8_t>(buffer << (8 - bits)));
    }
    std::vector<uint8_t> decoded(kRun);
    size_t written = 0;
    const bool ok = pulse::preview::DecodeTiffLzw(lzw.data(), lzw.size(), decoded.data(), decoded.size(), written);
    Check(ok && written == kRun && std::all_of(decoded.begin(), decoded.end(), [](uint8_t v) { return v == 'a'; }),
          "LZW: repeated strings (KwKwK) across the code width switch");
}

void TestDamagedInput(const Fixture* fixtures, size_t count) {
    std::mt19937 rng(20261017);
    int made = 0, runs = 0;
    for (size_t i = 0; i < count; ++i) {
        for (int round = 0; round < 2000; ++round, ++runs) {
            std::vector<uint8_t> bytes(fixtures[i].data, fixtures[i].data + fixtures[i].size);
            const int flips = 1 + static_cast<int>(rng() % 8);
            for (int f = 0; f < flips; ++f) bytes[rng() % bytes.size()] = static_cast<uint8_t>(rng());
            if (round % 5 == 0) bytes.resize(1 + rng() % bytes.size());
            TiffReduced out;
            if (ReduceTiffMemory(bytes.data(), bytes.size(), kMinEdge, 2, out) == TiffReduceStatus::Made) {
                ++made;
                if (out.pixels.size() < size_t{out.stride} * out.height) {
                    Check(false, "damaged input produced a short buffer");
                    return;
                }
            }
        }
    }
    char detail[64];
    std::snprintf(detail, sizeof(detail), "%d runs, %d still reduced", runs, made);
    Check(true, "damaged input never crashes", detail);
}

} // namespace

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;
    const Fixture fixtures[] = {
        {"rgb8_lzw_strips", kTiff_rgb8_lzw_strips, sizeof(kTiff_rgb8_lzw_strips), 3},
        {"rgb8_lzw_pred2_tiles", kTiff_rgb8_lzw_pred2_tiles, sizeof(kTiff_rgb8_lzw_pred2_tiles), 3},
        {"rgb8_none_be", kTiff_rgb8_none_be, sizeof(kTiff_rgb8_none_be), 3},
        {"rgb8_packbits", kTiff_rgb8_packbits, sizeof(kTiff_rgb8_packbits), 3},
        {"gray16_lzw_pred2_be", kTiff_gray16_lzw_pred2_be, sizeof(kTiff_gray16_lzw_pred2_be), 1},
        {"gray8_miniswhite", kTiff_gray8_miniswhite, sizeof(kTiff_gray8_miniswhite), 1},
        {"rgba8_unassoc", kTiff_rgba8_unassoc, sizeof(kTiff_rgba8_unassoc), 4},
        {"rgba8_assoc", kTiff_rgba8_assoc, sizeof(kTiff_rgba8_assoc), 4},
    };
    for (const Fixture& f : fixtures) TestFixture(f);
    TestFileReader(fixtures[1]);
    TestRejections();
    TestDamagedInput(fixtures, ARRAYSIZE(fixtures));
    std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    CoUninitialize();
    return g_failures ? 1 : 0;
}
