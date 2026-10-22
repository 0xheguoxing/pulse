#pragma once

#include "release_note_tags.h"

#include <string>
#include <vector>

namespace pulse::ui {

// One embedded release note (docs/releases/<version>.md), pre-split for painting.
struct ReleaseNoteView {
    std::wstring version;
    bool current = false;              // the version that is running
    std::vector<std::wstring> lines;   // plain text, Markdown markers stripped
    std::vector<bool> bullets;         // parallel to lines: list item vs. paragraph
    std::vector<bool> headings;        // parallel: a "#" heading line
    std::vector<ReleaseTag> tags;      // parallel: an item's leading [新功能] etc., None if untagged
    std::vector<std::wstring> tag_labels;  // parallel: that tag as the note writes it
};

} // namespace pulse::ui
