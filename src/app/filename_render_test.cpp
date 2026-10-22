#ifdef PULSE_WITH_SELFTEST
#include "name_highlight_ui_test.h"
#include "../common/windows_compat.h"
#include "../ui/ui_renderer_internal.h"
#include "../ui/typography.h"
#include <filesystem>
#include <fstream>

namespace pulse::ui {
struct FilenameRenderTest {
    static bool Run() {
        const std::filesystem::path root = L"bench_data/filename-render";
        std::filesystem::create_directories(root);
        std::ofstream log(root / "results.log");
        int failures = 0;
        auto check = [&](bool pass, const char* label) {
            log << (pass ? "[PASS] " : "[FAIL] ") << label << std::endl;
            failures += !pass;
        };
        const HWND hwnd = CreateWindowExW(0, L"STATIC", L"Filename rendering regression",
            WS_OVERLAPPEDWINDOW, 0, 0, 900, 650, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        {
            Compositor compositor;
            if (!hwnd || !compositor.Init(hwnd)) {
                if (hwnd) DestroyWindow(hwnd);
                check(false, "initialize isolated renderer");
                return false;
            }
            MainRenderer renderer;
            renderer.SetCompositor(&compositor);
            log << "LumaText=" << compositor.LumaTextEnabled() << std::endl;
            const wchar_t* names[] = {L"Drawing1.dwg", L"Drawing2.dwg", L"图纸设计2.dwg",
                L"office-final.pdf", L"README", L".gitignore", L"archive.tar.gz",
                L"Drawing1234567890-very-long-file-name.dwg"};
            for (const wchar_t* language : {L"zh-CN", L"en-US"}) {
                l10n::Initialize(GetModuleHandleW(nullptr), language);
                for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
                    compositor.Resize(static_cast<UINT>(900 * scale), static_cast<UINT>(650 * scale));
                    compositor.RecreateTextFormats(scale);
                    renderer.SetScale(scale);
                    auto* dc = compositor.Dc();
                    auto* fmt = compositor.FileNameFormat();
                    auto* factory = compositor.DwriteFactory();
                    for (bool dark : {false, true}) {
                        const auto theme = MakeTheme(dark, HexColor(0x0078D4));
                        ComPtr<ID2D1SolidColorBrush> normal, dim;
                        dc->CreateSolidColorBrush(theme.text, &normal);
                        dc->CreateSolidColorBrush(WithAlpha(theme.text, dark ? .58f : .62f), &dim);
                        dc->BeginDraw();
                        dc->Clear(theme.bg);
                        for (int row = 0; row < 16; ++row) {
                            const std::wstring name = names[row % 8];
                            const float budget = (row < 8 ? 380.0f : 90.0f) * scale;
                            const float y = (10.0f + row * 39.0f) * scale;
                            const float height = 34.0f * scale;
                            const std::vector<NameMatchRange> matches = row == 11
                                ? std::vector<NameMatchRange>{{0, 3}} : std::vector<NameMatchRange>{};
                            const auto shown = FitHighlightedFileName(&compositor, factory, fmt, name, budget, matches, scale);
                            const float name_width = std::min(budget, MeasureLayoutText(&compositor, factory, fmt, shown) +
                                HighlightPaddingWidth(name, shown, matches, scale));
                            if (row < 8) check(shown == name, "wide filename retains every character before drawing");
                            const size_t dot = shown.find_last_of(L'.');
                            const bool has_extension = dot != std::wstring::npos && dot > 0 &&
                                shown.size() - dot <= 8 && shown.find(L'\u2026', dot) == std::wstring::npos;
                            const float phase = row % 2 ? 0.375f : 0.0f;
                            const bool reported = renderer.DrawTruncatedName(
                                name, 20 * scale + phase, y, name_width, height, theme, false, matches, true);
                            // The row tooltip relies on this (B站 #15).
                            check(reported == (shown != name), "drawn name reports exactly when it was shortened");
                            // Independent reference: shape the complete visible name once,
                            // then color the extension by UTF-16 range. No substring widths.
                            const auto rc = D2D1::RectF(460 * scale + phase, y, 460 * scale + phase + name_width, y + height);
                            const auto visible = VisibleNameMatchRanges(name, shown, matches);
                            // LumaText colours the extension of its single shaped line by
                            // clipping at the caret of the dot in that same layout.
                            const bool split_drawn = has_extension && visible.empty() &&
                                compositor.DrawLumaTextSplit(shown, fmt, rc, theme.text,
                                    static_cast<std::uint32_t>(dot), dim->GetColor(), theme.bg);
                            if (!split_drawn && (has_extension || !visible.empty())) {
                                ComPtr<IDWriteTextLayout> layout;
                                factory->CreateTextLayout(shown.c_str(), static_cast<UINT32>(shown.size()), fmt,
                                    name_width, height, &layout);
                                if (!layout.get()) { check(false, "create complete filename reference"); continue; }
                                layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                                layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                                layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                                if (has_extension) layout->SetDrawingEffect(dim.get(),
                                    {static_cast<UINT32>(dot), static_cast<UINT32>(shown.size() - dot)});
                                DrawNameHighlightBackground(&compositor, layout.get(), {rc.left, rc.top}, rc, visible, theme, scale);
                                dc->DrawTextLayout({rc.left, rc.top}, layout.get(), normal.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                            } else if (!split_drawn && !compositor.DrawLumaText(shown, fmt, rc, theme.text, theme.bg)) {
                                dc->DrawText(shown.c_str(), static_cast<UINT32>(shown.size()), fmt, rc,
                                    normal.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                            }
                        }
                        check(SUCCEEDED(dc->EndDraw()), "draw production and full-name reference");
                        const auto file = root / (std::wstring(language) + L"-" + std::to_wstring(static_cast<int>(scale * 100)) +
                            (dark ? L"-dark.png" : L"-light.png"));
                        check(compositor.SaveSnapshot(file.c_str()), "save production/reference comparison");
                        ComPtr<ID2D1Image> image;
                        dc->GetTarget(&image);
                        ComPtr<ID2D1Bitmap1> target, readback;
                        HRESULT hr = image->QueryInterface(__uuidof(ID2D1Bitmap1), reinterpret_cast<void**>(&target.p));
                        if (SUCCEEDED(hr)) {
                            auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                target->GetPixelFormat(), 96, 96);
                            hr = dc->CreateBitmap(target->GetPixelSize(), nullptr, 0, &props, &readback);
                        }
                        if (SUCCEEDED(hr)) hr = readback->CopyFromBitmap(nullptr, target.get(), nullptr);
                        D2D1_MAPPED_RECT mapped{};
                        if (SUCCEEDED(hr)) hr = readback->Map(D2D1_MAP_OPTIONS_READ, &mapped);
                        check(SUCCEEDED(hr), "read rendered glyph pixels");
                        if (SUCCEEDED(hr)) {
                            for (int row = 0; row < 16; ++row) {
                                size_t differences = 0, ink = 0;
                                const UINT left = static_cast<UINT>(20 * scale), right = static_cast<UINT>(460 * scale);
                                const UINT top = static_cast<UINT>((10 + row * 39) * scale);
                                for (UINT y = top; y < static_cast<UINT>((44 + row * 39) * scale); ++y) {
                                    const auto* pixels = reinterpret_cast<const uint32_t*>(mapped.bits + y * mapped.pitch);
                                    for (UINT x = 0; x < static_cast<UINT>(390 * scale); ++x) {
                                        differences += pixels[left + x] != pixels[right + x];
                                        ink += pixels[left + x] != pixels[0];
                                    }
                                }
                                log << "[INFO] scale=" << scale << " dark=" << dark << " row=" << row
                                    << " different_pixels=" << differences << " ink_pixels=" << ink << std::endl;
                                check(ink > 0 && differences == 0, "production preserves full shaped glyphs and extension color");
                            }
                            readback->Unmap();
                        }
                    }
                }
            }
            // Icon views wrap the name over lines; cut-off lines count as shortened.
            {
                const float scale = 1.0f;
                compositor.Resize(900, 650);
                compositor.RecreateTextFormats(scale);
                renderer.SetScale(scale);
                const auto theme = MakeTheme(false, HexColor(0x0078D4));
                auto* dc = compositor.Dc();
                bool short_cut = true, long_cut = false;
                dc->BeginDraw();
                dc->Clear(theme.bg);
                renderer.DrawCenteredIconName(L"README", D2D1::RectF(20, 20, 220, 60), theme.text, theme, {}, &short_cut);
                renderer.DrawCenteredIconName(std::wstring(L"Drawing1234567890-very-long-file-name-") + std::wstring(60, L'x') + L".dwg",
                    D2D1::RectF(20, 80, 120, 120), theme.text, theme, {}, &long_cut);
                check(SUCCEEDED(dc->EndDraw()), "draw icon-view names");
                check(!short_cut, "icon-view name that fits is not reported as shortened");
                check(long_cut, "icon-view name cut off after its wrapped lines is reported as shortened");
            }
        }
        DestroyWindow(hwnd);
        log << "failures=" << failures << std::endl;
        return failures == 0;
    }
};
}
namespace pulse::app {
bool RunFilenameRenderTest() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return false;
    compat::EnableDpiAwareness();
    const bool ok = ui::FilenameRenderTest::Run();
    CoUninitialize();
    return ok;
}
}
#endif
