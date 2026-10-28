#pragma once

// Image preview pack: HEIC / AVIF / JPEG XL without the Store extensions, and
// OpenEXR, Radiance HDR and QOI, which Windows never reads. The decoder is
// pulse-imgpack.exe (packs/images); the wire format is in
// common/image_pack_protocol.h.
//
// Performance rules (see docs/design/preview_packs_plan.md):
//  - Windows' own codecs and thumbnail cache go first wherever they exist;
//  - one short-lived pulse-imgpack per picture, below-normal priority, two
//    decode threads, in a kill-on-close Job Object with a memory cap;
//  - at most a few run at once however many thumbnails a folder asks for;
//  - the pack scales while decoding (HEIF's own thumbnails, EXR in bands), so
//    only cap x cap pixels ever cross the pipe.

#include "preview_properties.h"
#include <windows.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::preview {

// Every extension the pack reads (lower case, with the dot).
bool IsImagePackExtension(std::wstring_view extension);
// The ones outside the WIC image list (.jxl .exr .hdr .qoi): they get their own
// decoder entry instead of the image one.
bool IsImagePackOnlyExtension(std::wstring_view extension);

// True when the pack is installed and enabled. Cached; costs one tick-count
// compare on the hot path.
bool ImagePackAvailable();

struct ImagePackFrame {
    std::vector<uint8_t> pixels;        // 32bpp premultiplied BGRA, top-down
    UINT width = 0, height = 0, stride = 0;
    UINT source_width = 0, source_height = 0;
    uint32_t flags = 0;                 // imgpack::FrameFlags
};

// The picture scaled to fit `cap` x `cap` (never enlarged).
bool ImagePackDecode(const std::wstring& path, UINT cap, bool grid, ImagePackFrame& frame);

// Big still PNG / WebP (imgpack::IsAcceleratedExtension), which the pack
// decodes several times faster than WIC. False - WIC goes on - when the file is
// small, animated, turned by Exif, the pack is missing or busy, or the
// installed pack predates these formats (remembered per pack folder).
bool ImagePackAccelerate(const std::wstring& path, UINT cap, bool grid, ImagePackFrame& frame);

// Details-pane rows (size, format, channels). Appends at most `max_rows`
// rows not already present in `rows`.
bool ImagePackProperties(const std::wstring& path, std::vector<PreviewPropertyValue>& rows,
                         size_t max_rows);

// True when the file starts like one of the pack's formats (16 bytes read):
// tells "install the pack" apart from a text file that happens to be .hdr.
bool LooksLikeImagePackFile(const std::wstring& path);

// ---- Exposed for tests -------------------------------------------------------

// First bytes -> format name ("HEIF", "JPEG XL", "OpenEXR", ...), empty if none.
std::wstring_view SniffImagePackFormat(const uint8_t* bytes, size_t size);
// pulse-imgpack decode output -> frame; false on any inconsistency.
bool ParseImagePackFrame(const std::vector<uint8_t>& output, UINT cap, ImagePackFrame& frame);
// pulse-imgpack probe output ("key=value" lines) -> display rows.
std::vector<PreviewPropertyValue> ParseImagePackProbe(std::string_view output);
// First bytes of a PNG / WebP -> its pixel count when the pack should take it
// (still, not interlaced, no Exif orientation), else 0. Size is not judged here.
uint64_t AcceleratedSourcePixels(const uint8_t* bytes, size_t size);
// How many decoders may run at once on a machine with `cpus` logical CPUs.
unsigned ImagePackConcurrency(unsigned cpus);

} // namespace pulse::preview
