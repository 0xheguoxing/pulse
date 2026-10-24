#include "../ui/batch_rename_dialog.h"
#include "../ui/typography.h"
#include <windows.h>
#include <commctrl.h>
#include <cstdint>
#include <climits>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {
struct Upload {
    unsigned count = 0;
    bool ink = false;
    int width = 0, height = 0, ink_top = 0, ink_bottom = 0;
    std::wstring text;
};
std::map<HWND, Upload> uploads;
int failures = 0;
int shown = 0;
int initial_missing = 0;
float test_scale = 1.75f;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
BOOL CALLBACK Collect(HWND hwnd, LPARAM param) {
    wchar_t name[32]{};
    GetClassNameW(hwnd, name, ARRAYSIZE(name));
    if (_wcsicmp(name, L"EDIT") == 0)
        reinterpret_cast<std::vector<HWND>*>(param)->push_back(hwnd);
    return TRUE;
}
}
namespace pulse::ui {
float BatchRenameScaleForTest() { return test_scale; }
void ObserveEditUploadForTest(HWND hwnd, const void* bits, int width, int height) {
    auto& upload = uploads[hwnd];
    ++upload.count;
    wchar_t text[128]{};
    GetWindowTextW(hwnd, text, ARRAYSIZE(text));
    upload.text = text;
    const auto* pixels = static_cast<const std::uint32_t*>(bits);
    upload.width = width;
    upload.height = height;
    upload.ink_top = height;
    upload.ink_bottom = -1;
    int changed = 0;
    for (int i = 1; i < width * height; ++i) {
        if ((pixels[i] & 0xffffff) != (pixels[0] & 0xffffff)) {
            ++changed;
            const int row = i / width;
            if (row < upload.ink_top) upload.ink_top = row;
            if (row > upload.ink_bottom) upload.ink_bottom = row;
        }
    }
    upload.ink = changed > 20;
}
void BatchRenameShownForTest(HWND dialog) {
    ++shown;
    std::vector<HWND> edits;
    EnumChildWindows(dialog, Collect, reinterpret_cast<LPARAM>(&edits));
    Check(edits.size() >= 3, "real batch dialog creates the AI field and the default step fields");
    // Identify by geometry: the hidden numeric field retains the initial '1'.
    HWND number = nullptr, pattern = nullptr;
    std::vector<HWND> visible;
    for (HWND edit : edits) {
        DWORD flags = 0;
        BYTE alpha = 0;
        Check(GetLayeredWindowAttributes(edit, nullptr, &alpha, &flags) &&
            (flags & LWA_ALPHA) && alpha == 255,
            "batch field uses the same redirected surface as address/search editors");
        if (!IsWindowVisible(edit)) number = edit;
        else visible.push_back(edit);
    }
    Check(visible.size() == 2 && number, "replace step fields visible; AI and custom fields wait hidden");
    LONG bottom = LONG_MIN;
    for (HWND edit : visible) {
        RECT rect{}; GetWindowRect(edit, &rect);
        if (rect.top > bottom) { bottom = rect.top; pattern = edit; }
        if (uploads[edit].count == 0) ++initial_missing;
        std::printf("[DIAG initial-empty] uploads=%u (separate from entered-text result)\n", uploads[edit].count);
        const unsigned before = uploads[edit].count;
        SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"Typed 中文"));
        Check(uploads[edit].count > before && uploads[edit].ink && uploads[edit].text == L"Typed 中文",
            "real host text change uploads visible glyph pixels");
        const auto& upload = uploads[edit];
        RECT client{}; GetClientRect(edit, &client);
        std::printf("[DIAG glyphs] surface=%dx%d client=%ldx%ld ink_rows=%d..%d scale=%.2f\n",
            upload.width, upload.height, client.right, client.bottom,
            upload.ink_top, upload.ink_bottom, test_scale);
        Check(upload.width == client.right && upload.height == client.bottom &&
            upload.ink_top > 0 && upload.ink_bottom < upload.height - 1,
            "uploaded text fits actual editor height without touching top or bottom edge");
        const int length = GetWindowTextLengthW(edit);
        SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(length), static_cast<LPARAM>(length));
        DWORD selection_start = 0, selection_end = 0;
        SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&selection_start),
            reinterpret_cast<LPARAM>(&selection_end));
        Check(selection_start == static_cast<DWORD>(length) && selection_end == static_cast<DWORD>(length),
            "WM_CHAR fixture places collapsed selection at actual text end");
        const unsigned before_char = uploads[edit].count;
        SendMessageW(edit, WM_CHAR, L'Z', 1);
        const bool appended = uploads[edit].count > before_char && uploads[edit].ink &&
            uploads[edit].text == L"Typed 中文Z";
        if (!appended) {
            wchar_t native_text[128]{};
            GetWindowTextW(edit, native_text, ARRAYSIZE(native_text));
            std::printf("[DIAG WM_CHAR] selection=%lu..%lu length=%d uploads=%u->%u ink=%d native='%ls' uploaded='%ls'\n",
                selection_start, selection_end, length, before_char, uploads[edit].count,
                uploads[edit].ink ? 1 : 0, native_text, uploads[edit].text.c_str());
        }
        Check(appended,
            "real host WM_CHAR appends text and uploads glyph pixels");
    }
    // The AI sentence field shows a drawn placeholder while empty; typing
    // into it must bring the real editor up and upload its glyphs.
    if (HWND ai = GetDlgItem(dialog, 100)) {
        Check(!IsWindowVisible(ai), "empty AI field shows the drawn placeholder instead of the editor");
        SendMessageW(ai, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"去掉 (1)"));
        UpdateWindow(dialog);
        RedrawWindow(dialog, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        Check(IsWindowVisible(ai) && uploads[ai].count > 0 && uploads[ai].ink,
            "typed AI sentence brings the editor up and uploads glyph pixels");
    } else {
        Check(false, "AI sentence field exists");
    }
    (void)pattern;
    (void)number;
    PostMessageW(dialog, WM_CLOSE, 0, 0);
}
}
int wmain() {
    // Never switch the interactive desktop or write fixture/user files.
    HDESK original = GetThreadDesktop(GetCurrentThreadId());
    const std::wstring name = L"PulseBatchPresent_" + std::to_wstring(GetCurrentProcessId());
    HDESK isolated = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!isolated || !SetThreadDesktop(isolated)) {
        std::printf("[FAIL] isolated desktop unavailable: %lu\n", GetLastError());
        if (isolated) CloseDesktop(isolated);
        return 1;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    for (const auto mode : {pulse::ui::typography::TextRenderMode::Auto,
            pulse::ui::typography::TextRenderMode::Sharp, pulse::ui::typography::TextRenderMode::Smooth}) {
        pulse::ui::typography::SetTextRenderMode(mode);
        for (const bool dark : {false, true}) {
            std::printf("[CASE] simulated_scale=1.75 dark=%d mode=%d; Windows DPI is unchanged\n",
                dark ? 1 : 0, static_cast<int>(mode));
            uploads.clear();
            const auto result = pulse::ui::ShowBatchRenameDialog(nullptr, {}, dark, D2D1::ColorF(0, 0.5f, 1));
            Check(!result.accepted, "test closes real dialog without applying rename");
        }
    }
    Check(shown == 6, "all six mode/theme Show paths reached presentation checkpoint");
    std::printf("[DIAG initial-empty] %d fields had no pre-input upload; not counted as entered-text failure\n",
        initial_missing);
    CoUninitialize();
    SetThreadDesktop(original);
    CloseDesktop(isolated);
    return failures ? 1 : 0;
}
