#pragma once
// cloud_swap.h - the design's .cbadge.swap / .st.swap ("newf") for cloud state
// icons: when a row's icon changes (保留 -> 同步中 -> 始终保留, 释放空间 ->
// 仅在云端) it pops in from 0.6 over 500 ms on cubic-bezier(.3, 1.5, .5, 1),
// opacity riding the same curve. Header-only and clock-free so the bench can
// drive it; the renderer keeps one tracker per process keyed by row.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace pulse::ui {

// y of the CSS cubic-bezier(x1, y1, x2, y2) at progress x (Newton, then bisection).
inline float CubicBezierAt(float x1, float y1, float x2, float y2, float x) noexcept {
    x = std::clamp(x, 0.0f, 1.0f);
    const auto bez = [](float a, float b, float t) {
        const float u = 1.0f - t;
        return 3.0f * u * u * t * a + 3.0f * u * t * t * b + t * t * t;
    };
    float t = x;
    for (int i = 0; i < 6; ++i) {
        const float u = 1.0f - t;
        const float dx = 3.0f * u * u * x1 + 6.0f * u * t * (x2 - x1) + 3.0f * t * t * (1.0f - x2);
        if (std::fabs(dx) < 1e-5f) break;
        t = std::clamp(t - (bez(x1, x2, t) - x) / dx, 0.0f, 1.0f);
    }
    float lo = 0.0f, hi = 1.0f;
    for (int i = 0; i < 20 && std::fabs(bez(x1, x2, t) - x) > 1e-4f; ++i) {
        (bez(x1, x2, t) < x ? lo : hi) = t;
        t = (lo + hi) * 0.5f;
    }
    return bez(y1, y2, t);
}

struct CloudSwapPose {
    float scale = 1.0f;
    float alpha = 1.0f;
    bool running = false; // ask for another frame
};

class CloudSwapTracker {
public:
    static constexpr uint64_t kSwapMs = 500;
    static constexpr size_t kCap = 4096;

    // |key| names a drawn icon (row + slot + window), |glyph| what it shows now.
    // The first sighting of a key never animates; |animate| false (system
    // animations off) settles a change at once.
    CloudSwapPose Pose(uint64_t key, wchar_t glyph, uint64_t now_ms, bool animate) {
        if (seen_.size() > kCap) seen_.clear();
        auto [it, fresh] = seen_.try_emplace(key, Seen{glyph, 0});
        if (!fresh && it->second.glyph != glyph) it->second = {glyph, animate ? std::max<uint64_t>(now_ms, 1) : 0};
        CloudSwapPose pose;
        const uint64_t start = it->second.start;
        if (start == 0 || now_ms < start || now_ms - start >= kSwapMs) return pose;
        const float y = CubicBezierAt(0.3f, 1.5f, 0.5f, 1.0f, static_cast<float>(now_ms - start) / kSwapMs);
        pose.scale = 0.6f + 0.4f * y;
        pose.alpha = std::clamp(y, 0.0f, 1.0f);
        pose.running = true;
        return pose;
    }
    size_t Size() const noexcept { return seen_.size(); }

private:
    struct Seen {
        wchar_t glyph;
        uint64_t start;
    };
    std::unordered_map<uint64_t, Seen> seen_;
};

} // namespace pulse::ui
