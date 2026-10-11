#include "../ui/shell_icons.h"
#include "../ui/thumbnail_artwork_layout.h"
#include <shellapi.h>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <algorithm>

static bool TestArtworkBounds() {
    bool ok = true;
    const auto check = [&](pulse::ui::IconArtworkBounds bounds,
                           float left, float top, float right, float bottom,
                           const char* name) {
        const bool pass = bounds.left == left && bounds.top == top &&
                          bounds.right == right && bounds.bottom == bottom;
        std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", name);
        ok &= pass;
    };
    std::vector<uint8_t> pixels(8 * 4 * 4, 0);
    using pulse::ui::MeasureIconArtwork;
    check(MeasureIconArtwork(pixels, 8, 4), 0, 0, 1, 1, "transparent artwork uses full-frame fallback");
    for (size_t y = 1; y < 3; ++y)
        for (size_t x = 2; x < 6; ++x) pixels[(y * 8 + x) * 4 + 3] = 255;
    check(MeasureIconArtwork(pixels, 8, 4), .25f, .25f, .75f, .75f,
          "transparent padding produces normalized pixel edges");
    pixels[(3 * 8 + 7) * 4 + 3] = 1;
    check(MeasureIconArtwork(pixels, 8, 4), .25f, .25f, 1, 1,
          "faint edge pixels count as artwork");
    std::fill(pixels.begin(), pixels.end(), uint8_t{255});
    check(MeasureIconArtwork(pixels, 8, 4), 0, 0, 1, 1, "opaque artwork preserves full frame");
    check(MeasureIconArtwork(pixels, 0, 4), 0, 0, 1, 1, "zero-size input uses fallback");
    check(MeasureIconArtwork(pixels, 9, 4), 0, 0, 1, 1, "truncated input uses fallback");
    std::vector<uint8_t> padded(2 * 16, 0);
    padded[16 + 4 + 3] = 255;
    padded[15] = 255;  // Padding is not image artwork.
    check(MeasureIconArtwork(padded, 2, 2, 16), .5f, .5f, 1, 1,
          "thumbnail alpha bounds honor padded row stride");
    check(MeasureIconArtwork(padded, 2, 2, 4), 0, 0, 1, 1,
          "undersized row stride uses fallback");
    const D2D1_RECT_F dest = D2D1::RectF(10, 20, 110, 120);
    const pulse::ui::IconArtworkBounds inset{.25f, .25f, .75f, .75f};
    const auto geometry = [&](uint32_t w, uint32_t h,
                              pulse::ui::IconArtworkBounds bounds, bool bottom,
                              float left, float top, float right, float lower,
                              const char* name) {
        const auto fitted = pulse::ui::ContainedThumbnailRect(dest, w, h, bounds, bottom);
        const auto artwork = pulse::ui::ThumbnailArtworkRect(fitted, bounds);
        check({artwork.left, artwork.top, artwork.right, artwork.bottom},
              left, top, right, lower, name);
    };
    geometry(100, 100, inset, false, 35, 45, 85, 95,
             "default thumbnail retains centered transparent artwork");
    geometry(100, 100, inset, true, 35, 70, 85, 120,
             "transparent thumbnail aligns actual artwork to slot bottom");
    geometry(200, 100, {}, false, 10, 45, 110, 95,
             "default opaque landscape thumbnail retains contain layout");
    geometry(200, 100, {}, true, 10, 70, 110, 120,
             "landscape thumbnail bottom alignment preserves aspect ratio");
    geometry(100, 200, {}, true, 35, 20, 85, 120,
             "portrait thumbnail bottom alignment preserves horizontal centering");
    return ok;
}

namespace pulse::ui {
struct ShellIconCacheTestAccess {
    static bool Wait(ShellIconCache& cache) {
        const auto end = GetTickCount64() + 10000;
        while (GetTickCount64() < end) {
            {
                std::lock_guard lock(cache.mutex_);
                if (cache.queued_.empty()) return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }
    static bool Run() {
        ShellIconCache cache;
        bool ok = true;
        auto check = [&](bool result, const char* name) {
            std::printf("[%s] %s\n", result ? "PASS" : "FAIL", name);
            ok &= result;
        };
        cache.GenericIndex(L"", true, FILE_ATTRIBUTE_DIRECTORY);
        cache.GenericIndex(L"test.txt", false, FILE_ATTRIBUTE_NORMAL);
        check(Wait(cache), "native type queries complete");
        {
            std::lock_guard lock(cache.mutex_);
            check(cache.generic_index_.contains(L"<dir>") && cache.generic_index_.contains(L".txt"),
                  "Windows folder and associated file icons resolve");
        }
        wchar_t windows[MAX_PATH]{};
        GetWindowsDirectoryW(windows, MAX_PATH);
        check(cache.NeedsExactIcon(L"Windows", true, windows), "folders use actual Shell paths");
        // Seed a full cache to exercise eviction without thousands of Shell calls.
        {
            std::lock_guard lock(cache.mutex_);
            for (int i = 0; i < 4096; ++i) {
                const auto key = L"seed-" + std::to_wstring(i);
                cache.exact_index_[key] = 0;
                cache.last_used_[key] = ++cache.access_clock_;
            }
        }
        cache.RequestExact(windows);
        check(Wait(cache), "exact folder query completes");
        SHFILEINFOW info{};
        const bool resolved = SHGetFileInfoW(windows, 0, &info, sizeof(info),
                                            SHGFI_SYSICONINDEX | SHGFI_SMALLICON) != 0;
        {
            std::lock_guard lock(cache.mutex_);
            check(resolved && cache.exact_index_.contains(windows) &&
                  cache.exact_index_.at(windows) == info.iIcon, "index matches native Shell query");
            check(cache.exact_index_.size() == 4096 && !cache.exact_index_.contains(L"seed-0") &&
                  cache.exact_index_.contains(L"seed-1"), "evicts only oldest entry");
        }
        const std::wstring missing = std::wstring(windows) + L"\\pulse-nonexistent-icon-test\\missing.exe";
        cache.RequestExact(missing);
        check(Wait(cache), "missing path query completes");
        {
            std::lock_guard lock(cache.mutex_);
            check(!cache.exact_index_.contains(missing) && cache.retry_after_.contains(missing),
                  "failure is retryable rather than permanently cached");
            cache.retry_after_[windows] = 0;
            cache.exact_index_.erase(windows);
            cache.last_used_.erase(windows);
        }
        cache.RequestExact(windows);
        check(Wait(cache), "expired failure retries");
        {
            std::lock_guard lock(cache.mutex_);
            check(cache.exact_index_.contains(windows) && !cache.retry_after_.contains(windows),
                  "successful retry clears failure state");
        }
        cache.Reset();
        cache.GenericIndex(L"", true, FILE_ATTRIBUTE_DIRECTORY);
        check(Wait(cache), "worker restarts after reset");
        ComPtr<ID3D11Device> d3d;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<ID2D1Factory1> factory;
        ComPtr<ID2D1Device> device;
        ComPtr<ID2D1DeviceContext> context;
        const bool graphics = SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP,
            nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &d3d, nullptr, nullptr)) &&
            SUCCEEDED(d3d->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
            SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateDevice(dxgi.get(), &device)) &&
            SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context));
        check(graphics, "create Direct2D software device");
        if (graphics) {
            cache.SetDeviceContext(context.get());
            for (const float size : {16.0f, 24.0f, 32.0f, 48.0f, 96.0f, 256.0f}) {
                auto* bitmap = cache.BitmapFor(L"", L"", true, FILE_ATTRIBUTE_DIRECTORY, size);
                check(bitmap && bitmap->GetPixelSize().width >= 16,
                      "native folder converts to Direct2D bitmap at requested scale");
                const auto bounds = cache.CachedArtworkBounds(L"", L"", true,
                    FILE_ATTRIBUTE_DIRECTORY, size);
                check(bounds.left >= 0 && bounds.top >= 0 && bounds.right <= 1 &&
                      bounds.bottom <= 1 && bounds.left < bounds.right && bounds.top < bounds.bottom &&
                      cache.artwork_bounds_.size() == cache.bitmaps_.size(),
                      "converted icon caches valid normalized artwork bounds");
            }
            // Multi-window M3d: a second window's context on the same device
            // keeps every bitmap, and the first window's context may go away.
            ComPtr<ID2D1DeviceContext> second;
            ComPtr<ID2D1Bitmap1> second_target;
            const auto target_props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            const bool second_ok =
                SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &second)) &&
                SUCCEEDED(second->CreateBitmap(D2D1::SizeU(64, 64), nullptr, 0, &target_props, &second_target));
            check(second_ok, "second window context on the same device");
            if (second_ok) {
                second->SetTarget(second_target.get());
                auto* const before = cache.BitmapFor(L"", L"", true, FILE_ATTRIBUTE_DIRECTORY, 32);
                // Held so a flushed cache cannot free it or hand its address to a new bitmap.
                ComPtr<ID2D1Bitmap> held;
                if (before) { before->AddRef(); held.p = before; }
                const size_t cached = cache.bitmaps_.size();
                cache.SetDeviceContext(second.get());
                check(cache.bitmaps_.size() == cached &&
                          cache.BitmapFor(L"", L"", true, FILE_ATTRIBUTE_DIRECTORY, 32) == before,
                      "another window on the same device keeps the cached bitmaps");
                second->BeginDraw();
                if (held.get()) second->DrawBitmap(held.get(), D2D1::RectF(0, 0, 32, 32));
                check(before && SUCCEEDED(second->EndDraw()), "a cached icon draws in the other window");
                context.reset();
                check(cache.BitmapFor(L"", L"", true, FILE_ATTRIBUTE_DIRECTORY, 20) != nullptr,
                      "icons still convert after the first window's context is released");
                cache.SetDeviceContext(second.get());
            }
            cache.SetDeviceContext(nullptr);
            check(cache.artwork_bounds_.empty(), "device replacement clears artwork metadata");
            if (!context.get()) context = std::move(second);
            cache.SetDeviceContext(context.get());
            check(cache.BitmapFor(L"", L"", true, FILE_ATTRIBUTE_DIRECTORY, 32) != nullptr,
                  "device recreation preserves native indices and rebuilds bitmap");
            cache.Reset();
        }
        return ok;
    }
};
}

int main(int argc, char** argv) {
    const bool bounds_ok = TestArtworkBounds();
    if (argc == 2 && std::strcmp(argv[1], "--bounds-only") == 0) return bounds_ok ? 0 : 1;
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool ok = pulse::ui::ShellIconCacheTestAccess::Run() && bounds_ok;
    if (SUCCEEDED(hr)) CoUninitialize();
    return ok ? 0 : 1;
}
