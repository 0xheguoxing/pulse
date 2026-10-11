#pragma once
// tab_spring.h - Multi-window M6e: a file drag resting on a tab of the strip
// opens that tab (spring-loading), in any window, so files can be dropped
// into any tab without letting go.
#include <cstddef>
#include <cstdint>

namespace pulse::app {
// How long a file drag rests on another tab before it opens.
constexpr uint64_t kTabSpringMs = 650;

// One DragOver step. `hit_tab` is the strip tab under the cursor (-1: none)
// and `active` the active tab; `spring_tab` / `since` hold the tab being
// rested on and since when (`now`, ms). True when `hit_tab` should open now;
// the rest starts over afterwards. The active tab never springs.
inline bool AdvanceTabSpring(int& spring_tab, uint64_t& since, int hit_tab, size_t active, uint64_t now) {
    if (hit_tab < 0 || static_cast<size_t>(hit_tab) == active) {
        spring_tab = -1;
        return false;
    }
    if (spring_tab != hit_tab) {
        spring_tab = hit_tab;
        since = now;
        return false;
    }
    if (now - since < kTabSpringMs) return false;
    spring_tab = -1;
    return true;
}
}  // namespace pulse::app
