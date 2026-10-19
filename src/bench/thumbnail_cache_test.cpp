#include "../ui/thumbnail_cache.h"
#include "../ui/thumbnail_artwork_layout.h"
#include <cstdio>

namespace pulse::ui {
struct ThumbnailCacheTestAccess {
    // A cut-out whose lower half is transparent: pictures rest the whole
    // bitmap on the grid baseline, icons rest their visible ink on it.
    static bool ArtworkCanvasRegression() {
        std::vector<uint8_t> px(4 * 4 * 4, 0);
        for (size_t i = 0; i < 8; ++i) px[i * 4 + 3] = 255;
        const auto place = [&](const wchar_t* path) {
            return ContainedThumbnailRect(D2D1::RectF(0, 0, 100, 100), 4, 4,
                ThumbnailArtworkBounds(path, px, 4, 4, 16), true);
        };
        const D2D1_RECT_F webp = place(L"C:\\cut\\Product.WEBP"), psd = place(L"art.psd");
        const D2D1_RECT_F icon = place(L"app.ico"), svg = place(L"logo.svg");
        const bool ok = webp.top == 0.0f && webp.bottom == 100.0f && psd.bottom == 100.0f &&
            icon.bottom == 150.0f && svg.bottom == 150.0f && IsPictureCanvasPath(L"raw.NEF") &&
            !IsPictureCanvasPath(L"dir.v2\\noext");
        std::printf("[%s] grid bottom alignment keeps pictures whole and measures icon ink\n",
                    ok ? "PASS" : "FAIL");
        return ok;
    }
    static bool DeadlineAndPageRegression() {
        ThumbnailCache cache;
        cache.running_ = true;
        const auto epoch = cache.epoch_.load();
        ThumbnailCache::Item item;
        item.failed = item.transient = true;
        item.retry_at = GetTickCount64() + 50;
        cache.items_[L"retry"] = std::move(item);
        cache.retry_interest_.insert(L"retry");
        const bool deadline = cache.NextRetryDeadline() > GetTickCount64();
        cache.items_[L"retry"].retry_at = GetTickCount64() - 1;
        const bool expired = cache.NextRetryDeadline() != 0;
        cache.BeginFrame();
        const bool inactive = cache.NextRetryDeadline() == 0;
        cache.retry_interest_.insert(L"retry");
        cache.pending_.insert(L"retry");
        const bool queued = cache.NextRetryDeadline() == 0;
        cache.Evict();
        const bool evicted = cache.NextRetryDeadline() == 0 && cache.epoch_.load() != epoch;
        cache.running_ = false;
        std::printf("[%s] preview retry deadline excludes pending work and capability eviction changes epoch\n", deadline && expired && inactive && queued && evicted ? "PASS" : "FAIL");
        return deadline && expired && inactive && queued && evicted;
    }
    static bool SheetRegression() {
        ComPtr<ID3D11Device> d3d;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
        ComPtr<IDXGIDevice> dxgi;
        if (SUCCEEDED(hr)) hr = d3d->QueryInterface(IID_PPV_ARGS(&dxgi));
        ComPtr<ID2D1Device> device;
        if (SUCCEEDED(hr)) hr = D2D1CreateDevice(dxgi.get(), nullptr, &device);
        ComPtr<ID2D1DeviceContext> dc;
        if (SUCCEEDED(hr)) hr = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
        if (FAILED(hr)) return false;
        ThumbnailCache cache;
        cache.SetDeviceContext(dc.get());
        cache.running_ = true;
        bool ok = true;
        auto check = [&](bool value, const char* label) {
            std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
            ok &= value;
        };
        std::wstring text;
        preview::Integrity integrity;
        const auto draw = [&](uint32_t sheet) {
            return cache.Draw(dc.get(), D2D1::RectF(0, 0, 100, 100), L"workbook.XLSX",
                0, 512, 1, 2, 3, 1, &text, nullptr, nullptr, true, nullptr,
                nullptr, nullptr, nullptr, nullptr, sheet, nullptr, nullptr, nullptr,
                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, nullptr, &integrity);
        };
        draw(0);
        const auto old = cache.queue_.front();
        draw(16);
        draw(99);
        check(cache.queue_.size() == 1 && cache.queue_.front().frame_index == 99 &&
            cache.pending_.size() == 1, "rapid sheet switches keep only latest queued content request");
        const auto current = cache.queue_.front();
        cache.queue_.clear();
        ThumbnailCache::Item stale;
        stale.kind = ipc::PreviewContentKind::Table;
        stale.text = L"sheet0";
        cache.StoreResult(old, std::move(stale));
        check(draw(99) == PreviewDrawResult::Pending && text.empty(), "old sheet response cannot satisfy current sheet request");
        ThumbnailCache::Item ready;
        ready.kind = ipc::PreviewContentKind::Table;
        ready.text = L"sheet99";
        ready.integrity.state = preview::IntegrityState::Partial;
        ready.integrity.reason = preview::IntegrityReason::OnDemand;
        ready.integrity.loaded = 1;
        ready.integrity.total = 100;
        cache.StoreResult(current, std::move(ready));
        check(draw(99) == PreviewDrawResult::Table && text == L"sheet99" &&
            integrity.reason == preview::IntegrityReason::OnDemand && integrity.total == 100,
            "cached sheet response retains structured integrity and selected content");
        cache.Evict();
        ThumbnailCache::Item first_page;
        const uint32_t pixel = 0xffffffffu;
        const auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        dc->CreateBitmap(D2D1::SizeU(1,1), &pixel, 4, props, &first_page.bitmap);
        first_page.w = first_page.h = 1;
        first_page.kind = ipc::PreviewContentKind::Bitmap;
        const auto first_key = cache.Key(L"pages.tiff", 128, 2, 3, 0);
        cache.lru_.push_front(first_key);
        first_page.lru_position = cache.lru_.begin();
        cache.items_[first_key] = std::move(first_page);
        cache.still_by_identity_[cache.Key(L"pages.tiff", 0, 2, 3)] = first_key;
        ThumbnailCache::Item failed_page;
        failed_page.failed = true;
        const auto failed_key = cache.Key(L"pages.tiff", 512, 2, 3, 1);
        cache.lru_.push_front(failed_key);
        failed_page.lru_position = cache.lru_.begin();
        cache.items_[failed_key] = std::move(failed_page);
        const auto failed = cache.Draw(dc.get(), D2D1::RectF(0,0,100,100), L"pages.tiff", 0,512,1,2,3,
            1,nullptr,nullptr,nullptr,true,nullptr,nullptr,nullptr,nullptr,nullptr,1);
        check(failed == PreviewDrawResult::Failed, "failed nonfirst page cannot borrow the cached first-page bitmap");
        cache.running_ = false;
        return ok;
    }
    static bool PropertiesRegression() {
        ThumbnailCache cache;
        // Inspect queued requests deterministically without launching a worker.
        cache.running_ = true;
        bool ok = true;
        auto check = [&](bool result, const char* name) {
            std::printf("[%s] %s\n", result ? "PASS" : "FAIL", name);
            ok &= result;
        };
        std::vector<PreviewProperty> properties;
        cache.Properties(L"preview-a.docx", 0, 1, 2, 3, properties);
        check(cache.queue_.size() == 1, "properties queue without any cover drawing");
        const auto old = cache.queue_.front();
        cache.Properties(L"preview-b.mp3", 0, 2, 3, 4, properties);
        check(cache.queue_.size() == 1 && cache.queue_.front().path == L"preview-b.mp3",
              "selection switch cancels old queued properties");
        check(!cache.StoreResult(old, {}), "late properties cannot replace new selection");
        const auto req = cache.queue_.front();
        cache.queue_.clear();
        ThumbnailCache::Item failed;
        failed.failed = failed.transient = true;
        cache.StoreResult(req, std::move(failed));
        cache.Properties(req.path, 0, 2, 3, 4, properties);
        check(cache.queue_.empty(), "transient failure waits for retry deadline");
        cache.items_.at(req.key).retry_at = 0;
        cache.Properties(req.path, 0, 2, 3, 4, properties);
        cache.Properties(req.path, 0, 2, 3, 4, properties);
        check(cache.queue_.size() == 1, "expired properties failure retries once");
        ThumbnailCache::Item good;
        good.properties.push_back({L"Title", L"correct selection"});
        cache.queue_.clear();
        cache.StoreResult(req, std::move(good));
        check(cache.Properties(req.path, 0, 2, 3, 4, properties) && properties.size() == 1,
              "successful retry returns current properties");
        cache.running_ = false;
        return ok;
    }
    static bool Run() {
        ThumbnailCache cache;
        bool ok = true;
        auto check = [&](bool result, const char* name) {
            std::printf("[%s] %s\n", result ? "PASS" : "FAIL", name);
            ok &= result;
        };
        auto request = [&](const std::wstring& key) {
            ThumbnailCache::Request req;
            req.key = key;
            req.epoch = cache.epoch_.load();
            return req;
        };
        auto insert = [&](const std::wstring& key, size_t cost = 1) {
            ThumbnailCache::Item item;
            item.cost = cost;
            cache.StoreResult(request(key), std::move(item));
        };
        for (int i = 0; i < 128; ++i) insert(std::to_wstring(i));
        cache.Touch(cache.items_.at(L"0"));
        insert(L"128");
        check(cache.items_.contains(L"0") && !cache.items_.contains(L"1"),
              "recently used preview survives capacity eviction");
        insert(L"0", 3);
        check(cache.items_.size() == 128 && cache.lru_.size() == 128 &&
              cache.cache_bytes_ == 130, "replacement has one LRU entry and exact accounting");

        auto old = request(L"old");
        cache.Evict();
        cache.pending_.insert(old.key);
        check(!cache.StoreResult(old, {}) && cache.pending_.contains(old.key) &&
              cache.items_.empty(), "old generation cannot erase replacement pending request");

        insert(L"current", 7);
        auto stale = request(L"current");
        stale.details = true;
        stale.identity = L"previous selection";
        cache.latest_details_identity_ = L"new selection";
        check(!cache.StoreResult(stale, {}) && cache.cache_bytes_ == 7 &&
              cache.items_.at(L"current").cost == 7,
              "rapid selection change preserves existing cache accounting");

        cache.Evict();
        insert(L"cold", 8 * 1024 * 1024);
        insert(L"hot", 8 * 1024 * 1024);
        cache.Touch(cache.items_.at(L"cold"));
        insert(L"new", 1);
        check(cache.items_.contains(L"cold") && !cache.items_.contains(L"hot") &&
              cache.cache_bytes_ == 8 * 1024 * 1024 + 1,
              "byte budget evicts least recently used preview");
        cache.Evict();
        for (int i = 0; i < 5; ++i) {
            auto req = request(std::to_wstring(i));
            req.identity = L"animation";
            req.details = true;
            cache.latest_details_identity_ = req.identity;
            ThumbnailCache::Item item;
            item.kind = ipc::PreviewContentKind::Bitmap;
            req.frame_index = static_cast<uint32_t>(i);
            item.frame_count = 10;
            item.frame_delay_ms = 100;
            item.frame_index = static_cast<uint32_t>(i);
            item.animation_identity = req.identity;
            item.cost = 1;
            if (i == 4) cache.Touch(cache.items_.at(L"0"));
            cache.StoreResult(req, std::move(item));
        }
        check(cache.items_.size() == 4 && cache.items_.contains(L"0") &&
              !cache.items_.contains(L"1") && cache.cache_bytes_ == 4,
              "animation keeps four most recently used frames");
        check(!cache.still_by_identity_.contains(L"animation"),
              "direct animation is not reused as a static grid fallback");
        cache.Evict();
        for (int i = 0; i < 5; ++i) {
            auto req = request(std::to_wstring(i));
            req.identity = L"paged document";
            ThumbnailCache::Item item;
            item.frame_count = 10;
            item.frame_delay_ms = 0;
            item.frame_index = static_cast<uint32_t>(i);
            item.animation_identity = req.identity;
            item.cost = 1;
            cache.StoreResult(req, std::move(item));
        }
        check(cache.items_.size() == 5 && cache.items_.contains(L"0") &&
              cache.items_.contains(L"4") && cache.cache_bytes_ == 5,
              "zero-delay document pages are not limited to four animation frames");
        cache.Reset();
        check(cache.items_.empty() && cache.lru_.empty() && cache.cache_bytes_ == 0,
              "reset clears all cache state");

        {
            // Grid budget: a screenful of 256 px thumbnails must fit without
            // the view thrashing between thumbnails and icons.
            ThumbnailCache grid(96ull * 1024ull * 1024ull, 1024);
            for (int i = 0; i < 300; ++i) {
                ThumbnailCache::Request req;
                req.key = L"g" + std::to_wstring(i);
                req.epoch = grid.epoch_.load();
                ThumbnailCache::Item item;
                item.cost = 256u * 256u * 4u;
                grid.StoreResult(req, std::move(item));
            }
            check(grid.items_.size() == 300 && grid.items_.contains(L"g0"),
                  "grid budget keeps 300 thumbnails of 256 px");
        }
        {
            cache.Evict();
            auto req = request(L"slow");
            bool retried = true;
            for (int attempt = 0; attempt < 3; ++attempt) {
                ThumbnailCache::Item item;
                item.failed = item.transient = true;
                const ULONGLONG before = GetTickCount64();
                cache.StoreResult(req, std::move(item));
                const auto& stored = cache.items_.at(L"slow");
                retried &= stored.transient && stored.retry_at > before;
            }
            check(retried, "host timeout is remembered as retryable, not as no preview");
            ThumbnailCache::Item last;
            last.failed = last.transient = true;
            cache.StoreResult(req, std::move(last));
            check(!cache.items_.at(L"slow").transient && !cache.transient_failures_.contains(L"slow"),
                  "fourth consecutive timeout settles as a failure");
            ThumbnailCache::Item good;
            good.kind = ipc::PreviewContentKind::Bitmap;
            good.cost = 1;
            auto ok_req = request(L"slow-ok");
            ok_req.identity = L"slow-identity";
            cache.StoreResult(ok_req, std::move(good));
            check(cache.still_by_identity_.contains(L"slow-identity") &&
                  cache.still_by_identity_.at(L"slow-identity") == L"slow-ok",
                  "decoded still bitmap is remembered for other sizes");
            cache.Evict();
            check(cache.still_by_identity_.empty() && cache.transient_failures_.empty(),
                  "evict clears retry and stale bookkeeping");
        }

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
        check(graphics, "create preview software rendering device");
        if (graphics) {
            cache.SetDeviceContext(context.get());
            ComPtr<ID2D1Bitmap1> target, readback;
            auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            bool ready = SUCCEEDED(context->CreateBitmap(D2D1::SizeU(100, 100),
                nullptr, 0, &props, &target));
            props.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
            ready = ready && SUCCEEDED(context->CreateBitmap(D2D1::SizeU(100, 100),
                nullptr, 0, &props, &readback));
            check(ready, "create preview render and readback targets");
            if (ready) {
                context->SetTarget(target.get());
                for (bool portrait : {false, true}) {
                    cache.Evict();
                    ThumbnailCache::Item item;
                    item.w = portrait ? 100u : 200u;
                    item.h = portrait ? 200u : 100u;
                    item.stride = item.w * 4;
                    item.pixels.assign(static_cast<size_t>(item.stride) * item.h, 255);
                    item.cost = item.pixels.size();
                    item.kind = ipc::PreviewContentKind::Bitmap;
                    cache.StoreResult(request(cache.Key(L"image", 200, 0, 0)), std::move(item));
                    for (float offset : {0.0f, 50.0f, 100.0f, 200.0f}) {
                        float x = offset, y = offset;
                        context->BeginDraw();
                        context->Clear(D2D1::ColorF(0, 0.0f));
                        const auto result = cache.Draw(context.get(), D2D1::RectF(0, 0, 100, 100),
                            L"image", 0, 200, 0, 0, 0, 1.0f, nullptr, nullptr, nullptr,
                            false, nullptr, &x, &y);
                        bool filled = result == PreviewDrawResult::Bitmap && SUCCEEDED(context->EndDraw());
                        filled = filled && SUCCEEDED(readback->CopyFromBitmap(nullptr, target.get(), nullptr));
                        D2D1_MAPPED_RECT mapped{};
                        if (filled && SUCCEEDED(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped))) {
                            for (UINT row = 0; row < 100; ++row)
                                for (UINT col = 0; col < 100; ++col)
                                    filled &= mapped.bits[row * mapped.pitch + col * 4 + 3] == 255;
                            readback->Unmap();
                        } else {
                            filled = false;
                        }
                        check(filled, "cover preview remains filled at pan boundaries");
                    }
                }
                for (bool animated : {false, true}) {
                    // A different size of an already decoded file draws the
                    // old bitmap instead of falling back to the icon.
                    cache.Evict();
                    const std::wstring identity = cache.Key(L"stale", 0, 5, 6);
                    ThumbnailCache::Item item;
                    item.w = item.h = 64;
                    item.stride = 64 * 4;
                    item.pixels.assign(static_cast<size_t>(item.stride) * item.h, 255);
                    item.cost = item.pixels.size();
                    item.kind = ipc::PreviewContentKind::Bitmap;
                    auto req = request(cache.Key(L"stale", 128, 5, 6));
                    req.identity = identity;
                    item.frame_count = animated ? 2 : 1;
                    item.frame_delay_ms = animated ? 100 : 0;
                    cache.StoreResult(req, std::move(item));
                    context->BeginDraw();
                    cache.running_ = true; // Inspect the queued resize without launching a helper.
                    const auto result = animated
                        ? cache.DrawGridThumbnail(context.get(), D2D1::RectF(0, 0, 100, 100),
                            L"stale", 0, 256, 0, 5, 6, 1.0f, false, nullptr)
                        : cache.Draw(context.get(), D2D1::RectF(0, 0, 100, 100),
                            L"stale", 0, 256, 0, 5, 6);
                    cache.running_ = false;
                    context->EndDraw();
                    bool queued = false;
                    {
                        std::lock_guard lock(cache.mutex_);
                        const std::wstring wanted = cache.Key(L"stale", 256, 5, 6);
                        queued = cache.pending_.contains(wanted) || cache.items_.contains(wanted);
                    }
                    check(result == PreviewDrawResult::Bitmap && queued,
                          animated ? "AUD-024: grid animated first frame survives new-size decode (2 frames, 100ms)"
                                   : "new size shows previous bitmap while it decodes");
                }
                context->SetTarget(nullptr);
            }
            cache.Reset();
        }
        return ok;
    }
};
}

bool RunThumbnailCacheTests() {
    return pulse::ui::ThumbnailCacheTestAccess::Run();
}

bool RunThumbnailPropertiesRegression() {
    return pulse::ui::ThumbnailCacheTestAccess::PropertiesRegression();
}

bool RunThumbnailSheetRegression() {
    return pulse::ui::ThumbnailCacheTestAccess::SheetRegression();
}

bool RunThumbnailDeadlineRegression() {
    const bool deadline = pulse::ui::ThumbnailCacheTestAccess::DeadlineAndPageRegression();
    const bool artwork = pulse::ui::ThumbnailCacheTestAccess::ArtworkCanvasRegression();
    return deadline && artwork;
}
