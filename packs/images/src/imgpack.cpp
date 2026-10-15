// pulse-imgpack.exe - decoder of Pulse's optional image preview pack.
//
// HEIC / HEIF (libheif + libde265), AVIF (libheif + dav1d), JPEG XL (libjxl),
// OpenEXR, Radiance HDR (radiance.h) and QOI. The preview host starts one
// short-lived process per picture inside a kill-on-close job with a memory
// cap; see src/common/image_pack_protocol.h for the command line and output.
//
// Files are memory-mapped and decoded from memory (wide paths work, no
// decoder ever opens a file itself). Formats are recognised by their bytes,
// not their extension. Output is scaled while decoding where the format
// allows it: HEIF's own thumbnails for small requests, EXR in bands of rows.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include "../../../src/common/image_pack_protocol.h"
#include "pixel_ops.h"
#include "radiance.h"

#include <libheif/heif.h>
#include <jxl/decode.h>
#include <jxl/cms.h>
#include <jxl/thread_parallel_runner.h>
#include <ImfIO.h>
#include <ImfRgbaFile.h>
#include <ImfHeader.h>
#include <ImfChannelList.h>
#include <ImfCompression.h>
#include <ImfMultiPartInputFile.h>
#include <ImfThreading.h>

#define QOI_NO_STDIO
#define QOI_IMPLEMENTATION
#include "../third_party/qoi.h"

#include <climits>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace pulse::imgpack;

namespace {

constexpr int kThreads = 2;
constexpr uint64_t kMaxFileBytes = 2ull << 30;   // 2 GB

// ---- Input -------------------------------------------------------------------

class MappedFile {
public:
    ~MappedFile() {
        if (data_) UnmapViewOfFile(data_);
        if (map_) CloseHandle(map_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }
    int Open(const wchar_t* path) {
        file_ = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return kExitIoError;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file_, &size) || size.QuadPart <= 0) return kExitIoError;
        if (static_cast<uint64_t>(size.QuadPart) > kMaxFileBytes) return kExitTooLarge;
        map_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!map_) return kExitIoError;
        data_ = static_cast<const uint8_t*>(MapViewOfFile(map_, FILE_MAP_READ, 0, 0, 0));
        if (!data_) return kExitIoError;
        size_ = static_cast<size_t>(size.QuadPart);
        return kExitOk;
    }
    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }
private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE map_ = nullptr;
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

enum class Format { Unknown, Heif, Jxl, Exr, Radiance, Qoi };

Format Detect(const uint8_t* d, size_t n) {
    if (n >= 12 && std::memcmp(d + 4, "ftyp", 4) == 0) return Format::Heif;
    const JxlSignature jxl = JxlSignatureCheck(d, n);
    if (jxl == JXL_SIG_CODESTREAM || jxl == JXL_SIG_CONTAINER) return Format::Jxl;
    if (n >= 4 && d[0] == 0x76 && d[1] == 0x2f && d[2] == 0x31 && d[3] == 0x01) return Format::Exr;
    if (radiance::LooksLikeRadiance(d, n)) return Format::Radiance;
    if (n >= 14 && std::memcmp(d, "qoif", 4) == 0) return Format::Qoi;
    return Format::Unknown;
}

// ---- Output ------------------------------------------------------------------

struct Picture {
    uint32_t width = 0, height = 0;                 // of `bgra`
    uint32_t source_width = 0, source_height = 0;   // of the file's main image
    uint32_t flags = 0;
    std::vector<uint8_t> bgra;                      // premultiplied, top-down
};

bool WriteAll(const void* data, size_t bytes) {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    const auto* p = static_cast<const uint8_t*>(data);
    while (bytes) {
        DWORD wrote = 0;
        const DWORD chunk = static_cast<DWORD>((std::min)(bytes, size_t{1} << 20));
        if (!WriteFile(out, p, chunk, &wrote, nullptr) || !wrote) return false;
        p += wrote;
        bytes -= wrote;
    }
    return true;
}

int Emit(const Picture& picture) {
    FrameHeader header;
    header.width = picture.width;
    header.height = picture.height;
    header.stride = picture.width * 4;
    header.source_width = picture.source_width;
    header.source_height = picture.source_height;
    header.flags = picture.flags;
    if (!picture.width || !picture.height ||
        picture.bgra.size() != static_cast<size_t>(header.stride) * picture.height)
        return kExitDecodeFailed;
    return WriteAll(&header, sizeof(header)) && WriteAll(picture.bgra.data(), picture.bgra.size())
        ? kExitOk : kExitIoError;
}

void Fail(const char* what) { std::fprintf(stderr, "pulse-imgpack: %s\n", what); }

using Properties = std::vector<std::pair<std::string, std::string>>;

int EmitProperties(const Properties& rows) {
    std::string text;
    for (const auto& [key, value] : rows) {
        if (value.empty()) continue;
        text += key; text += '='; text += value; text += '\n';
    }
    return WriteAll(text.data(), text.size()) ? kExitOk : kExitIoError;
}

// ---- HEIF / AVIF ---------------------------------------------------------------

struct HeifDeleter {
    void operator()(heif_context* c) const { heif_context_free(c); }
    void operator()(heif_image_handle* h) const { heif_image_handle_release(h); }
    void operator()(heif_image* i) const { heif_image_release(i); }
};
template <typename T> using HeifPtr = std::unique_ptr<T, HeifDeleter>;

bool IsAvifBrand(const uint8_t* d, size_t n) {
    return n >= 12 && (std::memcmp(d + 8, "avif", 4) == 0 || std::memcmp(d + 8, "avis", 4) == 0);
}

struct HeifSource {
    HeifPtr<heif_context> context;
    HeifPtr<heif_image_handle> primary;
};

int OpenHeif(const MappedFile& file, HeifSource& source) {
    source.context.reset(heif_context_alloc());
    if (!source.context) return kExitDecodeFailed;
    heif_context_set_max_decoding_threads(source.context.get(), kThreads);
    if (heif_context_read_from_memory_without_copy(source.context.get(), file.data(), file.size(), nullptr).code !=
        heif_error_Ok)
        return kExitUnsupported;
    heif_image_handle* primary = nullptr;
    if (heif_context_get_primary_image_handle(source.context.get(), &primary).code != heif_error_Ok)
        return kExitDecodeFailed;
    source.primary.reset(primary);
    return kExitOk;
}

struct Transfer { bool pq = false, hlg = false, bt2020 = false; };

Transfer HeifTransfer(const heif_image_handle* handle) {
    Transfer t;
    heif_color_profile_nclx* nclx = nullptr;
    if (heif_image_handle_get_nclx_color_profile(handle, &nclx).code == heif_error_Ok && nclx) {
        t.pq = nclx->transfer_characteristics == heif_transfer_characteristic_ITU_R_BT_2100_0_PQ;
        t.hlg = nclx->transfer_characteristics == heif_transfer_characteristic_ITU_R_BT_2100_0_HLG;
        t.bt2020 = nclx->color_primaries == heif_color_primaries_ITU_R_BT_2020_2_and_2100_0;
        heif_nclx_color_profile_free(nclx);
    }
    return t;
}

int DecodeHeif(const MappedFile& file, uint32_t cap, Picture& out) {
    HeifSource source;
    if (const int opened = OpenHeif(file, source); opened != kExitOk) return opened;
    const int width = heif_image_handle_get_width(source.primary.get());
    const int height = heif_image_handle_get_height(source.primary.get());
    if (width <= 0 || height <= 0) return kExitDecodeFailed;
    if (static_cast<uint64_t>(width) * static_cast<uint64_t>(height) > kMaxSourcePixels) return kExitTooLarge;
    out.source_width = static_cast<uint32_t>(width);
    out.source_height = static_cast<uint32_t>(height);

    // Small requests (grid thumbnails) use the file's own thumbnail when it is
    // at least as large as asked: no full-size HEVC / AV1 decode at all.
    HeifPtr<heif_image_handle> thumbnail;
    const int thumbnails = heif_image_handle_get_number_of_thumbnails(source.primary.get());
    if (cap <= 512 && thumbnails > 0) {
        std::vector<heif_item_id> ids(static_cast<size_t>(thumbnails));
        heif_image_handle_get_list_of_thumbnail_IDs(source.primary.get(), ids.data(), thumbnails);
        int best_side = INT_MAX;
        for (const heif_item_id id : ids) {
            heif_image_handle* candidate = nullptr;
            if (heif_image_handle_get_thumbnail(source.primary.get(), id, &candidate).code != heif_error_Ok) continue;
            HeifPtr<heif_image_handle> owned(candidate);
            const int side = (std::max)(heif_image_handle_get_width(candidate), heif_image_handle_get_height(candidate));
            if (side >= static_cast<int>(cap) && side < best_side) {
                best_side = side;
                thumbnail = std::move(owned);
            }
        }
    }
    heif_image_handle* handle = thumbnail ? thumbnail.get() : source.primary.get();
    if (thumbnail) out.flags |= kFlagEmbeddedThumbnail;

    const bool alpha = heif_image_handle_has_alpha_channel(handle) != 0;
    const int bits = heif_image_handle_get_luma_bits_per_pixel(handle);
    const Transfer transfer = HeifTransfer(handle);
    const bool hdr = bits > 8 && (transfer.pq || transfer.hlg);
    const heif_chroma chroma = bits > 8
        ? (alpha ? heif_chroma_interleaved_RRGGBBAA_LE : heif_chroma_interleaved_RRGGBB_LE)
        : (alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB);
    heif_decoding_options* options = heif_decoding_options_alloc();
    heif_image* decoded = nullptr;
    const heif_error error = heif_decode_image(handle, &decoded, heif_colorspace_RGB, chroma, options);
    heif_decoding_options_free(options);
    if (error.code != heif_error_Ok || !decoded) {
        Fail(error.message ? error.message : "heif decode");
        return kExitDecodeFailed;
    }
    HeifPtr<heif_image> image(decoded);
    int stride = 0;
    const uint8_t* plane = heif_image_get_plane_readonly(decoded, heif_channel_interleaved, &stride);
    const int iw = heif_image_get_width(decoded, heif_channel_interleaved);
    const int ih = heif_image_get_height(decoded, heif_channel_interleaved);
    if (!plane || iw <= 0 || ih <= 0 || stride <= 0) return kExitDecodeFailed;
    if (!thumbnail) { out.source_width = static_cast<uint32_t>(iw); out.source_height = static_cast<uint32_t>(ih); }

    const imgpack::Size fit = imgpack::FitSize(static_cast<uint32_t>(iw), static_cast<uint32_t>(ih), cap);
    out.width = fit.w;
    out.height = fit.h;
    if (alpha) out.flags |= kFlagAlpha;
    const int channels = alpha ? 4 : 3;
    if (bits <= 8) {
        imgpack::ByteDownscaler scale(iw, ih, fit.w, fit.h);
        for (int y = 0; y < ih; ++y) scale.AddRow(y, plane + static_cast<size_t>(y) * stride, channels);
        out.bgra = scale.Finish();
        return kExitOk;
    }
    const float max_code = static_cast<float>((1 << bits) - 1);
    if (!hdr) {   // 10/12-bit SDR: to 8 bits
        imgpack::ByteDownscaler scale(iw, ih, fit.w, fit.h);
        std::vector<uint8_t> row(static_cast<size_t>(iw) * channels);
        for (int y = 0; y < ih; ++y) {
            const auto* src = reinterpret_cast<const uint16_t*>(plane + static_cast<size_t>(y) * stride);
            for (size_t i = 0; i < row.size(); ++i)
                row[i] = static_cast<uint8_t>(std::lround((std::min)(static_cast<float>(src[i]), max_code) / max_code * 255.0f));
            scale.AddRow(y, row.data(), channels);
        }
        out.bgra = scale.Finish();
        return kExitOk;
    }
    // PQ / HLG: to linear light (1.0 = reference white), BT.709, filmic.
    imgpack::FloatDownscaler scale(iw, ih, fit.w, fit.h);
    std::vector<float> row(static_cast<size_t>(iw) * 4);
    for (int y = 0; y < ih; ++y) {
        const auto* src = reinterpret_cast<const uint16_t*>(plane + static_cast<size_t>(y) * stride);
        for (int x = 0; x < iw; ++x, src += channels) {
            float rgb[3];
            for (int c = 0; c < 3; ++c) {
                const float e = (std::min)(static_cast<float>(src[c]), max_code) / max_code;
                rgb[c] = transfer.pq ? imgpack::PqToNits(e) / imgpack::kReferenceWhiteNits
                                     : imgpack::HlgToLinear(e) * (1000.0f / imgpack::kReferenceWhiteNits);
            }
            if (transfer.bt2020 || transfer.pq || transfer.hlg) imgpack::Bt2020To709(rgb[0], rgb[1], rgb[2]);
            float* p = &row[static_cast<size_t>(x) * 4];
            p[0] = rgb[0]; p[1] = rgb[1]; p[2] = rgb[2];
            p[3] = alpha ? (std::min)(static_cast<float>(src[3]), max_code) / max_code : 1.0f;
        }
        scale.AddRow(y, row.data());
    }
    out.bgra = imgpack::ToneMapToBgra(scale.Finish(), 1.0f);
    out.flags |= kFlagToneMapped;
    return kExitOk;
}

int ProbeHeif(const MappedFile& file, Properties& rows) {
    HeifSource source;
    if (const int opened = OpenHeif(file, source); opened != kExitOk) return opened;
    const heif_image_handle* h = source.primary.get();
    const Transfer t = HeifTransfer(h);
    rows.push_back({"format", IsAvifBrand(file.data(), file.size()) ? "AVIF" : "HEIF"});
    rows.push_back({"width", std::to_string(heif_image_handle_get_width(h))});
    rows.push_back({"height", std::to_string(heif_image_handle_get_height(h))});
    rows.push_back({"bit_depth", std::to_string(heif_image_handle_get_luma_bits_per_pixel(h))});
    rows.push_back({"alpha", heif_image_handle_has_alpha_channel(h) ? "yes" : ""});
    rows.push_back({"hdr", t.pq ? "PQ" : t.hlg ? "HLG" : ""});
    rows.push_back({"primaries", t.bt2020 ? "BT.2020" : ""});
    const int images = heif_context_get_number_of_top_level_images(source.context.get());
    rows.push_back({"images", images > 1 ? std::to_string(images) : ""});
    rows.push_back({"depth", heif_image_handle_has_depth_image(h) ? "yes" : ""});
    return kExitOk;
}

// ---- JPEG XL -------------------------------------------------------------------

struct JxlState {
    JxlDecoder* decoder = nullptr;
    void* runner = nullptr;
    ~JxlState() {
        if (runner) JxlThreadParallelRunnerDestroy(runner);
        if (decoder) JxlDecoderDestroy(decoder);
    }
};

int DecodeJxl(const MappedFile& file, uint32_t cap, Picture& out, Properties* probe) {
    JxlState s;
    s.decoder = JxlDecoderCreate(nullptr);
    s.runner = JxlThreadParallelRunnerCreate(nullptr, kThreads);
    if (!s.decoder || !s.runner) return kExitDecodeFailed;
    JxlDecoderSetParallelRunner(s.decoder, JxlThreadParallelRunner, s.runner);
    JxlDecoderSetCms(s.decoder, *JxlGetDefaultCms());
    const int events = probe ? (JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING)
                             : (JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE);
    if (JxlDecoderSubscribeEvents(s.decoder, events) != JXL_DEC_SUCCESS) return kExitDecodeFailed;
    JxlDecoderSetInput(s.decoder, file.data(), file.size());
    JxlDecoderCloseInput(s.decoder);

    JxlBasicInfo info{};
    bool hdr = false;
    JxlPixelFormat format{4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    std::vector<uint8_t> bytes;
    std::vector<float> floats;
    for (;;) {
        const JxlDecoderStatus status = JxlDecoderProcessInput(s.decoder);
        if (status == JXL_DEC_ERROR || status == JXL_DEC_NEED_MORE_INPUT) {
            Fail("jxl decode");
            return kExitDecodeFailed;
        }
        if (status == JXL_DEC_SUCCESS) break;
        if (status == JXL_DEC_BASIC_INFO) {
            if (JxlDecoderGetBasicInfo(s.decoder, &info) != JXL_DEC_SUCCESS) return kExitDecodeFailed;
            if (static_cast<uint64_t>(info.xsize) * info.ysize > kMaxSourcePixels) return kExitTooLarge;
            out.source_width = info.xsize;
            out.source_height = info.ysize;
            hdr = info.intensity_target > 255.0f;
        } else if (status == JXL_DEC_COLOR_ENCODING) {
            JxlColorEncoding encoding{};
            if (JxlDecoderGetColorAsEncodedProfile(s.decoder, JXL_COLOR_PROFILE_TARGET_DATA, &encoding) ==
                JXL_DEC_SUCCESS)
                hdr = hdr || encoding.transfer_function == JXL_TRANSFER_FUNCTION_PQ ||
                      encoding.transfer_function == JXL_TRANSFER_FUNCTION_HLG;
            if (probe) {
                probe->push_back({"format", "JPEG XL"});
                probe->push_back({"width", std::to_string(info.xsize)});
                probe->push_back({"height", std::to_string(info.ysize)});
                probe->push_back({"bit_depth", std::to_string(info.bits_per_sample)});
                probe->push_back({"alpha", info.alpha_bits ? "yes" : ""});
                probe->push_back({"hdr", hdr ? "yes" : ""});
                probe->push_back({"animated", info.have_animation ? "yes" : ""});
                probe->push_back({"lossless", info.uses_original_profile ? "yes" : ""});
                return kExitOk;
            }
            // SDR: 8-bit sRGB straight from libjxl. HDR: linear float, mapped here.
            // Filled by hand: JxlColorEncodingSetToSRGB lives in the encoder API.
            JxlColorEncoding wanted{};
            wanted.color_space = JXL_COLOR_SPACE_RGB;
            wanted.white_point = JXL_WHITE_POINT_D65;
            wanted.primaries = JXL_PRIMARIES_SRGB;
            wanted.transfer_function = hdr ? JXL_TRANSFER_FUNCTION_LINEAR : JXL_TRANSFER_FUNCTION_SRGB;
            wanted.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;
            if (hdr) format.data_type = JXL_TYPE_FLOAT;
            JxlDecoderSetPreferredColorProfile(s.decoder, &wanted);
        } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            size_t size = 0;
            if (JxlDecoderImageOutBufferSize(s.decoder, &format, &size) != JXL_DEC_SUCCESS) return kExitDecodeFailed;
            void* buffer = nullptr;
            if (format.data_type == JXL_TYPE_FLOAT) { floats.resize(size / sizeof(float)); buffer = floats.data(); }
            else { bytes.resize(size); buffer = bytes.data(); }
            if (JxlDecoderSetImageOutBuffer(s.decoder, &format, buffer, size) != JXL_DEC_SUCCESS)
                return kExitDecodeFailed;
        } else if (status == JXL_DEC_FULL_IMAGE) {
            break;   // the first frame of an animation is enough
        }
    }
    if (probe) return kExitDecodeFailed;
    const uint32_t w = info.xsize, h = info.ysize;
    if (!w || !h) return kExitDecodeFailed;
    const imgpack::Size fit = imgpack::FitSize(w, h, cap);
    out.width = fit.w;
    out.height = fit.h;
    if (info.alpha_bits) out.flags |= kFlagAlpha;
    if (format.data_type == JXL_TYPE_FLOAT) {
        if (floats.size() < static_cast<size_t>(w) * h * 4) return kExitDecodeFailed;
        imgpack::FloatDownscaler scale(w, h, fit.w, fit.h);
        for (uint32_t y = 0; y < h; ++y) scale.AddRow(y, &floats[static_cast<size_t>(y) * w * 4]);
        // libjxl's linear output has 1.0 at the image's intensity target.
        const float exposure = info.intensity_target > 0 ? info.intensity_target / imgpack::kReferenceWhiteNits : 1.0f;
        out.bgra = imgpack::ToneMapToBgra(scale.Finish(), (std::min)(exposure, 16.0f));
        out.flags |= kFlagToneMapped;
        return kExitOk;
    }
    if (bytes.size() < static_cast<size_t>(w) * h * 4) return kExitDecodeFailed;
    imgpack::ByteDownscaler scale(w, h, fit.w, fit.h);
    for (uint32_t y = 0; y < h; ++y) scale.AddRow(y, &bytes[static_cast<size_t>(y) * w * 4], 4);
    out.bgra = scale.Finish();
    return kExitOk;
}

// ---- OpenEXR -------------------------------------------------------------------

class MemoryStream : public Imf::IStream {
public:
    MemoryStream(const uint8_t* data, size_t size) : Imf::IStream("memory"), data_(data), size_(size) {}
    bool isMemoryMapped() const override { return true; }
    char* readMemoryMapped(int n) override {
        if (n < 0 || at_ + static_cast<size_t>(n) > size_) throw std::runtime_error("read past end");
        char* p = const_cast<char*>(reinterpret_cast<const char*>(data_ + at_));
        at_ += static_cast<size_t>(n);
        return p;
    }
    bool read(char c[], int n) override {
        if (n < 0 || at_ + static_cast<size_t>(n) > size_) throw std::runtime_error("read past end");
        std::memcpy(c, data_ + at_, static_cast<size_t>(n));
        at_ += static_cast<size_t>(n);
        return at_ < size_;
    }
    uint64_t tellg() override { return at_; }
    void seekg(uint64_t pos) override { at_ = static_cast<size_t>((std::min)(pos, static_cast<uint64_t>(size_))); }
    void clear() override {}
private:
    const uint8_t* data_;
    size_t size_;
    size_t at_ = 0;
};

const char* CompressionName(Imf::Compression c) {
    switch (c) {
    case Imf::NO_COMPRESSION: return "None";
    case Imf::RLE_COMPRESSION: return "RLE";
    case Imf::ZIPS_COMPRESSION: return "ZIPS";
    case Imf::ZIP_COMPRESSION: return "ZIP";
    case Imf::PIZ_COMPRESSION: return "PIZ";
    case Imf::PXR24_COMPRESSION: return "PXR24";
    case Imf::B44_COMPRESSION: return "B44";
    case Imf::B44A_COMPRESSION: return "B44A";
    case Imf::DWAA_COMPRESSION: return "DWAA";
    case Imf::DWAB_COMPRESSION: return "DWAB";
    default: return "";
    }
}

int DecodeExr(const MappedFile& file, uint32_t cap, Picture& out, Properties* probe) {
    try {
        Imf::setGlobalThreadCount(kThreads);
        MemoryStream stream(file.data(), file.size());
        Imf::RgbaInputFile input(stream, kThreads);
        const Imath::Box2i dw = input.dataWindow();
        const int64_t w64 = static_cast<int64_t>(dw.max.x) - dw.min.x + 1;
        const int64_t h64 = static_cast<int64_t>(dw.max.y) - dw.min.y + 1;
        if (w64 <= 0 || h64 <= 0) return kExitDecodeFailed;
        if (static_cast<uint64_t>(w64) * static_cast<uint64_t>(h64) > kMaxSourcePixels) return kExitTooLarge;
        const uint32_t w = static_cast<uint32_t>(w64), h = static_cast<uint32_t>(h64);
        out.source_width = w;
        out.source_height = h;
        const bool alpha = (input.channels() & Imf::WRITE_A) != 0;
        if (probe) {
            std::string channels;
            for (auto it = input.header().channels().begin(); it != input.header().channels().end(); ++it) {
                if (!channels.empty()) channels += ", ";
                if (channels.size() > 120) { channels += "..."; break; }
                channels += it.name();
            }
            probe->push_back({"format", "OpenEXR"});
            probe->push_back({"width", std::to_string(w)});
            probe->push_back({"height", std::to_string(h)});
            probe->push_back({"channels", channels});
            probe->push_back({"compression", CompressionName(input.compression())});
            probe->push_back({"tiled", input.header().hasTileDescription() ? "yes" : ""});
            MemoryStream parts_stream(file.data(), file.size());
            Imf::MultiPartInputFile parts(parts_stream);
            probe->push_back({"parts", parts.parts() > 1 ? std::to_string(parts.parts()) : ""});
            return kExitOk;
        }
        const imgpack::Size fit = imgpack::FitSize(w, h, cap);
        out.width = fit.w;
        out.height = fit.h;
        if (alpha) out.flags |= kFlagAlpha;
        imgpack::FloatDownscaler scale(w, h, fit.w, fit.h);
        // Bands of rows: memory stays small however large the picture is.
        constexpr uint32_t kBand = 64;
        std::vector<Imf::Rgba> band(static_cast<size_t>(w) * kBand);
        std::vector<float> row(static_cast<size_t>(w) * 4);
        for (uint32_t y0 = 0; y0 < h; y0 += kBand) {
            const uint32_t rows = (std::min)(kBand, h - y0);
            const int first = dw.min.y + static_cast<int>(y0);
            Imf::Rgba* base = band.data() - static_cast<ptrdiff_t>(dw.min.x) -
                              static_cast<ptrdiff_t>(first) * static_cast<ptrdiff_t>(w);
            input.setFrameBuffer(base, 1, w);
            input.readPixels(first, first + static_cast<int>(rows) - 1);
            for (uint32_t r = 0; r < rows; ++r) {
                const Imf::Rgba* src = &band[static_cast<size_t>(r) * w];
                for (uint32_t x = 0; x < w; ++x) {
                    float* p = &row[static_cast<size_t>(x) * 4];
                    p[0] = src[x].r; p[1] = src[x].g; p[2] = src[x].b;
                    p[3] = alpha ? static_cast<float>(src[x].a) : 1.0f;
                }
                scale.AddRow(y0 + r, row.data());
            }
        }
        std::vector<float> linear = scale.Finish();
        out.bgra = imgpack::ToneMapToBgra(linear, imgpack::AutoExposure(linear));
        out.flags |= kFlagToneMapped;
        return kExitOk;
    } catch (const std::exception& e) {
        Fail(e.what());
        return kExitDecodeFailed;
    }
}

// ---- Radiance HDR, QOI ---------------------------------------------------------

// Scanlines go straight into the downscaler: memory follows the output size,
// not the panorama's (a 16K .hdr is 2 GB as floats).
int DecodeRadiance(const MappedFile& file, uint32_t cap, Picture& out, Properties* probe) {
    radiance::Header h;
    switch (radiance::ParseHeader(file.data(), file.size(), h)) {
    case radiance::Result::Ok: break;
    case radiance::Result::Unsupported: Fail("unsupported Radiance pixel format"); return kExitDecodeFailed;
    default: Fail("damaged Radiance header"); return kExitDecodeFailed;
    }
    if (static_cast<uint64_t>(h.width) * h.height > kMaxSourcePixels) return kExitTooLarge;
    out.source_width = h.width;
    out.source_height = h.height;
    if (probe) {
        probe->push_back({"format", h.xyz ? "Radiance HDR (XYZE)" : "Radiance HDR"});
        probe->push_back({"width", std::to_string(h.width)});
        probe->push_back({"height", std::to_string(h.height)});
        return kExitOk;
    }
    const imgpack::Size fit = imgpack::FitSize(h.width, h.height, cap);
    out.width = fit.w;
    out.height = fit.h;
    imgpack::FloatDownscaler scale(h.width, h.height, fit.w, fit.h);
    const radiance::Result decoded = radiance::Decode(file.data(), file.size(), h,
        [&](uint32_t y, const float* rgba) { scale.AddRow(y, rgba); });
    if (decoded == radiance::Result::TooLarge) return kExitTooLarge;
    if (decoded != radiance::Result::Ok) {
        Fail("damaged Radiance scanlines");
        return kExitDecodeFailed;
    }
    std::vector<float> linear = scale.Finish();
    out.bgra = imgpack::ToneMapToBgra(linear, imgpack::AutoExposure(linear));
    out.flags |= kFlagToneMapped;
    return kExitOk;
}

// qoi.h pads a short chunk stream with the last pixel instead of failing, so a
// cut-off download would preview as a smeared picture. Walk the chunks first:
// they must cover every pixel before the 8-byte end marker.
bool QoiStreamComplete(const uint8_t* d, size_t size, uint64_t pixels) {
    if (size < 14 + 8) return false;
    const size_t end = size - 8;
    size_t p = 14;
    uint64_t n = 0;
    while (n < pixels) {
        if (p >= end) return false;
        const uint8_t b = d[p];
        if (b == 0xFE) p += 4;                    // QOI_OP_RGB
        else if (b == 0xFF) p += 5;               // QOI_OP_RGBA
        else if ((b & 0xC0) == 0xC0) { n += (b & 0x3F) + 1u; ++p; continue; }  // QOI_OP_RUN
        else if ((b & 0xC0) == 0x80) p += 2;      // QOI_OP_LUMA
        else p += 1;                              // QOI_OP_INDEX / QOI_OP_DIFF
        if (p > end) return false;
        ++n;
    }
    return true;
}
int DecodeQoi(const MappedFile& file, uint32_t cap, Picture& out, Properties* probe) {
    if (file.size() > static_cast<size_t>(INT_MAX)) return kExitTooLarge;
    const uint8_t* d = file.data();
    const uint32_t w = (uint32_t{d[4]} << 24) | (uint32_t{d[5]} << 16) | (uint32_t{d[6]} << 8) | d[7];
    const uint32_t h = (uint32_t{d[8]} << 24) | (uint32_t{d[9]} << 16) | (uint32_t{d[10]} << 8) | d[11];
    if (!w || !h) return kExitDecodeFailed;
    if (static_cast<uint64_t>(w) * h > kMaxSourcePixels) return kExitTooLarge;
    out.source_width = w;
    out.source_height = h;
    const bool alpha = d[12] == 4;
    if (probe) {
        probe->push_back({"format", "QOI"});
        probe->push_back({"width", std::to_string(w)});
        probe->push_back({"height", std::to_string(h)});
        probe->push_back({"alpha", alpha ? "yes" : ""});
        return kExitOk;
    }
    if (!QoiStreamComplete(d, file.size(), static_cast<uint64_t>(w) * h)) return kExitDecodeFailed;
    qoi_desc desc{};
    void* pixels = qoi_decode(d, static_cast<int>(file.size()), &desc, 4);
    if (!pixels) return kExitDecodeFailed;
    std::unique_ptr<void, void (*)(void*)> owned(pixels, std::free);
    const imgpack::Size fit = imgpack::FitSize(desc.width, desc.height, cap);
    out.width = fit.w;
    out.height = fit.h;
    if (alpha) out.flags |= kFlagAlpha;
    imgpack::ByteDownscaler scale(desc.width, desc.height, fit.w, fit.h);
    for (uint32_t y = 0; y < desc.height; ++y)
        scale.AddRow(y, static_cast<const uint8_t*>(pixels) + static_cast<size_t>(y) * desc.width * 4, 4);
    out.bgra = scale.Finish();
    return kExitOk;
}

// ---- Commands ------------------------------------------------------------------

int Run(const wchar_t* command, const wchar_t* path, uint32_t cap) {
    MappedFile file;
    if (const int opened = file.Open(path); opened != kExitOk) {
        Fail("cannot read the file");
        return opened;
    }
    const bool probe = std::wcscmp(command, L"probe") == 0;
    Picture picture;
    Properties rows;
    int result = kExitUnsupported;
    switch (Detect(file.data(), file.size())) {
    case Format::Heif: result = probe ? ProbeHeif(file, rows) : DecodeHeif(file, cap, picture); break;
    case Format::Jxl: result = DecodeJxl(file, cap, picture, probe ? &rows : nullptr); break;
    case Format::Exr: result = DecodeExr(file, cap, picture, probe ? &rows : nullptr); break;
    case Format::Radiance: result = DecodeRadiance(file, cap, picture, probe ? &rows : nullptr); break;
    case Format::Qoi: result = DecodeQoi(file, cap, picture, probe ? &rows : nullptr); break;
    case Format::Unknown: Fail("not a format the image pack reads"); break;
    }
    if (result != kExitOk) return result;
    return probe ? EmitProperties(rows) : Emit(picture);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    // A crashing decoder must not raise Windows Error Reporting dialogs.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _setmode(_fileno(stdout), _O_BINARY);
    if (argc >= 2 && std::wcscmp(argv[1], L"version") == 0) {
        std::printf("pulse-imgpack %d libheif %s libjxl %u.%u.%u\n", kVersion, heif_get_version(),
                    JPEGXL_MAJOR_VERSION, JPEGXL_MINOR_VERSION, JPEGXL_PATCH_VERSION);
        return kExitOk;
    }
    if (argc >= 2 && std::wcscmp(argv[1], L"formats") == 0) {
        std::printf(".heic .heif .hif .avif .jxl .exr .hdr .qoi\n");
        std::printf("hevc=%d av1=%d\n", heif_have_decoder_for_format(heif_compression_HEVC),
                    heif_have_decoder_for_format(heif_compression_AV1));
        return kExitOk;
    }
    if (argc == 4 && std::wcscmp(argv[1], L"decode") == 0) {
        const unsigned long cap = std::wcstoul(argv[3], nullptr, 10);
        if (cap < 16 || cap > kMaxCap) return kExitUsage;
        return Run(argv[1], argv[2], static_cast<uint32_t>(cap));
    }
    if (argc == 3 && std::wcscmp(argv[1], L"probe") == 0) return Run(argv[1], argv[2], 0);
    std::fprintf(stderr, "usage: pulse-imgpack decode <file> <cap> | probe <file> | formats | version\n");
    return kExitUsage;
}
