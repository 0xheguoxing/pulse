#pragma once

#include "../ui/view_layout.h"
#include <map>
#include <optional>
#include <string>

namespace pulse::app {

// Normalized key for per-folder preferences; empty for virtual views and
// nonabsolute paths, which are never persisted.
std::wstring FolderPrefKey(const std::wstring& path);

// This PC (the empty path) has no folder key but keeps its own choice, like
// File Explorer; other virtual views are still never remembered.
class FolderViewPrefs {
public:
    std::optional<ui::ViewMode> Find(const std::wstring& path) const;
    bool Set(const std::wstring& path, ui::ViewMode mode);
    // Continuous icon size (DIP) chosen in a folder or This PC; none = the
    // mode's preset. dip 0 forgets it. Kept apart from the modes so older
    // builds read the same file.
    std::optional<float> FindIconDip(const std::wstring& path) const;
    bool SetIconDip(const std::wstring& path, float dip);
    // Photo wall (the header's wall button) chosen in a folder or This PC;
    // stored apart from the modes like the sizes.
    bool FindWall(const std::wstring& path) const;
    bool SetWall(const std::wstring& path, bool wall);
    // Mode for folders without a saved choice.
    ui::ViewMode Default() const { return default_; }
    // "Apply to all folders": `mode` becomes the default, per-folder choices go.
    // This PC is not a folder, so its own choice stays.
    void ApplyToAll(ui::ViewMode mode);
    void Clear() {
        views_.clear();
        icon_dips_.clear();
        walls_.clear();
        this_pc_.reset();
        default_ = ui::ViewMode::Details;
    }
    void AppendJson(std::wstring& out) const;
    void ReadJson(const std::wstring& json);

private:
    struct PathLess {
        bool operator()(const std::wstring& a, const std::wstring& b) const;
    };
    std::map<std::wstring, ui::ViewMode, PathLess> views_;
    std::map<std::wstring, int, PathLess> icon_dips_;  // "" = This PC
    std::map<std::wstring, bool, PathLess> walls_;     // "" = This PC; true only
    std::optional<ui::ViewMode> this_pc_;
    ui::ViewMode default_ = ui::ViewMode::Details;
};

} // namespace pulse::app
