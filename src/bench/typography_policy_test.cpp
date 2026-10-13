#include "../ui/typography.h"
#include "../ui/fluent_components.h"
#include "../ui/fluent_menu.h"
#include "../ui/edit_host.h"
#include "../common/localization.h"
#include <cmath>
#include <cstdio>
#include <string>

namespace pulse::ui::fluent {
struct PainterTestPeer {
    static IDWriteTextFormat* Body(const Painter& painter) { return painter.BodyFormat(); }
    static IDWriteTextFormat* Nav(const Painter& painter) { return painter.NavFormat(); }
};
}

namespace pulse::ui {
struct FluentMenuTestPeer {
    static bool CheckSnapshotReplacement() {
        Compositor compositor;
        FluentMenu menu;
        menu.compositor_ = &compositor;
        menu.open_ = true;
        FluentMenuItem parent{1, L"Parent"};
        parent.children = {{10, L"A"}, {11, L"B"}};
        menu.model_.SetItems({parent});
        menu.sub_model_.SetItems(parent.children);
        menu.sub_parent_row_ = 0;
        parent.children[0].command = 11;
        parent.children[1].command = 10;
        bool ok = !menu.ReplaceItems({parent});
        ok &= menu.model_.At(0)->children[0].command == 10;
        ok &= menu.sub_model_.At(0)->command == 10;
        parent.children[0].command = 20;
        parent.children[1].command = 21;
        ok &= !menu.ReplaceItems({parent});
        std::swap(parent.children[0], parent.children[1]);
        ok &= !menu.ReplaceItems({parent});
        std::swap(parent.children[0], parent.children[1]);
        menu.sub_parent_row_ = -1;
        ok &= menu.ReplaceItems({parent});
        menu.sub_model_.SetItems(menu.model_.At(0)->children);
        ok &= menu.sub_model_.At(0)->command == 20;
        menu.open_ = false;
        return ok;
    }
    static HWND Filter(FluentMenu& menu) {
        menu.filter_fn_ = [](const std::wstring&) {
            return std::vector<FluentMenuItem>{{1, L"A long reusable menu command label"}};
        };
        menu.model_.SetItems(menu.filter_fn_(L""));
        menu.model_.Layout(menu.compositor_->DwriteFactory(), menu.scale_);
        return menu.EnsureFilterEdit() ? menu.edit_ : nullptr;
    }
    static void Show(FluentMenu& menu) {
        SetWindowPos(menu.hwnd_, nullptr, -30000, -30000, 500, 160,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetWindowPos(menu.edit_, nullptr, 10, 10, 400, 32,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    static void Reopen(FluentMenu& menu) {
        ShowWindow(menu.hwnd_, SW_HIDE);
        menu.EnsureFilterEdit();
        Show(menu);
    }
};
}

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
}

int wmain(int argc, wchar_t** argv) {
    using namespace pulse::ui;
    if (argc == 2 && std::wstring(argv[1]) == L"--menu-identity") {
        Check(FluentMenuTestPeer::CheckSnapshotReplacement(),
            "open flyout rejects swapped/shifted/reordered snapshots; reopen uses accepted IDs");
        return failures ? 1 : 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HWND foreground = GetForegroundWindow();
    HWND owner = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE, L"STATIC", L"",
        WS_POPUP, -30000, -30000, 700, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    {
        Compositor compositor;
        Check(owner && compositor.Init(owner), "create typography test compositor");
        if (!compositor.DwriteFactory()) return 1;
        fluent::Painter painter(&compositor);
        const auto theme = MakeTheme(true, D2D1::ColorF(0.2f, 0.4f, 0.8f));
        FluentMenuModel model;
        model.SetItems({{1, L"Reusable menu command"}});
        typography::SetUiFontScale(100);
        pulse::l10n::SetLanguage(L"zh-CN");
        typography::InvalidateCaches();
        Check(painter.BeginFrame(theme), "warm painter frame initializes its own font cache");
        compositor.RecreateTextFormats(1.0f);
        Check(compositor.FileNameFormat() && fluent::PainterTestPeer::Nav(painter) &&
            std::abs(fluent::PainterTestPeer::Nav(painter)->GetFontSize() -
                     compositor.FileNameFormat()->GetFontSize()) < 0.01f &&
            std::abs(compositor.FileNameFormat()->GetFontSize() - FluentMenuModel::kLabelDip) < 0.01f,
            "menu labels use the file list name size");
        const float original_size = fluent::PainterTestPeer::Body(painter)->GetFontSize();
        model.Layout(compositor.DwriteFactory(), 1.0f);
        const int original_width = model.WidthPx();
        typography::SetUiFontScale(125);
        Check(painter.BeginFrame(theme), "scaled painter frame refreshes its own font cache");
        model.Layout(compositor.DwriteFactory(), 1.0f);
        std::printf("scale values: body %.2f -> %.2f, menu %d -> %d, truncated=%d\n", original_size,
            fluent::PainterTestPeer::Body(painter)->GetFontSize(), original_width, model.WidthPx(), model.Truncated(0));
        Check(std::abs(fluent::PainterTestPeer::Body(painter)->GetFontSize() - original_size * 1.25f) < 0.01f &&
            model.WidthPx() > original_width, "warm painter and menu layout follow font scale without manual painter invalidation");
        pulse::l10n::SetLanguage(L"zh-TW");
        typography::InvalidateCaches(); // same language-change entry point as the application.
        Check(painter.BeginFrame(theme), "language painter frame refreshes its own font cache");
        wchar_t locale[32]{};
        fluent::PainterTestPeer::Body(painter)->GetLocaleName(locale, 32);
        wchar_t family[128]{};
        fluent::PainterTestPeer::Body(painter)->GetFontFamilyName(family, 128);
        IDWriteTextFormat* expected = nullptr;
        typography::CreateTextFormat(compositor.DwriteFactory(), {}, &expected);
        wchar_t expected_family[128]{};
        if (expected) expected->GetFontFamilyName(expected_family, 128);
        std::wprintf(L"language values: locale=%s family=%s expected=%s generation=%llu\n", locale, family,
            expected_family, static_cast<unsigned long long>(typography::Generation()));
        Check(CompareStringOrdinal(locale, -1, L"zh-TW", -1, TRUE) == CSTR_EQUAL &&
            std::wstring(family) == expected_family,
            "warm painter follows traditional Chinese locale and family");
        if (expected) expected->Release();
        for (int percent : {90, 100, 112, 125}) {
            typography::SetUiFontScale(percent);
            const float interface_scale = static_cast<float>(percent) / 100.0f;
            for (float dpi : {1.0f, 1.5f}) {
                HFONT font = typography::CreateEditFont(dpi);
                LOGFONTW details{};
                const int height = static_cast<int>(std::lround(14.0f * dpi * interface_scale));
                Check(font && GetObjectW(font, sizeof(details), &details) && details.lfHeight == -height,
                    "native edit font follows interface scale and DPI matrix");
                IDWriteTextFormat* format = nullptr;
                typography::CreateTextFormat(compositor.DwriteFactory(), {typography::FontRole::Text, 14.0f * dpi}, &format);
                Check(format && std::abs(format->GetFontSize() - 14.0f * dpi * interface_scale) < 0.01f,
                    "DirectWrite font follows the same interface scale and DPI matrix");
                if (font) DeleteObject(font);
                if (format) format->Release();
            }
        }
        typography::SetUiFontScale(100);
        FluentMenu menu;
        Check(menu.Create(owner, &compositor, 1.0f), "create real reusable filter menu");
        HWND edit = FluentMenuTestPeer::Filter(menu);
        Check(edit != nullptr, "create real filter editor through menu lifecycle");
        if (edit) {
            typography::SetUiFontScale(125);
            Check(FluentMenuTestPeer::Filter(menu) == edit, "filter editor is reused after generation change");
            LOGFONTW font{};
            HFONT handle = reinterpret_cast<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0));
            Check(GetObjectW(handle, sizeof(font), &font) && font.lfHeight == -18,
                "reused filter native font refreshes to 125 percent");
            FluentMenuTestPeer::Show(menu);
            SetEnvironmentVariableW(L"PULSE_TEST_LUMATEXT_PRESENT_FAILURE", L"1");
            SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"fallback 中文"));
            SetEnvironmentVariableW(L"PULSE_TEST_LUMATEXT_PRESENT_FAILURE", nullptr);
            Check(GetPropW(edit, L"Pulse.NativeEditFallback") != nullptr,
                "real menu presentation failure selects persistent native fallback");
            SendMessageW(edit, EM_SETSEL, 0, 8);
            SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"native"));
            wchar_t text[64]{};
            GetWindowTextW(edit, text, 64);
            Check(std::wstring(text) == L"native 中文", "real menu native fallback preserves Unicode selection and input");
            SendMessageW(edit, WM_UNDO, 0, 0);
            GetWindowTextW(edit, text, 64);
            Check(std::wstring(text) == L"fallback 中文", "real menu native fallback preserves undo");
            FluentMenuTestPeer::Reopen(menu);
            Check(GetPropW(edit, L"Pulse.NativeEditFallback") != nullptr,
                "reopening the menu preserves the working native fallback");
        }
        typography::SetUiFontScale(100);
    }
    DestroyWindow(owner);
    Check(GetForegroundWindow() == foreground, "typography tests preserve foreground window");
    CoUninitialize();
    return failures ? 1 : 0;
}
