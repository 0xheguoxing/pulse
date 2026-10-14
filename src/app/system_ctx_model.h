// system_ctx_model.h — Settings > 系统右键菜单: scans Explorer's context menu
// registrations on a worker thread and switches them on/off.
//
// Per-user entries change at once; machine-wide ones queue until 应用, then go
// to pulse_integration.exe --system-ctx-apply in one elevated batch. Every
// write is preceded by a .reg export and logged, and each batch can be undone.
#pragma once

#include "system_ctx_registry.h"
#include "../ui/sysctx_view.h"

#include <windows.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pulse::app {

class SystemCtxModel {
public:
    SystemCtxModel();
    ~SystemCtxModel();
    SystemCtxModel(const SystemCtxModel&) = delete;
    SystemCtxModel& operator=(const SystemCtxModel&) = delete;

    // Window repainted when a scan or batch finishes (any thread may call).
    void SetNotifyWindow(HWND hwnd);
    // First visit to the page: scan once in the background.
    void EnsureLoaded();
    void Refresh();

    // UI thread. Cheap when nothing changed since the last call.
    std::shared_ptr<const ui::SysCtxView> View();

    void SelectScene(int scene);
    void SelectSub(int sub);
    void SetFilter(int filter);
    // Selects `row` of the current view (again: deselects) and shows 详情.
    void SelectRow(int row);
    void SetSideTab(int tab);
    // Flips the entry behind `row` of the current view; -1: the selected row.
    void ToggleRow(int row);
    void ApplyPending();
    void DiscardPending();
    void Undo();
    void RestoreAll();  // reverses every batch applied this session

    // 概览, the first row of the scene list: cards instead of the entry list.
    void ShowOverview();
    // Pulse's own measurements of the handlers (fed while the page is open);
    // the 概览 ranks them. Cheap when nothing changed.
    void SetHandlerTimings(std::vector<ui::SysCtxHandlerTiming> timings);
    // 概览 buttons; `index` into the view's slow_rows / mark_rows.
    void ToggleSlowRow(int index);         // 禁用: like the row switch
    void RestoreMarkRow(int index);        // 还原: switches the marked entry on
    std::wstring SlowRowClsid(int index);  // Pulse 跳过 (the prefs live with the caller)
    // ContextMenuPrefs catalog keys Pulse hides in its own menu (sorted); rows
    // show 在 Pulse 中隐藏. Cheap when nothing changed.
    void SetPulseHidden(std::vector<std::wstring> keys);

    // Live preview drag (UI thread): a press arms it, moves past 3 px make the
    // ghost follow the pointer, the drop goes through DropPreview.
    void BeginPreviewDrag(int item, float y, float grab);
    bool PreviewDragMove(float y);  // true: repaint
    int PreviewDragItem() const;
    bool PreviewDragLive() const;
    void EndPreviewDrag();
    // Puts preview item `item` into a SysCtxZone: the 已隐藏 shelf switches it
    // off, a menu zone switches it on and (static verbs) sets Position. Queued
    // like the switches: per-user entries change at once, machine-wide ones
    // wait for 应用. Dropping an entry back where it was cancels its queue.
    void DropPreview(int item, int zone);
    // A press without dragging selects the entry (list row and preview).
    void SelectPreview(int item);

    // "…" menu helpers for `row` (-1: the selected row); empty when there is none.
    std::wstring RowRegistryPath(int row);  // HKEY_…\Software\Classes\…
    std::wstring RowFile(int row);
    // Pulse's catalog key for the entry (ipc::HandlerCatalogKey / CatalogKey);
    // empty for kinds Pulse does not list as rows of its own.
    std::wstring RowPulseKey(int row);
    std::wstring DataDirectory() const;     // holds ops.jsonl and the Backups folder
    std::wstring BackupDirectory() const;

    // Paths under %LOCALAPPDATA%\Pulse\SystemContextMenu (tests override).
    void SetDataDirectoryForTesting(std::wstring directory);

private:
    struct Entry {
        sysctx::Item item;
        std::wstring icon;         // resolved .exe/.ico, checked on the worker
        std::wstring publisher;    // CompanyName of the program/DLL
        std::wstring description;  // FileDescription (names bare handler keys)
    };
    struct Batch {
        std::vector<sysctx::Op> ops;
    };
    struct Change {            // 概览 · 最近的改动
        std::wstring title;    // 禁用「7-Zip」等 3 项
        std::wstring caption;  // HH:mm · backup.reg
        bool undone = false;
    };
    // A row of the scene list; extensions and perceived types pick a sub scene.
    struct Nav {
        const wchar_t* zh;
        const wchar_t* en;
        const wchar_t* glyph;
        int group;
        std::vector<int> scenes;   // indices into scene_ids_
        std::vector<std::wstring> subs;
    };

    void StartJob(ui::SysCtxView::State state, std::vector<sysctx::Op> ops, size_t pop_undo, bool record_undo);
    void RunJob(std::vector<sysctx::Op> ops, size_t pop_undo, bool record_undo);
    struct ScanResult {
        std::vector<std::vector<Entry>> entries;
        std::vector<std::wstring> keys;  // per scene: where its entries live
    };
    ScanResult Scan() const;
    // Machine ops through the elevated helper; fills `results`.
    bool RunElevated(const std::vector<sysctx::Op>& ops, std::vector<sysctx::OpResult>& results,
                     std::wstring& error) const;
    std::wstring WriteBackup(const std::vector<sysctx::Op>& ops) const;  // the .reg file name
    void AppendLog(const std::vector<sysctx::Op>& ops, const std::vector<sysctx::OpResult>& results) const;
    const Entry* RowEntryLocked(int row) const;
    int CurrentSceneLocked() const;
    // Flips `e` like the row switch (lock held): a per-user entry gives an op
    // to run now (true); a machine entry queues; a queued one leaves the queue.
    bool ToggleEntryLocked(const Entry& e, sysctx::Op& op);
    void ToggleMapped(const std::vector<std::pair<int, size_t>>& map, int index);
    std::wstring ChangeTitleLocked(const std::vector<sysctx::Op>& ops) const;

    static std::wstring OpKey(sysctx::Root root, sysctx::Kind kind, const std::wstring& key_path);
    // pending_ key: the switch and the placement of an entry queue separately.
    static std::wstring PendingKey(const sysctx::Op& op);

    std::vector<std::wstring> scene_ids_;
    std::vector<Nav> nav_;

    mutable std::mutex mutex_;
    HWND notify_ = nullptr;
    std::thread worker_;
    bool loaded_ = false;
    ui::SysCtxView::State state_ = ui::SysCtxView::State::Idle;
    std::vector<std::vector<Entry>> entries_;          // per scene id
    std::vector<std::wstring> scene_keys_;
    std::map<std::wstring, sysctx::Op> pending_;       // OpKey -> queued op
    std::vector<Batch> undo_;
    std::vector<Change> changes_;                      // applied this session, oldest first
    int nav_index_ = 2;                                // 文件夹: the busiest menu
    std::vector<int> sub_index_;                       // per nav row
    int filter_ = ui::kSysCtxFilterAll;
    std::wstring selected_;                            // OpKey of the selected row
    int side_tab_ = 0;
    std::wstring message_;
    bool message_error_ = false;
    std::wstring data_dir_;
    std::vector<std::pair<int, size_t>> row_map_;      // view row -> (scene, entry)
    int selected_row_ = -1;
    std::vector<std::pair<int, size_t>> preview_map_;  // preview item -> (scene, entry)
    bool overview_ = true;                             // 概览 shown; the first visit starts there
    std::vector<ui::SysCtxHandlerTiming> timings_;
    std::vector<std::wstring> pulse_hidden_;  // sorted
    std::vector<std::pair<int, size_t>> slow_map_, mark_map_;  // 概览 rows -> (scene, entry); scene -1: not scanned
    int drag_item_ = -1;
    bool drag_live_ = false;
    float drag_start_ = 0.0f, drag_y_ = 0.0f, drag_grab_ = 0.0f;
    std::shared_ptr<const ui::SysCtxView> view_;
    bool dirty_ = true;
};

} // namespace pulse::app
