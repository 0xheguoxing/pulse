#pragma once

// WIC's CreateDecoderFromFilename opens the file without FILE_SHARE_DELETE
// and keeps it open while the decoder lives (a 12 MP JPEG thumbnail holds it
// ~150 ms). Meanwhile the file cannot be recycled, deleted or renamed: a bulk
// delete that reaches a picture whose thumbnail is being made fails on "in
// use". Decoders here open the file themselves with full sharing instead.

#include <windows.h>
#include <wincodec.h>
#include <string>
#include <utility>

namespace pulse::preview {

// The handle a decoder reads from. WIC does not own it: declare it before
// the decoder (and everything taken from the decoder) so it closes last.
class SharedImageFile {
public:
    SharedImageFile() = default;
    ~SharedImageFile() { Reset(); }
    SharedImageFile(const SharedImageFile&) = delete;
    SharedImageFile& operator=(const SharedImageFile&) = delete;
    SharedImageFile(SharedImageFile&& other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
    SharedImageFile& operator=(SharedImageFile&& other) noexcept {
        if (this != &other) {
            Reset();
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    // Read access; others may read, write, rename and delete meanwhile.
    HRESULT Open(const std::wstring& path) {
        Reset();
        handle_ = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        return handle_ == INVALID_HANDLE_VALUE ? HRESULT_FROM_WIN32(GetLastError()) : S_OK;
    }
    void Reset() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
    HANDLE Get() const { return handle_; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

// CreateDecoderFromFilename without holding off deletes; `file` must outlive
// `decoder`.
inline HRESULT CreateSharedDecoder(IWICImagingFactory* factory, const std::wstring& path,
                                   WICDecodeOptions options, SharedImageFile& file,
                                   IWICBitmapDecoder** decoder) {
    if (!factory || !decoder) return E_POINTER;
    *decoder = nullptr;
    if (const HRESULT opened = file.Open(path); FAILED(opened)) return opened;
    return factory->CreateDecoderFromFileHandle(reinterpret_cast<ULONG_PTR>(file.Get()), nullptr,
                                                options, decoder);
}

} // namespace pulse::preview
