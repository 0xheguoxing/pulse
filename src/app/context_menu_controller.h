#pragma once

#include "context_menu.h"
#include "context_menu_prefs.h"
#include "ctx_menu_timing.h"
#include "shell_verbs.h"
#include "../ops/ops_manager.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulse::app {

class ContextMenuController {
public:
    struct ShellOperations {
        std::function<uint32_t(std::vector<std::wstring>, HWND, bool, bool,
                               std::vector<std::wstring>)> query;
        std::function<void(uint32_t)> close;
        std::function<void(uint32_t, uint32_t, std::wstring, std::wstring)> invoke;
        std::function<void(const std::wstring&, const std::wstring&)> execute_verb;
        std::function<void(const std::wstring&, const std::wstring&)> open_with_app;
        std::function<void(const std::wstring&, const std::wstring&)> execute_command;
    };
    struct QueryCompletion {
        bool accepted = false;
        bool partial = false;
        uint32_t elapsed_ms = 0;
        std::wstring cache_key;
    };

    static std::wstring CacheKey(bool background, const std::wstring& extension);

    void SetShellOperations(ShellOperations operations) { operations_ = std::move(operations); }
    void StartQuery(ContextMenuPrefs& prefs, HWND hwnd,
                    std::vector<std::wstring> paths, bool background,
                    std::wstring extension, bool extended,
                    const std::function<std::wstring(const std::wstring&)>& normalize_path,
                    const std::function<void(const std::wstring&)>& prefetch_static);
    void Close();
    void WaitUntilReady(HWND hwnd, const std::function<void()>& pump_messages,
                        uint32_t timeout_ms = 15000);
    std::vector<ui::FluentMenuItem> BuildDisplay(ContextMenuPrefs& prefs,
        const std::vector<ui::FluentMenuItem>& base_items, bool& prefs_changed) const;
    bool ExecuteShellCommand(int command,
        const std::function<void()>& wait_until_ready,
        const std::function<void()>& refresh_folder);

    bool RequestStaticPrefetch(const std::wstring& extension);
    // A cached type is read again at most every kStaticRevalidateMs: verbs a
    // ProgID defines itself are outside the registry watch. True = enumerate
    // now and pass the result to CompleteStaticVerbs as usual.
    static constexpr uint64_t kStaticRevalidateMs = 5000;
    bool RequestStaticRevalidate(const std::wstring& extension, uint64_t now);
    void MergeStaticCache(
        std::unordered_map<std::wstring, std::vector<StaticVerb>> entries);
    // `generation` is cache_generation() when the enumeration started; results
    // that predate InvalidateCaches() are dropped. True when the current menu's
    // rows changed (a re-read that found the same verbs changes nothing).
    bool CompleteStaticVerbs(const std::wstring& extension,
                             std::vector<StaticVerb> verbs, uint32_t generation);
    void InvalidateCaches();
    uint32_t cache_generation() const { return cache_generation_; }
    bool HasCachedStaticVerbs(const std::wstring& extension) const {
        return static_cache_.contains(extension);
    }

    // Rows of the last complete answer for this type, painted before the live
    // one arrives; StartQuery seeds them at once, so a partial answer that beats
    // the popup lands on them too. They get ids from kPredictedIdFirst up (never a host id:
    // those come from the previous menu), and partial answers only patch them
    // (see CompleteComQuery), so a menu painted from the cache never shrinks
    // to a partial list and grows back.
    static constexpr uint32_t kPredictedIdFirst = 0x7F00;
    static constexpr uint32_t kPredictedIdLast = 0x7FFF;
    bool SeedComItemsFromCache();
    const std::vector<ops::ShellMenuItem>& com_items() const noexcept { return com_items_; }
    bool com_predicted() const noexcept { return com_predicted_; }
    QueryCompletion CompleteComQuery(uint32_t token,
                                     std::vector<ops::ShellMenuItem> items,
                                     uint64_t completed_at, bool partial = false);

    void OpenMenu(std::vector<ui::FluentMenuItem> base_items);
    void NotePatchedDisplay() { menu_com_items_ = com_items_; menu_static_verbs_ = static_verbs_; }
    void CloseMenu() noexcept { menu_open_ = false; }
    bool menu_open() const noexcept { return menu_open_; }
    const std::vector<ui::FluentMenuItem>& base_items() const noexcept {
        return base_items_;
    }

    // Timing record of the menu on screen (ctx_menu_timing.h): begun right
    // before the popup is tracked, taken once it closed.
    void BeginTiming(uint32_t input_to_open_ms, size_t rows, bool seeded, uint64_t now);
    void NoteDisplayUpdate(size_t rows, uint64_t now);
    bool TakeTiming(bool shell_command, CtxMenuTiming& out);

    void ScheduleFolderRefresh(uint64_t now) noexcept;
    int ConsumeDueRefreshes(uint64_t now) noexcept;

private:
    std::vector<ShellMenuEntry> ComposeEntries(ContextMenuPrefs& prefs,
                                               bool& prefs_changed) const;
    const ops::ShellMenuItem* FindComItem(uint32_t id) const;
    const ops::ShellMenuItem* FindComItemIn(const std::vector<ops::ShellMenuItem>& items,
                                            uint32_t id) const;
    uint32_t FindLiveComId(const std::wstring& text) const;
    uint32_t FindLiveComId(const std::wstring& verb, const std::wstring& text) const;

    uint32_t token_ = 0;
    std::vector<std::wstring> paths_;
    bool background_ = false;
    bool extended_ = false;
    std::vector<std::wstring> disabled_handlers_;
    bool menu_open_ = false;
    bool com_ready_ = false;
    bool com_predicted_ = false;  // com_items_ were seeded from com_cache_
    std::vector<ops::ShellMenuItem> com_items_;
    std::vector<ops::ShellMenuItem> menu_com_items_;
    std::vector<StaticVerb> static_verbs_;
    std::vector<StaticVerb> menu_static_verbs_;
    bool display_snapshot_valid_ = false;
    std::wstring extension_;
    std::vector<ui::FluentMenuItem> base_items_;
    std::unordered_map<std::wstring, std::vector<StaticVerb>> static_cache_;
    // Extensions whose rows came from the on-disk cache: they still re-enumerate
    // once, so a cache written by an older build heals itself.
    std::unordered_set<std::wstring> static_seeded_;
    std::unordered_map<std::wstring, std::vector<ops::ShellMenuItem>> com_cache_;
    std::unordered_set<std::wstring> static_pending_;
    std::unordered_map<std::wstring, uint64_t> static_read_at_; // last enumeration request
    uint32_t cache_generation_ = 0;  // bumped by InvalidateCaches()
    uint64_t query_started_at_ = 0;
    CtxMenuTiming timing_;
    bool timing_active_ = false;
    uint64_t opened_at_ = 0;
    size_t display_rows_ = 0;
    uint64_t refresh_at_ = 0;
    uint64_t refresh_again_at_ = 0;
    ShellOperations operations_;
};

} // namespace pulse::app
