// ui_motion.h — Small motion primitives for the immediate-mode renderer.
//
// Motion only confirms an action or explains where something went; it never
// delays input (hit-testing always uses the target layout) and it honours the
// Windows "Animation effects" switch (SPI_GETCLIENTAREAANIMATION).
#pragma once
#include <windows.h>
#include <d2d1.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace pulse::ui::motion {

// Settings > Accessibility > Visual effects > Animation effects. Queried only
// when a transition starts, never per frame.
inline bool SystemAnimationsEnabled() noexcept {
    BOOL enabled = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0)) return true;
    return enabled != FALSE;
}

// Millisecond animation clock. GetTickCount64 advances in 15.6 ms steps, so
// at 60-144 Hz consecutive frames would sample 0, 16, 16, 31 ms... and glides
// stutter; the performance counter is exact. Only for motion start/sample
// times - never mix with GetTickCount64 values.
inline uint64_t NowMs() noexcept {
    static const LONGLONG frequency = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? f.QuadPart : 1;
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<uint64_t>(counter.QuadPart / frequency * 1000 +
                                 counter.QuadPart % frequency * 1000 / frequency);
}

inline float EaseOutCubic(float t) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    const float r = 1.0f - t;
    return 1.0f - r * r * r;
}

// Slight overshoot for small "pop" confirmations (copy check mark).
inline float EaseOutBack(float t) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    constexpr float c1 = 1.70158f;
    constexpr float c3 = c1 + 1.0f;
    const float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

// A highlight rectangle (selection pill, hover plate) that glides when the
// highlighted item changes and follows instantly when the same item merely
// moves (scroll, resize, relayout). Anything that stops calling Update for a
// frame — hover cleared, view hidden — reappears in place instead of flying in
// from a stale position.
// Extra time an animation may ask for frames after its nominal end, so the
// frame that lands it exactly on target is still drawn at a low frame rate.
constexpr uint64_t kSettleGraceMs = 100;

class RectMotion {
public:
    // context: identity of the surface (pane path + view mode, page id, ...).
    // key: identity of the highlighted item inside that surface.
    // frame: MainRenderer's per-Render counter.
    D2D1_RECT_F Update(uint64_t context, int64_t key, const D2D1_RECT_F& target,
                       uint64_t frame, uint64_t now, uint32_t duration_ms) noexcept {
        const bool continuous = visible_ && context == context_ &&
                                (frame == frame_ || frame == frame_ + 1);
        if (!continuous) {
            Snap(target);
        } else if (key != key_) {
            if (SystemAnimationsEnabled() && duration_ms > 0) {
                from_ = current_;
                to_ = target;
                start_ = now;
                duration_ = duration_ms;
                settled_ = false;
            } else {
                Snap(target);
            }
        } else if (!Same(target, to_)) {
            if (settled_) {
                Snap(target);
            } else {
                // Same item moved mid-flight (e.g. wheel scroll): shift the
                // whole path so the plate stays attached to its row.
                from_.left += target.left - to_.left;
                from_.top += target.top - to_.top;
                from_.right += target.right - to_.right;
                from_.bottom += target.bottom - to_.bottom;
                to_ = target;
            }
        }
        visible_ = true;
        context_ = context;
        key_ = key;
        frame_ = frame;
        if (!settled_) {
            const float t = static_cast<float>(now - start_) / static_cast<float>(duration_);
            if (t >= 1.0f) {
                current_ = to_;
                settled_ = true;
            } else {
                const float e = EaseOutCubic(t);
                current_ = D2D1::RectF(from_.left + (to_.left - from_.left) * e,
                                       from_.top + (to_.top - from_.top) * e,
                                       from_.right + (to_.right - from_.right) * e,
                                       from_.bottom + (to_.bottom - from_.bottom) * e);
            }
        }
        return current_;
    }

    // True while another frame is needed to finish the glide. Bounded by the
    // glide's own duration (now: NowMs clock): a surface that stops calling
    // Update mid-glide - the pointer left the list, the page closed - must not
    // keep the frame pump running for ever. It snaps on its next Update.
    bool Active(uint64_t now) const noexcept {
        return visible_ && !settled_ && now - start_ < duration_ + kSettleGraceMs;
    }

private:
    static bool Same(const D2D1_RECT_F& a, const D2D1_RECT_F& b) noexcept {
        return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
    }
    void Snap(const D2D1_RECT_F& target) noexcept {
        from_ = to_ = current_ = target;
        settled_ = true;
    }

    bool visible_ = false;
    bool settled_ = true;
    uint64_t context_ = 0;
    int64_t key_ = 0;
    uint64_t frame_ = 0;
    uint64_t start_ = 0;
    uint32_t duration_ = 0;
    D2D1_RECT_F from_{};
    D2D1_RECT_F to_{};
    D2D1_RECT_F current_{};
};

// "Copied" confirmation shown in place on the button that copied, instead of a
// toast. Region/index identify the button (HitTestResult region + index).
struct CopyFeedback {
    static constexpr uint32_t kHoldMs = 1200;
    static constexpr uint32_t kPopMs = 240;

    int region = -1;
    int index = -1;
    uint64_t start = 0;

    void Trigger(int r, int i, uint64_t now) noexcept { region = r; index = i; start = now; }

    // Elapsed ms for the matching button, or -1 when it shows its normal face.
    int64_t Elapsed(int r, int i, uint64_t now) const noexcept {
        if (region != r || index != i || now - start >= kHoldMs) return -1;
        return static_cast<int64_t>(now - start);
    }

    // Frames are needed only while the check pops in, plus one repaint when the
    // button reverts.
    bool Tick(uint64_t now) noexcept {
        if (region < 0) return false;
        const uint64_t elapsed = now - start;
        if (elapsed >= kHoldMs) { region = -1; return true; }
        return elapsed < kPopMs + 32;
    }
};

// List rows that slide into place when entries are removed or inserted (FLIP):
// rows keep their previous on-screen position and ease to the new layout, and
// newly appeared rows get a short accent flash. Only small changes in the
// same folder, view and scroll position animate; everything else snaps.
class ListShiftMotion {
public:
    static constexpr uint32_t kShiftMs = 180;
    static constexpr uint32_t kFlashMs = 900;
    static constexpr size_t kMaxChanged = 30;

    // allowed: false while loading / streaming search results.
    void BeginFrame(uint64_t context, float scroll_x, float scroll_y, size_t count,
                    uint64_t frame, uint64_t now, bool allowed) noexcept {
        const bool continuous = valid_ && context == context_ &&
                                (frame == frame_ || frame == frame_ + 1);
        const bool same_view = continuous && scroll_x == scroll_x_ && scroll_y == scroll_y_;
        const size_t diff = count > count_ ? count - count_ : count_ - count;
        trigger_ = same_view && allowed && count_ > 0 && count != count_ &&
                   diff <= kMaxChanged && SystemAnimationsEnabled();
        if (!same_view) {
            shifts_.clear();
            born_.clear();
        }
        if (trigger_) {
            shifts_.clear();
            grew_ = count > count_;
            start_ = now;
        }
        previous_.swap(current_);
        current_.clear();
        if (!same_view) previous_.clear();
        valid_ = true;
        context_ = context;
        scroll_x_ = scroll_x;
        scroll_y_ = scroll_y;
        count_ = count;
        frame_ = frame;
        now_ = now;
    }

    // Offset to add to the row identified by key whose layout position is pos.
    D2D1_POINT_2F Offset(uint64_t key, D2D1_POINT_2F pos) {
        if (trigger_) {
            const auto it = previous_.find(key);
            if (it != previous_.end()) {
                const float dx = it->second.x - pos.x;
                const float dy = it->second.y - pos.y;
                if (dx != 0.0f || dy != 0.0f) shifts_[key] = D2D1_POINT_2F{dx, dy};
            } else if (grew_) {
                born_[key] = now_;
            }
        }
        D2D1_POINT_2F offset{0.0f, 0.0f};
        const auto shift = shifts_.find(key);
        if (shift != shifts_.end()) {
            const float t = static_cast<float>(now_ - start_) / static_cast<float>(kShiftMs);
            const float k = t >= 1.0f ? 0.0f : 1.0f - EaseOutCubic(t);
            offset = D2D1_POINT_2F{shift->second.x * k, shift->second.y * k};
        }
        current_[key] = D2D1_POINT_2F{pos.x + offset.x, pos.y + offset.y};
        return offset;
    }

    // 1 -> 0 accent flash strength for a newly appeared row.
    float Flash(uint64_t key) const {
        const auto it = born_.find(key);
        if (it == born_.end() || now_ - it->second >= kFlashMs) return 0.0f;
        return 1.0f - static_cast<float>(now_ - it->second) / static_cast<float>(kFlashMs);
    }

    void EndFrame() {
        if (now_ - start_ >= kShiftMs) shifts_.clear();
        for (auto it = born_.begin(); it != born_.end();) {
            if (now_ - it->second >= kFlashMs) it = born_.erase(it);
            else ++it;
        }
        trigger_ = false;
    }

    // Time-bounded like RectMotion::Active: EndFrame only runs while the pane
    // is drawn, so a pane that stops drawing mid-animation would otherwise
    // leave shifts / flashes behind that never expire.
    bool Active(uint64_t now) const noexcept {
        if (!shifts_.empty() && now - start_ < kShiftMs + kSettleGraceMs) return true;
        for (const auto& born : born_) {
            if (now - born.second < kFlashMs + kSettleGraceMs) return true;
        }
        return false;
    }

private:
    bool valid_ = false;
    bool trigger_ = false;
    bool grew_ = false;
    uint64_t context_ = 0;
    float scroll_x_ = 0.0f;
    float scroll_y_ = 0.0f;
    size_t count_ = 0;
    uint64_t frame_ = 0;
    uint64_t now_ = 0;
    uint64_t start_ = 0;
    std::unordered_map<uint64_t, D2D1_POINT_2F> previous_;
    std::unordered_map<uint64_t, D2D1_POINT_2F> current_;
    std::unordered_map<uint64_t, D2D1_POINT_2F> shifts_;
    std::unordered_map<uint64_t, uint64_t> born_;
};

// Damped spring (mass 1) evaluated in closed form, so a transition is a pure
// function of elapsed time: no per-frame integration state, frame drops never
// change the path, and the same curve can be baked into a DComp animation.
// k: stiffness, c: damping. Values match the motion tokens of the design
// prototype (pulse_motion_v2.html).
struct SpringSpec {
    float k = 520.0f;
    float c = 34.0f;
};

namespace spring {
inline constexpr SpringSpec Snappy{520.0f, 34.0f};  // buttons, small moves
inline constexpr SpringSpec Smooth{380.0f, 36.0f};  // list reflow, no overshoot
inline constexpr SpringSpec Plate{700.0f, 50.0f};   // hover / selection plates
inline constexpr SpringSpec Pop{620.0f, 30.0f};     // menus, badges, landings
inline constexpr SpringSpec Open{480.0f, 38.0f};    // windows growing from a source
inline constexpr SpringSpec Close{700.0f, 52.0f};   // ...and shrinking back
inline constexpr SpringSpec Soft{260.0f, 24.0f};    // cancelled drag flying home
} // namespace spring

// Position of a spring released at 0 towards 1 after t_ms (may overshoot 1).
inline float SpringProgress(SpringSpec s, float t_ms) noexcept {
    if (t_ms <= 0.0f) return 0.0f;
    const float t = t_ms * 0.001f;
    const float w0 = std::sqrt(s.k);
    const float zeta = s.c / (2.0f * w0);
    if (zeta < 0.999f) {
        const float wd = w0 * std::sqrt(1.0f - zeta * zeta);
        const float e = std::exp(-zeta * w0 * t);
        return 1.0f - e * (std::cos(wd * t) + (zeta * w0 / wd) * std::sin(wd * t));
    }
    if (zeta <= 1.001f) return 1.0f - std::exp(-w0 * t) * (1.0f + w0 * t);
    const float r = w0 * std::sqrt(zeta * zeta - 1.0f);
    const float r1 = -zeta * w0 + r, r2 = -zeta * w0 - r;
    return 1.0f - (r2 * std::exp(r1 * t) - r1 * std::exp(r2 * t)) / (r2 - r1);
}

// Time after which the spring stays within 0.5% of its target; used to bound
// frame requests (the visible motion ends well before).
inline uint32_t SpringSettleMs(SpringSpec s) noexcept {
    const float w0 = std::sqrt(s.k);
    const float zeta = s.c / (2.0f * w0);
    const float decay = zeta < 1.0f ? zeta * w0 : w0 * (zeta - std::sqrt(std::max(0.0f, zeta * zeta - 1.0f)));
    const float ms = 1000.0f * 5.3f / std::max(decay, 1.0f);  // ln(200)
    return static_cast<uint32_t>(std::clamp(ms, 60.0f, 900.0f));
}

// RectMotion driven by a spring instead of a fixed ease: the plate keeps its
// momentum when the target changes mid-flight (re-aims from where it is).
class SpringRectMotion {
public:
    D2D1_RECT_F Update(uint64_t context, int64_t key, const D2D1_RECT_F& target,
                       uint64_t frame, uint64_t now, SpringSpec spec) noexcept {
        const bool continuous = visible_ && context == context_ &&
                                (frame == frame_ || frame == frame_ + 1);
        if (!continuous || !animations_) {
            Snap(target);
        } else if (key != key_) {
            from_ = current_;
            to_ = target;
            start_ = now;
            spec_ = spec;
            settle_ = SpringSettleMs(spec);
            settled_ = false;
        } else if (!Same(target, to_)) {
            if (settled_) {
                Snap(target);
            } else {
                from_.left += target.left - to_.left;
                from_.top += target.top - to_.top;
                from_.right += target.right - to_.right;
                from_.bottom += target.bottom - to_.bottom;
                to_ = target;
            }
        }
        visible_ = true;
        context_ = context;
        key_ = key;
        frame_ = frame;
        if (!settled_) {
            const uint64_t elapsed = now - start_;
            if (elapsed >= settle_) {
                current_ = to_;
                settled_ = true;
            } else {
                const float e = SpringProgress(spec_, static_cast<float>(elapsed));
                current_ = D2D1::RectF(from_.left + (to_.left - from_.left) * e,
                                       from_.top + (to_.top - from_.top) * e,
                                       from_.right + (to_.right - from_.right) * e,
                                       from_.bottom + (to_.bottom - from_.bottom) * e);
            }
        }
        return current_;
    }
    // Query SystemAnimationsEnabled once per surface open, not per frame.
    void SetAnimationsEnabled(bool on) noexcept { animations_ = on; }
    void Hide() noexcept { visible_ = false; }
    bool Active(uint64_t now) const noexcept {
        return visible_ && !settled_ && now - start_ < settle_ + kSettleGraceMs;
    }

private:
    static bool Same(const D2D1_RECT_F& a, const D2D1_RECT_F& b) noexcept {
        return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
    }
    void Snap(const D2D1_RECT_F& target) noexcept {
        from_ = to_ = current_ = target;
        settled_ = true;
    }
    bool visible_ = false;
    bool settled_ = true;
    bool animations_ = true;
    uint64_t context_ = 0;
    int64_t key_ = 0;
    uint64_t frame_ = 0;
    uint64_t start_ = 0;
    uint32_t settle_ = 0;
    SpringSpec spec_{};
    D2D1_RECT_F from_{};
    D2D1_RECT_F to_{};
    D2D1_RECT_F current_{};
};

} // namespace pulse::ui::motion