// jpeg_dc_preview.h - 1/8-scale pictures of progressive JPEGs from the DC
// scan alone. A progressive file starts with the DC coefficients of every
// 8x8 block; they are the block averages, i.e. the picture shrunk by 8. WIC
// cannot scale progressive files in the DCT domain and decodes every scan at
// full size (seconds for a 100 MP photo), so grid thumbnails read just this
// first part of the file instead.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::preview {

struct JpegDcPicture {
    uint32_t width = 0, height = 0;                // ceil(source / 8)
    uint32_t source_width = 0, source_height = 0;  // the frame size in SOF
    uint32_t channels = 0;                         // 1 = gray, 3 = BGR
    uint32_t stride = 0;
    std::vector<uint8_t> pixels;
};

enum class JpegDcStatus {
    Made,
    NeedMoreData,  // the DC scan(s) run past the end of the bytes given
    Unsupported,   // not progressive / CMYK / 12-bit / smaller than min_edge...
    Corrupt,
};

// min_edge: the longest side of the 1/8 picture must reach it, else
// Unsupported (the caller wants more detail than the DC scan holds).
JpegDcStatus DecodeProgressiveJpegDc(const uint8_t* data, size_t size, uint32_t min_edge,
                                     JpegDcPicture& out);

// Reads the file from the start, growing the read only when the DC scans
// need it. False whenever the full WIC decode should be used instead.
bool DecodeProgressiveJpegDcFile(const std::wstring& path, uint32_t min_edge, JpegDcPicture& out);

} // namespace pulse::preview
