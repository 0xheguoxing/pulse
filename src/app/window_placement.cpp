// window_placement.cpp — see window_placement.h.
#include "window_placement.h"

#include <algorithm>

namespace pulse::app {
namespace {

bool MonitorRects(HMONITOR monitor, RECT& bounds, RECT& work) {
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return false;
    bounds = info.rcMonitor;
    work = info.rcWork;
    return true;
}

} // namespace

RECT WorkspaceToScreenRect(const RECT& workspace, const RECT& monitor, const RECT& work) {
    RECT screen = workspace;
    OffsetRect(&screen, work.left - monitor.left, work.top - monitor.top);
    return screen;
}

RECT RestoredWindowRect(const RECT& workspace) {
    RECT bounds{}, work{};
    HMONITOR monitor = MonitorFromRect(&workspace, MONITOR_DEFAULTTONEAREST);
    if (!MonitorRects(monitor, bounds, work)) return workspace;
    RECT screen = WorkspaceToScreenRect(workspace, bounds, work);
    // Near a monitor edge the unconverted rectangle can pick the neighbour;
    // the converted one names the monitor whose work area it belongs to.
    HMONITOR converted = MonitorFromRect(&screen, MONITOR_DEFAULTTONEAREST);
    if (converted != monitor && MonitorRects(converted, bounds, work))
        screen = WorkspaceToScreenRect(workspace, bounds, work);
    return screen;
}

RECT CascadeWindowRect(const RECT& opener, const RECT& work, int offset) {
    const LONG work_w = work.right - work.left;
    const LONG work_h = work.bottom - work.top;
    const bool has_work = work_w > 0 && work_h > 0;
    LONG w = opener.right - opener.left;
    LONG h = opener.bottom - opener.top;
    if (has_work) {
        w = (std::min)(w, work_w);
        h = (std::min)(h, work_h);
    }
    RECT r{opener.left + offset, opener.top + offset, 0, 0};
    if (has_work && (r.left < work.left || r.top < work.top || r.left + w > work.right || r.top + h > work.bottom)) {
        r.left = work.left;
        r.top = work.top;
    }
    r.right = r.left + w;
    r.bottom = r.top + h;
    return r;
}

} // namespace pulse::app
