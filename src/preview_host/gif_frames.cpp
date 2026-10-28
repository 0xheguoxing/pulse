#include "gif_frames.h"
#include "preview_file_utils.h"
#include "shared_image_file.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
namespace pulse::preview {
using Microsoft::WRL::ComPtr;
namespace {
constexpr uint64_t kGifWorkingBytes = 64ull * 1024 * 1024;
struct GifState {
    std::wstring path;
    WIN32_FILE_ATTRIBUTE_DATA identity{};
    std::vector<uint8_t> canvas, saved;
    UINT width = 0, height = 0;
    uint32_t next_frame = 0, delay = 100;
    uint32_t left = 0, top = 0, frame_width = 0, frame_height = 0, disposal = 0;
};
thread_local GifState state;
}
static bool MetadataUInt(IWICMetadataQueryReader* reader, const wchar_t* name,
                         uint32_t& value) {
    if (!reader) return false;
    PROPVARIANT pv{}; PropVariantInit(&pv);
    const HRESULT hr = reader->GetMetadataByName(name, &pv);
    bool ok = SUCCEEDED(hr);
    if (ok) {
        if (pv.vt == VT_UI1) value = pv.bVal;
        else if (pv.vt == VT_UI2) value = pv.uiVal;
        else if (pv.vt == VT_UI4) value = pv.ulVal;
        else if (pv.vt == VT_I4 && pv.lVal >= 0) value = static_cast<uint32_t>(pv.lVal);
        else ok = false;
    }
    PropVariantClear(&pv);
    return ok;
}

static void GifFrameMetadata(IWICBitmapFrameDecode* frame, uint32_t& left,
                             uint32_t& top, uint32_t& width, uint32_t& height,
                             uint32_t& disposal, uint32_t& delay_ms) {
    left = top = 0; width = height = 0; disposal = 0; delay_ms = 100;
    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(&reader)) || !reader) {
        frame->GetSize(&width, &height); return;
    }
    MetadataUInt(reader.Get(), L"/imgdesc/Left", left);
    MetadataUInt(reader.Get(), L"/imgdesc/Top", top);
    MetadataUInt(reader.Get(), L"/imgdesc/Width", width);
    MetadataUInt(reader.Get(), L"/imgdesc/Height", height);
    MetadataUInt(reader.Get(), L"/grctlext/Disposal", disposal);
    uint32_t delay = 0;
    if (MetadataUInt(reader.Get(), L"/grctlext/Delay", delay) && delay > 0)
        delay_ms = std::clamp(delay * 10u, 20u, 2000u);
    if (!width || !height) frame->GetSize(&width, &height);
}

static void AlphaBlendPbgra(uint8_t* dst, const uint8_t* src) {
    const uint32_t sa = src[3];
    if (sa == 255) { dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255; return; }
    if (sa == 0) return;
    const uint32_t inv = 255u - sa;
    dst[0] = static_cast<uint8_t>(src[0] + (dst[0] * inv + 127u) / 255u);
    dst[1] = static_cast<uint8_t>(src[1] + (dst[1] * inv + 127u) / 255u);
    dst[2] = static_cast<uint8_t>(src[2] + (dst[2] * inv + 127u) / 255u);
    dst[3] = static_cast<uint8_t>(sa + (dst[3] * inv + 127u) / 255u);
}

bool DecodeGifFrame(const std::wstring& path, DWORD attrs, UINT pixels,
                           uint32_t frame_index, std::vector<uint8_t>& out,
                           UINT& width, UINT& height, UINT& stride,
                           uint32_t& frame_count, uint32_t& delay_ms,
                           uint32_t& loop_count, UINT& source_width, UINT& source_height, std::wstring* error) {
    if (IsOfflinePlaceholder(attrs)) return false;
    WIN32_FILE_ATTRIBUTE_DATA identity{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &identity)) return false;
    if (state.path != path || identity.nFileSizeHigh != state.identity.nFileSizeHigh ||
        identity.nFileSizeLow != state.identity.nFileSizeLow ||
        CompareFileTime(&identity.ftLastWriteTime, &state.identity.ftLastWriteTime) != 0 ||
        frame_index + 1 < state.next_frame) state = GifState{};
    struct FailureReset {
        bool committed = false;
        ~FailureReset() { if (!committed) state = GifState{}; }
    } completion;
    SharedImageFile file;  // outlives the decoder
    ComPtr<IWICImagingFactory> factory; ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))) ||
        FAILED(CreateSharedDecoder(factory.Get(), path, WICDecodeMetadataCacheOnLoad, file, &decoder))) return false;
    UINT count = 0; if (FAILED(decoder->GetFrameCount(&count)) || !count) return false;
    frame_count = count; frame_index = (std::min)(frame_index, count - 1);
    loop_count = 0;
    ComPtr<IWICMetadataQueryReader> decoder_reader;
    if (SUCCEEDED(decoder->GetMetadataQueryReader(&decoder_reader)) && decoder_reader) {
        PROPVARIANT pv{}; PropVariantInit(&pv);
        if (SUCCEEDED(decoder_reader->GetMetadataByName(L"/appext/Data", &pv)) &&
            ((pv.vt & VT_VECTOR) != 0) && ((pv.vt & VT_TYPEMASK) == VT_UI1) &&
            pv.caub.cElems >= 16) {
            const auto* b = pv.caub.pElems;
            for (ULONG i = 0; i + 4 < pv.caub.cElems; ++i)
                if (b[i] == 'N' && b[i + 1] == 'E' && b[i + 2] == 'T' &&
                    b[i + 3] == 'S' && b[i + 4] == 'C') {
                    for (ULONG j = i; j + 15 < pv.caub.cElems; ++j)
                        if (b[j] == 0x03 && b[j + 1] == 0x01) {
                            loop_count = b[j + 2] | (static_cast<uint32_t>(b[j + 3]) << 8); break;
                        }
                    break;
                }
        }
        PropVariantClear(&pv);
    }
    ComPtr<IWICBitmapFrameDecode> first; if (FAILED(decoder->GetFrame(0, &first))) return false;
    UINT canvas_w = 0, canvas_h = 0; first->GetSize(&canvas_w, &canvas_h);
    ComPtr<IWICMetadataQueryReader> first_reader;
    if (SUCCEEDED(decoder->GetMetadataQueryReader(&first_reader)) && first_reader) {
        uint32_t v = 0;
        if (MetadataUInt(first_reader.Get(), L"/logscrdesc/Width", v) && v) canvas_w = v;
        if (MetadataUInt(first_reader.Get(), L"/logscrdesc/Height", v) && v) canvas_h = v;
    }
    if (!canvas_w || !canvas_h || canvas_w > 16384 || canvas_h > 16384) {
        if (error) *error = L"gif-resource-limit";
        return false;
    }
    source_width = canvas_w;
    source_height = canvas_h;
    const uint64_t canvas_bytes = static_cast<uint64_t>(canvas_w) * canvas_h * 4;
    // Reserve space for canvas, disposal-3 restore, current frame and output.
    if (canvas_bytes > kGifWorkingBytes / 4) {
        if (error) *error = L"gif-resource-limit";
        return false;
    }
    if (state.canvas.empty() || state.width != canvas_w || state.height != canvas_h) {
        state = GifState{};
        state.path = path;
        state.identity = identity;
        state.width = canvas_w;
        state.height = canvas_h;
        state.canvas.assign(static_cast<size_t>(canvas_bytes), 0);
    }
    auto& canvas = state.canvas;
    auto& saved = state.saved;
    auto& prev_left = state.left; auto& prev_top = state.top;
    auto& prev_w = state.frame_width; auto& prev_h = state.frame_height;
    auto& prev_disposal = state.disposal;
    delay_ms = state.delay;
    const ULONGLONG deadline = GetTickCount64() + 2000;
    for (uint32_t i = state.next_frame; i <= frame_index; ++i) {
        if (GetTickCount64() >= deadline) {
            if (error) *error = L"gif-time-limit";
            return false;
        }
        if (i > 0) {
            if (prev_disposal == 2) {
                const uint32_t x0 = (std::min)(prev_left, canvas_w);
                const uint32_t x1 = (std::min)(canvas_w, prev_left + prev_w);
                for (uint32_t y = (std::min)(prev_top, canvas_h);
                     y < (std::min)(canvas_h, prev_top + prev_h); ++y)
                    std::fill(canvas.begin() + (static_cast<size_t>(y) * canvas_w + x0) * 4,
                              canvas.begin() + (static_cast<size_t>(y) * canvas_w + x1) * 4, uint8_t{0});
            } else if (prev_disposal == 3 && saved.size() == canvas.size()) canvas = saved;
        }
        ComPtr<IWICBitmapFrameDecode> frame; if (FAILED(decoder->GetFrame(i, &frame))) return false;
        uint32_t left, top, fw, fh, disposal, current_delay;
        GifFrameMetadata(frame.Get(), left, top, fw, fh, disposal, current_delay);
        if (i == frame_index) delay_ms = current_delay;
        if (disposal == 3) saved = canvas;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(factory->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom))) return false;
        UINT rw = 0, rh = 0; converter->GetSize(&rw, &rh);
        if (!rw || !rh || static_cast<uint64_t>(rw) * rh * 4 > kGifWorkingBytes - canvas_bytes * 3) {
            if (error) *error = L"gif-resource-limit";
            return false;
        }
        const UINT copy_w = (std::min)(fw, rw), copy_h = (std::min)(fh, rh);
        std::vector<uint8_t> pixels_data(static_cast<size_t>(rw) * rh * 4);
        if (FAILED(converter->CopyPixels(nullptr, rw * 4, static_cast<UINT>(pixels_data.size()), pixels_data.data()))) return false;
        for (UINT y = 0; y < copy_h && top + y < canvas_h; ++y)
            for (UINT x = 0; x < copy_w && left + x < canvas_w; ++x)
                AlphaBlendPbgra(&canvas[(static_cast<size_t>(top + y) * canvas_w + left + x) * 4],
                                &pixels_data[(static_cast<size_t>(y) * rw + x) * 4]);
        prev_left = left; prev_top = top; prev_w = fw; prev_h = fh; prev_disposal = disposal;
        state.next_frame = i + 1;
        state.delay = current_delay;
    }
    width = canvas_w; height = canvas_h; stride = canvas_w * 4;
    const UINT longest = (std::max)(canvas_w, canvas_h);
    if (longest > pixels) {
        const double ratio = static_cast<double>(pixels) / longest;
        width = (std::max)(1u, static_cast<UINT>(canvas_w * ratio + 0.5));
        height = (std::max)(1u, static_cast<UINT>(canvas_h * ratio + 0.5));
        ComPtr<IWICBitmap> bitmap;
        if (FAILED(factory->CreateBitmapFromMemory(canvas_w, canvas_h, GUID_WICPixelFormat32bppPBGRA,
                                                   canvas_w * 4, static_cast<UINT>(canvas.size()), canvas.data(), &bitmap))) return false;
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(bitmap.Get(), width, height, WICBitmapInterpolationModeFant))) return false;
        out.resize(static_cast<size_t>(width) * height * 4); stride = width * 4;
        completion.committed = SUCCEEDED(scaler->CopyPixels(nullptr, stride, static_cast<UINT>(out.size()), out.data()));
        return completion.committed;
    }
    out = canvas;
    completion.committed = true;
    return true;
}


}
