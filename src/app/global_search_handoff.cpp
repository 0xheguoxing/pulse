#include "global_search_handoff.h"
#include "app_internal.h"
#include "app_hosted_edit.h"
#include "app_navigation.h"
#include "places.h"
#include "search_query.h"
#include "startup_location.h"
#include "tray_reveal.h"

namespace pulse {

void ContinueGlobalSearchInMain(AppState& s, const GlobalSearchHandoff& request) {
    // Taken here so the tray hook below does not start over a second time.
    const bool fresh = TakeFreshStart(s);
    const auto path = GlobalSearchHandoffPath(request);
    if (s.renameIndex >= 0) HideRenameOverlay(s, false);
    if (s.addressEditing) HideAddressEditor(s, false);
    if (!path.empty()) {
        const std::wstring origin = !request.folder.empty() ? request.folder
            : fresh ? app::DefaultLocation(s.appPrefs) : NewTabPath(s);
        if (fresh) StartFreshAt(s, origin);
        else OpenTabAt(s, origin);
        NavigateTo(s, path);
    } else if (fresh) {
        StartFreshAt(s, app::DefaultLocation(s.appPrefs));
    }
    RevealWindow(s);
    if (path.empty()) ShowAddressSearch(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

} // namespace pulse
