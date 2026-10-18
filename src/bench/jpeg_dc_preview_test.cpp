// jpeg_dc_preview_test.cpp - the progressive JPEG DC-scan thumbnail path
// (src/preview_host/jpeg_dc_preview.cpp) against WIC's full decode.
// The 1/8 picture must equal the block means of the full-size image.
#include "../preview_host/jpeg_dc_preview.h"
#include "jpeg_dc_fixtures.inc"
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;
using pulse::preview::DecodeProgressiveJpegDc;
using pulse::preview::DecodeProgressiveJpegDcFile;
using pulse::preview::JpegDcPicture;
using pulse::preview::JpegDcStatus;

namespace {

int g_failures = 0;

// Rounding to 8-bit BGR and the encoder's IDCT leave about one level on
// average (measured: at most 1.1 mean, 9.1 max). A misplaced block or a wrong
// predictor is off by tens of levels.
constexpr double kMeanLimit = 2.0;
constexpr double kMaxLimit = 12.0;

void Check(bool ok, const char* name, const char* detail = "") {
    std::printf("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", name, *detail ? " - " : "", detail);
    if (!ok) ++g_failures;
}

struct Full {
    UINT width = 0, height = 0, channels = 0;
    std::vector<uint8_t> pixels;  // BGR or gray, tightly packed
};

bool DecodeWithWic(const uint8_t* data, size_t size, UINT channels, Full& out) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(size))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
                                                &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(),
            channels == 1 ? GUID_WICPixelFormat8bppGray : GUID_WICPixelFormat24bppBGR,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&out.width, &out.height))) return false;
    out.channels = channels;
    const UINT stride = out.width * channels;
    out.pixels.resize(static_cast<size_t>(stride) * out.height);
    return SUCCEEDED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(out.pixels.size()),
                                           out.pixels.data()));
}

struct Diff { double mean = 0; double max = 0; };

// JFIF YCbCr of a BGR (or gray) pixel.
void ToYcc(const uint8_t* px, uint32_t channels, double ycc[3]) {
    if (channels == 1) { ycc[0] = px[0]; ycc[1] = ycc[2] = 128.0; return; }
    const double b = px[0], g = px[1], r = px[2];
    ycc[0] = 0.299 * r + 0.587 * g + 0.114 * b;
    ycc[1] = 128.0 - 0.168736 * r - 0.331264 * g + 0.5 * b;
    ycc[2] = 128.0 + 0.5 * r - 0.418688 * g - 0.081312 * b;
}

// The DC picture against the full decode, per JPEG component: luma against
// the 8x8 block means, chroma against the means of its own (subsampled)
// blocks, i.e. 8*h_factor x 8*v_factor pixels. Those are exactly what the DC
// coefficients hold, so the only error left is rounding and the IDCT.
void CompareComponents(const JpegDcPicture& dc, const Full& full, uint32_t h_factor,
                       uint32_t v_factor, Diff& luma, Diff& chroma) {
    double luma_total = 0, chroma_total = 0;
    size_t luma_count = 0, chroma_count = 0;
    for (uint32_t by = 0; by < dc.height; ++by) {
        for (uint32_t bx = 0; bx < dc.width; ++bx) {
            double got[3];
            ToYcc(&dc.pixels[static_cast<size_t>(by) * dc.stride + bx * dc.channels], dc.channels, got);
            for (int c = 0; c < (full.channels == 1 ? 1 : 3); ++c) {
                const uint32_t sx = c == 0 ? 8 : 8 * h_factor, sy = c == 0 ? 8 : 8 * v_factor;
                const uint32_t x0 = (bx * 8 / sx) * sx, y0 = (by * 8 / sy) * sy;
                double sum = 0;
                int n = 0;
                for (uint32_t y = y0; y < (std::min)(y0 + sy, full.height); ++y) {
                    for (uint32_t x = x0; x < (std::min)(x0 + sx, full.width); ++x, ++n) {
                        double ycc[3];
                        ToYcc(&full.pixels[(static_cast<size_t>(y) * full.width + x) * full.channels],
                              full.channels, ycc);
                        sum += ycc[c];
                    }
                }
                const double d = std::fabs(got[c] - sum / n);
                Diff& diff = c == 0 ? luma : chroma;
                diff.max = (std::max)(diff.max, d);
                (c == 0 ? luma_total : chroma_total) += d;
                ++(c == 0 ? luma_count : chroma_count);
            }
        }
    }
    luma.mean = luma_count ? luma_total / luma_count : 1e9;
    chroma.mean = chroma_count ? chroma_total / chroma_count : 0;
}

struct Fixture {
    const char* name;
    const uint8_t* data;
    size_t size;
    UINT channels;
    uint32_t h_factor, v_factor;  // chroma subsampling (2,2 = 4:2:0)
};

void TestFixture(const Fixture& f) {
    char detail[256];
    JpegDcPicture dc;
    const JpegDcStatus status = DecodeProgressiveJpegDc(f.data, f.size, 1, dc);
    std::string name = std::string(f.name) + " decodes";
    Check(status == JpegDcStatus::Made, name.c_str());
    if (status != JpegDcStatus::Made) return;

    Full full;
    name = std::string(f.name) + " WIC reference";
    Check(DecodeWithWic(f.data, f.size, f.channels, full), name.c_str());
    if (full.pixels.empty()) return;

    std::snprintf(detail, sizeof(detail), "source %ux%u, dc %ux%u ch=%u (WIC %ux%u)",
                  dc.source_width, dc.source_height, dc.width, dc.height, dc.channels,
                  full.width, full.height);
    name = std::string(f.name) + " geometry";
    Check(dc.source_width == full.width && dc.source_height == full.height &&
          dc.width == (full.width + 7) / 8 && dc.height == (full.height + 7) / 8 &&
          dc.channels == f.channels && dc.stride >= dc.width * dc.channels &&
          dc.pixels.size() >= static_cast<size_t>(dc.stride) * dc.height, name.c_str(), detail);

    Diff luma, chroma;
    CompareComponents(dc, full, f.h_factor, f.v_factor, luma, chroma);
    std::snprintf(detail, sizeof(detail), "Y mean %.2f max %.1f, CbCr mean %.2f max %.1f",
                  luma.mean, luma.max, chroma.mean, chroma.max);
    name = std::string(f.name) + " matches the block means";
    Check(luma.mean <= kMeanLimit && luma.max <= kMaxLimit &&
          chroma.mean <= kMeanLimit && chroma.max <= kMaxLimit, name.c_str(), detail);

    JpegDcPicture too_small;
    name = std::string(f.name) + " min_edge above the 1/8 size is Unsupported";
    Check(DecodeProgressiveJpegDc(f.data, f.size, (std::max)(dc.width, dc.height) + 1, too_small) ==
          JpegDcStatus::Unsupported, name.c_str());
}

void TestFileReader(const Fixture& f) {
    wchar_t dir[MAX_PATH]{}, path[MAX_PATH]{};
    GetTempPathW(MAX_PATH, dir);
    GetTempFileNameW(dir, L"pdc", 0, path);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    const bool wrote = file != INVALID_HANDLE_VALUE &&
        WriteFile(file, f.data, static_cast<DWORD>(f.size), &written, nullptr) && written == f.size;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    JpegDcPicture from_file, from_memory;
    const bool ok = wrote && DecodeProgressiveJpegDcFile(path, 1, from_file) &&
        DecodeProgressiveJpegDc(f.data, f.size, 1, from_memory) == JpegDcStatus::Made &&
        from_file.pixels == from_memory.pixels && from_file.width == from_memory.width;
    DeleteFileW(path);
    Check(ok, "file reader matches the in-memory decode");
}

void TestRejections() {
    JpegDcPicture dc;
    Check(DecodeProgressiveJpegDc(kJpeg_baseline_420, sizeof(kJpeg_baseline_420), 1, dc) ==
          JpegDcStatus::Unsupported, "baseline JPEG is Unsupported (WIC scales it itself)");
    // Cut inside the first (DC) scan.
    size_t sos = 0;
    for (size_t i = 0; i + 1 < sizeof(kJpeg_progressive_420); ++i)
        if (kJpeg_progressive_420[i] == 0xFF && kJpeg_progressive_420[i + 1] == 0xDA) { sos = i; break; }
    Check(sos > 0 && DecodeProgressiveJpegDc(kJpeg_progressive_420, sos + 40, 1, dc) ==
          JpegDcStatus::NeedMoreData, "progressive cut inside the DC scan needs more data");
    Check(DecodeProgressiveJpegDc(kJpeg_progressive_420, sos, 1, dc) == JpegDcStatus::NeedMoreData,
          "progressive cut before the first scan needs more data");
    const uint8_t garbage[] = {0x00, 0x11, 0x22, 0x33, 0x44};
    Check(DecodeProgressiveJpegDc(garbage, sizeof(garbage), 1, dc) != JpegDcStatus::Made,
          "non-JPEG bytes are rejected");
    Check(DecodeProgressiveJpegDc(nullptr, 0, 1, dc) != JpegDcStatus::Made, "empty input is rejected");
}

// Damaged files must fail cleanly (or still decode), never crash or hang.
void TestDamagedInput(const Fixture* fixtures, size_t count) {
    std::mt19937 rng(20261017);
    int made = 0, runs = 0;
    for (size_t i = 0; i < count; ++i) {
        for (int round = 0; round < 3000; ++round, ++runs) {
            std::vector<uint8_t> bytes(fixtures[i].data, fixtures[i].data + fixtures[i].size);
            const int flips = 1 + static_cast<int>(rng() % 8);
            for (int k = 0; k < flips; ++k) bytes[rng() % bytes.size()] = static_cast<uint8_t>(rng());
            if (round % 5 == 0) bytes.resize(1 + rng() % bytes.size());
            JpegDcPicture dc;
            if (DecodeProgressiveJpegDc(bytes.data(), bytes.size(), 1, dc) == JpegDcStatus::Made) {
                ++made;
                if (dc.pixels.size() < static_cast<size_t>(dc.stride) * dc.height) {
                    Check(false, "damaged input produced a short buffer");
                    return;
                }
            }
        }
    }
    char detail[64];
    std::snprintf(detail, sizeof(detail), "%d runs, %d still decoded", runs, made);
    Check(true, "damaged input never crashes", detail);
}

} // namespace

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;
    const Fixture fixtures[] = {
        {"progressive_420", kJpeg_progressive_420, sizeof(kJpeg_progressive_420), 3, 2, 2},
        {"progressive_444", kJpeg_progressive_444, sizeof(kJpeg_progressive_444), 3, 1, 1},
        {"progressive_422", kJpeg_progressive_422, sizeof(kJpeg_progressive_422), 3, 2, 1},
        {"progressive_restart", kJpeg_progressive_restart, sizeof(kJpeg_progressive_restart), 3, 2, 2},
        {"progressive_gray", kJpeg_progressive_gray, sizeof(kJpeg_progressive_gray), 1, 1, 1},
    };
    for (const Fixture& f : fixtures) TestFixture(f);
    TestFileReader(fixtures[0]);
    TestRejections();
    TestDamagedInput(fixtures, ARRAYSIZE(fixtures));
    std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    CoUninitialize();
    return g_failures ? 1 : 0;
}
