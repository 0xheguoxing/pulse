// app_hover_preview.h - The hover preview card (Settings > 快速预览 >
// 预览方式 = 悬停预览): resting the pointer on a file shows a small card with
// its preview and key facts; Space opens that file in quick look.
#pragma once
#include <windows.h>

namespace pulse {
struct AppState;
namespace ui { struct WindowViewModel; struct HoverPreviewView; }

constexpr UINT_PTR kTimerHoverPreview = 0x4850;  // 'HP': the dwell before the card

// The hovered row changed (app_hover.cpp): hides the card and starts the
// dwell for the new row, sooner when moving on from a card that was shown.
void NoteHoverPreviewRow(AppState& s);
// Hides the card and cancels a pending dwell.
void HideHoverPreview(AppState& s);
// Dwell timer, and dismissal on clicks, wheel, keys and focus loss. Returns
// true only for messages it consumes (its own timer).
bool HoverPreviewMessage(AppState& s, HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
// Space while the card shows: its file is selected and opens in quick look.
// False when no card is shown (Space then previews the selection as before).
bool OpenHoverPreviewInQuickLook(AppState& s);
// This frame's card for the renderer; a card whose row is gone or scrolled
// away is dropped.
// GUI verification (--shot-hover-preview N): shows the card over the Nth file
// (0-based, folders skipped) of the focused pane, pointer at the name.
bool StageHoverPreviewShot(AppState& s, int nth_file);
void FillHoverPreviewView(AppState& s, const ui::WindowViewModel& vm, ui::HoverPreviewView& view);
} // namespace pulse
