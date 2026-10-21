#pragma once

#include <d2d1.h>
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace pulse::ui {

// The icon tile and its colour. Auto picks Danger for `danger` specs and
// Question otherwise.
enum class ConfirmTone { Auto, Info, Question, Warning, Danger };

enum class ConfirmChoice { Cancel, Confirm, Secondary };

struct ConfirmDialogSpec {
    std::wstring title;
    std::wstring message;
    std::wstring confirm_text;
    std::wstring cancel_text;
    bool danger = false;
    // Optional middle button, e.g. "No" in a Yes / No / Cancel question.
    std::wstring secondary_text;
    // Optional list shown in a card under the message (one row per item).
    std::vector<std::wstring> items;
    // Optional: how many items `items` stands for when only the first ones
    // are listed (0: items.size()). The card then ends with "and N more".
    size_t items_total = 0;
    std::wstring item_glyph;
    // Optional small print under the card.
    std::wstring note;
    ConfirmTone tone = ConfirmTone::Auto;
    std::wstring glyph;  // overrides the tone's icon
    // The button Enter presses at first. Destructive questions start on
    // Cancel, like MB_DEFBUTTON2, so Enter alone never confirms them.
    ConfirmChoice default_choice = ConfirmChoice::Confirm;
    // Information only: just the confirm button (Esc / close still dismiss).
    bool single_button = false;
    // Optional: called with the dialog once it shows and with null when it
    // goes, so a question asked for someone else (an AI client) can be
    // closed from outside with WM_CLOSE, which answers Cancel.
    std::function<void(HWND)> on_open;
};

ConfirmChoice ShowConfirmDialogEx(HWND owner, const ConfirmDialogSpec& spec, bool dark,
                                  D2D1_COLOR_F accent);

// True when the user picked the confirm button.
bool ShowConfirmDialog(HWND owner, const ConfirmDialogSpec& spec, bool dark,
                       D2D1_COLOR_F accent);

} // namespace pulse::ui