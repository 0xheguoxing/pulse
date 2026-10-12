#pragma once
#include "fluent_components.h"

#include <string>
#include <vector>

namespace pulse::ui {
// Multi-window M7i (toast redesign A): what a capsule notification shows on
// its left.
enum class SnackIcon { Tab, Window, Undo, Merge, Suggest };
// A capsule button: posts `message` to the owner and closes it (0: closes).
struct SnackAction {
    std::wstring label;
    UINT message = 0;
};

class NotificationToast {
public:
    void Show(HWND owner, std::wstring title, std::wstring message, bool persistent = true, UINT action_message = 0);
    void ShowError(HWND owner, std::wstring title, std::wstring message);
    // A one-line capsule at the bottom center (M7i): an icon, what happened
    // (`text`) and a quieter `detail`, up to two buttons and a shortcut chip.
    // It goes after `duration_ms` (paused under the cursor), with a line that
    // runs down; only the buttons act, the close button and Esc dismiss.
    void ShowSnack(HWND owner, SnackIcon icon, std::wstring text, std::wstring detail,
                   std::vector<SnackAction> actions, std::wstring key = {}, UINT duration_ms = 6000);
    bool HandleMessage(HWND owner, UINT message, WPARAM wparam, LPARAM lparam);
    void Draw(Compositor& compositor, const Theme& theme, float scale, bool high_contrast);
    bool IsVisible() const noexcept { return visible_; }
    D2D1_RECT_F Bounds() const noexcept { return card_; }
    // Hides the toast when clicking it would post `action_message`: what it
    // offers was done another way or no longer applies.
    void DismissFor(HWND owner, UINT action_message) {
        if (!visible_ || !action_message) return;
        bool offered = action_message_ == action_message;
        for (const SnackAction& action : actions_) offered = offered || action.message == action_message;
        if (offered) Dismiss(owner);
    }
private:
    void Dismiss(HWND owner);
    void DrawSnack(Compositor& compositor, const Theme& theme, float scale, bool high_contrast);
    static constexpr UINT_PTR timer = 0x50554C54;
    fluent::Painter painter_;
    std::wstring title_, message_;
    UINT action_message_ = 0;
    D2D1_RECT_F card_{}, close_{};
    ULONGLONG born_ = 0, last_tick_ = 0, remaining_ = 0;
    bool visible_ = false, hovered_ = false, pressed_close_ = false, pressed_card_ = false, persistent_ = true;
    bool top_center_ = false;
    fluent::InfoBarKind kind_ = fluent::InfoBarKind::Warning;
    // The capsule (M7i).
    bool snack_ = false;
    SnackIcon icon_ = SnackIcon::Tab;
    std::wstring key_;
    std::vector<SnackAction> actions_;
    std::vector<D2D1_RECT_F> action_rects_;
    int pressed_action_ = -1;
    ULONGLONG duration_ = 6000;
};
}
