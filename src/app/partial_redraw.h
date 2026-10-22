// partial_redraw.h - The client pixels a window asked to repaint on their own
// (plans/Pulse_局部重绘方案.md). A list hover change marks its rows here; any
// other invalidation, InvalidateRect(hwnd, nullptr) above all, grows the
// update region past them and the paint becomes a whole frame again.
#pragma once
#include <windows.h>
#include <d2d1.h>
#include <cmath>
#include <unordered_map>

namespace pulse::app {

// Per UI thread: each window's messages arrive on the thread that owns it.
inline std::unordered_map<HWND, RECT>& PartialDamageMap() {
    static thread_local std::unordered_map<HWND, RECT> map;
    return map;
}

// Client px covering `r`, whole pixels.
inline RECT DamagePixels(const D2D1_RECT_F& r) {
    return RECT{static_cast<LONG>(std::floor(r.left)), static_cast<LONG>(std::floor(r.top)),
                static_cast<LONG>(std::ceil(r.right)), static_cast<LONG>(std::ceil(r.bottom))};
}

inline void InvalidatePartial(HWND hwnd, const RECT& rect) {
    if (!hwnd || IsRectEmpty(&rect)) return;
    auto& map = PartialDamageMap();
    const auto it = map.find(hwnd);
    if (it == map.end()) map.emplace(hwnd, rect);
    else UnionRect(&it->second, &it->second, &rect);
    InvalidateRect(hwnd, &rect, FALSE);
}

// WM_PAINT, after BeginPaint: true when the whole update region (`paint`)
// lies in what was marked, which is then the frame's damage.
inline bool TakePartialDamage(HWND hwnd, const RECT& paint, RECT* out) {
    auto& map = PartialDamageMap();
    const auto it = map.find(hwnd);
    if (it == map.end()) return false;
    const RECT damage = it->second;
    map.erase(it);
    RECT both{};
    UnionRect(&both, &damage, &paint);
    if (!EqualRect(&both, &damage)) return false;
    *out = damage;
    return true;
}

} // namespace pulse::app
