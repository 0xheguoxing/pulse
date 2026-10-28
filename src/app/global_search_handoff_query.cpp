#include "global_search_handoff.h"
#include "search_query.h"
#include "places.h"

namespace pulse {
std::wstring GlobalSearchHandoffPath(const GlobalSearchHandoff& request) {
    if (request.query.empty()) return {};
    if (!request.content)
        return app::MakeSearchPath(app::CompileNameQueryInput(request.query + request.filters, request.folder));
    app::AdvancedSearchSpec spec;
    spec.content = request.query;
    spec.current_folder = request.folder;
    spec.location = request.folder.empty() ? app::LocationScope::Indexed : app::LocationScope::CurrentFolder;
    return app::MakeSearchPath(app::CompileSearchQuery(spec));
}

} // namespace pulse
