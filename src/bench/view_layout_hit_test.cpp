#include "../ui/view_layout.h"
#include <iostream>
#include <algorithm>

int main() {
    using namespace pulse::ui;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        failures += !ok;
    };
    {
        // Rows are centred: both margins around an extra-large row are blank.
        const ViewLayout xl(ViewMode::ExtraLargeIcons, {0, 0, 900, 900}, 20, 0, 0, 1);
        const auto first = xl.ItemRect(0), last = xl.ItemRect(xl.Metrics().columns - 1);
        check(first.left > 0 && last.right < 900 && xl.HitTest(first.left - 1, 20) == -1 &&
              xl.HitTest(last.right + 1, 20) == -1, "extra-large row margins are blank");
    }
    check(ViewLayout(ViewMode::List, {0, 0, 800, 250}, 30, 0, 0, 1).HitTest(30, 245) == -1,
          "column-major bottom remainder is blank");
    for (int mode = 0; mode < 8; ++mode) {
        bool valid = true;
        for (float scale : {1.0f, 1.5f, 2.0f}) for (float width : {241.0f, 541.0f, 900.0f})
        for (float height : {101.0f, 250.0f, 701.0f}) for (size_t count : {size_t{0}, size_t{1}, size_t{17}, size_t{120}})
        for (float scroll : {0.0f, 27.0f, 123.0f}) {
            const D2D1_RECT_F viewport{13, 19, 13 + width * scale, 19 + height * scale};
            ViewLayout layout(ViewModeFromIndex(mode), viewport, count, scroll, scroll, scale);
            auto contains = [](D2D1_RECT_F r, float x, float y) {
                return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
            };
            for (float y = viewport.top; y <= viewport.bottom + 1; y += 7.3f)
                for (float x = viewport.left; x <= viewport.right + 1; x += 11.7f) {
                    const int hit = layout.HitTest(x, y);
                    if (hit >= 0 && (!contains(viewport, x, y) || !contains(layout.ItemRect(hit), x, y))) valid = false;
                }
            // Also verify interiors remain reachable; rejecting every point is not a fix.
            for (size_t i = 0; i < count; ++i) {
                const auto r = layout.ItemRect(static_cast<int>(i));
                const float x = (r.left + r.right) / 2, y = (r.top + r.bottom) / 2;
                if (contains(viewport, x, y) && layout.HitTest(x, y) != static_cast<int>(i)) valid = false;
            }
        }
        std::cout << "[MODE] " << mode << '\n';
        check(valid, "real layout hit implies rendered rectangle containment and visible interiors remain reachable");
    }
    ListGroups groups{{0, 3}, {3, 2}}; groups[1].collapsed = true;
    ViewLayout grouped(ViewMode::Details, {0, 0, 700, 400}, 5, 0, 0, 1, 0, &groups);
    const auto header = grouped.HeaderRect(0);
    check(grouped.HitTest(30, (header.top + header.bottom) / 2) == -1, "group header keeps priority over item hits");
    const auto collapsed = grouped.ItemRect(3);
    check(collapsed.top == collapsed.bottom, "collapsed group has no hittable item area");
    return failures ? 1 : 0;
}
