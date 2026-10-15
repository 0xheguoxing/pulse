// imgpack_test - pixel arithmetic of the image preview pack (pixel_ops.h) and
// its Radiance reader (radiance.h).
#include "pixel_ops.h"
#include "radiance.h"
#include <cstdio>
#include <string>

using namespace imgpack;

namespace {
int g_failed = 0, g_passed = 0;
void Check(bool ok, const char* what) {
    if (ok) { ++g_passed; return; }
    ++g_failed;
    std::printf("FAIL: %s\n", what);
}
bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

// Decodes a whole Radiance file into upright RGB floats (width * height * 3).
radiance::Result DecodeAll(const std::string& file, radiance::Header& h, std::vector<float>& rgb) {
    const auto* d = reinterpret_cast<const uint8_t*>(file.data());
    const radiance::Result parsed = radiance::ParseHeader(d, file.size(), h);
    if (parsed != radiance::Result::Ok) return parsed;
    rgb.assign(static_cast<size_t>(h.width) * h.height * 3, -1.0f);
    return radiance::Decode(d, file.size(), h, [&](uint32_t y, const float* rgba) {
        for (uint32_t x = 0; x < h.width; ++x)
            for (int c = 0; c < 3; ++c) rgb[(static_cast<size_t>(y) * h.width + x) * 3 + c] = rgba[x * 4 + c];
    });
}

// A pixel whose red mantissa tells it apart; exponent 129 -> value (m + 0.5) / 128.
std::string Px(uint8_t m) { return std::string{static_cast<char>(m), 0, 0, static_cast<char>(129)}; }
float Red(uint8_t m) { return (m + 0.5f) / 128.0f; }

// 3 x 2 picture  a b c / d e f  written in the given orientation.
std::string Oriented(const char* resolution) {
    const std::string s(resolution);
    const uint8_t a = 10, b = 20, c = 30, d = 40, e = 50, f = 60;
    std::string data;
    if (s == "-Y 2 +X 3") data = Px(a) + Px(b) + Px(c) + Px(d) + Px(e) + Px(f);
    if (s == "+Y 2 +X 3") data = Px(d) + Px(e) + Px(f) + Px(a) + Px(b) + Px(c);
    if (s == "-Y 2 -X 3") data = Px(c) + Px(b) + Px(a) + Px(f) + Px(e) + Px(d);
    if (s == "+Y 2 -X 3") data = Px(f) + Px(e) + Px(d) + Px(c) + Px(b) + Px(a);
    if (s == "+X 3 -Y 2") data = Px(a) + Px(d) + Px(b) + Px(e) + Px(c) + Px(f);
    if (s == "+X 3 +Y 2") data = Px(d) + Px(a) + Px(e) + Px(b) + Px(f) + Px(c);
    if (s == "-X 3 -Y 2") data = Px(c) + Px(f) + Px(b) + Px(e) + Px(a) + Px(d);
    if (s == "-X 3 +Y 2") data = Px(f) + Px(c) + Px(e) + Px(b) + Px(d) + Px(a);
    return "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n" + s + "\n" + data;
}
} // namespace

int main() {
    Check(FitSize(4000, 3000, 256) == Size{256, 192}, "landscape fits the cap");
    Check(FitSize(1000, 4000, 512) == Size{128, 512}, "portrait fits the cap");
    Check(FitSize(100, 80, 512) == Size{100, 80}, "small pictures are never enlarged");
    Check(FitSize(100000, 1, 256) == Size{256, 1}, "extreme aspect keeps one pixel");
    Check(FitSize(0, 10, 256) == Size{}, "empty source");

    {   // 4x2 -> 2x1: each output pixel averages a 2x2 block.
        ByteDownscaler d(4, 2, 2, 1);
        const uint8_t r0[] = {255,0,0,255, 255,0,0,255, 0,0,255,255, 0,0,255,0};
        const uint8_t r1[] = {255,0,0,255, 255,0,0,255, 0,0,255,255, 0,0,255,0};
        d.AddRow(0, r0, 4); d.AddRow(1, r1, 4);
        const auto out = d.Finish();
        Check(out.size() == 8 && out[2] == 255 && out[0] == 0 && out[3] == 255, "opaque red block stays red (BGRA)");
        Check(out[4] == 128 && out[7] == 128 && out[6] == 0, "half transparent blue block is premultiplied");
    }
    {   // RGB input is opaque.
        ByteDownscaler d(2, 1, 1, 1);
        const uint8_t row[] = {10,20,30, 30,40,50};
        d.AddRow(0, row, 3);
        const auto out = d.Finish();
        Check(out[0] == 40 && out[1] == 30 && out[2] == 20 && out[3] == 255, "RGB rows average opaque");
    }
    {   // Float: a NaN alpha counts as transparent.
        FloatDownscaler d(2, 1, 1, 1);
        const float row[] = {1.0f, 1.0f, 1.0f, 1.0f,   9.0f, 9.0f, 9.0f, NAN};
        d.AddRow(0, row);
        const auto out = d.Finish();
        Check(Near(out[0], 0.5f, 1e-6f) && Near(out[3], 0.5f, 1e-6f), "NaN alpha is transparent");
    }
    {
        FloatDownscaler d(2, 1, 1, 1);
        const float row[] = {2.0f, 1.0f, -5.0f, 1.0f,   4.0f, 0.0f, 0.0f, 0.5f};
        d.AddRow(0, row);
        const auto out = d.Finish();
        Check(Near(out[0], (2.0f + 2.0f) / 2, 1e-5f) && Near(out[2], 0.0f, 1e-6f) && Near(out[3], 0.75f, 1e-6f),
              "float rows premultiply and drop negative light");
    }
    Check(Near(PqToNits(0.0f), 0.0f, 1e-3f) && Near(PqToNits(1.0f), 10000.0f, 1.0f), "PQ endpoints");
    Check(Near(PqToNits(0.58f), 203.0f, 8.0f), "PQ 0.58 is about reference white");
    Check(Near(HlgToLinear(0.5f), 1.0f / 12.0f, 1e-5f) && Near(HlgToLinear(1.0f), 1.0f, 1e-3f), "HLG knee and peak");
    {
        float r = 1, g = 1, b = 1;
        Bt2020To709(r, g, b);
        Check(Near(r, 1, 0.002f) && Near(g, 1, 0.002f) && Near(b, 1, 0.002f), "white stays white across primaries");
    }
    {
        std::vector<float> grey = {0.02f, 0.02f, 0.02f, 1.0f, 0.02f, 0.02f, 0.02f, 1.0f};
        Check(Near(AutoExposure(grey), 9.0f, 0.1f), "dark picture is brightened to middle grey");
        std::vector<float> clear = {5, 5, 5, 0};
        Check(AutoExposure(clear) == 1.0f, "fully transparent picture keeps exposure 1");
        std::vector<float> sun = {1e6f, 1e6f, 1e6f, 1.0f};
        Check(Near(AutoExposure(sun), 1.0f / 64.0f, 1e-6f), "exposure is clamped to -6 stops");
    }
    Check(Filmic(0.0f) == 0.0f && Filmic(1e6f) == 1.0f && Filmic(0.5f) > Filmic(0.18f), "filmic curve is monotonic 0..1");
    {
        std::vector<float> px = {0.18f, 0.18f, 0.18f, 1.0f,   0.0f, 0.0f, 0.0f, 0.0f,   100.0f, 0.0f, 0.0f, 1.0f};
        const auto out = ToneMapToBgra(px, 1.0f);
        Check(out[3] == 255 && out[0] > 110 && out[0] < 160, "middle grey maps near the middle");
        Check(out[4] == 0 && out[7] == 0, "transparent stays transparent");
        Check(out[10] == 255 && out[8] == 0, "a very bright red clips softly to red (BGRA)");
    }
    {   // Radiance: recognised by its magic, whatever program wrote it.
        const auto looks = [](const char* s) {
            return radiance::LooksLikeRadiance(reinterpret_cast<const uint8_t*>(s), std::strlen(s));
        };
        Check(looks("#?RADIANCE\n") && looks("#?RGBE\n") && looks("#?PFSTOOLS\r\n"), "Radiance magic of any program");
        Check(looks("FORMAT=32-bit_rle_rgbe\n"), "Radiance file without magic");
        Check(!looks("#?\n") && !looks("#!/bin/sh\n") && !looks("ENVI\nsamples = 10\n"), "other .hdr files are not Radiance");
    }
    for (const char* orientation : {"-Y 2 +X 3", "+Y 2 +X 3", "-Y 2 -X 3", "+Y 2 -X 3",
                                    "+X 3 -Y 2", "+X 3 +Y 2", "-X 3 -Y 2", "-X 3 +Y 2"}) {
        radiance::Header h;
        std::vector<float> rgb;
        const bool ok = DecodeAll(Oriented(orientation), h, rgb) == radiance::Result::Ok;
        const uint8_t want[] = {10, 20, 30, 40, 50, 60};
        bool upright = ok && h.width == 3 && h.height == 2;
        for (int i = 0; upright && i < 6; ++i) upright = Near(rgb[i * 3], Red(want[i]), 1e-6f);
        std::string what = std::string("orientation ") + orientation + " decodes upright";
        Check(upright, what.c_str());
    }
    {   // CRLF header lines, no FORMAT line (RGBE by default), variables after a comment.
        radiance::Header h;
        std::vector<float> rgb;
        const std::string file = "#?RADIANCE\r\n# from windows\r\nEXPOSURE=1\r\n\r\n-Y 1 +X 2\r\n" + Px(1) + Px(2);
        Check(DecodeAll(file, h, rgb) == radiance::Result::Ok && Near(rgb[3], Red(2), 1e-6f), "CRLF header, default format");
        Check(DecodeAll("#?RADIANCE\nFORMAT=32-bit_rle_cie\n\n-Y 1 +X 1\n" + Px(1), h, rgb) ==
              radiance::Result::Unsupported, "unknown pixel format is refused");
        Check(DecodeAll("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 -Y 2\n" + Px(1), h, rgb) == radiance::Result::Damaged,
              "resolution needs one X and one Y axis");
    }
    {   // XYZE: equal-energy white stays neutral.
        radiance::Header h;
        std::vector<float> rgb;
        const std::string white{static_cast<char>(128), static_cast<char>(128), static_cast<char>(128), static_cast<char>(129)};
        const bool ok = DecodeAll("#?RADIANCE\nFORMAT=32-bit_rle_xyze\n\n-Y 1 +X 1\n" + white, h, rgb) == radiance::Result::Ok;
        Check(ok && h.xyz && Near(rgb[0], rgb[1], 0.01f) && Near(rgb[1], rgb[2], 0.01f) && Near(rgb[1], 1.0f, 0.02f),
              "XYZE white converts to neutral RGB");
    }
    {   // Adaptive RLE: an 8-pixel scanline, run of 8 in red, literals elsewhere.
        std::string line = "\x02\x02";
        line += '\0'; line += '\x08';
        line += static_cast<char>(128 + 8); line += static_cast<char>(100);          // R: run
        line += '\x08'; line += std::string(8, '\0');                                 // G: literals
        line += static_cast<char>(128 + 8); line += '\0';                              // B: run
        line += static_cast<char>(128 + 8); line += static_cast<char>(129);            // E: run
        radiance::Header h;
        std::vector<float> rgb;
        Check(DecodeAll("#?RADIANCE\n\n-Y 1 +X 8\n" + line, h, rgb) == radiance::Result::Ok && Near(rgb[21], Red(100), 1e-6f),
              "adaptive RLE scanline");
        Check(DecodeAll("#?RADIANCE\n\n-Y 2 +X 8\n" + line + line.substr(0, 9), h, rgb) == radiance::Result::Damaged,
              "cut-off scanline is damage, not a smeared picture");
    }
    {   // Old-style RLE: (1,1,1,n) repeats the pixel before it n times.
        radiance::Header h;
        std::vector<float> rgb;
        const std::string repeat{1, 1, 1, 4};
        const bool ok = DecodeAll("#?RADIANCE\n\n-Y 1 +X 6\n" + Px(7) + repeat + Px(9), h, rgb) == radiance::Result::Ok;
        Check(ok && Near(rgb[12], Red(7), 1e-6f) && Near(rgb[15], Red(9), 1e-6f), "old-style RLE repeat");
        // Flat file of exactly w*h pixels: a (1,1,1,e) pixel is just a dark colour.
        const std::string dark{1, 1, 1, static_cast<char>(129)};
        const bool flat = DecodeAll("#?RADIANCE\n\n-Y 1 +X 3\n" + Px(7) + dark + Px(9), h, rgb) == radiance::Result::Ok;
        Check(flat && Near(rgb[3], Red(1), 1e-6f) && Near(rgb[6], Red(9), 1e-6f), "flat scanlines keep dark pixels");
    }
    std::printf("imgpack_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
