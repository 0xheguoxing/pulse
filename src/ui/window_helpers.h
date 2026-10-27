#pragma once

#include "FluentTokens.h"
#include "dialog_lifecycle.h"
#include "fluent_components.h"
#include "ui_compositor.h"

#include <d2d1.h>
#include <windows.h>

#include <functional>
#include <string>
#include <string_view>

namespace pulse::ui {

float ScaleDip(float scale, float value);
D2D1_RECT_F DipRect(float scale, float x, float y, float width, float height);
bool ContainsRect(const D2D1_RECT_F& rect, float x, float y);

LRESULT BorderlessHitTest(HWND hwnd, LPARAM lparam, float title_height,
                          const D2D1_RECT_F& client_buttons);
// System acrylic for dialogs (Mica Alt / Mica before 22H2); false = paint solid
// (unsupported, transparency effects off, high contrast, Windows 10).
bool ApplyBackdrop(HWND hwnd, bool dark);
bool IsAcrylicBackdrop(HWND hwnd);
// See-through cards and footer bands for dialogs over acrylic.
D2D1_COLOR_F GlassCard(bool dark);
D2D1_COLOR_F GlassSheet(bool dark);
void ApplyGlass(Theme& theme, bool dark);
// Windows 10 (or PULSE_TEST_OWN_CORNERS=1): BeginSurface clips the card to
// 8 px corners and ApplyBackdrop gives the window a shadow window of its own.
bool DrawsOwnCorners();
// Hides that shadow until the window is next hidden: for exit animations
// that shrink the card.
void SuppressWindowShadow(HWND hwnd);
// Longest prefix of `text` that fits `width` with a trailing ellipsis.
std::wstring FitTextEnd(const std::wstring& text, float width,
                        const std::function<float(std::wstring_view)>& measure);
// A path that fits `width`: keeps the drive (or UNC server) and as many last
// segments as fit ("C:\\…\\fx\\notes.txt"); non-paths get a trailing ellipsis.
std::wstring FitPathMiddle(const std::wstring& text, float width,
                           const std::function<float(std::wstring_view)>& measure);
// Modal loops: keyboard input aimed at a disabled window (the owner forced to
// the foreground) goes to `dialog` instead, which is brought back to the
// front. Plain keys arrive as WM_SYSKEY* in that state.
void RedirectStrayModalKey(MSG& message, HWND dialog);
void CenterOwnedWindow(HWND hwnd, HWND owner, int width, int height,
                       bool clamp_to_work_area = true);

void BeginSurface(Compositor& compositor, fluent::Painter& painter,
                  const Theme& theme, bool dark, bool high_contrast,
                  bool backdrop_enabled, float scale);
void EndSurface(Compositor& compositor);

} // namespace pulse::ui
