#include "app_internal.h"
#include "../ops/clipboard.h"
#include "../ui/quick_preview_command.h"

namespace pulse {
void HandleQuickPreviewCommand(AppState& s, const ui::QuickPreviewCommand& command) {
    if (!s.quickPreview.visible() || command.target.path.empty()) return;
    if (command.action == ui::QuickPreviewAction::InstallMediaPack) {
        // Same download as Settings > 预览增强包; BuildVm feeds its progress
        // back to the card and reopens the preview once it is installed.
        s.settings.InstallMediaPack(s.hwnd);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    if (command.action == ui::QuickPreviewAction::InstallImagePack) {
        s.settings.InstallImagePack(s.hwnd);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    const auto& target = command.target;
    const auto action = command.action;
    if (action == ui::QuickPreviewAction::OpenInNewTab) {
        if (!(target.attrs & FILE_ATTRIBUTE_DIRECTORY)) return;
        s.quickPreview.Close();
        OpenFolderTab(s, target.path);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    if (target.read_only && (action == ui::QuickPreviewAction::Cut ||
        action == ui::QuickPreviewAction::Rename || action == ui::QuickPreviewAction::Delete)) return;

    if (command.folder_child) {
        switch (action) {
        case ui::QuickPreviewAction::Open: OpenPath(s, target.path); break;
        case ui::QuickPreviewAction::Copy:
        case ui::QuickPreviewAction::Cut:
            CollectPathsToTray(s, {target.path}, action == ui::QuickPreviewAction::Cut);
            break;
        case ui::QuickPreviewAction::CopyPath:
            ops::WriteClipboardText(ClipboardPath(target.path));
            break;
        case ui::QuickPreviewAction::ToggleStar:
            ToggleStarred(s, target.path, target.attrs & FILE_ATTRIBUTE_DIRECTORY
                ? app::PlaceItemKind::Folder : app::PlaceItemKind::File);
            break;
        case ui::QuickPreviewAction::Properties: s.ops.ShowProperties(target.path); break;
        case ui::QuickPreviewAction::Delete:
            DeletePaths(s, {target.path}, command.shift);
            break;
        case ui::QuickPreviewAction::Rename: {
            // Reuse the existing inline editor in the item's containing folder.
            s.quickPreview.Close();
            const auto parent = fs::ParentPath(target.path);
            NavigateTo(s, parent);
            if (auto* tab = ActiveTab(s); tab && tab->current_path == fs::NormalizePath(parent)) {
                if (!tab->loading && tab->snapshot) {
                    for (size_t i = 0; i < tab->EntryCount(); ++i) {
                        if (tab->EntryAt(i).name != target.name) continue;
                        tab->SelectOnly(static_cast<int>(i));
                        ShowRenameOverlay(s);
                        return;
                    }
                }
                tab->pending_preview_rename = target.name;
            }
            break;
        }
        default: break;
        }
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }

    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot) return;
    int index = -1;
    for (size_t i = 0; i < tab->EntryCount(); ++i) {
        const auto& entry = tab->EntryAt(i);
        if (entry.name == target.name && EntryFullPath(*tab, static_cast<int>(i)) == target.path) {
            index = static_cast<int>(i);
            break;
        }
    }
    if (index < 0) return;
    tab->SelectOnly(index);
    EnsureRowVisible(s, *tab, index);
    switch (action) {
    case ui::QuickPreviewAction::Open: OpenSelected(s); break;
    case ui::QuickPreviewAction::Cut: CollectToTray(s, true); break;
    case ui::QuickPreviewAction::Copy: CollectToTray(s, false); break;
    case ui::QuickPreviewAction::CopyPath: CopySelectedPath(s); break;
    case ui::QuickPreviewAction::ToggleStar:
        ToggleStarred(s, target.path, target.attrs & FILE_ATTRIBUTE_DIRECTORY
            ? app::PlaceItemKind::Folder : app::PlaceItemKind::File);
        s.quickPreview.SetStarred(s.places.IsStarred(target.path));
        break;
    case ui::QuickPreviewAction::Rename:
        s.quickPreview.Close();
        ShowRenameOverlay(s);
        break;
    case ui::QuickPreviewAction::Delete:
        s.quickPreviewAnchorView = BuildVm(s).pane.ViewIndex(index);
        DeleteSelected(s, command.shift);
        break;
    case ui::QuickPreviewAction::Properties: s.ops.ShowProperties(target.path); break;
    default: break;
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}
} // namespace pulse
