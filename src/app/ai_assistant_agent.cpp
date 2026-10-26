// ai_assistant_agent.cpp - Pulse 助手's agent loop on DeepSeek.
//
// One user turn is a series of rounds: stream a chat completion (thinking,
// then content or tool calls), run the tool calls in order, send their
// results back, until the model answers without tools. Everything lands in
// the same conversation blocks the design shows:
//
//   reasoning_content   -> the thinking block (folds into 已思考 N 秒)
//   get_state           -> step 查看当前窗口 (UI thread; actions/action_state.h)
//   list_folder         -> step 读取文件夹 (worker thread, ai_tools_fs.h)
//   get_properties      -> step 查看属性 (worker thread)
//   organize_files      -> plan card + amber confirm card; on 整理 the moves
//                          run through Pulse's operation queue as one undo
//                          step, with a progress step and a result card
//   content             -> streamed answer, then the acts row
//
// Requests carry the tools, so every assistant message keeps its
// reasoning_content in the history (the service requires it back). Nothing
// moves without the user's 整理; the model cannot delete or overwrite.
#include "ai_assistant_chat.h"

#include "ai_assistant.h"
#include "ai_broker.h"
#include "ai_cleanup_scan.h"
#include "ai_dup_scan.h"
#include "ai_model_key.h"
#include "ai_model_stream.h"
#include "ai_search_spec.h"
#include "ai_settings.h"
#include "ai_tools_fs.h"
#include "app_commands.h"
#include "app_hosted_edit.h"
#include "app_internal.h"
#include "app_messages.h"
#include "app_navigation.h"
#include "app_network_page.h"
#include "app_ops_ui.h"
#include "batch_rename_ai.h"
#include "batch_rename_plan.h"
#include "batch_rename_presets.h"
#include "favorites_bar.h"
#include "folder_alias.h"
#include "keymap.h"
#include "places.h"
#include "tab_transfer.h"
#include "vertical_tabs.h"
#include "window_factory.h"
#include "window_overview_ui.h"
#include "window_placement.h"
#include "zip_compress.h"
#include "actions/action_params.h"
#include "actions/action_policy.h"
#include "actions/action_settings.h"
#include "actions/action_state.h"
#include "../common/path_utils.h"
#include "../common/json_value.h"
#include "../common/localization.h"
#include "../fs/fs_enum.h"
#include "../ops/recycle_empty.h"
#include "../ui/ai_list_overlay.h"
#include "../ui/ui_motion.h"
#include "../ui/ui_renderer.h"
#include "../ui/window_material.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <map>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <propkey.h>
#include <propsys.h>
#include <shobjidl.h>
#include <shlobj.h>

namespace pulse {

struct AiAgent {
    json::Value history = json::Value::Array();   // user / assistant / tool messages
    std::wstring pending_note;                    // told to the model with the next prompt
    uint64_t generation = 0;                      // bumped by stop: late callbacks drop out

    // The running turn.
    size_t turn_history = 0;                      // history size before the turn
    std::shared_ptr<app::ai::ModelStream> stream;
    int rounds = 0;
    int tool_count = 0;
    bool organized = false;                       // organize_files ran this turn
    uint32_t think_id = 0;
    uint32_t text_id = 0;
    uint32_t steps_id = 0;
    std::string reasoning;                        // this round, UTF-8
    std::string content;
    std::vector<app::ai::ModelToolCall> calls;    // this round's
    size_t call_index = 0;
    std::wstring read_folder;                     // for the folded steps line
    size_t read_files = 0;

    // search_files (flow A): the card and the search it shows.
    struct QueryCard {
        app::AdvancedSearchSpec spec;
        std::vector<app::ai::SearchField> fields;   // per chip on the card
        std::wstring path;                          // pulse:search: path of the tab
        std::wstring label;                         // the model's title (发票 · 9 月)
        std::vector<std::wstring> paths;            // results Pulse loaded (复制路径)
        size_t count = 0;
        uint64_t ms = 0;
        uint32_t block_id = 0, follow_id = 0;
    };
    std::shared_ptr<QueryCard> pending_query;     // this turn's; shown under the answer
    std::vector<std::shared_ptr<QueryCard>> queries;
    size_t searched = 0;                          // results of this turn's search
    bool did_search = false;

    struct Job {
        std::atomic<bool> done{false};
        std::atomic<bool> cancelled{false};
        app::ai::ToolOutcome out;
    };
    std::shared_ptr<Job> job;
};

namespace {

using I = l10n::StringId;
using ui::AiBlockKind;
using ui::AiBlockView;
using app::ai::ModelEvent;
using app::ai::ToolOutcome;

constexpr wchar_t kBaseUrl[] = L"https://api.deepseek.com";
constexpr int kMaxRounds = 12;
constexpr size_t kMaxToolText = 48 * 1024;   // a tool result sent back, in UTF-8 bytes
constexpr size_t kMaxOrganize = 2000;

std::wstring Text(I id) { return l10n::Get(id).c_str(); }

std::wstring Fill(std::wstring text, std::wstring_view key, const std::wstring& value) {
    if (const size_t at = text.find(key); at != std::wstring::npos) text.replace(at, key.size(), value);
    return text;
}

std::wstring Num(size_t n) { return std::to_wstring(n); }
uint64_t Now() { return ui::motion::NowMs(); }

void Redraw(AppState& s) {
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

AiAgent& Agent(AppState& s) {
    AiChat& c = Chat(s);
    if (!c.agent) c.agent = std::make_shared<AiAgent>();
    return *c.agent;
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    return dir.back() == L'\\' ? dir + name : dir + L"\\" + name;
}

std::wstring Leaf(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos || slash + 1 >= p.size() ? p : p.substr(slash + 1);
}

// The folder's name as Pulse shows it (下载 for Downloads).
std::wstring FolderLabel(const std::wstring& path) {
    std::wstring label = app::TabTitle(path);
    return label.empty() ? Leaf(path) : label;
}

// ------------------------------------------------- the list overlay (A4) --
// What the file list shows of the assistant's work (ui/ai_list_overlay.h).
std::shared_ptr<ui::AiListOverlay> g_list;

// Tabs keep the \\?\ form of long paths; tool arguments come back without
// it (CheckPathArg) or with it (the model echoes get_state).
std::wstring NormDir(std::wstring p) {
    for (wchar_t& c : p)
        if (c == L'/') c = L'\\';
    if (p.rfind(L"\\\\?\\UNC\\", 0) == 0) p = L"\\\\" + p.substr(8);
    else if (p.rfind(L"\\\\?\\", 0) == 0) p.erase(0, 4);
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return p;
}

bool SameDir(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && _wcsicmp(NormDir(a).c_str(), NormDir(b).c_str()) == 0;
}

// The folder is on screen: the active tab shows it and motion is on.
bool Shown(AppState& s, const std::wstring& folder) {
    app::Tab* tab = ActiveTab(s);
    return tab && SameDir(tab->current_path, folder) && ui::motion::SystemAnimationsEnabled();
}

ui::AiListOverlay& ListOverlay(const std::wstring& folder) {
    if (!g_list || !SameDir(g_list->folder, folder)) {
        g_list = std::make_shared<ui::AiListOverlay>();
        g_list->folder = NormDir(folder);
    }
    g_list->end_ms = 0;
    return *g_list;
}

ui::AiListOverlay* OverlayOf(const std::wstring& folder) {
    return g_list && SameDir(g_list->folder, folder) ? g_list.get() : nullptr;
}

// A sort still feeding its requests; Stop submits the rest at once (the
// user confirmed the whole plan).
std::function<void(AppState&)> g_flush_sort;

void EndOverlay(uint64_t in_ms) {
    if (g_list) g_list->end_ms = ui::motion::NowMs() + in_ms;
}

bool IsDirectory(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(app::ai::Win32Path(path).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(app::ai::Win32Path(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

// The model writes Markdown; the panel draws "- " lists and **bold**. Headings
// become bold lines, other bullets "- ", code ticks go.
std::wstring Display(const std::string& utf8) {
    const std::wstring in = json::Wide(utf8);
    std::wstring out;
    out.reserve(in.size());
    size_t start = 0;
    while (start <= in.size()) {
        size_t end = in.find(L'\n', start);
        if (end == std::wstring::npos) end = in.size();
        std::wstring line = in.substr(start, end - start);
        size_t lead = 0;
        while (lead < line.size() && (line[lead] == L' ' || line[lead] == L'\t')) ++lead;
        std::wstring body = line.substr(lead);
        if (!body.empty() && body[0] == L'#') {
            size_t h = 0;
            while (h < body.size() && body[h] == L'#') ++h;
            while (h < body.size() && body[h] == L' ') ++h;
            body = body.substr(h);
            if (!body.empty() && body.find(L"**") == std::wstring::npos) body = L"**" + body + L"**";
            line = body;
        } else if (body.size() >= 2 && (body[0] == L'*' || body[0] == L'+' || body[0] == L'-') && body[1] == L' ') {
            line = L"- " + body.substr(2);
        } else if (body.rfind(L"```", 0) == 0) {
            line.clear();
        }
        line.erase(std::remove(line.begin(), line.end(), L'`'), line.end());
        out += line;
        if (end >= in.size()) break;
        out += L'\n';
        start = end + 1;
    }
    // Trailing blank lines would leave a gap under the answer.
    while (!out.empty() && (out.back() == L'\n' || out.back() == L' ')) out.pop_back();
    return out;
}

std::string Utf8(const std::wstring& w) { return json::Utf8(w); }

// ---------------------------------------------------------------- request --

const json::Value& Tools() {
    static const json::Value tools = [] {
        static constexpr char kTools[] = R"JSON([
{"type":"function","function":{"name":"get_state",
 "description":"What the user sees in this Pulse window: the panes of the active tab in screen order (pane 1 is the left or top one; focused marks the one in use, target the Ctrl+Alt+D destination) with each folder (null for This PC or a virtual view such as a search, named in location), view, sort, filter, item counts and selection (up to 200 full paths for the focused pane, 20 names for the others), plus the other tabs and windows by title, the staging tray, the preview, an open dialog and the running file task.",
 "parameters":{"type":"object","properties":{}}}},
{"type":"function","function":{"name":"search_files",
 "description":"Finds files and folders anywhere with Pulse's file index in milliseconds (no disk walk, never reads contents). Pulse opens the results in the active tab as a search and shows the conditions to the user as chips they can edit. Returns the count, total size, the folders they are in and the first items. Use it whenever the user wants to find files by name, type, date or size, even without saying where.",
 "parameters":{"type":"object","properties":{
   "label":{"type":"string","description":"2 to 6 words in the user's language naming the results, e.g. 发票 · 9 月"},
   "name":{"type":"string","description":"Words the name contains (all of them), e.g. 发票. Omit when the user gave no name words"},
   "exclude":{"type":"string","description":"A word the name must not contain"},
   "type":{"type":"string","enum":["folder","document","image","video","audio","archive","code"],"description":"A kind of item; omit when extensions are given"},
   "extensions":{"type":"array","items":{"type":"string"},"description":"Exact extensions without dots, e.g. [\"pdf\",\"png\",\"jpg\"] for PDF and pictures"},
   "modified_after":{"type":"string","description":"First day, YYYY-MM-DD (inclusive)"},
   "modified_before":{"type":"string","description":"Last day, YYYY-MM-DD (inclusive)"},
   "larger_than":{"type":"string","description":"A size such as 100MB or 1GB"},
   "smaller_than":{"type":"string","description":"A size such as 500KB"},
   "folder":{"type":"string","description":"Absolute folder to search in; omit to search every indexed location"}}}}},
{"type":"function","function":{"name":"list_folder",
 "description":"Lists a folder's items: folders first, then files, each with type, size and modified time. Returns next_cursor when there are more.",
 "parameters":{"type":"object","properties":{
   "path":{"type":"string","description":"Absolute folder path, e.g. C:\\Users\\me\\Downloads"},
   "limit":{"type":"integer","description":"Items per page, 1-500 (default 200)"},
   "cursor":{"type":"string","description":"next_cursor of the previous page"}},
  "required":["path"]}}},
{"type":"function","function":{"name":"get_properties",
 "description":"File system and shell properties of up to 50 items: sizes, dates, attributes, image size, pages, duration, camera, program version. Never file contents.",
 "parameters":{"type":"object","properties":{
   "paths":{"type":"array","items":{"type":"string"},"description":"Absolute paths"}},
  "required":["paths"]}}},
{"type":"function","function":{"name":"organize_files",
 "description":"Moves items of one folder into subfolders of that folder. Missing subfolders are created, existing ones reused; nothing is overwritten (a clashing name gets a suffix). Pulse shows this plan to the user, who must accept it in Pulse before anything moves; the result says whether it ran. The user can undo it in Pulse.",
 "parameters":{"type":"object","properties":{
   "folder":{"type":"string","description":"Absolute path of the folder whose items move"},
   "groups":{"type":"array","description":"One entry per destination subfolder","items":{"type":"object","properties":{
      "name":{"type":"string","description":"Subfolder name, short, in the user's language"},
      "items":{"type":"array","items":{"type":"string"},"description":"Names of items directly inside folder"}},
     "required":["name","items"]}}},
  "required":["folder","groups"]}}},
{"type":"function","function":{"name":"rename_files",
 "description":"Renames files by one rule that Pulse applies on this PC. You give the rule as batch rename steps, not the new names: Pulse reads each file's date taken and other properties itself, makes every new name, checks for clashes (a clashing name gets a number such as (2)), shows old and new names to the user and asks in Pulse before anything changes. The user can undo it in Pulse.",
 "parameters":{"type":"object","properties":{
   "paths":{"type":"array","items":{"type":"string"},"description":"Absolute paths of the files; omit to rename the files selected in the active tab"},
   "steps":{"type":"array","description":"Applied in order. {\"type\":\"template\",\"pattern\":\"{taken}_Hangzhou_{n}\",\"start\":1,\"digits\":2} replaces the whole name (the extension stays); {\"type\":\"replace\",\"find\":\"a\",\"with\":\"b\",\"regex\":false,\"match_case\":false,\"all\":true}; {\"type\":\"remove\",\"brackets\":false,\"spaces\":false,\"digits\":false,\"custom\":\"\"} (brackets: copy markers such as (1)); {\"type\":\"insert\",\"text\":\"...\",\"where\":\"start|end|index\",\"at\":1}; {\"type\":\"case\",\"mode\":\"lower|upper|title\"}; {\"type\":\"number\",\"where\":\"start|end\",\"sep\":\" \",\"start\":1,\"digits\":3}; {\"type\":\"ext\",\"mode\":\"lower|upper|set\",\"to\":\"jpg\"}. Tokens in pattern and insert text: {name} original name, {n} running number, {taken} date taken yyyy-MM-dd (falls back to the modified date; {taken:yyyyMMdd} for another format), {modified} modified date, {folder} parent folder name, {size} image size WxH.","items":{"type":"object"}},
   "order":{"type":"string","enum":["list","taken","modified","name"],"description":"Numbering order: list (as shown), taken (date taken: use it for photos), modified or name"},
   "daily":{"type":"boolean","description":"true: the number starts again each day (by date taken), e.g. 2026-10-03_01, 2026-10-03_02, 2026-10-04_01"}},
  "required":["steps"]}}},
{"type":"function","function":{"name":"run_task",
 "description":"Does a multi-step file task in one go: finds files with Pulse's index (or takes the selected files), moves them into a destination folder, optionally into subfolders by modified month, year or kind, and optionally packs them into a zip there. Pulse builds the plan from the real files, shows every step and asks the user once in Pulse; the whole task is one undo step, and if a file cannot be moved Pulse pauses and asks the user. Nothing is deleted or overwritten (a clashing name gets a number). Use it instead of search_files plus organize_files when files should be found and then filed or zipped.",
 "parameters":{"type":"object","properties":{
   "label":{"type":"string","description":"The files in a few words in the user's language, e.g. 今年的发票"},
   "noun":{"type":"string","description":"What the files are in one word, e.g. 发票"},
   "find":{"type":"object","description":"Search conditions (as search_files). Omit to use the files selected in the active tab","properties":{
     "name":{"type":"string","description":"Every word the name must contain, separated by spaces: all name words the user gave, e.g. Pelican 发票"},
     "exclude":{"type":"string"},
     "type":{"type":"string","enum":["document","image","video","audio","archive","code"]},
     "extensions":{"type":"array","items":{"type":"string"}},
     "modified_after":{"type":"string","description":"YYYY-MM-DD (inclusive)"},
     "modified_before":{"type":"string","description":"YYYY-MM-DD (inclusive)"},
     "larger_than":{"type":"string"},
     "smaller_than":{"type":"string"},
     "folder":{"type":"string","description":"Absolute folder to search in; omit for everywhere"}}},
   "dest":{"type":"string","description":"Destination folder: an absolute path, or a known folder first (Documents, Downloads, Desktop, Pictures, Videos, Music), e.g. Documents\\发票. Created if missing"},
   "group_by":{"type":"string","enum":["none","month","year","type"],"description":"Subfolders inside dest by modified month (2026-01), year (2026) or kind of file; none puts the files in dest itself"},
   "move":{"type":"boolean","description":"false: leave the files where they are and only make the zip (default true)"},
   "zip":{"type":"string","description":"File name of a zip of the files to make in dest, e.g. 发票-2026.zip; omit for none. With subfolders the zip has the same folders"}},
  "required":["label","dest"]}}},
{"type":"function","function":{"name":"disk_usage",
 "description":"Where a drive's space went, read from Pulse's index in under a second (no disk walk): total, free, system / programs / users / other, the Recycle Bin, and the biggest folders and files. Pulse shows it to the user as a card.",
 "parameters":{"type":"object","properties":{
   "drive":{"type":"string","description":"Drive letter, e.g. C (default C)"}}}}},
{"type":"function","function":{"name":"find_cleanup",
 "description":"Finds what in the user's own folders can safely go to free space: installers of programs already installed, archives already extracted next to them, (1)/(2) copies identical to the original, Temp files untouched for 7 days, and videos unopened for a year (unchecked). Pulse checks it all on this PC, shows the categories with checkboxes and their reasons, checks the Recycle Bin has room, and asks the user; only after they accept in Pulse are the ticked files moved to the Recycle Bin (undoable). Never system or program files. Returns what was moved, or that the user declined.",
 "parameters":{"type":"object","properties":{
   "drive":{"type":"string","description":"Drive letter to free, e.g. C (default C)"}}}}},
{"type":"function","function":{"name":"find_duplicates",
 "description":"Finds files with exactly the same content in the given folders. Pulse compares them on this PC (size, the first 64 KB, then every byte; contents never leave the PC), picks the copy each group keeps (in an organised folder > a name without (1) or copy > the earliest, or the newest with prefer=newest; copies in two organised folders all stay), shows the groups, checks the Recycle Bin has room and asks the user; only after they accept in Pulse are the extra copies moved to the Recycle Bin (undoable), with each kept file locked and re-checked first. Returns what was moved, or that the user declined.",
 "parameters":{"type":"object","properties":{
   "folders":{"type":"array","items":{"type":"string"},"description":"Known folders downloads, pictures, desktop, documents, videos, music, or full folder paths; empty for the current folder. When the user names 下载 / 图片 / 桌面 / 文档 / 视频 / 音乐, pass the known folder name even if the open folder has the same name, unless they point at the open folder (这里 / 当前文件夹)"},
   "prefer":{"type":"string","enum":["earliest","newest"],"description":"Which copy the last rule keeps (default earliest)"}}}}},
)JSON" R"JSON({"type":"function","function":{"name":"find_actions",
 "description":"Looks up Pulse's actions (its menu commands, panels, view options, settings) for run_actions, when what the user wants is not in run_actions' common list. Matches words in Chinese, pinyin or English. Returns up to 8, best first: id, title, args, the current value (now), whether it can run now (enabled false with why), its risk, and the user's shortcut. domain settings lists every setting with its key, values and current value.",
 "parameters":{"type":"object","properties":{
   "query":{"type":"string","description":"A few words naming the action, e.g. 详情面板 文件夹大小"},
   "domain":{"type":"string","enum":["settings"],"description":"settings: list all settings instead of searching"}}}}},
{"type":"function","function":{"name":"run_actions",
 "description":"Does things in Pulse itself, in order, as one undo group (at most 12 steps): panes and the folders in them, windows, tabs, view, sort, grouping, panels, selection, the staging tray, tags, favorites, new folders, opening files, settings, and any action find_actions returns. View, organize and settings steps act at once: Pulse shows each step and one result card with old -> new, Undo and the user's own shortcuts. Opening a program or script and system actions show the user a confirm card first (Pulse asks, not you). Files are never moved, renamed or deleted here: use organize_files, rename_files, run_task, find_cleanup or find_duplicates. Locked settings show the user why. Stops at the first step that fails; earlier steps stay done. Returns each step's outcome.\nCommon actions (args): window.layout {layout: single|side_by_side|stacked|three|four, panes: [a folder per pane, left or top first; \"\" leaves one]}, nav.go {path, pane}, window.new {path}, tab.new {path}, tab.close, tab.to_window, pane.to_window {pane}, window.merge, window.overview, nav.back, nav.up, view.mode {view: extra_large_icons|large_icons|medium_icons|small_icons|list|details|tiles|content, pane}, view.sort {by: name|modified|type|size|created, desc, pane}, view.group {by: none|name|date|type|size, pane}, sidebar.toggle {show}, favbar.toggle {show}, details.toggle {show}, select.items {pane, name, extensions, items, type: folder|document|image|video|audio|archive|code, modified: today|yesterday|this_week|last_7_days|this_month|last_month|{after, before}, size: {larger_than, smaller_than}, tag, selected, mode: replace|add|remove}, select.all, select.clear, tray.add {items}, tag.toggle {tag, items} (a new tag is made), fav.add {path or items}, file.new_folder {name} (in the open folder), nav.open {path} (a file or program), settings.set {key, value}, edit.undo. pane: 1-4 in screen order as in get_state, focused, target, or all (view / sort / group). Paths: absolute, a drive (D:) or a known folder (Downloads, Desktop, Documents, Pictures, Videos, Music). items: full paths; omitted = the selection.\nSettings (settings.set key and values; switches take true/false): theme light|dark|system, window_effect none|acrylic|mica|mica_alt, text_rendering auto|sharp|smooth, font_size small|default|large|larger, language system|zh-CN|zh-TW|en-US, menu_acrylic, row_height compact|standard|roomy, click_to_open double|single, name_highlight hover|always, hover_select, item_checkboxes, blank_double_click nothing|back|up, show_hidden, show_protected, folders_first top|follow|mixed, smart_dates, zebra_rows, size_bars, tag_name_color, selection_outline, thumbnail_app_badges, alias_mark, mtime_marks off|dots|bars|text, mtime_range today|3|7|30, mtime_dim_older, mtime_bold_today, mtime_folders, mtime_other_views, mtime_flash, onedrive_status, svn_status, startup_open last_tabs|default_location, new_tab_open current|default_location, external_open smart|tab|window, vertical_tabs, pinned_tab_names, double_click_closes_tab, close_window_with_last_tab, launch_at_signin (Windows), start_in_tray, keep_running, tray_icon always|background|never, explorer_tag_menu (Windows), open_folders_with_pulse (default file manager; Windows), staging_icon_size small|standard|large, staging_hide_when_empty, change_tracking, change_days today|3|7, lan_discovery, network_recent, hidden_shares, confirm_delete (can only be turned on), global_search (Windows), pinyin_search. Locked (only the user, in Settings): confirm_delete off, assistant_permissions, always_allow, connections, api_key.",
 "parameters":{"type":"object","properties":{
   "steps":{"type":"array","description":"The steps in order (at most 12)","items":{"type":"object","properties":{
     "action":{"type":"string","description":"An action id, e.g. view.sort"},
     "args":{"type":"object","description":"The action's arguments"},
     "wait":{"type":"string","enum":["listing_loaded","op_finished","preview_open"],"description":"Wait for this before the step (a selection waits for its folder by itself)"}},
    "required":["action"]}}},
  "required":["steps"]}}}
])JSON";
        json::Value v;
        json::Parse(kTools, v);
        return v;
    }();
    return tools;
}

std::string SystemPrompt() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char date[32];
    sprintf_s(date, "%04u-%02u-%02u", now.wYear, now.wMonth, now.wDay);
    std::string p =
        "You are Pulse Assistant (Pulse \xE5\x8A\xA9\xE6\x89\x8B), built into Pulse, a file manager for Windows. Today is ";
    p += date;
    p += ".\n"
         "You help the user find, understand and organize files with the tools provided.\n"
         "Rules:\n"
         "- Base every statement about files on tool results; never invent files, sizes or folders.\n"
         "- When the request is about this folder, the selection or what is open, call get_state first; "
         "use list_folder to see a folder's items.\n"
         "- To find files by name, type, date or size, call search_files once with the conditions (work out dates "
         "such as last month from today's date; without a place, search everywhere). Pulse lists the results itself, "
         "so answer with one short sentence saying how many were found. The card already shows the conditions, size and "
         "folders, so do not repeat them and never list files or paths.\n"
         "- To move items into folders call organize_files. Pulse shows the plan and asks the user itself, so do not "
         "ask for confirmation in text first, and never say anything was moved unless organize_files returned ok. "
         "If the user declines, do not propose the same plan again.\n"
         "- Before organize_files, write one short sentence that introduces the plan (what you found and how you "
         "will group it).\n"
         "- To rename files call rename_files with one rule (steps), never a list of names: Pulse reads dates taken "
         "itself, makes the names, shows them and asks the user. For photos by date use a template with {taken}, "
         "order taken, and daily when the user wants numbers per day. Copy words the user gave exactly. Before "
         "rename_files write one short sentence saying how you will name them; afterwards say how many were renamed "
         "and mention files that had no date taken. Never say files were renamed unless rename_files returned ok.\n"
         "- When one request has several steps (find files, then file them into folders and/or zip them), call "
         "run_task once with all of it instead of search_files or organize_files; Pulse builds the plan, asks the user "
         "and pauses on problems itself. Put every name word the user gave in find.name (e.g. both Pelican and 发票) and "
         "dates in modified_after / modified_before. Before run_task write one short sentence saying what you will do; afterwards "
         "say in one or two sentences what was done and name skipped files. Never say files were moved or zipped "
         "unless run_task returned ok.\n"
         "- When the user asks why a drive is full or what takes its space, call disk_usage. To free space call "
         "find_cleanup (after disk_usage when the question is about a drive); Pulse finds what is safe, shows it with "
         "checkboxes and asks the user itself. Before each write one short sentence; never list files or repeat the "
         "card's numbers. After find_cleanup say in one or two sentences what went to the Recycle Bin, and that it "
         "still takes space until the user clicks \xE6\xB0\xB8\xE4\xB9\x85\xE5\x88\xA0\xE9\x99\xA4\xE8\xBF\x99\xE4\xBA\x9B on the card. "
         "Never say space was freed unless Pulse told you so. Never suggest deleting system or program files.\n"
         "- To find or clean duplicate files call find_duplicates with the folders the user named (none: the current "
         "folder); prefer newest only when they ask to keep the newest. Pulse compares on this PC, picks what each group "
         "keeps, shows it and asks the user itself. Before it write one short sentence; never list the files or repeat "
         "the card's numbers. Afterwards say in one or two sentences how many copies went to the Recycle Bin, name a "
         "group where all copies stayed, and that the copies still take space until the user clicks "
         "\xE6\xB0\xB8\xE4\xB9\x85\xE5\x88\xA0\xE9\x99\xA4\xE8\xBF\x99\xE4\xBA\x9B on the card.\n"
         "- To change Pulse itself (panes and the folders in them, windows, tabs, view, sort, grouping, panels, "
         "selection, the staging tray, tags, favorites, new folders, opening a file or program, settings) call "
         "run_actions once with every step in order. Use its common actions; call find_actions first only for anything "
         "else (domain settings gives current setting values). For left/right or top/bottom folders use window.layout "
         "with panes; to pick files by name, type, date or size use select.items. View, organize and settings steps act "
         "at once (the card shows old -> new, Undo and the user's shortcuts), so never ask for confirmation first and "
         "never list the shortcuts; Pulse itself asks before running a program. Afterwards say in one short sentence "
         "what changed and, when useful, how to use it (e.g. drag files between the panes). If a step failed, say which "
         "and why and what was not done. Never say something changed unless run_actions returned it ok.\n"
         "- Some settings are locked (the delete confirmation cannot be turned off; your own permissions, the Always "
         "allow list, connections and API keys): still call run_actions with settings.set so Pulse shows the user why. "
         "Then, in two short sentences, say the step is there so a misunderstanding of yours can never delete their "
         "files (or that you cannot give yourself permissions), and that for moves and renames they can check Always "
         "allow on the confirm card. Add no other reasons.\n"
         "- You cannot delete, overwrite or read file contents yourself; only find_cleanup and find_duplicates move "
         "files to the Recycle Bin, and only after the user accepts in Pulse.\n"
         "- Reply in the user's language and keep it brief. Format with short paragraphs, lists whose lines start "
         "with \"- \", and **bold**. No headings, tables, code blocks or links.\n"
         "- Names of folders you create: short, in the user's language, no special characters.";
    return p;
}

std::string Body(AppState& s, const AiAgent& a) {
    json::Value body = json::Value::Object();
    body.set("model", Utf8(AiChatModelId(s)));
    json::Value messages = json::Value::Array();
    json::Value sys = json::Value::Object();
    sys.set("role", "system");
    sys.set("content", SystemPrompt());
    messages.push(std::move(sys));
    for (const json::Value& m : a.history.items()) messages.push(m);
    body.set("messages", std::move(messages));
    body.set("stream", true);
    json::Value so = json::Value::Object();
    so.set("include_usage", true);
    body.set("stream_options", std::move(so));
    body.set("tools", Tools());
    json::Value thinking = json::Value::Object();
    const int effort = std::clamp(s.aiAssistant.effort, 0, 3);
    thinking.set("type", effort == 0 ? "disabled" : "enabled");
    body.set("thinking", std::move(thinking));
    if (effort > 0) {
        static constexpr const char* kEffort[] = {"", "low", "high", "max"};
        body.set("reasoning_effort", kEffort[effort]);
    }
    return json::Serialize(body);
}

// ------------------------------------------------------------------ turn --

void SendRound(AppState& s);
void NextTool(AppState& s);
void Fail(AppState& s, AiChatError error);

AiBlockView* Block(AppState& s, uint32_t id) { return Chat(s).Find(id); }

void FoldThinking(AppState& s, AiAgent& a) {
    AiChat& c = Chat(s);
    AiBlockView* b = Block(s, a.think_id);
    if (b) {
        if (a.reasoning.empty()) {
            // Nothing was said: the block goes rather than reading 已思考 0 秒.
            c.blocks.erase(std::remove_if(c.blocks.begin(), c.blocks.end(),
                                          [&](const AiBlockView& v) { return v.id == a.think_id; }),
                           c.blocks.end());
        } else {
            const uint64_t secs = std::max<uint64_t>(1, (Now() - c.think_start + 500) / 1000);
            b->running = false;
            b->streaming = false;
            b->label = Fill(Text(I::AiAsstThought), L"{n}", std::to_wstring(secs)) + L" \u00B7 " + AiChatEffortName(s);
            b->meta.clear();
            b->seconds.clear();
        }
    }
    if (c.think_id == a.think_id) c.think_id = 0;
    a.think_id = 0;
}

// The steps so far fold into one line once the model speaks again.
void FoldSteps(AppState& s, AiAgent& a) {
    AiBlockView* b = Block(s, a.steps_id);
    if (b && !b->collapsed) {
        int n = static_cast<int>(b->steps.size());
        // A search is the point of the turn; context reads are just groundwork.
        if (a.did_search)
            b->label = Fill(Text(I::AiQStepsLabel), L"{n}", Num(a.searched));
        else if (!a.read_folder.empty())
            b->label = Fill(Fill(Text(I::AiAsstStepsRead), L"{folder}", a.read_folder), L"{n}", Num(a.read_files));
        else
            b->label = Fill(Text(I::AiAsstStepsUsed), L"{n}", std::to_wstring(n));
        b->collapsed = true;
        b->open = false;
    }
    a.steps_id = 0;
}

ui::AiStepView& AddStep(AppState& s, AiAgent& a, ui::AiIcon icon, std::wstring title, std::wstring arg,
                        std::wstring sub) {
    AiBlockView* b = Block(s, a.steps_id);
    if (!b) {
        AiBlockView& nb = Chat(s).Add(AiBlockKind::Steps, Now());
        a.steps_id = nb.id;
        b = &nb;
    }
    ui::AiStepView st;
    st.icon = icon;
    st.title = std::move(title);
    st.arg = std::move(arg);
    st.sub = std::move(sub);
    st.born_ms = Now();
    b->steps.push_back(std::move(st));
    return b->steps.back();
}

ui::AiStepView* LastStep(AppState& s, AiAgent& a) {
    AiBlockView* b = Block(s, a.steps_id);
    return b && !b->steps.empty() ? &b->steps.back() : nullptr;
}

void EndStep(AppState& s, AiAgent& a, bool ok, std::wstring sub) {
    if (ui::AiStepView* st = LastStep(s, a)) {
        st->state = ok ? ui::AiStepState::Done : ui::AiStepState::Failed;
        st->done_ms = Now();
        st->progress = -1.0f;
        st->sub = std::move(sub);
    }
}

void AddToolResult(AiAgent& a, const std::string& id, const ToolOutcome& out) {
    json::Value result = json::Value::Object();
    if (out.ok) {
        result = out.data.is_null() ? json::Value::Object() : out.data;
    } else {
        result.set("ok", false);
        json::Value err = json::Value::Object();
        err.set("code", out.error.code);
        err.set("message", out.error.message);
        result.set("error", std::move(err));
    }
    std::string text = json::Serialize(result);
    if (text.size() > kMaxToolText) {
        // Cut on a character boundary and say so; the model can page.
        size_t cut = kMaxToolText;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
        text.resize(cut);
        text += "... [truncated: ask for a smaller page]";
    }
    json::Value msg = json::Value::Object();
    msg.set("role", "tool");
    msg.set("tool_call_id", id);
    msg.set("content", std::move(text));
    a.history.push(std::move(msg));
}

// Runs `work` on a worker thread; the queue polls until it is done.
void RunJob(AppState& s, std::function<ToolOutcome(const std::atomic<bool>&)> work,
            std::function<void(AppState&, const ToolOutcome&)> done) {
    AiAgent& a = Agent(s);
    auto job = std::make_shared<AiAgent::Job>();
    a.job = job;
    std::thread([job, work = std::move(work)] {
        ToolOutcome out = work(job->cancelled);
        job->out = std::move(out);
        job->done.store(true);
    }).detach();
    const uint64_t gen = a.generation;
    const uint64_t started = Now();
    AiChatPush(s, 0, [job, gen, started, done = std::move(done)](AppState& st) {
        AiAgent& ag = Agent(st);
        if (ag.generation != gen) return -1;
        // Keep the running step on screen long enough to read.
        if (!job->done.load() || Now() - started < 450) return 40;
        ag.job.reset();
        done(st, job->out);
        return -1;
    });
}

// --------------------------------------------------------------- tools --

void ToolGetState(AppState& s, const app::ai::ModelToolCall& call) {
    AiAgent& a = Agent(s);
    AddStep(s, a, ui::AiIcon::Window, Text(I::AiAsstStepWindow), {}, Text(I::AiAsstStepWindowRun));
    json::Value out = app::actions::StateSnapshot(s);
    // The step card reads the focused pane, as before.
    std::wstring sub;
    app::Tab* tab = ActiveTab(s);
    const std::wstring folder = tab ? tab->current_path : std::wstring();
    if (!tab || folder.empty() || fs::IsVirtualPath(folder)) {
        sub = Text(I::AiAsstStepWindowNone);
    } else {
        const size_t picked = static_cast<size_t>((std::max)(0, tab->SelectedCount()));
        const std::wstring label = FolderLabel(folder);
        sub = picked == 0
                  ? Fill(Fill(Text(I::AiAsstStepWindowDone), L"{folder}", label), L"{n}", Num(tab->EntryCount()))
                  : Fill(Fill(Fill(Text(I::AiAsstStepWindowSel), L"{folder}", label), L"{n}", Num(tab->EntryCount())),
                         L"{k}", Num(picked));
    }
    ToolOutcome result;
    result.data = std::move(out);
    AddToolResult(a, call.id, result);
    const uint64_t gen = a.generation;
    AiChatPush(s, 450, [gen, sub](AppState& st) {
        AiAgent& ag = Agent(st);
        if (ag.generation != gen) return -1;
        EndStep(st, ag, true, sub);
        ++ag.call_index;
        NextTool(st);
        return -1;
    });
}

void ToolListFolder(AppState& s, const app::ai::ModelToolCall& call, json::Value args) {
    AiAgent& a = Agent(s);
    const std::wstring path = json::Wide(args.get_string("path"));
    const std::wstring label = path.empty() ? std::wstring() : FolderLabel(path);
    AddStep(s, a, ui::AiIcon::Rows, Text(I::AiAsstStepRead), label, Text(I::AiAsstStepReadWait));
    AiChatSetChip(Chat(s), ui::StatusAiState::Busy, Text(I::AiAsstChipReadingAny), 0.3f);
    if (!args.find("limit")) args.set("limit", 200);
    const std::string id = call.id;
    RunJob(s, [args](const std::atomic<bool>& cancelled) { return app::ai::ListFolder(args, cancelled); },
           [id, label, path](AppState& st, const ToolOutcome& out) {
               AiAgent& ag = Agent(st);
               std::wstring sub;
               if (out.ok) {
                   size_t files = 0, dirs = 0;
                   uint64_t bytes = 0;
                   if (const json::Value* items = out.data.find("items"); items && items->is_array()) {
                       for (const json::Value& it : items->items()) {
                           if (it.get_string("type") == "folder") { ++dirs; continue; }
                           ++files;
                           if (const json::Value* sz = it.find("size")) bytes += static_cast<uint64_t>(sz->as_number());
                       }
                   }
                   sub = dirs ? Fill(Fill(Fill(Text(I::AiAsstStepReadDoneDirs), L"{n}", Num(files)), L"{d}", Num(dirs)),
                                     L"{size}", AiChatFormatSize(bytes))
                              : Fill(Fill(Text(I::AiAsstStepReadDone), L"{n}", Num(files)), L"{size}",
                                     AiChatFormatSize(bytes));
                   ag.read_folder = label;
                   ag.read_files = files;
               } else {
                   sub = Fill(Text(I::AiAsstStepFailed), L"{reason}", json::Wide(out.error.message));
               }
               AddToolResult(ag, id, out);
               ++ag.call_index;
               // The folder is on screen: a spark walks the rows it read, the
               // step and the chip count along (scene 3).
               std::unordered_set<std::wstring> names;
               if (const json::Value* items = out.ok ? out.data.find("items") : nullptr; items && items->is_array())
                   for (const json::Value& it : items->items())
                       names.insert(ui::AiListLower(json::Wide(it.get_string("name"))));
               names.erase(std::wstring());
               if (names.empty() || !Shown(st, path)) {
                   EndStep(st, ag, out.ok, sub);
                   AiChatSetChip(Chat(st), ui::StatusAiState::Busy, Text(I::AiAsstThinking));
                   AiChatPush(st, 250, [](AppState& s2) { NextTool(s2); return -1; });
                   return;
               }
               ui::AiListOverlay& o = ListOverlay(path);
               const uint64_t n = names.size();
               o.read_total = names.size();
               o.read_names = std::move(names);
               o.read_ms = (std::min<uint64_t>)(185, (std::max<uint64_t>)(12, 2400 / n)) * n;
               o.read_start = Now() + 300;   // the title pill comes in first
               o.watch_text = Text(I::AiListWatch);
               std::shared_ptr<const ui::AiListOverlay> ov = g_list;
               const uint64_t gen = ag.generation;
               const bool ok = out.ok;
               AiChatPush(st, 0, [ov, gen, ok, sub](AppState& s2) {
                   AiAgent& a2 = Agent(s2);
                   if (a2.generation != gen) return -1;
                   const uint64_t now = Now();
                   if (const size_t k = ui::AiListReadK(*ov, now)) {
                       const std::wstring kk = Num(k), nn = Num(ov->read_total);
                       if (ui::AiStepView* step = LastStep(s2, a2))
                           step->sub = Fill(Fill(Text(I::AiAsstStepReading), L"{k}", kk), L"{n}", nn);
                       AiChatSetChip(Chat(s2), ui::StatusAiState::Busy,
                                     Fill(Fill(Text(I::AiAsstChipReading), L"{k}", kk), L"{n}", nn),
                                     static_cast<float>(k) / static_cast<float>(ov->read_total));
                   }
                   if (now < ov->read_start + ov->read_ms + 300) return 50;
                   EndStep(s2, a2, ok, sub);
                   AiChatSetChip(Chat(s2), ui::StatusAiState::Busy, Text(I::AiAsstThinking));
                   AiChatPush(s2, 350, [](AppState& s3) { NextTool(s3); return -1; });
                   return -1;
               });
           });
}

void ToolGetProperties(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
    AiAgent& a = Agent(s);
    size_t n = 0;
    std::wstring arg;
    if (const json::Value* paths = args.find("paths"); paths && paths->is_array()) {
        n = paths->size();
        if (n) arg = Leaf(json::Wide(paths->at(0).as_string()));
        if (n > 1) arg += L" +" + Num(n - 1);
    }
    AddStep(s, a, ui::AiIcon::Eye, Text(I::AiAsstStepProps), arg,
            Fill(Text(I::AiAsstStepPropsRun), L"{n}", Num(n)));
    const std::string id = call.id;
    RunJob(s, [args](const std::atomic<bool>& cancelled) { return app::ai::GetProperties(args, cancelled); },
           [id, n](AppState& st, const ToolOutcome& out) {
               AiAgent& ag = Agent(st);
               EndStep(st, ag, out.ok,
                       out.ok ? Fill(Text(I::AiAsstStepPropsDone), L"{n}", Num(n))
                              : Fill(Text(I::AiAsstStepFailed), L"{reason}", json::Wide(out.error.message)));
               AddToolResult(ag, id, out);
               ++ag.call_index;
               AiChatPush(st, 250, [](AppState& s2) { NextTool(s2); return -1; });
           });
}

// ------------------------------------------------------- search_files --
// Flow A (plans/ai/Pulse_AI助手_第一批扩展.html): the model only turns the
// sentence into conditions; Pulse's own search runs them in the active tab
// (sortable, previewable, Back returns to the folder) and the 搜索条件 card
// follows the answer. A chip's × and + 条件 search again locally: no model.

struct SearchStats {
    size_t total = 0;
    uint64_t bytes = 0;
    bool dirs = false;
    std::vector<std::pair<std::wstring, size_t>> folders;   // parent, count; most first
    std::vector<const fs::DirEntry*> items;                  // the page Pulse loaded
};

SearchStats StatsOf(const app::Tab& tab) {
    SearchStats st;
    std::unordered_map<std::wstring, size_t> at;   // lower-case parent -> index in folders
    if (tab.search_entries) {
        for (const fs::DirEntry& e : *tab.search_entries) {
            st.items.push_back(&e);
            if (e.is_dir) st.dirs = true;
            else st.bytes += e.size;
            const std::wstring parent = app::ai::ParentFolder(NormDir(e.full_path));
            const std::wstring key = ui::AiListLower(parent);
            auto it = at.find(key);
            if (it == at.end()) {
                at.emplace(key, st.folders.size());
                st.folders.push_back({parent, 1});
            } else {
                ++st.folders[it->second].second;
            }
        }
    }
    st.total = (std::max)(static_cast<size_t>(tab.search_total), st.items.size());
    std::stable_sort(st.folders.begin(), st.folders.end(),
                     [](const auto& x, const auto& y) { return x.second > y.second; });
    return st;
}

// A results folder as the card names it: 下载, or 文档 › 财务 inside a known
// folder.
std::wstring ResultFolderLabel(const std::wstring& folder) {
    const std::wstring label = FolderLabel(folder);
    if (label != Leaf(folder)) return label;
    const std::wstring parent = app::ai::ParentFolder(folder);
    if (parent.empty()) return label;
    const std::wstring up = FolderLabel(parent);
    return up != Leaf(parent) ? up + L" \u203A " + label : label;
}

std::wstring Secs(uint64_t ms) {
    wchar_t buf[16];
    swprintf_s(buf, L"%.2f", static_cast<double>((std::max<uint64_t>)(ms, 10)) / 1000.0);
    return buf;
}

std::wstring SearchRest(const std::wstring& path) {
    std::wstring kind, rest;
    return app::ParsePulsePath(path, &kind, &rest) && kind == L"search" ? rest : std::wstring();
}

// Calls `done` once Pulse's search of `path` in the active tab has its first
// page (nullptr: the tab went elsewhere or the index did not answer).
void AwaitSearch(AppState& s, const std::wstring& path, std::function<void(AppState&, const app::Tab*, uint64_t)> done) {
    const uint64_t started = Now();
    AiChatPush(s, 0, [path, started, done = std::move(done)](AppState& st) {
        app::Tab* tab = ActiveTab(st);
        const bool here = tab && tab->current_path == path;
        if (here && !tab->loading && tab->pending_generation == 0) {
            done(st, tab, Now() - started);
            return -1;
        }
        if (!here || Now() - started > 15000) {
            done(st, nullptr, Now() - started);
            return -1;
        }
        return 16;
    });
}

AiAgent::QueryCard* CardOf(AiAgent& a, uint32_t block_id, bool follow) {
    for (auto& q : a.queries)
        if ((follow ? q->follow_id : q->block_id) == block_id) return q.get();
    return nullptr;
}

// The chips of `q.spec` (new ones pop in one after another).
void FillChips(AppState& s, AiAgent::QueryCard& q, bool pop) {
    AiBlockView* b = Block(s, q.block_id);
    if (!b) return;
    const std::vector<app::ai::SearchChip> chips = app::ai::SearchChips(q.spec, &ResultFolderLabel);
    size_t removable = 0;
    for (const auto& c : chips)
        if (!c.fixed && c.field != app::ai::SearchField::Folder) ++removable;
    std::vector<ui::AiQueryChipView> views;
    q.fields.clear();
    uint64_t at = Now() + 120;
    for (const auto& c : chips) {
        ui::AiQueryChipView v;
        v.key = c.key;
        v.value = c.value;
        // The last condition stays: without it the search would list everything.
        v.fixed = c.fixed || (removable <= 1 && c.field != app::ai::SearchField::Folder);
        bool known = false;
        for (const ui::AiQueryChipView& old : b->chips)
            if (!old.out_ms && old.key == v.key && old.value == v.value) known = true;
        if (pop && !known) {
            v.in_ms = at;
            at += 90;
        }
        views.push_back(std::move(v));
        q.fields.push_back(c.field);
    }
    b->chips = std::move(views);
}

void FillCounts(AppState& s, AiAgent::QueryCard& q, const SearchStats& st, uint64_t ms, bool rerun) {
    q.count = st.total;
    q.paths.clear();
    for (const fs::DirEntry* e : st.items) q.paths.push_back(NormDir(e->full_path));
    if (AiBlockView* b = Block(s, q.block_id)) {
        b->title = Text(I::AiQTitle);
        b->meta = Fill(Text(I::AiQMeta), L"{t}", Secs(ms));
        b->add = Text(I::AiQAdd);
        const std::wstring count = Num(st.total);
        if (rerun && b->count != count) b->bump_ms = Now();
        b->count = count;
        b->label = Fill(Text(st.dirs ? I::AiQItems : I::AiQFiles), L"{size}", AiChatFormatSize(st.bytes));
        if (rerun) {
            if (!b->re_ms) b->re_ms = Now();
            b->hint = Text(I::AiQRerun);
        }
        b->tags.clear();
        size_t shown = 0, rest = 0;
        for (const auto& [folder, n] : st.folders) {
            if (shown < 4) {
                ui::AiTagView t;
                t.text = ResultFolderLabel(folder);
                t.bold = Num(n);
                b->tags.push_back(std::move(t));
                ++shown;
            } else {
                rest += n;
            }
        }
        if (rest) {
            ui::AiTagView t;
            t.text = Text(I::AiQOther);
            t.bold = Num(rest);
            b->tags.push_back(std::move(t));
        }
    }
    if (AiBlockView* f = Block(s, q.follow_id); f && !f->follow.empty())
        f->follow[0].text = Fill(Text(I::AiFollowZip), L"{n}", Num(st.total));
}

json::Value SearchResultForModel(const AiAgent::QueryCard& q, const SearchStats& st) {
    json::Value out = json::Value::Object();
    out.set("query", Utf8(SearchRest(q.path)));
    out.set("shown_in", "the active Pulse tab, as a search the user can sort, preview and edit");
    out.set("total", static_cast<uint64_t>(st.total));
    out.set("total_size", st.bytes);
    json::Value folders = json::Value::Array();
    for (size_t i = 0; i < st.folders.size() && i < 8; ++i) {
        json::Value f = json::Value::Object();
        f.set("folder", Utf8(st.folders[i].first));
        f.set("count", static_cast<uint64_t>(st.folders[i].second));
        folders.push(std::move(f));
    }
    out.set("folders", std::move(folders));
    json::Value items = json::Value::Array();
    for (size_t i = 0; i < st.items.size() && i < 40; ++i) {
        const fs::DirEntry& e = *st.items[i];
        json::Value it = json::Value::Object();
        it.set("path", Utf8(NormDir(e.full_path)));
        it.set("type", e.is_dir ? "folder" : "file");
        if (!e.is_dir) it.set("size", e.size);
        const uint64_t ft = (static_cast<uint64_t>(e.mtime.dwHighDateTime) << 32) | e.mtime.dwLowDateTime;
        if (const std::string t = app::ai::IsoTime(ft); !t.empty()) it.set("modified", t);
        items.push(std::move(it));
    }
    out.set("items", std::move(items));
    out.set("note", "Pulse already shows these results and the conditions as chips the user can edit. Do not list "
                    "the files, folders, paths or size; say in one short sentence how many were found.");
    return out;
}

void ToolSearch(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
    AiAgent& a = Agent(s);
    auto q = std::make_shared<AiAgent::QueryCard>();
    ToolOutcome error;
    std::wstring arg = json::Wide(args.get_string("label"));
    if (arg.size() > 40) arg = arg.substr(0, 40);
    AddStep(s, a, ui::AiIcon::Search, Text(I::AiQStep), arg, Text(I::AiQStepRun));
    if (!app::ai::SearchSpecFromArgs(args, q->spec, error) || !s.appPrefs.search_enabled) {
        if (!s.appPrefs.search_enabled && error.ok)
            error = ToolOutcome::Fail("unsupported", "Search is turned off in Pulse's settings, so nothing can be found by name.");
        EndStep(s, a, false, Fill(Text(I::AiAsstStepFailed), L"{reason}", json::Wide(error.error.message)));
        AddToolResult(a, call.id, error);
        ++a.call_index;
        AiChatPush(s, 250, [](AppState& s2) { NextTool(s2); return -1; });
        return;
    }
    q->label = arg;
    q->path = app::MakeSearchPath(app::CompileSearchQuery(q->spec));
    AiChatSetChip(Chat(s), ui::StatusAiState::Busy, Text(I::AiQChipSearching), 0.4f);
    NavigateTo(s, q->path);
    const std::string id = call.id;
    const uint64_t gen = a.generation;
    const uint64_t shown_at = Now();
    AwaitSearch(s, q->path, [q, id, gen, shown_at](AppState& st, const app::Tab* tab, uint64_t ms) {
        AiAgent& ag = Agent(st);
        if (ag.generation != gen) return;
        if (!tab) {
            EndStep(st, ag, false, Fill(Text(I::AiAsstStepFailed), L"{reason}", Text(I::AiQNoAnswer)));
            AddToolResult(ag, id, ToolOutcome::Fail("timeout", "Pulse's search did not finish (the tab went elsewhere or the index did not answer).", true));
        } else {
            const SearchStats stats = StatsOf(*tab);
            q->count = stats.total;
            q->ms = ms;
            ToolOutcome out;
            out.data = SearchResultForModel(*q, stats);
            AddToolResult(ag, id, out);
            EndStep(st, ag, true, Fill(Fill(Text(I::AiQStepDone), L"{n}", Num(stats.total)), L"{t}", Secs(ms)));
            AiChatSetChip(Chat(st), ui::StatusAiState::Busy, Text(I::AiQChipSearching), 1.0f);
            ag.pending_query = q;
            ag.searched = stats.total;
            ag.did_search = true;
        }
        ++ag.call_index;
        // Keep the running step on screen long enough to read.
        const uint64_t held = Now() - shown_at;
        AiChatPush(st, held < 450 ? static_cast<uint32_t>(450 - held) : 120u, [](AppState& s2) {
            AiChatSetChip(Chat(s2), ui::StatusAiState::Busy, Text(I::AiAsstThinking));
            NextTool(s2);
            return -1;
        });
    });
}

// The turn's search card and 接着做 go under the answer (EndTurn).
void AddQueryCard(AppState& s, AiAgent& a) {
    std::shared_ptr<AiAgent::QueryCard> q = std::move(a.pending_query);
    a.pending_query.reset();
    if (!q) return;
    AiChat& c = Chat(s);
    AiBlockView& b = c.Add(AiBlockKind::Query, Now());
    q->block_id = b.id;
    AiBlockView& f = c.Add(AiBlockKind::Follow, Now() + 500);
    q->follow_id = f.id;
    f.label = Text(I::AiFollowLabel);
    f.follow = {{ui::AiIcon::Package, Fill(Text(I::AiFollowZip), L"{n}", Num(q->count))},
                {ui::AiIcon::Move, Text(I::AiFollowMonth)},
                {ui::AiIcon::Rename, Text(I::AiFollowRename)},
                {ui::AiIcon::Copy, Text(I::AiFollowCopy)}};
    FillChips(s, *q, true);
    // The card shows what the tab shows now (the user may have moved on).
    app::Tab* tab = ActiveTab(s);
    SearchStats stats;
    if (tab && tab->current_path == q->path) stats = StatsOf(*tab);
    else stats.total = q->count;
    FillCounts(s, *q, stats, q->ms, false);
    c.peek_text = Fill(Text(I::AiQPeek), L"{n}", Num(q->count));
    a.queries.push_back(std::move(q));
    if (a.queries.size() > 8) a.queries.erase(a.queries.begin());
}

// The card's search changed in Pulse: show it in the tab, count again, and
// tell the model with the next prompt.
void Research(AppState& s, AiAgent::QueryCard& q) {
    const uint32_t block = q.block_id;
    q.path = app::MakeSearchPath(app::CompileSearchQuery(q.spec));
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->current_path != q.path) NavigateTo(s, q.path);
    AiChatSetChip(Chat(s), ui::StatusAiState::Busy, Text(I::AiQChipSearching), 0.5f);
    const std::wstring path = q.path;
    AwaitSearch(s, path, [block, path](AppState& st, const app::Tab* t, uint64_t ms) {
        AiAgent& ag = Agent(st);
        AiAgent::QueryCard* card = CardOf(ag, block, false);
        AiChat& c = Chat(st);
        if (!card || card->path != path) return;
        if (!t) {
            AiChatSetChip(c, ui::StatusAiState::Idle, L"");
            return;
        }
        const SearchStats stats = StatsOf(*t);
        FillCounts(st, *card, stats, ms, true);
        if (!c.busy) AiChatSetChip(c, ui::StatusAiState::Done, Fill(Text(I::AiQChipFound), L"{n}", Num(stats.total)));
        ag.pending_note = L"(In Pulse the user changed the search conditions locally; the search is now \u201C" +
                          SearchRest(path) + L"\u201D with " + Num(stats.total) +
                          L" results. Follow-up requests about \u201Cthese files\u201D mean these results.)";
        Redraw(st);
    });
}

} // namespace

void AiAgentQueryChip(AppState& s, int block, int chip) {
    AiChat& c = Chat(s);
    if (!c.agent || block < 0 || block >= static_cast<int>(c.blocks.size())) return;
    AiBlockView& b = c.blocks[static_cast<size_t>(block)];
    AiAgent::QueryCard* q = CardOf(*c.agent, b.id, false);
    if (!q || chip < 0 || chip >= static_cast<int>(b.chips.size()) || chip >= static_cast<int>(q->fields.size())) return;
    ui::AiQueryChipView& v = b.chips[static_cast<size_t>(chip)];
    if (v.fixed || v.out_ms) return;
    v.out_ms = Now();
    app::ai::ClearSearchField(q->spec, q->fields[static_cast<size_t>(chip)]);
    // Search first (the count bumps while the chip shrinks), then the chip
    // leaves; a cleared folder becomes 所有位置.
    Research(s, *q);
    const uint32_t id = b.id;
    AiChatPush(s, 0, [id](AppState& st) {
        AiBlockView* card = Chat(st).Find(id);
        if (!card) return -1;
        bool waiting = false;
        for (const auto& v2 : card->chips)
            if (v2.out_ms && Now() - v2.out_ms < 460) waiting = true;
        if (waiting) return 30;
        if (AiAgent::QueryCard* q2 = Chat(st).agent ? CardOf(*Chat(st).agent, id, false) : nullptr)
            FillChips(st, *q2, true);
        Redraw(st);
        return -1;
    });
    Redraw(s);
}

void AiAgentQueryAdd(AppState& s, int block) {
    AiChat& c = Chat(s);
    if (!c.agent || block < 0 || block >= static_cast<int>(c.blocks.size())) return;
    AiAgent::QueryCard* q = CardOf(*c.agent, c.blocks[static_cast<size_t>(block)].id, false);
    if (!q) return;
    const uint32_t id = q->block_id;
    // Pulse's own advanced search, filled with the card's search.
    if (app::Tab* tab = ActiveTab(s); !tab || tab->current_path != q->path) NavigateTo(s, q->path);
    ShowAdvancedSearch(s);
    AiAgent::QueryCard* card = Chat(s).agent ? CardOf(*Chat(s).agent, id, false) : nullptr;
    app::Tab* tab = ActiveTab(s);
    if (!card || !tab || tab->current_path == card->path) return;
    const std::wstring rest = SearchRest(tab->current_path);
    if (rest.empty()) return;
    card->spec = app::ai::SearchSpecFromQuery(rest);
    FillChips(s, *card, true);
    Research(s, *card);
    Redraw(s);
}

void AiAgentFollow(AppState& s, int block, int item) {
    AiChat& c = Chat(s);
    if (!c.agent || block < 0 || block >= static_cast<int>(c.blocks.size())) return;
    AiBlockView& f = c.blocks[static_cast<size_t>(block)];
    AiAgent::QueryCard* q = CardOf(*c.agent, f.id, true);
    if (!q || item < 0 || item >= static_cast<int>(f.follow.size())) return;
    switch (item) {
        case 0: AiAssistantFillComposer(s, Fill(Text(I::AiFollowZipPrompt), L"{n}", Num(q->count))); break;
        case 1: AiAssistantFillComposer(s, Text(I::AiFollowMonthPrompt)); break;
        case 2: AiAssistantFillComposer(s, Text(I::AiFollowRenamePrompt)); break;
        default: {
            // 复制路径 needs no model: the results' paths, one per line.
            std::wstring text;
            for (const std::wstring& p : q->paths) text += (text.empty() ? L"" : L"\r\n") + p;
            if (text.empty()) break;
            AiChatCopyText(s.hwnd, text);
            const std::wstring back = f.follow[3].text;
            f.follow[3].icon = ui::AiIcon::Check;
            f.follow[3].text = Fill(Text(I::AiFollowCopied), L"{n}", Num(q->paths.size()));
            const uint32_t id = f.id;
            AiChatPush(s, 1600, [id, back](AppState& st) {
                if (AiBlockView* fb = Chat(st).Find(id); fb && fb->follow.size() > 3) {
                    fb->follow[3].icon = ui::AiIcon::Copy;
                    fb->follow[3].text = back;
                }
                Redraw(st);
                return -1;
            });
            break;
        }
    }
    Redraw(s);
}

namespace {

// organize_files after validation.
struct OrganizePlan {
    std::wstring folder;
    std::wstring label;   // FolderLabel(folder)
    struct Group {
        std::wstring name;
        std::wstring path;
        bool exists = false;
        std::vector<std::wstring> items;   // names
    };
    std::vector<Group> groups;
    size_t total = 0;
    size_t created = 0;   // groups whose folder is new
    uint64_t undo_group = 0;
    std::vector<std::wstring> sources;   // every moved item's full path
};

bool ValidName(const std::wstring& name) {
    if (name.empty() || name.size() > 80 || name == L"." || name == L"..") return false;
    for (wchar_t ch : name)
        if (ch < 32 || wcschr(L"\\/:*?\"<>|", ch)) return false;
    if (name.back() == L'.' || name.back() == L' ') return false;
    static const wchar_t* kReserved[] = {L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4",
                                         L"LPT1", L"LPT2", L"LPT3"};
    std::wstring stem = name.substr(0, name.find(L'.'));
    for (const wchar_t* r : kReserved)
        if (_wcsicmp(stem.c_str(), r) == 0) return false;
    return true;
}

std::wstring Trim(std::wstring v) {
    while (!v.empty() && std::iswspace(v.back())) v.pop_back();
    size_t i = 0;
    while (i < v.size() && std::iswspace(v[i])) ++i;
    return v.substr(i);
}

bool PlanOrganize(const json::Value& args, OrganizePlan& plan, ToolOutcome& error) {
    if (!app::ai::CheckPathArg(args.find("folder") ? *args.find("folder") : json::Value(), "folder", plan.folder, error))
        return false;
    if (!IsDirectory(plan.folder)) {
        error = ToolOutcome::Fail("not_found", "folder does not exist or is not a folder");
        return false;
    }
    if (app::ai::IsSystemFolder(plan.folder)) {
        error = ToolOutcome::Fail("forbidden", "Pulse does not reorganize system folders");
        return false;
    }
    const json::Value* groups = args.find("groups");
    if (!groups || !groups->is_array() || groups->size() == 0 || groups->size() > 30) {
        error = ToolOutcome::Fail("invalid_args", "groups must list 1-30 destination folders");
        return false;
    }
    std::vector<std::wstring> seen;   // lower-cased item names already planned
    for (const json::Value& g : groups->items()) {
        OrganizePlan::Group group;
        group.name = Trim(json::Wide(g.get_string("name")));
        if (!ValidName(group.name)) {
            error = ToolOutcome::Fail("invalid_args", "invalid folder name: " + Utf8(group.name));
            return false;
        }
        for (const OrganizePlan::Group& other : plan.groups)
            if (_wcsicmp(other.name.c_str(), group.name.c_str()) == 0) {
                error = ToolOutcome::Fail("invalid_args", "two groups share the name " + Utf8(group.name));
                return false;
            }
        group.path = JoinPath(plan.folder, group.name);
        group.exists = IsDirectory(group.path);
        if (!group.exists && Exists(group.path)) {
            error = ToolOutcome::Fail("conflict", "a file named " + Utf8(group.name) + " already exists in the folder");
            return false;
        }
        const json::Value* items = g.find("items");
        if (!items || !items->is_array()) {
            error = ToolOutcome::Fail("invalid_args", "each group needs items");
            return false;
        }
        for (const json::Value& it : items->items()) {
            std::wstring name = Trim(json::Wide(it.as_string()));
            // A full path inside the folder is fine too.
            if (name.size() > plan.folder.size() && _wcsnicmp(name.c_str(), plan.folder.c_str(), plan.folder.size()) == 0) {
                std::wstring rest = name.substr(plan.folder.size());
                if (!rest.empty() && (rest[0] == L'\\' || rest[0] == L'/')) rest.erase(0, 1);
                name = rest;
            }
            if (name.empty() || name.find_first_of(L"\\/") != std::wstring::npos || name == L"." || name == L"..") {
                error = ToolOutcome::Fail("invalid_args", "items must be names directly inside folder: " + Utf8(name));
                return false;
            }
            if (_wcsicmp(name.c_str(), group.name.c_str()) == 0) continue;   // the folder itself
            bool is_target = false;
            for (const json::Value& g2 : groups->items())
                if (_wcsicmp(Trim(json::Wide(g2.get_string("name"))).c_str(), name.c_str()) == 0) is_target = true;
            if (is_target) continue;   // a destination folder never moves
            std::wstring lower = name;
            for (wchar_t& ch : lower) ch = static_cast<wchar_t>(std::towlower(ch));
            if (std::find(seen.begin(), seen.end(), lower) != seen.end()) continue;
            if (!Exists(JoinPath(plan.folder, name))) {
                error = ToolOutcome::Fail("not_found", "no item named " + Utf8(name) + " in the folder");
                return false;
            }
            seen.push_back(lower);
            group.items.push_back(name);
        }
        if (group.items.empty()) continue;
        plan.total += group.items.size();
        if (!group.exists) ++plan.created;
        plan.groups.push_back(std::move(group));
    }
    if (plan.groups.empty()) {
        error = ToolOutcome::Fail("invalid_args", "nothing to move");
        return false;
    }
    if (plan.total > kMaxOrganize) {
        error = ToolOutcome::Fail("too_many", "at most 2000 items per call");
        return false;
    }
    plan.label = FolderLabel(plan.folder);
    for (const auto& g : plan.groups)
        for (const auto& name : g.items) plan.sources.push_back(JoinPath(plan.folder, name));
    return true;
}

std::wstring GroupsText(const OrganizePlan& plan, I all_new, I any) {
    return Fill(Fill(Text(plan.created == plan.groups.size() ? all_new : any), L"{n}", Num(plan.total)), L"{g}",
                Num(plan.groups.size()));
}

void SettleOrganize(AppState& s, std::shared_ptr<OrganizePlan> plan, const std::string& call_id, size_t moved);

// One request of the sort: a new folder, one item, or a whole group.
struct OrganizeAct {
    enum Kind { Create, MoveOne, MoveGroup } kind = Create;
    size_t group = 0;
    size_t item = 0;
    uint32_t delay = 0;   // ms after the previous one
};

void SubmitAct(AppState& s, const OrganizePlan& plan, const OrganizeAct& act, const std::wstring& undo_label,
               ui::AiListOverlay* ov) {
    const OrganizePlan::Group& g = plan.groups[act.group];
    const uint64_t now = Now();
    ops::OpRequest r;
    r.undo_group = plan.undo_group;   // one undo step for the whole sort, like 合并到文件夹
    r.undo_label = undo_label;
    if (act.kind == OrganizeAct::Create) {
        r.type = ops::OpType::CreateFolder;
        r.sources.push_back(g.path);
        if (ov) ov->new_folders[ui::AiListLower(g.name)] = now;
    } else {
        r.type = ops::OpType::Move;
        r.dest_dir = g.path;
        r.collision_policy = ops::CollisionPolicy::KeepBoth;
        const size_t first = act.kind == OrganizeAct::MoveOne ? act.item : 0;
        const size_t last = act.kind == OrganizeAct::MoveOne ? act.item + 1 : g.items.size();
        for (size_t i = first; i < last; ++i) {
            r.sources.push_back(JoinPath(plan.folder, g.items[i]));
            if (ov) ov->leaving[ui::AiListLower(g.items[i])] = now;
        }
        // The folder bumps as the card lands (organizeAnim: +470 ms).
        if (ov) ov->bumps[ui::AiListLower(g.name)].push_back(now + 470);
    }
    s.ops.Submit(std::move(r));
}

void RunOrganize(AppState& s, std::shared_ptr<OrganizePlan> plan, const std::string& call_id) {
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    a.organized = true;
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    plan->undo_group = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    const std::wstring undo_label = Text(I::AiAsstUndoLabel);
    // On screen the sort plays like the design (scene 5): the new folders
    // open 120 ms apart, then the items slide into them one by one; a big
    // sort, or one off screen, moves a group at a time.
    std::shared_ptr<ui::AiListOverlay> ov;
    if (Shown(s, plan->folder)) {
        ListOverlay(plan->folder);
        ov = g_list;
        ov->new_badge = Text(I::AiAsstNewBadge);
        ov->new_folders.clear();
        ov->leaving.clear();
        ov->target.clear();
        ov->targets.clear();
        ov->bumps.clear();
        for (const auto& g : plan->groups) {
            const std::wstring folder = ui::AiListLower(g.name);
            ov->targets.insert(folder);
            for (const auto& name : g.items) ov->target[ui::AiListLower(name)] = folder;
        }
    }
    auto acts = std::make_shared<std::vector<OrganizeAct>>();
    for (size_t gi = 0; gi < plan->groups.size(); ++gi)
        if (!plan->groups[gi].exists)
            acts->push_back({OrganizeAct::Create, gi, 0, ov ? (acts->empty() ? 300u : 120u) : 0u});
    const uint32_t first_move = ov ? (acts->empty() ? 300u : 120u + 420u) : 0u;
    if (ov && plan->total <= 60) {
        // Items in list order (the snapshot's), each its own request.
        std::vector<std::pair<size_t, size_t>> order;
        for (size_t gi = 0; gi < plan->groups.size(); ++gi)
            for (size_t ii = 0; ii < plan->groups[gi].items.size(); ++ii) order.push_back({gi, ii});
        if (app::Tab* tab = ActiveTab(s); tab && tab->EntryCount() <= 20000) {
            std::unordered_map<std::wstring, size_t> at;
            for (size_t i = 0; i < tab->EntryCount(); ++i) at.emplace(ui::AiListLower(tab->EntryAt(i).name), i);
            auto rank = [&](const std::pair<size_t, size_t>& p) {
                auto it = at.find(ui::AiListLower(plan->groups[p.first].items[p.second]));
                return it == at.end() ? SIZE_MAX : it->second;
            };
            std::stable_sort(order.begin(), order.end(),
                             [&](const auto& l, const auto& r) { return rank(l) < rank(r); });
        }
        const uint32_t step = (std::max)(20u, (std::min)(190u, static_cast<uint32_t>(3000 / (std::max<size_t>)(plan->total, 1))));
        for (size_t k = 0; k < order.size(); ++k)
            acts->push_back({OrganizeAct::MoveOne, order[k].first, order[k].second, k == 0 ? first_move : step});
    } else {
        for (size_t gi = 0; gi < plan->groups.size(); ++gi)
            acts->push_back({OrganizeAct::MoveGroup, gi, 0, gi == 0 ? first_move : (ov ? 190u : 0u)});
    }
    // The progress step.
    FoldSteps(s, a);
    ui::AiStepView& st = AddStep(s, a, ui::AiIcon::Move, Text(I::AiAsstStepMove), plan->label,
                                 Fill(Text(I::AiAsstStepMoveRun), L"{g}", Num(plan->created)));
    st.progress = 0.0f;
    AiChatSetChip(c, ui::StatusAiState::Busy,
                  Fill(Fill(Text(I::AiAsstChipOrganizing), L"{k}", L"0"), L"{n}", Num(plan->total)), 0.0f);
    struct Watch {
        size_t moved = 0;
        uint64_t last_change = 0;
        uint64_t started = 0;
        uint64_t polled = 0;
        size_t next = 0;        // the next act
        uint64_t next_at = 0;
    };
    auto w = std::make_shared<Watch>();
    w->started = w->last_change = Now();
    w->next_at = Now() + (acts->empty() ? 0 : acts->front().delay);
    const uint64_t gen = a.generation;
    g_flush_sort = [plan, w, acts, undo_label](AppState& st2) {
        while (w->next < acts->size()) SubmitAct(st2, *plan, (*acts)[w->next++], undo_label, nullptr);
    };
    AiChatPush(s, 0, [plan, call_id, w, gen, acts, ov, undo_label](AppState& st2) {
        AiAgent& ag = Agent(st2);
        if (ag.generation != gen) {
            while (w->next < acts->size()) SubmitAct(st2, *plan, (*acts)[w->next++], undo_label, nullptr);
            return -1;
        }
        const uint64_t now = Now();
        // Submit what is due.
        while (w->next < acts->size() && now >= w->next_at) {
            SubmitAct(st2, *plan, (*acts)[w->next], undo_label, ov.get());
            ++w->next;
            if (w->next < acts->size()) w->next_at = now + (*acts)[w->next].delay;
            w->last_change = now;
        }
        const bool fed = w->next >= acts->size();
        if (fed) g_flush_sort = nullptr;
        if (now - w->polled >= 150 || fed) {
            w->polled = now;
            size_t gone = 0;
            for (const auto& src : plan->sources)
                if (!Exists(src)) ++gone;
            if (gone != w->moved) {
                w->moved = gone;
                w->last_change = now;
            }
            const float p = plan->total ? static_cast<float>(gone) / static_cast<float>(plan->total) : 1.0f;
            if (ui::AiStepView* step = LastStep(st2, ag)) {
                step->progress = p;
                if (gone) step->sub = Fill(Fill(Text(I::AiAsstStepMoving), L"{k}", Num(gone)), L"{n}", Num(plan->total));
            }
            AiChatSetChip(Chat(st2), ui::StatusAiState::Busy,
                          Fill(Fill(Text(I::AiAsstChipOrganizing), L"{k}", Num(gone)), L"{n}", Num(plan->total)), p);
            const bool idle = !st2.ops.Status().active;
            const bool all = gone >= plan->total;
            // Everything moved; or the queue went quiet with no progress (a
            // failure or a skipped item); or ten minutes passed.
            if (fed && (all || (idle && now - w->last_change > 2500) || now - w->started > 600000)) {
                // Let the last card land and its folder bump (organizeAnim: +650 ms).
                if (ov && all && now - w->last_change < 650) return 650 - static_cast<int>(now - w->last_change);
                SettleOrganize(st2, plan, call_id, gone);
                return -1;
            }
        }
        uint64_t wait = 150;
        if (!fed) wait = (std::min)(wait, w->next_at > now ? w->next_at - now : 1);
        return static_cast<int>(wait);
    });
}

// The result card's 撤销: one group undo through the queue, then the card
// turns grey once the items are back.
std::function<void(AppState&)> MakeUndo(std::shared_ptr<OrganizePlan> plan) {
    return [plan](AppState& st) {
        AiChat& c2 = Chat(st);
        const auto outcome = st.ops.UndoGroup(plan->undo_group);
        if (outcome != ops::OpsManager::GroupUndo::Started) {
            AiBlockView& note = c2.Add(AiBlockKind::Note, Now());
            const bool pending = outcome == ops::OpsManager::GroupUndo::Pending;
            note.text = Text(pending ? I::AiAsstUndoBusy : I::AiAsstUndoGone);
            if (pending) c2.on_undo = MakeUndo(plan);   // the button works again later
            Redraw(st);
            return;
        }
        AiChatSetChip(c2, ui::StatusAiState::Busy, Text(I::AiAsstChipUndoing), 0.5f);
        c2.peek_undo = false;
        AiChatHidePeek(st);
        if (ui::AiListOverlay* o = OverlayOf(plan->folder)) {
            // undoAnim: the folders the sort made slide out; the items come
            // back with the list's own appear motion.
            const uint64_t now = Now();
            o->dests.clear();
            o->new_folders.clear();
            o->target.clear();
            o->targets.clear();
            o->bumps.clear();
            o->leaving.clear();
            for (const auto& g : plan->groups)
                if (!g.exists) o->leaving[ui::AiListLower(g.name)] = now;
            o->end_ms = now + 4000;
        }
        auto started = std::make_shared<uint64_t>(Now());
        AiChatPush(st, 300, [plan, started](AppState& s3) {
            size_t back = 0;
            for (const auto& src : plan->sources)
                if (Exists(src)) ++back;
            if (back < plan->total && !(Now() - *started > 3000 && !s3.ops.Status().active) &&
                Now() - *started < 600000)
                return 150;
            AiChat& c3 = Chat(s3);
            if (AiBlockView* rr = c3.Find(c3.result_id)) {
                rr->done = true;
                rr->title = Text(I::AiAsstUndone);
                rr->sub = plan->created
                              ? Fill(Fill(Fill(Text(I::AiAsstUndoneSub), L"{n}", Num(back)), L"{folder}", plan->label),
                                     L"{g}", Num(plan->created))
                              : Fill(Fill(Text(I::AiAsstUndoneSubAny), L"{n}", Num(back)), L"{folder}", plan->label);
            }
            AiBlockView& note = c3.Add(AiBlockKind::Note, Now());
            note.text = Text(I::AiAsstUndoNote);
            AiChatSetChip(c3, ui::StatusAiState::Done, Text(I::AiAsstUndone));
            if (c3.agent) c3.agent->pending_note = L"(The user clicked Undo in Pulse: the last organize_files was reversed and every item is back where it was.)";
            Redraw(s3);
            return -1;
        });
    };
}

void SettleOrganize(AppState& s, std::shared_ptr<OrganizePlan> plan, const std::string& call_id, size_t moved) {
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    const bool all = moved >= plan->total;
    EndStep(s, a, moved > 0,
            all ? GroupsText(*plan, I::AiAsstStepMoveDone, I::AiAsstStepMoveDoneAny)
                : Fill(Fill(Text(I::AiAsstMovedSome), L"{k}", Num(moved)), L"{n}", Num(plan->total)));
    if (AiBlockView* b = Block(s, a.steps_id)) {
        b->label = GroupsText(*plan, I::AiAsstStepsMove, I::AiAsstStepsMoveAny);
        b->collapsed = true;
        b->open = false;
    }
    a.steps_id = 0;
    // The result card.
    AiBlockView& r = c.Add(AiBlockKind::Result, Now());
    r.title = Fill(Text(I::AiAsstResultTitle), L"{n}", Num(moved));
    r.sub = Fill(Fill(Text(plan->created == plan->groups.size() ? I::AiAsstResultSub : I::AiAsstResultSubAny),
                      L"{folder}", plan->label),
                 L"{g}", Num(plan->groups.size()));
    for (const auto& g : plan->groups) {
        ui::AiIcon icon = ui::AiIcon::Folder;
        ui::AiTint tint = ui::AiTint::Neutral;
        AiChatGroupLook(g.items, icon, tint);
        r.tags.push_back({tint, g.name + L" " + Num(g.items.size())});
    }
    r.deny = Text(I::AiAsstUndo);
    r.accept = Text(I::AiAsstShowInPulse);
    c.result_id = r.id;
    c.on_show = [plan](AppState& st) { NavigateTo(st, plan->folder); };
    c.on_undo = MakeUndo(plan);
    // The peek bubble's line (shown if the panel is closed when the turn ends).
    std::wstring names;
    for (const auto& g : plan->groups) {
        if (!names.empty()) names += L"、";
        names += g.name;
    }
    c.peek_text = Fill(Fill(Fill(Text(plan->created == plan->groups.size() ? I::AiPeekSorted : I::AiPeekSortedAny),
                                 L"{n}", Num(moved)),
                            L"{g}", Num(plan->groups.size())),
                       L"{names}", names);
    c.peek_undo = moved > 0;
    if (ui::AiListOverlay* o = OverlayOf(plan->folder)) o->end_ms = Now() + 6000;
    // Back to the model.
    ToolOutcome out;
    out.data = json::Value::Object();
    out.data.set("ok", moved > 0);
    out.data.set("moved", static_cast<uint64_t>(moved));
    out.data.set("total", static_cast<uint64_t>(plan->total));
    json::Value folders = json::Value::Array();
    for (const auto& g : plan->groups) {
        json::Value f = json::Value::Object();
        f.set("name", Utf8(g.name));
        f.set("path", Utf8(g.path));
        f.set("created", !g.exists);
        f.set("items", static_cast<uint64_t>(g.items.size()));
        folders.push(std::move(f));
    }
    out.data.set("folders", std::move(folders));
    out.data.set("undo", "The user can undo the whole sort with the Undo button on the result card in Pulse.");
    if (!all) out.data.set("note", "Some items did not move (locked, skipped or failed).");
    AddToolResult(a, call_id, out);
    ++a.call_index;
    AiChatSetChip(c, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
    AiChatPush(s, 350, [](AppState& st) { NextTool(st); return -1; });
}

void ToolOrganize(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    auto plan = std::make_shared<OrganizePlan>();
    ToolOutcome error;
    if (!PlanOrganize(args, *plan, error)) {
        AddToolResult(a, call.id, error);
        ++a.call_index;
        NextTool(s);
        return;
    }
    FoldSteps(s, a);
    // The plan card, its groups sliding in.
    AiBlockView& b = c.Add(AiBlockKind::Plan, Now());
    b.label = Text(I::AiAsstPlanTitle);
    b.meta = Fill(Fill(Text(plan->created == plan->groups.size() ? I::AiAsstPlanMeta : I::AiAsstPlanMetaAny), L"{g}",
                       Num(plan->groups.size())),
                  L"{n}", Num(plan->total));
    for (const auto& g : plan->groups) {
        ui::AiGroupView v;
        AiChatGroupLook(g.items, v.icon, v.tint);
        v.name = g.name;
        v.count = Fill(Text(I::AiAsstCount), L"{n}", Num(g.items.size()));
        for (size_t i = 0; i < g.items.size() && i < 40; ++i) {
            if (!v.list.empty()) v.list += L"\u3001";
            v.list += g.items[i];
        }
        if (!g.exists) v.badge = Text(I::AiAsstNewBadge);
        b.groups.push_back(std::move(v));
    }
    const uint32_t plan_id = b.id;
    const uint64_t gen = a.generation;
    auto gi = std::make_shared<size_t>(0);
    AiChatPush(s, 0, [plan_id, gi, gen](AppState& st) {
        if (Agent(st).generation != gen) return -1;
        AiBlockView* pb = Chat(st).Find(plan_id);
        if (!pb || *gi >= pb->groups.size()) return -1;
        pb->groups[(*gi)++].in_ms = Now();
        return *gi < pb->groups.size() ? 170 : -1;
    });
    // The amber card; the turn waits here for the user.
    const std::string call_id = call.id;
    AiChatPush(s, 400, [plan, plan_id, call_id, gen](AppState& st) {
        AiAgent& ag = Agent(st);
        if (ag.generation != gen) return -1;
        AiChat& ch = Chat(st);
        AiChatSetChip(ch, ui::StatusAiState::Waiting, Text(I::AiAsstChipWaiting));
        if (!st.aiAssistant.open) ch.unread = true;
        AiBlockView& cf = ch.Add(AiBlockKind::Confirm, Now());
        cf.title = GroupsText(*plan, I::AiAsstConfirmTitle, I::AiAsstConfirmTitleAny);
        cf.sub = Fill(Text(I::AiAsstConfirmSub), L"{folder}", plan->label);
        cf.deny = Text(I::AiAsstDeny);
        cf.accept = Text(I::AiAsstOrganize);
        ch.confirm_id = cf.id;
        // Each item shows its folder in the list before you decide (scene 4).
        if (Shown(st, plan->folder)) {
            ui::AiListOverlay& o = ListOverlay(plan->folder);
            o.dests.clear();
            for (const auto& g : plan->groups) {
                ui::AiIcon icon = ui::AiIcon::Folder;
                ui::AiTint tint = ui::AiTint::Neutral;
                AiChatGroupLook(g.items, icon, tint);
                for (const auto& name : g.items)
                    o.dests[ui::AiListLower(name)] = {g.name, static_cast<uint8_t>(tint)};
            }
            o.dest_start = Now();
            o.dest_end = 0;
        }
        ch.on_confirm = [plan, plan_id, call_id, gen](AppState& s2, bool accept) {
            AiChat& c2 = Chat(s2);
            AiAgent& a2 = Agent(s2);
            AiBlockView* card = c2.Find(c2.confirm_id);
            if (card) {
                const uint64_t now = Now();
                if (now > card->born_ms) c2.turn_start += now - card->born_ms;   // waiting is not model time
                card->done = true;
                card->title = Fill(Text(accept ? I::AiAsstConfirmed : I::AiAsstDenied), L"{n}", Num(plan->total));
            }
            if (AiBlockView* pb = c2.Find(plan_id)) pb->collapsed = true;
            c2.confirm_id = 0;
            c2.on_confirm = nullptr;
            if (a2.generation != gen) return;
            if (!accept) {
                if (ui::AiListOverlay* o = OverlayOf(plan->folder)) {
                    o->dest_end = Now();
                    o->end_ms = Now() + 500;
                }
                AddToolResult(a2, call_id,
                              ToolOutcome::Fail("user_denied",
                                                "The user declined this plan in Pulse; nothing moved. Do not propose "
                                                "the same plan again; ask what they would like instead."));
                ++a2.call_index;
                AiChatSetChip(c2, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
                NextTool(s2);
                Redraw(s2);
                return;
            }
            RunOrganize(s2, plan, call_id);
            Redraw(s2);
        };
        return -1;
    });
}

// ------------------------------------------------------- rename_files --
// Flow B (plans/ai/Pulse_AI助手_第一批扩展.html): the model gives one rule in
// the batch rename engine's steps; Pulse reads the dates taken, makes every
// name on this PC, shows old → new and asks once. 在批量重命名中调整… opens
// the dialog with the rule filled in.

namespace rn = app::rename;

constexpr size_t kMaxRename = 2000;
constexpr size_t kRenameRows = 200;   // rows the card lists when open

struct RenameJob {
    std::vector<rn::Source> sources;
    std::vector<std::wstring> cameras;   // per source (empty: unknown)
    // lower-case parent -> lower-case names in it that are not renamed
    std::unordered_map<std::wstring, std::unordered_set<std::wstring>> existing;
    std::vector<rn::Step> steps;
    rn::Options opts;
    rn::Plan plan;
    bool needs_taken = false;
    size_t media = 0, with_taken = 0, cams = 0;
    std::wstring folder;                 // the first file's folder
    std::wstring label;                  // its name, or 3 个文件夹
    bool one_folder = true;
    uint64_t undo_group = 0;
    std::vector<size_t> rows;            // plan rows that rename, in display order
    std::vector<std::wstring> old_paths, new_paths;   // per entry of rows
};

std::wstring ParentDir(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring ExtPart(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos || dot == 0 ? std::wstring() : name.substr(dot);
}

bool ExtIn(const std::wstring& lower_ext, std::initializer_list<const wchar_t*> list) {
    for (const wchar_t* e : list)
        if (lower_ext == e) return true;
    return false;
}
bool IsVideoExt(const std::wstring& e) {
    return ExtIn(e, {L".mp4", L".mov", L".m4v", L".avi", L".mkv", L".3gp", L".wmv", L".mts", L".m2ts"});
}
bool IsPhotoExt(const std::wstring& e) {
    return ExtIn(e, {L".jpg", L".jpeg", L".png", L".heic", L".heif", L".tif", L".tiff", L".webp", L".gif", L".bmp",
                     L".dng", L".cr2", L".cr3", L".nef", L".arw", L".orf", L".rw2", L".raf", L".srw", L".jxl", L".avif"});
}

uint64_t LocalTicksOf(const FILETIME& utc) {
    FILETIME local{};
    const FILETIME& ft = FileTimeToLocalFileTime(&utc, &local) ? local : utc;
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// The worker: file times, the other names in each folder, dates taken and
// cameras (properties only, never the pictures), then the plan.
ToolOutcome ReadRenameSources(RenameJob& job, const std::atomic<bool>& cancelled) {
    std::unordered_map<std::wstring, std::unordered_set<std::wstring>> selected;
    std::unordered_map<std::wstring, std::wstring> dirs;   // lower -> as written
    for (rn::Source& src : job.sources) {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(src.path.c_str(), GetFileExInfoStandard, &fad)) src.modified = LocalTicksOf(fad.ftLastWriteTime);
        const std::wstring dir = ParentDir(src.path);
        selected[rn::Lower(dir)].insert(rn::Lower(src.name));
        dirs.emplace(rn::Lower(dir), dir);
    }
    for (const auto& kv : selected) {
        if (cancelled.load()) break;
        auto& names = job.existing[kv.first];
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileExW((dirs[kv.first] + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2]))) continue;
            std::wstring lower = rn::Lower(fd.cFileName);
            if (!kv.second.count(lower)) names.insert(std::move(lower));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    job.cameras.assign(job.sources.size(), std::wstring());
    if (job.needs_taken) {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::unordered_set<std::wstring> cams;
        for (size_t i = 0; i < job.sources.size() && !cancelled.load(); ++i) {
            rn::Source& src = job.sources[i];
            const std::wstring ext = rn::Lower(ExtPart(src.name));
            const bool video = IsVideoExt(ext);
            if (!video && !IsPhotoExt(ext)) continue;
            ++job.media;
            // The shell parses no \\?\ paths (tabs keep the prefix).
            std::wstring shell_path = src.path;
            if (shell_path.rfind(L"\\\\?\\UNC\\", 0) == 0) shell_path = L"\\\\" + shell_path.substr(8);
            else if (shell_path.rfind(L"\\\\?\\", 0) == 0) shell_path.erase(0, 4);
            IPropertyStore* store = nullptr;
            if (FAILED(SHGetPropertyStoreFromParsingName(shell_path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&store))))
                continue;
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(store->GetValue(PKEY_Photo_DateTaken, &v)) && v.vt == VT_FILETIME) src.taken = LocalTicksOf(v.filetime);
            PropVariantClear(&v);
            if (!src.taken && video) {
                PropVariantInit(&v);
                if (SUCCEEDED(store->GetValue(PKEY_Media_DateEncoded, &v)) && v.vt == VT_FILETIME)
                    src.taken = LocalTicksOf(v.filetime);
                PropVariantClear(&v);
            }
            PropVariantInit(&v);
            if (SUCCEEDED(store->GetValue(PKEY_Photo_CameraModel, &v)) && v.vt == VT_LPWSTR && v.pwszVal && *v.pwszVal)
                job.cameras[i] = v.pwszVal;
            PropVariantClear(&v);
            store->Release();
            if (src.taken) ++job.with_taken;
            if (!job.cameras[i].empty()) cams.insert(job.cameras[i]);
        }
        job.cams = cams.size();
        if (SUCCEEDED(init)) CoUninitialize();
    }
    if (cancelled.load()) return ToolOutcome::Fail("cancelled", "stopped");
    const auto* existing = &job.existing;
    job.plan = rn::Run(job.sources, job.steps, job.opts,
                       [existing](const std::wstring& source_path, const std::wstring& lower_name) {
                           auto it = existing->find(rn::Lower(ParentDir(source_path)));
                           return it != existing->end() && it->second.count(lower_name) != 0;
                       });
    ToolOutcome out;
    out.data = json::Value::Object();
    return out;
}

// A template or insert text split into literal text and {tokens}.
struct PatternPiece {
    bool token = false;
    std::wstring text;   // literal text, or the token's key
    std::wstring fmt;    // {taken:yyyyMMdd} -> yyyyMMdd
};

bool KeyIn(const std::wstring& k, std::initializer_list<const wchar_t*> names) {
    for (const wchar_t* n : names)
        if (k == n) return true;
    return false;
}

bool KnownKey(const std::wstring& k) {
    return KeyIn(k, {L"原名", L"name", L"扩展名", L"ext", L"序号", L"序號", L"n", L"number", L"拍摄日期", L"拍攝日期",
                     L"taken", L"修改日期", L"modified", L"date", L"文件夹", L"資料夾", L"folder", L"尺寸", L"size"});
}

std::vector<PatternPiece> SplitPattern(const std::wstring& p) {
    std::vector<PatternPiece> out;
    std::wstring lit;
    for (size_t i = 0; i < p.size();) {
        if (p[i] == L'{') {
            const size_t close = p.find(L'}', i + 1);
            if (close != std::wstring::npos) {
                std::wstring key = p.substr(i + 1, close - i - 1), fmt;
                const size_t colon = key.find(L':');
                if (colon != std::wstring::npos) {
                    fmt = key.substr(colon + 1);
                    key.resize(colon);
                }
                if (KnownKey(key)) {
                    if (!lit.empty()) out.push_back({false, std::move(lit), {}});
                    lit.clear();
                    out.push_back({true, std::move(key), std::move(fmt)});
                    i = close + 1;
                    continue;
                }
            }
        }
        lit += p[i++];
    }
    if (!lit.empty()) out.push_back({false, std::move(lit), {}});
    return out;
}

std::wstring PadNum(long long v, int digits) {
    std::wstring s = std::to_wstring(v);
    while (static_cast<int>(s.size()) < digits) s.insert(s.begin(), L'0');
    return s;
}

std::wstring NumberLabel(int start, int digits, bool daily) {
    return Fill(Text(daily ? I::AiRnTokNumDaily : I::AiRnTokNum), L"{s}", PadNum(start, (std::max)(1, digits)));
}

std::wstring TokenLabel(const PatternPiece& t, const rn::Step& st, bool daily) {
    const std::wstring& k = t.text;
    if (KeyIn(k, {L"拍摄日期", L"拍攝日期", L"taken"})) return Text(I::BrTokTaken);
    if (KeyIn(k, {L"序号", L"序號", L"n", L"number"})) {
        int digits = st.digits_n;
        if (!t.fmt.empty()) digits = static_cast<int>(std::wcstol(t.fmt.c_str(), nullptr, 10));
        else if (k == L"n") digits = 1;
        return NumberLabel(st.start, digits, daily);
    }
    if (KeyIn(k, {L"原名", L"name"})) return Text(I::BrTokName);
    if (KeyIn(k, {L"修改日期", L"modified", L"date"})) return Text(I::BrTokModified);
    if (KeyIn(k, {L"文件夹", L"資料夾", L"folder"})) return Text(I::BrTokFolder);
    if (KeyIn(k, {L"尺寸", L"size"})) return Text(I::BrTokSize);
    return L".ext";
}

// The rule as the card shows it; `hint` is the extension line (扩展名不变).
std::vector<ui::AiRuleToken> RuleOf(const RenameJob& job, std::wstring& hint) {
    std::vector<ui::AiRuleToken> out;
    hint = Text(I::AiRnExtKeep);
    const auto word = [&](std::wstring w) { out.push_back({std::move(w), false, true}); };
    const auto lit = [&](std::wstring w) { out.push_back({std::move(w), false, false}); };
    const auto chip = [&](std::wstring w) { out.push_back({std::move(w), true, false}); };
    const auto text = [&](const std::wstring& t, const rn::Step& st) {
        for (const PatternPiece& p : SplitPattern(t)) {
            if (p.token) chip(TokenLabel(p, st, job.opts.daily));
            else lit(p.text);
        }
    };
    bool first = true;
    for (const rn::Step& st : job.steps) {
        if (!st.on) continue;
        if (st.type == rn::StepType::Ext) {
            if (st.mode == rn::CaseMode::Change) hint = Fill(Text(I::AiRnExtTo), L"{ext}", L"." + st.ext_to);
            else hint = Text(st.mode == rn::CaseMode::Upper ? I::AiRnExtUpper : I::AiRnExtLower);
            continue;
        }
        if (!first) word(L"\u00B7");
        first = false;
        switch (st.type) {
        case rn::StepType::Template: text(st.pattern, st); break;
        case rn::StepType::Replace:
            if (st.with.empty()) {
                word(Text(I::AiRnStepRemove));
                lit(st.find);
            } else {
                lit(st.find);
                word(Text(I::AiRnStepReplace));
                lit(st.with);
            }
            break;
        case rn::StepType::Remove:
            if (st.brackets) word(Text(I::AiRnStepBrackets));
            if (st.spaces) word(Text(I::AiRnStepSpaces));
            if (st.digits) word(Text(I::AiRnStepDigits));
            if (!st.custom.empty()) {
                word(Text(I::AiRnStepRemove));
                lit(st.custom);
            }
            break;
        case rn::StepType::Insert:
            if (st.where == rn::Where::Start) word(Text(I::AiRnStepStart));
            else if (st.where == rn::Where::End) word(Text(I::AiRnStepEnd));
            else word(Fill(Text(I::AiRnStepAt), L"{n}", Num(static_cast<size_t>((std::max)(0, st.at)))));
            text(st.text, st);
            break;
        case rn::StepType::Case:
            word(Text(st.mode == rn::CaseMode::Upper   ? I::AiRnStepUpper
                      : st.mode == rn::CaseMode::Title ? I::AiRnStepTitleCase
                                                        : I::AiRnStepLower));
            break;
        case rn::StepType::Number:
            word(Text(st.where == rn::Where::Start ? I::AiRnStepStart : I::AiRnStepEnd));
            chip(NumberLabel(st.start, st.digits_n, job.opts.daily));
            break;
        default: break;
        }
    }
    return out;
}

// A new name in parts: with one template rule what the tokens made is accent
// and its literal text plain; other rules mark what changed (Diff).
std::vector<ui::AiRuleToken> NewNameParts(const RenameJob& job, const rn::Row& row, std::wstring& ext) {
    const rn::Source& src = job.sources[row.src];
    ext = ExtPart(row.new_name);
    std::wstring stem = row.new_name.substr(0, row.new_name.size() - ext.size());
    std::wstring suffix;
    if (row.added_n > 0) {
        const std::wstring sfx = L" (" + std::to_wstring(row.added_n) + L")";
        if (stem.size() > sfx.size() && stem.compare(stem.size() - sfx.size(), sfx.size(), sfx) == 0) {
            suffix = sfx;
            stem.resize(stem.size() - sfx.size());
        }
    }
    std::vector<ui::AiRuleToken> parts;
    const rn::Step* only = nullptr;
    int active = 0;
    for (const rn::Step& st : job.steps)
        if (st.on && st.type != rn::StepType::Ext) {
            ++active;
            only = &st;
        }
    bool done = false;
    if (active == 1 && only->type == rn::StepType::Template && only->pattern.find(L"{ext}") == std::wstring::npos &&
        only->pattern.find(L"{扩展名}") == std::wstring::npos) {
        const std::vector<PatternPiece> pieces = SplitPattern(only->pattern);
        size_t pos = 0;
        bool ok = true;
        for (size_t k = 0; k < pieces.size() && ok;) {
            if (!pieces[k].token) {
                if (stem.compare(pos, pieces[k].text.size(), pieces[k].text) == 0) {
                    parts.push_back({pieces[k].text, false, false});
                    pos += pieces[k].text.size();
                } else {
                    ok = false;
                }
                ++k;
                continue;
            }
            // Tokens up to the next literal make one accent run.
            size_t j = k;
            while (j < pieces.size() && pieces[j].token) ++j;
            size_t end = stem.size();
            if (j < pieces.size()) {
                end = stem.find(pieces[j].text, pos);
                if (end == std::wstring::npos) ok = false;
            }
            if (ok && end > pos) parts.push_back({stem.substr(pos, end - pos), true, false});
            if (ok) pos = end;
            k = j;
        }
        done = ok && pos == stem.size();
        if (!done) parts.clear();
    }
    if (!done) {
        const std::wstring old_ext = ExtPart(src.name);
        const std::wstring old_stem = src.name.substr(0, src.name.size() - old_ext.size());
        std::vector<rn::Seg> old_side, new_side;
        rn::Diff(old_stem, stem, old_side, new_side);
        for (const rn::Seg& sg : new_side) {
            if (sg.kind == L'-' || sg.text.empty()) continue;
            const bool accent = sg.kind == L'+';
            if (!parts.empty() && parts.back().token == accent) parts.back().text += sg.text;
            else parts.push_back({sg.text, accent, false});
        }
        if (parts.empty()) parts.push_back({stem, false, false});
    }
    if (!suffix.empty()) parts.push_back({suffix, false, false});
    return parts;
}

void SettleRename(AppState& s, std::shared_ptr<RenameJob> job, const std::string& call_id, size_t renamed);

// 撤销 on the result card: the group back through the queue; the names slide
// back in the list (60 ms apart, renameRows(false, 60)).
std::function<void(AppState&)> MakeRenameUndo(std::shared_ptr<RenameJob> job) {
    return [job](AppState& st) {
        AiChat& c2 = Chat(st);
        const auto outcome = st.ops.UndoGroup(job->undo_group);
        if (outcome != ops::OpsManager::GroupUndo::Started) {
            AiBlockView& note = c2.Add(AiBlockKind::Note, Now());
            const bool pending = outcome == ops::OpsManager::GroupUndo::Pending;
            note.text = Text(pending ? I::AiAsstUndoBusy : I::AiAsstUndoGone);
            if (pending) c2.on_undo = MakeRenameUndo(job);
            Redraw(st);
            return;
        }
        AiChatSetChip(c2, ui::StatusAiState::Busy, Text(I::AiAsstChipUndoing), 0.5f);
        c2.peek_undo = false;
        c2.on_show = nullptr;
        AiChatHidePeek(st);
        if (job->one_folder && job->rows.size() <= 60) {
            if (ui::AiListOverlay* o = OverlayOf(job->folder)) {
                const uint64_t now = Now();
                o->dests.clear();
                o->morph.clear();
                const uint64_t step = (std::min<uint64_t>)(60, 1200 / (std::max<size_t>)(job->rows.size(), 1));
                for (size_t k = 0; k < job->rows.size(); ++k) {
                    const rn::Row& row = job->plan.rows[job->rows[k]];
                    o->morph[ui::AiListLower(job->sources[row.src].name)] = {row.new_name, now + 250 + step * k};
                }
                o->end_ms = now + 250 + step * job->rows.size() + 3000;
            }
        }
        if (app::Tab* tab = ActiveTab(st); tab && job->one_folder && SameDir(tab->current_path, job->folder)) {
            tab->pending_selected_names.clear();
            for (size_t k : job->rows) tab->pending_selected_names.push_back(job->sources[job->plan.rows[k].src].name);
            if (!tab->pending_selected_names.empty()) tab->pending_selected_name = tab->pending_selected_names.front();
        }
        auto started = std::make_shared<uint64_t>(Now());
        AiChatPush(st, 300, [job, started](AppState& s3) {
            size_t back = 0;
            for (const auto& p : job->old_paths)
                if (Exists(p)) ++back;
            if (back < job->old_paths.size() && !(Now() - *started > 3000 && !s3.ops.Status().active) &&
                Now() - *started < 600000)
                return 150;
            AiChat& c3 = Chat(s3);
            if (AiBlockView* rr = c3.Find(c3.result_id)) {
                rr->done = true;
                rr->title = Text(I::AiAsstUndone);
                rr->sub = Fill(Text(I::AiRnUndoneSub), L"{n}", Num(back));
            }
            AiBlockView& note = c3.Add(AiBlockKind::Note, Now());
            note.text = Text(I::AiRnUndoNote);
            AiChatSetChip(c3, ui::StatusAiState::Done, Text(I::AiAsstUndone));
            if (c3.agent)
                c3.agent->pending_note =
                    L"(The user clicked Undo in Pulse: the last rename_files was reversed and every file has its old name back.)";
            Redraw(s3);
            return -1;
        });
    };
}

// Opens 批量重命名 on `paths` with the job's rule; how many it renamed.
size_t OpenRenameDialog(AppState& s, const RenameJob& job, const std::vector<std::wstring>& paths) {
    rn::Preset preset;
    preset.steps = job.steps;
    for (rn::Step& st : preset.steps) {
        st.ai = true;
        // Back to the dialog's {n} (shown as its 序号 token, padded by digits).
        for (std::wstring* t : {&st.pattern, &st.text})
            for (size_t at = t->find(L"{number}"); at != std::wstring::npos; at = t->find(L"{number}", at + 3)) t->replace(at, 8, L"{n}");
    }
    preset.opts = job.opts;
    app::Tab* select_in = nullptr;
    if (app::Tab* tab = ActiveTab(s); tab && job.one_folder && SameDir(tab->current_path, job.folder)) select_in = tab;
    return ShowBatchRenameWith(s, paths, preset, select_in);
}

void RunRename(AppState& s, std::shared_ptr<RenameJob> job, const std::string& call_id) {
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    job->undo_group = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    ops::OpRequest r;
    r.type = ops::OpType::BatchRename;
    r.undo_group = job->undo_group;
    r.undo_label = Text(I::AiRnUndoLabel);
    for (size_t idx : job->plan.exec_order) {
        const rn::Row& row = job->plan.rows[idx];
        if (!row.Renames()) continue;
        r.sources.push_back(job->sources[row.src].path);
        r.new_names.push_back(row.new_name);
    }
    const size_t n = job->rows.size();
    // In the list: each name swaps in turn (renameRows(true, 170)), the tags go.
    uint64_t last_at = Now();
    if (job->one_folder && Shown(s, job->folder)) {
        ui::AiListOverlay& o = ListOverlay(job->folder);
        const uint64_t now = Now();
        o.dest_end = now;
        o.morph.clear();
        if (n <= 60) {
            const uint64_t step = (std::min<uint64_t>)(170, 1200 / (std::max<size_t>)(n, 1));
            for (size_t k = 0; k < n; ++k) {
                const rn::Row& row = job->plan.rows[job->rows[k]];
                last_at = now + 200 + step * k;
                o.morph[ui::AiListLower(row.new_name)] = {job->sources[row.src].name, last_at};
            }
        } else {
            for (size_t k : job->rows) o.flash[ui::AiListLower(job->plan.rows[k].new_name)] = now + 200;
            last_at = now + 200;
        }
        o.end_ms = 0;
    }
    if (app::Tab* tab = ActiveTab(s); tab && job->one_folder && SameDir(tab->current_path, job->folder) && !r.new_names.empty()) {
        tab->pending_selected_names = r.new_names;
        tab->pending_selected_name = r.new_names.front();
    }
    if (!r.sources.empty()) s.ops.Submit(std::move(r));
    // The progress step.
    a.steps_id = 0;
    ui::AiStepView& st = AddStep(s, a, ui::AiIcon::Rename, Text(I::AiRnStep), Fill(Text(I::AiRnMeta), L"{n}", Num(n)),
                                 L"0 / " + Num(n));
    st.progress = 0.0f;
    AiChatSetChip(c, ui::StatusAiState::Busy, Fill(Fill(Text(I::AiRnChipRun), L"{k}", L"0"), L"{n}", Num(n)), 0.0f);
    struct Watch {
        size_t on_disk = 0;
        uint64_t last_change = 0, started = 0, polled = 0;
    };
    auto w = std::make_shared<Watch>();
    w->started = w->last_change = Now();
    const uint64_t gen = a.generation;
    std::shared_ptr<const ui::AiListOverlay> ov = job->one_folder ? AiListOverlayFor(job->folder) : nullptr;
    AiChatPush(s, 0, [job, call_id, w, gen, ov, n, last_at](AppState& st2) {
        AiAgent& ag = Agent(st2);
        if (ag.generation != gen) return -1;
        const uint64_t now = Now();
        if (now - w->polled >= 150) {
            w->polled = now;
            size_t done = 0;
            for (const auto& p : job->new_paths)
                if (Exists(p)) ++done;
            if (done != w->on_disk) {
                w->on_disk = done;
                w->last_change = now;
            }
        }
        // What the list shows: the names that swapped so far.
        size_t shown = w->on_disk;
        if (ov && !ov->morph.empty()) {
            size_t swapped = 0;
            for (const auto& kv : ov->morph)
                if (now >= kv.second.at) ++swapped;
            shown = (std::min)(shown, swapped);
        }
        const float p = n ? static_cast<float>(shown) / static_cast<float>(n) : 1.0f;
        if (ui::AiStepView* step = LastStep(st2, ag)) {
            step->progress = p;
            step->sub = Num(shown) + L" / " + Num(n);
        }
        AiChatSetChip(Chat(st2), ui::StatusAiState::Busy,
                      Fill(Fill(Text(I::AiRnChipRun), L"{k}", Num(shown)), L"{n}", Num(n)), p);
        const bool idle = !st2.ops.Status().active;
        const bool all = w->on_disk >= n;
        if ((all && now >= last_at + 450) || (idle && now - w->last_change > 2500 && now >= last_at) ||
            now - w->started > 600000) {
            SettleRename(st2, job, call_id, w->on_disk);
            return -1;
        }
        return 50;
    });
}

void SettleRename(AppState& s, std::shared_ptr<RenameJob> job, const std::string& call_id, size_t renamed) {
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    const size_t n = job->rows.size();
    const bool all = renamed >= n;
    EndStep(s, a, renamed > 0,
            all ? Fill(Text(I::AiRnStepDone), L"{n}", Num(renamed))
                : Fill(Fill(Text(I::AiRnStepSome), L"{k}", Num(renamed)), L"{n}", Num(n)));
    if (AiBlockView* b = Block(s, a.steps_id)) {
        b->label = Fill(Text(I::AiRnSteps), L"{n}", Num(renamed));
        b->collapsed = true;
        b->open = false;
    }
    a.steps_id = 0;
    // The result card: 旅行 2026-10 · 10月3日 4 张 · 10月4日 3 张.
    AiBlockView& r = c.Add(AiBlockKind::Result, Now());
    r.title = Fill(Text(I::AiRnResultTitle), L"{n}", Num(renamed));
    r.sub = job->label;
    if (job->needs_taken) {
        std::vector<std::pair<std::pair<int, int>, size_t>> days;   // (month, day) -> files, in order
        for (size_t k : job->rows) {
            const rn::Source& src = job->sources[job->plan.rows[k].src];
            const uint64_t t = src.taken ? src.taken : src.modified;
            FILETIME f{static_cast<DWORD>(t & 0xFFFFFFFF), static_cast<DWORD>(t >> 32)};
            SYSTEMTIME stime{};
            if (!FileTimeToSystemTime(&f, &stime)) continue;
            const std::pair<int, int> md{stime.wMonth, stime.wDay};
            auto it = std::find_if(days.begin(), days.end(), [&](const auto& d) { return d.first == md; });
            if (it == days.end()) days.push_back({md, 1});
            else ++it->second;
        }
        std::sort(days.begin(), days.end());
        for (size_t i = 0; i < days.size() && i < 4; ++i)
            r.sub += L" \u00B7 " + Fill(Fill(Fill(Text(I::AiRnDay), L"{m}", std::to_wstring(days[i].first.first)), L"{d}",
                                             std::to_wstring(days[i].first.second)),
                                        L"{n}", Num(days[i].second));
        if (days.size() > 4) r.sub += L" \u00B7 \u2026";
    }
    r.deny = Text(I::AiAsstUndo);
    r.accept = Text(I::AiRnOpenDialog);
    r.accept_icon = ui::AiIcon::Sliders;
    c.result_id = r.id;
    // 在批量重命名中打开: the renamed files with the rule, to go on from there.
    c.on_show = [job](AppState& st) {
        std::vector<std::wstring> paths;
        for (const auto& p : job->new_paths)
            if (Exists(p)) paths.push_back(p);
        if (paths.size() < 2) {
            NavigateTo(st, job->folder);
            return;
        }
        OpenRenameDialog(st, *job, paths);
    };
    c.on_undo = MakeRenameUndo(job);
    if (renamed && !job->rows.empty())
        c.peek_text = Fill(Fill(Text(I::AiPeekRenamed), L"{n}", Num(renamed)), L"{name}",
                           job->plan.rows[job->rows.front()].new_name);
    c.peek_undo = renamed > 0;
    if (ui::AiListOverlay* o = job->one_folder ? OverlayOf(job->folder) : nullptr) o->end_ms = Now() + 3000;
    // Back to the model.
    ToolOutcome out;
    out.data = json::Value::Object();
    out.data.set("ok", renamed > 0);
    out.data.set("renamed", static_cast<uint64_t>(renamed));
    out.data.set("total", static_cast<uint64_t>(job->sources.size()));
    json::Value examples = json::Value::Array();
    json::Value no_taken = json::Value::Array();
    for (size_t i = 0; i < job->rows.size(); ++i) {
        const rn::Row& row = job->plan.rows[job->rows[i]];
        if (examples.size() < 5) {
            json::Value e = json::Value::Object();
            e.set("from", Utf8(job->sources[row.src].name));
            e.set("to", Utf8(row.new_name));
            examples.push(std::move(e));
        }
        if (row.why == rn::Why::NoTakenDate && no_taken.size() < 5) {
            json::Value e = json::Value::Object();
            e.set("name", Utf8(job->sources[row.src].name));
            SYSTEMTIME stime{};
            const uint64_t t = job->sources[row.src].modified;
            FILETIME f{static_cast<DWORD>(t & 0xFFFFFFFF), static_cast<DWORD>(t >> 32)};
            if (FileTimeToSystemTime(&f, &stime)) {
                char buf[32];
                sprintf_s(buf, "%04u-%02u-%02u %02u:%02u", stime.wYear, stime.wMonth, stime.wDay, stime.wHour, stime.wMinute);
                e.set("modified_used", buf);
            }
            no_taken.push(std::move(e));
        }
    }
    out.data.set("examples", std::move(examples));
    if (no_taken.size()) out.data.set("no_date_taken", std::move(no_taken));
    out.data.set("undo", "The user can undo the whole rename with the Undo button on the result card in Pulse.");
    if (!all) out.data.set("note", "Some files were not renamed (locked or changed meanwhile).");
    AddToolResult(a, call_id, out);
    ++a.call_index;
    AiChatSetChip(c, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
    AiChatPush(s, 350, [](AppState& st) { NextTool(st); return -1; });
}

// The 重命名方案 card, the tags in the list, then the confirmation.
void ShowRenamePlan(AppState& s, std::shared_ptr<RenameJob> job, const std::string& call_id) {
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    const rn::Plan& plan = job->plan;
    // Rows in the list's order when it shows the folder.
    std::vector<size_t> order(plan.rows.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    if (app::Tab* tab = ActiveTab(s); tab && job->one_folder && SameDir(tab->current_path, job->folder) &&
                                      tab->EntryCount() <= 20000) {
        std::unordered_map<std::wstring, size_t> at;
        for (size_t i = 0; i < tab->EntryCount(); ++i) at.emplace(ui::AiListLower(tab->EntryAt(i).name), i);
        const auto rank = [&](size_t i) {
            auto it = at.find(ui::AiListLower(job->sources[plan.rows[i].src].name));
            return it == at.end() ? SIZE_MAX : it->second;
        };
        std::stable_sort(order.begin(), order.end(), [&](size_t l, size_t r) { return rank(l) < rank(r); });
    }
    for (size_t i : order)
        if (plan.rows[i].Renames()) {
            job->rows.push_back(i);
            const rn::Row& row = plan.rows[i];
            job->old_paths.push_back(job->sources[row.src].path);
            job->new_paths.push_back(JoinPath(ParentDir(job->sources[row.src].path), row.new_name));
        }
    FoldSteps(s, a);
    AiBlockView& b = c.Add(AiBlockKind::Rename, Now());
    b.label = Text(I::AiRnTitle);
    b.meta = Fill(Text(I::AiRnMeta), L"{n}", Num(job->sources.size()));
    b.rule_label = Text(I::AiRnRule);
    b.rule = RuleOf(*job, b.hint);
    // Rows: four while folded (the first three and the first warned one).
    const size_t n = job->rows.size();
    size_t warned = SIZE_MAX;
    for (size_t k = 0; k < n && k < kRenameRows; ++k) {
        const rn::Row& row = plan.rows[job->rows[k]];
        ui::AiRenameRow v;
        v.old_name = job->sources[row.src].name;
        v.parts = NewNameParts(*job, row, v.ext);
        v.warn = row.why == rn::Why::NoTakenDate;
        if (v.warn && warned == SIZE_MAX) warned = k;
        b.rows.push_back(std::move(v));
    }
    const size_t listed = b.rows.size();
    const bool fold = listed > 5;
    const size_t fourth = warned != SIZE_MAX && warned >= 3 ? warned : 3;
    for (size_t k = 0; k < listed; ++k) b.rows[k].sample = !fold || k < 3 || k == fourth;
    size_t samples = 0;
    for (const auto& v : b.rows) samples += v.sample ? 1 : 0;
    if (fold) {
        b.more = Fill(Text(I::AiRnMore), L"{n}", Num(n - samples));
        b.less = Text(I::AiRnLess);
    }
    // Checks.
    size_t clashed = 0, no_taken = 0, bad = 0;
    std::wstring no_taken_name;
    for (const rn::Row& row : plan.rows) {
        if (row.status == rn::RowStatus::Conflict && row.added_n > 0) ++clashed;
        if (row.why == rn::Why::NoTakenDate) {
            if (!no_taken++) {
                const std::wstring& nm = job->sources[row.src].name;
                no_taken_name = nm.substr(0, nm.size() - ExtPart(nm).size());
            }
        }
        if (row.status == rn::RowStatus::Invalid || row.status == rn::RowStatus::Empty ||
            row.status == rn::RowStatus::Skipped || (row.status == rn::RowStatus::Conflict && !row.added_n))
            ++bad;
    }
    if (clashed) b.checks.push_back({Fill(Text(I::AiRnCheckClash), L"{n}", Num(clashed)), true});
    else b.checks.push_back({Text(I::AiRnCheckOk), false});
    if (job->needs_taken && job->opts.order == rn::Order::Taken && job->cams >= 2)
        b.checks.push_back({Fill(Text(I::AiRnCheckCams), L"{n}", Num(job->cams)), false});
    if (no_taken == 1)
        b.checks.push_back({Fill(Fill(Text(I::AiRnCheckNoTaken), L"{n}", L"1"), L"{name}", no_taken_name), true});
    else if (no_taken > 1)
        b.checks.push_back({Fill(Text(I::AiRnCheckNoTakenMany), L"{n}", Num(no_taken)), true});
    if (plan.same) b.checks.push_back({Fill(Text(I::AiRnCheckSame), L"{n}", Num(static_cast<size_t>(plan.same))), false});
    if (bad) b.checks.push_back({Fill(Text(I::AiRnCheckBad), L"{n}", Num(bad)), true});
    b.sub = Text(I::AiRnFoot);
    const uint32_t card_id = b.id;
    const uint64_t gen = a.generation;
    // The rows come in 140 ms apart (.rnr.in).
    auto ri = std::make_shared<size_t>(0);
    AiChatPush(s, 0, [card_id, ri, gen](AppState& st) {
        if (Agent(st).generation != gen) return -1;
        AiBlockView* cb = Chat(st).Find(card_id);
        if (!cb) return -1;
        while (*ri < cb->rows.size() && !cb->rows[*ri].sample && !cb->open) cb->rows[(*ri)++].in_ms = Now() - 1000;
        if (*ri >= cb->rows.size()) return -1;
        cb->rows[(*ri)++].in_ms = Now();
        return *ri < cb->rows.size() ? 140 : -1;
    });
    // The amber card; the turn waits here for the user.
    AiChatPush(s, 140 * 4 + 300, [job, card_id, call_id, gen](AppState& st) {
        AiAgent& ag = Agent(st);
        if (ag.generation != gen) return -1;
        AiChat& ch = Chat(st);
        // Rows the stagger skipped (folded) are simply there.
        if (AiBlockView* cb = ch.Find(card_id))
            for (auto& v : cb->rows)
                if (!v.in_ms) v.in_ms = Now() - 1000;
        AiChatSetChip(ch, ui::StatusAiState::Waiting, Text(I::AiAsstChipWaiting));
        if (!st.aiAssistant.open) ch.unread = true;
        AiBlockView& cf = ch.Add(AiBlockKind::Confirm, Now());
        cf.title = Fill(Text(I::AiRnConfirmTitle), L"{n}", Num(job->rows.size()));
        cf.sub = Fill(Text(I::AiRnConfirmSub), L"{folder}", job->label);
        cf.deny = Text(I::AiAsstDeny);
        cf.accept = Text(I::AiRnAccept);
        cf.extra = Text(I::AiRnAdjust);
        cf.extra_tip = Text(I::AiRnAdjustTip);
        ch.confirm_id = cf.id;
        // Each file shows its new name in the list before you decide.
        if (job->one_folder && Shown(st, job->folder)) {
            cf.hint = Text(st.aiAssistant.floating ? I::AiRnHint : I::AiRnHintRight);
            ui::AiListOverlay& o = ListOverlay(job->folder);
            o.dests.clear();
            o.morph.clear();
            for (size_t k : job->rows) {
                const rn::Row& row = job->plan.rows[k];
                o.dests[ui::AiListLower(job->sources[row.src].name)] = {row.new_name, ui::AiListOverlay::kDestAccent};
            }
            o.dest_start = Now();
            o.dest_end = 0;
        }
        // Closes the confirmation with `title`; false when the turn moved on.
        const auto close = [card_id, gen](AppState& s2, std::wstring title) {
            AiChat& c2 = Chat(s2);
            if (AiBlockView* card = c2.Find(c2.confirm_id)) {
                const uint64_t now = Now();
                if (now > card->born_ms) c2.turn_start += now - card->born_ms;   // waiting is not model time
                card->done = true;
                card->title = std::move(title);
            }
            if (AiBlockView* pb = c2.Find(card_id)) pb->collapsed = true;
            c2.confirm_id = 0;
            c2.on_confirm = nullptr;
            c2.on_extra = nullptr;
            return Agent(s2).generation == gen;
        };
        ch.on_confirm = [job, call_id, close](AppState& s2, bool accept) {
            if (!close(s2, Fill(Text(accept ? I::AiRnConfirmed : I::AiRnDenied), L"{n}", Num(job->rows.size())))) return;
            AiAgent& a2 = Agent(s2);
            if (!accept) {
                if (ui::AiListOverlay* o = job->one_folder ? OverlayOf(job->folder) : nullptr) {
                    o->dest_end = Now();
                    o->end_ms = Now() + 500;
                }
                AddToolResult(a2, call_id,
                              ToolOutcome::Fail("user_denied",
                                                "The user declined this rename in Pulse; nothing changed. Do not propose "
                                                "the same rule again; ask what they would like instead."));
                ++a2.call_index;
                AiChatSetChip(Chat(s2), ui::StatusAiState::Busy, Text(I::AiAsstThinking));
                NextTool(s2);
                Redraw(s2);
                return;
            }
            RunRename(s2, job, call_id);
            Redraw(s2);
        };
        // 在批量重命名中调整…: the dialog (modal) with the rule; the turn goes
        // on with what the user did there.
        ch.on_extra = [job, call_id, close, gen](AppState& s2) {
            AiChat& c2 = Chat(s2);
            const uint32_t confirm = c2.confirm_id;
            c2.on_confirm = nullptr;
            c2.on_extra = nullptr;
            if (ui::AiListOverlay* o = job->one_folder ? OverlayOf(job->folder) : nullptr) o->dest_end = Now();
            AiChatSetChip(c2, ui::StatusAiState::Waiting, Text(I::AiAsstChipWaiting));
            Redraw(s2);
            std::vector<std::wstring> paths;
            for (const auto& src : job->sources) paths.push_back(src.path);
            const size_t done = OpenRenameDialog(s2, *job, paths);
            AiChat& c3 = Chat(s2);
            c3.confirm_id = confirm;
            if (!close(s2, done ? Fill(Text(I::AiRnAdjusted), L"{n}", Num(done)) : Text(I::AiRnAdjustClosed))) return;
            if (ui::AiListOverlay* o = job->one_folder ? OverlayOf(job->folder) : nullptr) o->end_ms = Now() + 500;
            AiAgent& a2 = Agent(s2);
            if (a2.generation != gen) return;
            if (done) {
                ToolOutcome out;
                out.data = json::Value::Object();
                out.data.set("ok", true);
                out.data.set("renamed", static_cast<uint64_t>(done));
                out.data.set("via", "batch_rename_dialog");
                out.data.set("note", "The user adjusted the rule in Pulse's Batch rename dialog and renamed the files "
                                     "there. They can undo it with Ctrl+Z in Pulse.");
                AddToolResult(a2, call_id, out);
            } else {
                AddToolResult(a2, call_id,
                              ToolOutcome::Fail("user_closed", "The user opened Batch rename with this rule and closed "
                                                               "it without renaming anything. Ask what they want."));
            }
            ++a2.call_index;
            AiChatSetChip(c3, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
            NextTool(s2);
            Redraw(s2);
        };
        return -1;
    });
}

void ToolRename(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
    AiAgent& a = Agent(s);
    const auto fail = [&](const char* code, std::string message) {
        AddToolResult(a, call.id, ToolOutcome::Fail(code, std::move(message)));
        ++a.call_index;
        NextTool(s);
    };
    auto job = std::make_shared<RenameJob>();
    rn::AiReply reply;
    if (!rn::ReadAiSteps(args, reply) || reply.steps.empty())
        return fail("invalid_args", reply.note.empty() ? "steps must list at least one valid step"
                                                       : "no usable step: " + Utf8(reply.note));
    job->steps = std::move(reply.steps);
    // {n} is the engine's legacy unpadded number; the model's "digits" is
    // meant for it, so read it as {number}.
    for (rn::Step& st : job->steps) {
        for (std::wstring* t : {&st.pattern, &st.text})
            for (size_t at = t->find(L"{n}"); at != std::wstring::npos; at = t->find(L"{n}", at + 8)) t->replace(at, 3, L"{number}");
    }
    job->opts.order = reply.has_order ? reply.order : rn::Order::List;
    job->opts.daily = reply.daily;
    job->opts.policy = rn::Policy::AutoNumber;
    // The files: the paths given, else the selection of the active tab.
    std::vector<std::wstring> paths;
    if (const json::Value* p = args.find("paths"); p && p->is_array())
        for (const json::Value& v : p->items()) paths.push_back(json::Wide(v.as_string()));
    if (paths.empty()) {
        app::Tab* tab = ActiveTab(s);
        if (tab && tab->snapshot && !fs::IsVirtualPath(tab->current_path)) paths = SelectedFullPaths(*tab);
        if (paths.empty())
            return fail("no_files", "Nothing is selected in Pulse. Pass paths, or ask the user to select the files first.");
    }
    std::unordered_set<std::wstring> seen;
    for (const std::wstring& p : paths) {
        if (p.size() < 3 || fs::IsVirtualPath(p)) return fail("invalid_path", "not an absolute path: " + Utf8(p));
        const DWORD attr = GetFileAttributesW(p.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) return fail("not_found", "no such file: " + Utf8(p));
        if (attr & FILE_ATTRIBUTE_DIRECTORY) return fail("not_a_file", "only files can be renamed here: " + Utf8(p));
        if (!seen.insert(rn::Lower(p)).second) continue;
        rn::Source src;
        src.path = p;
        src.name = Leaf(p);
        src.folder = Leaf(ParentDir(p));
        job->sources.push_back(std::move(src));
    }
    if (job->sources.size() > kMaxRename) return fail("too_many", "at most 2000 files at once");
    job->folder = ParentDir(job->sources.front().path);
    std::unordered_set<std::wstring> parents;
    for (const auto& src : job->sources) parents.insert(rn::Lower(ParentDir(src.path)));
    job->one_folder = parents.size() == 1;
    job->label = job->one_folder ? FolderLabel(job->folder) : Fill(Text(I::AiRnFolders), L"{n}", Num(parents.size()));
    job->needs_taken = rn::NeedsTakenDate(job->steps, job->opts);
    // 读取拍摄时间: a spark walks the photos while the properties are read.
    const size_t n = job->sources.size();
    std::shared_ptr<const ui::AiListOverlay> ov;
    if (job->needs_taken) {
        AddStep(s, a, ui::AiIcon::Camera, Text(I::AiRnStepRead), Fill(Text(I::AiRnMeta), L"{n}", Num(n)),
                Text(I::AiAsstStepReadWait));
        AiChatSetChip(Chat(s), ui::StatusAiState::Busy, Text(I::AiAsstChipReadingAny), 0.1f);
        if (job->one_folder && Shown(s, job->folder) && n <= 400) {
            ui::AiListOverlay& o = ListOverlay(job->folder);
            o.read_names.clear();
            for (const auto& src : job->sources) o.read_names.insert(ui::AiListLower(src.name));
            o.read_total = o.read_names.size();
            o.read_ms = (std::min<uint64_t>)(185, (std::max<uint64_t>)(12, 2400 / o.read_total)) * o.read_total;
            o.read_start = Now() + 300;
            o.watch_text = Text(I::AiListWatch);
            o.dests.clear();
            o.morph.clear();
            o.end_ms = 0;
            ov = g_list;
        }
    }
    const std::string id = call.id;
    const uint64_t gen = a.generation;
    RunJob(s, [job](const std::atomic<bool>& cancelled) { return ReadRenameSources(*job, cancelled); },
           [job, id, gen, ov](AppState& st, const ToolOutcome& out) {
               AiAgent& ag = Agent(st);
               if (ag.generation != gen) return;
               const auto finish = [job, id](AppState& s2, const ToolOutcome& res) {
                   AiAgent& a2 = Agent(s2);
                   if (!res.ok) {
                       if (job->needs_taken) EndStep(s2, a2, false, Fill(Text(I::AiAsstStepFailed), L"{reason}", json::Wide(res.error.message)));
                       AddToolResult(a2, id, res);
                       ++a2.call_index;
                       AiChatPush(s2, 250, [](AppState& s3) { NextTool(s3); return -1; });
                       return;
                   }
                   if (job->needs_taken) {
                       const std::wstring read = Fill(Fill(Text(I::AiRnStepReadDone), L"{k}", Num(job->with_taken)), L"{m}",
                                                      Num(job->media - job->with_taken));
                       EndStep(s2, a2, true, read);
                       // Its own fold label (FoldSteps would repeat the context read's).
                       if (AiBlockView* b = Block(s2, a2.steps_id)) {
                           b->label = read;
                           b->collapsed = true;
                           b->open = false;
                       }
                       a2.steps_id = 0;
                   }
                   const rn::Plan& plan = job->plan;
                   if (plan.bad_regex || plan.blocked) {
                       AddToolResult(a2, id, ToolOutcome::Fail("bad_rule", plan.bad_regex ? "the regex is not valid"
                                                                                           : "the rule gives clashing names"));
                       ++a2.call_index;
                       AiChatPush(s2, 250, [](AppState& s3) { NextTool(s3); return -1; });
                       return;
                   }
                   if (plan.renames == 0) {
                       ToolOutcome none = ToolOutcome::Fail(
                           "no_change", plan.same ? "Every name stays the same with this rule; nothing to rename."
                                                  : "No file gets a usable new name with this rule (empty or invalid names).");
                       AddToolResult(a2, id, none);
                       ++a2.call_index;
                       AiChatPush(s2, 250, [](AppState& s3) { NextTool(s3); return -1; });
                       return;
                   }
                   ShowRenamePlan(s2, job, id);
                   Redraw(s2);
               };
               if (!ov) {
                   finish(st, out);
                   return;
               }
               // Let the walk reach the last photo first; the step and chip count along.
               AiChatPush(st, 0, [ov, gen, out, finish](AppState& s2) {
                   AiAgent& a2 = Agent(s2);
                   if (a2.generation != gen) return -1;
                   const uint64_t now = Now();
                   if (const size_t k = ui::AiListReadK(*ov, now)) {
                       const std::wstring kk = Num(k), nn = Num(ov->read_total);
                       if (ui::AiStepView* step = LastStep(s2, a2))
                           step->sub = Fill(Fill(Text(I::AiAsstStepReading), L"{k}", kk), L"{n}", nn);
                       AiChatSetChip(Chat(s2), ui::StatusAiState::Busy,
                                     Fill(Fill(Text(I::AiAsstChipReading), L"{k}", kk), L"{n}", nn),
                                     static_cast<float>(k) / static_cast<float>(ov->read_total));
                   }
                   if (now < ov->read_start + ov->read_ms + 300) return 50;
                   finish(s2, out);
                   return -1;
               });
           });
}

// ------------------------------------------------------------ run_task --
// Flow C (plans/ai/Pulse_AI助手_第一批扩展.html): find files (or take the
// selection), file them into a folder (by month, year or type) and zip them.
// The plan is built from the real files, the user confirms it once, the whole
// task is one undo group, and a file that cannot move pauses the task and asks.

std::unordered_set<uint64_t> g_task_ops;   // operations the running task submitted

constexpr size_t kMaxTask = 2000;

struct TaskPlan {
    std::wstring label;              // 今年的发票 (the model's words)
    std::wstring noun;               // 发票: what the files are
    bool from_selection = false;
    std::wstring dest;               // absolute
    std::wstring dest_label;         // 文档 › 发票
    bool dest_exists = false;
    enum class By : uint8_t { None, Month, Year, Type } by = By::None;
    bool move = true;
    struct Item {
        std::wstring src, name, group;   // group: subfolder name (empty: dest itself)
        std::wstring target;             // the folder it goes to
        uint64_t size = 0;
        bool clash = false;              // the name is taken there (KeepBoth numbers it)
        bool already = false;            // already where it goes
        enum class St : uint8_t { Todo, Moved, Skipped } st = St::Todo;
        std::wstring now;                // its path once moved
    };
    std::vector<Item> items;
    struct Folder {
        std::wstring name, path;
        bool exists = false;
    };
    std::vector<Folder> folders;     // subfolders, in order
    size_t new_folders = 0;          // dest itself counts when missing
    size_t moves = 0;                // items that move
    size_t clashes = 0;
    std::wstring zip_name, zip_path;
    std::wstring where;              // 下载 8 · 桌面 3 · 文档 3
    size_t ignored = 0;              // folders, hidden and system files left out
    uint64_t bytes = 0;
    uint64_t find_ms = 0;
    // The card.
    uint32_t card_id = 0;
    int step_folders = -1, step_move = -1, step_zip = -1;
    size_t steps = 1;
    // Running.
    uint64_t undo_group = 0;
    uint64_t started = 0;
    size_t moved = 0, skipped = 0;
    bool skipped_locked = false;
    bool zip_ok = false;
    uint64_t zip_size = 0;
    std::wstring zip_error;
};

std::wstring KnownFolder(const KNOWNFOLDERID& id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) && p) out = p;
    CoTaskMemFree(p);
    return out;
}

bool UnderPath(const std::wstring& path, const std::wstring& root) {
    if (root.empty() || path.size() < root.size()) return false;
    if (_wcsnicmp(path.c_str(), root.c_str(), root.size()) != 0) return false;
    return path.size() == root.size() || path[root.size()] == L'\\' || root.back() == L'\\';
}

// dest: absolute, or a known folder's name first (Documents\发票, 文档\发票).
std::wstring ResolveTaskDest(std::wstring d) {
    for (wchar_t& ch : d)
        if (ch == L'/') ch = L'\\';
    while (!d.empty() && std::iswspace(d.front())) d.erase(0, 1);
    while (!d.empty() && (std::iswspace(d.back()) || d.back() == L'\\')) d.pop_back();
    if (d.empty()) return {};
    std::wstring base, rest;
    if ((d.size() >= 2 && d[1] == L':') || d.rfind(L"\\\\", 0) == 0) {
        d = NormDir(d);
        if (d.size() == 2) d += L'\\';
        return d.size() >= 3 ? d : std::wstring();
    }
    const size_t cut = d.find(L'\\');
    const std::wstring head = d.substr(0, cut);
    rest = cut == std::wstring::npos ? std::wstring() : d.substr(cut + 1);
    struct Known {
        const KNOWNFOLDERID* id;
        const wchar_t* names;   // '|' separated
    };
    static const Known kKnown[] = {
        {&FOLDERID_Documents, L"documents|document|docs|my documents|文档|我的文档"},
        {&FOLDERID_Downloads, L"downloads|download|下载"},
        {&FOLDERID_Desktop, L"desktop|桌面"},
        {&FOLDERID_Pictures, L"pictures|picture|photos|图片|照片"},
        {&FOLDERID_Videos, L"videos|video|视频"},
        {&FOLDERID_Music, L"music|音乐"},
        {&FOLDERID_Profile, L"home|~|用户|个人文件夹"},
    };
    for (const Known& k : kKnown) {
        bool hit = false;
        std::wstring_view names = k.names;
        while (!names.empty() && !hit) {
            const size_t bar = names.find(L'|');
            const std::wstring_view one = names.substr(0, bar);
            hit = one.size() == head.size() && _wcsnicmp(one.data(), head.c_str(), head.size()) == 0;
            names = bar == std::wstring_view::npos ? std::wstring_view() : names.substr(bar + 1);
        }
        const std::wstring path = KnownFolder(*k.id);
        if (path.empty()) continue;
        if (!hit) hit = _wcsicmp(head.c_str(), Leaf(path).c_str()) == 0 || head == app::TabTitle(path);
        if (hit) {
            base = path;
            break;
        }
    }
    if (base.empty()) return {};
    return rest.empty() ? base : JoinPath(base, rest);
}

// Each part under a known root is a valid name.
bool ValidDestParts(const std::wstring& dest) {
    size_t start = dest.rfind(L"\\\\", 0) == 0 ? 2 : 3;
    while (start < dest.size()) {
        size_t end = dest.find(L'\\', start);
        if (end == std::wstring::npos) end = dest.size();
        if (!ValidName(dest.substr(start, end - start))) return false;
        start = end + 1;
    }
    return true;
}

I TypeFolder(const std::wstring& name) {
    const size_t dot = name.rfind(L'.');
    std::wstring ext = dot == std::wstring::npos ? std::wstring() : ui::AiListLower(name.substr(dot + 1));
    struct Kind {
        I id;
        const wchar_t* exts;
    };
    static const Kind kKinds[] = {
        {I::AiTkTypeDocs, L"|pdf|doc|docx|xls|xlsx|ppt|pptx|txt|rtf|odt|ods|odp|csv|md|epub|wps|et|dps|ofd|xps|pages|numbers|key|"},
        {I::AiTkTypeImages, L"|jpg|jpeg|png|gif|bmp|webp|heic|heif|tif|tiff|svg|raw|cr2|cr3|nef|arw|dng|avif|ico|psd|jxl|"},
        {I::AiTkTypeVideos, L"|mp4|mkv|mov|avi|wmv|flv|webm|m4v|ts|mts|m2ts|3gp|mpg|mpeg|"},
        {I::AiTkTypeAudio, L"|mp3|wav|flac|aac|m4a|ogg|wma|opus|ape|aiff|amr|mid|"},
        {I::AiTkTypeArchives, L"|zip|rar|7z|tar|gz|bz2|xz|tgz|zst|iso|cab|lzh|"},
        {I::AiTkTypeCode, L"|c|cc|cpp|h|hpp|cs|js|jsx|tsx|py|java|go|rs|json|xml|html|css|sql|sh|ps1|bat|yaml|yml|toml|kt|swift|php|rb|lua|"},
    };
    if (!ext.empty()) {
        const std::wstring key = L"|" + ext + L"|";
        for (const Kind& k : kKinds)
            if (wcsstr(k.exts, key.c_str())) return k.id;
    }
    return I::AiTkTypeOther;
}

int TypeRank(const std::wstring& name) {
    static const I kOrder[] = {I::AiTkTypeDocs, I::AiTkTypeImages, I::AiTkTypeVideos, I::AiTkTypeAudio,
                               I::AiTkTypeArchives, I::AiTkTypeCode, I::AiTkTypeOther};
    for (int i = 0; i < 7; ++i)
        if (name == Text(kOrder[i])) return i;
    return 7;
}

// The plan from the real files (worker thread: attributes and folder checks).
ToolOutcome BuildTaskPlan(TaskPlan& p, const std::vector<std::wstring>& paths,
                          const std::vector<std::wstring>& guarded, const std::vector<std::wstring>& type_names) {
    p.dest_exists = IsDirectory(p.dest);
    if (!p.dest_exists && Exists(p.dest)) return ToolOutcome::Fail("invalid_args", "dest is a file, not a folder");
    std::unordered_set<std::wstring> seen;
    std::unordered_map<std::wstring, std::unordered_set<std::wstring>> taken;   // target -> names claimed
    std::unordered_map<std::wstring, size_t> folder_at;
    for (const std::wstring& raw : paths) {
        const std::wstring path = NormDir(raw);
        if (!seen.insert(ui::AiListLower(path)).second) continue;
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW(app::ai::Win32Path(path).c_str(), GetFileExInfoStandard, &fa)) {
            ++p.ignored;
            continue;
        }
        bool skip = (fa.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
        for (const std::wstring& g : guarded) skip = skip || UnderPath(path, g);
        if (skip) {
            ++p.ignored;
            continue;
        }
        if (p.items.size() >= kMaxTask)
            return ToolOutcome::Fail("too_many", "more than 2000 files; narrow the search");
        TaskPlan::Item it;
        it.src = path;
        it.name = Leaf(path);
        it.size = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        FILETIME local{};
        SYSTEMTIME t{};
        FileTimeToLocalFileTime(&fa.ftLastWriteTime, &local);
        FileTimeToSystemTime(&local, &t);
        wchar_t buf[16];
        switch (p.by) {
        case TaskPlan::By::Month:
            swprintf_s(buf, L"%04u-%02u", t.wYear, t.wMonth);
            it.group = buf;
            break;
        case TaskPlan::By::Year:
            swprintf_s(buf, L"%04u", t.wYear);
            it.group = buf;
            break;
        case TaskPlan::By::Type:
            it.group = type_names[static_cast<size_t>(TypeRank(Text(TypeFolder(it.name))))];
            break;
        case TaskPlan::By::None:
            break;
        }
        it.target = it.group.empty() ? p.dest : JoinPath(p.dest, it.group);
        if (p.move) {
            it.already = SameDir(app::ai::ParentFolder(path), it.target);
            const std::wstring tkey = ui::AiListLower(it.target);
            const std::wstring nkey = ui::AiListLower(it.name);
            if (!it.already) {
                it.clash = !taken[tkey].insert(nkey).second || Exists(JoinPath(it.target, it.name));
                if (it.clash) ++p.clashes;
                ++p.moves;
            } else {
                taken[tkey].insert(nkey);
            }
            if (!it.group.empty() && !folder_at.count(ui::AiListLower(it.group))) {
                folder_at.emplace(ui::AiListLower(it.group), p.folders.size());
                TaskPlan::Folder f;
                f.name = it.group;
                f.path = it.target;
                f.exists = IsDirectory(f.path);
                if (!f.exists && Exists(f.path))
                    return ToolOutcome::Fail("invalid_args", "a file named " + Utf8(f.name) + " is in the way in dest");
                p.folders.push_back(std::move(f));
            }
        }
        p.bytes += it.size;
        p.items.push_back(std::move(it));
    }
    if (p.items.empty())
        return ToolOutcome::Fail("no_files", p.ignored ? "only folders, hidden or system files matched; nothing to do"
                                                       : "no files matched");
    if (p.by == TaskPlan::By::Type)
        std::stable_sort(p.folders.begin(), p.folders.end(),
                         [](const auto& x, const auto& y) { return TypeRank(x.name) < TypeRank(y.name); });
    else
        std::sort(p.folders.begin(), p.folders.end(), [](const auto& x, const auto& y) { return x.name < y.name; });
    p.new_folders = p.dest_exists ? 0 : 1;
    for (const auto& f : p.folders)
        if (!f.exists) ++p.new_folders;
    if (!p.zip_name.empty()) {
        std::wstring stem = p.zip_name;
        if (stem.size() > 4 && _wcsicmp(stem.c_str() + stem.size() - 4, L".zip") == 0) stem.resize(stem.size() - 4);
        p.zip_name = stem + L".zip";
        p.zip_path = JoinPath(p.dest, p.zip_name);
        for (int k = 2; Exists(p.zip_path) && k < 100; ++k) {
            p.zip_name = stem + L" (" + std::to_wstring(k) + L").zip";
            p.zip_path = JoinPath(p.dest, p.zip_name);
        }
    }
    if (p.moves == 0 && p.zip_name.empty())
        return ToolOutcome::Fail("nothing_to_do", "every file is already where it should go");
    return {};
}

// "2026-01、02、03…" for month folders, else the names; at most `max`.
std::wstring FolderList(const TaskPlan& p, bool new_only, size_t max) {
    std::vector<std::wstring> names;
    if (new_only && !p.dest_exists) names.push_back(Leaf(p.dest));
    for (const auto& f : p.folders)
        if (!new_only || !f.exists) names.push_back(f.name);
    std::wstring out, year;
    const std::wstring sep = Text(I::AiTkListSep);
    for (size_t i = 0; i < names.size() && i < max; ++i) {
        std::wstring n = names[i];
        if (p.by == TaskPlan::By::Month && n.size() == 7 && n[4] == L'-') {
            if (n.substr(0, 4) == year) n = n.substr(5);
            else year = n.substr(0, 4);
        }
        if (!out.empty()) out += sep;
        out += n;
    }
    if (names.size() > max) out += L"\u2026";
    return out;
}

// Splits "新建 {n} 个文件夹" around {n}.
ui::AiTaskSumView Sum(ui::AiIcon icon, I text, size_t n) {
    ui::AiTaskSumView v;
    v.icon = icon;
    const std::wstring t = Text(text);
    const size_t at = t.find(L"{n}");
    v.before = at == std::wstring::npos ? t : t.substr(0, at);
    v.bold = Num(n);
    v.after = at == std::wstring::npos ? std::wstring() : t.substr(at + 3);
    return v;
}

ui::AiResultLine Line(bool warn, std::wstring text, const std::wstring& key, const std::wstring& bold) {
    ui::AiResultLine l;
    l.warn = warn;
    const size_t at = text.find(key);
    if (at == std::wstring::npos) {
        l.before = std::move(text);
        return l;
    }
    l.before = text.substr(0, at);
    l.bold = bold;
    l.after = text.substr(at + key.size());
    return l;
}

struct TaskRun {
    std::shared_ptr<TaskPlan> p;
    std::string call_id;
    uint64_t gen = 0;
    enum class Phase : uint8_t { Folders, Moves, Zip, Done } phase = Phase::Folders;
    // Folders.
    bool dest_sent = false;
    size_t folder_next = 0;
    uint64_t next_at = 0;
    uint64_t last_change = 0;
    bool navigated = false;
    // Moves.
    size_t next = 0;                 // the next item to send
    size_t cur = SIZE_MAX;           // the item in flight
    uint64_t cur_id = 0, cur_at = 0;
    std::unordered_set<std::wstring> before;   // a clashing item's target names before it moved
    uint32_t stagger = 0;
    // Zip.
    std::shared_ptr<AiAgent::Job> zip_job;
    std::shared_ptr<std::atomic<uint64_t>> zip_read, zip_total;
    std::shared_ptr<app::ZipCompressResult> zip_res;
    std::vector<std::wstring> zip_sources, zip_names;
    uint64_t zip_started = 0;
    // Paused.
    bool paused = false;
    uint64_t paused_at = 0;
    bool pause_lock = false;
    bool pause_folder = false;
    std::wstring pause_name;
};

void PumpTask(AppState& s, std::shared_ptr<TaskRun> run, uint32_t delay);

AiBlockView* TaskCard(AppState& s, const TaskPlan& p) { return Chat(s).Find(p.card_id); }

void SetTaskStep(AppState& s, const TaskPlan& p, int idx, ui::AiTaskState state, float progress,
                 const std::wstring* desc = nullptr) {
    AiBlockView* b = TaskCard(s, p);
    if (!b || idx < 0 || static_cast<size_t>(idx) >= b->task.size()) return;
    ui::AiTaskStepView& st = b->task[static_cast<size_t>(idx)];
    if (st.state != state) {
        st.state = state;
        st.state_ms = Now();
    }
    st.progress = progress;
    if (desc) st.desc = *desc;
    if (state == ui::AiTaskState::Running || state == ui::AiTaskState::Paused)
        b->meta = Fill(Fill(Text(I::AiTkPlanMetaRun), L"{n}", Num(p.steps)), L"{k}", Num(static_cast<size_t>(idx) + 1));
}

void TaskChip(AppState& s, const TaskPlan& p, int idx, const std::wstring& extra, float progress) {
    std::wstring t = Fill(Fill(Text(I::AiTkChipStep), L"{k}", Num(static_cast<size_t>(idx) + 1)), L"{n}", Num(p.steps));
    if (!extra.empty()) t += L" \u00B7 " + extra;
    AiChatSetChip(Chat(s), ui::StatusAiState::Busy, t, progress);
}

std::wstring MovedOf(const TaskPlan& p) {
    return Fill(Fill(Text(I::AiTkMoving), L"{k}", Num(p.moved)), L"{n}", Num(p.moves));
}

ops::OpRequest TaskRequest(const TaskPlan& p) {
    ops::OpRequest r;
    r.undo_group = p.undo_group;
    r.undo_label = Fill(Text(I::AiTkUndoLabel), L"{label}", p.label);
    return r;
}

void SubmitTaskFolder(AppState& s, TaskRun& run, const std::wstring& path) {
    ops::OpRequest r = TaskRequest(*run.p);
    r.type = ops::OpType::CreateFolder;
    r.sources.push_back(path);
    g_task_ops.insert(s.ops.Submit(std::move(r)));
}

std::unordered_set<std::wstring> NamesLike(const std::wstring& dir, const std::wstring& name) {
    std::unordered_set<std::wstring> out;
    const size_t dot = name.rfind(L'.');
    const std::wstring stem = dot == std::wstring::npos ? name : name.substr(0, dot);
    const std::wstring ext = dot == std::wstring::npos ? std::wstring() : name.substr(dot);
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(app::ai::Win32Path(JoinPath(dir, stem + L"*" + ext)).c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        out.insert(ui::AiListLower(fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

void SubmitTaskMove(AppState& s, TaskRun& run, size_t i) {
    TaskPlan& p = *run.p;
    TaskPlan::Item& it = p.items[i];
    if (it.clash) run.before = NamesLike(it.target, it.name);
    ops::OpRequest r = TaskRequest(p);
    r.type = ops::OpType::Move;
    r.dest_dir = it.target;
    r.collision_policy = ops::CollisionPolicy::KeepBoth;
    r.sources.push_back(it.src);
    run.cur = i;
    run.cur_at = Now();
    run.cur_id = s.ops.Submit(std::move(r));
    g_task_ops.insert(run.cur_id);
    if (ui::AiListOverlay* o = OverlayOf(p.dest); o && !it.group.empty())
        o->bumps[ui::AiListLower(it.group)].push_back(Now() + 420);
}

// Where a moved item ended up.
std::wstring LandedPath(const TaskRun& run, const TaskPlan::Item& it) {
    if (!it.clash) return JoinPath(it.target, it.name);
    for (const std::wstring& n : NamesLike(it.target, it.name)) {
        if (run.before.count(n)) continue;
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileExW(app::ai::Win32Path(JoinPath(it.target, n)).c_str(), FindExInfoBasic, &fd,
                                    FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) continue;
        FindClose(h);
        return JoinPath(it.target, fd.cFileName);
    }
    return JoinPath(it.target, it.name);
}

void FinishTask(AppState& s, std::shared_ptr<TaskRun> run);
void TaskUndo(AppState& s, std::shared_ptr<TaskPlan> p, std::shared_ptr<TaskRun> abort_run);

// A file (or folder) would not go: the card turns amber and asks.
void PauseTask(AppState& s, std::shared_ptr<TaskRun> run, const ops::OpStatus& st) {
    TaskPlan& p = *run->p;
    AiChat& c = Chat(s);
    run->paused = true;
    run->paused_at = Now();
    run->pause_lock = !st.lock_owners.empty();
    const int idx = run->pause_folder ? p.step_folders : p.step_move;
    std::wstring desc;
    if (!run->pause_folder) {
        desc = Fill(Fill(Fill(Text(I::AiTkMovePaused), L"{k}", Num(p.moved)), L"{n}", Num(p.moves)), L"{name}",
                    run->pause_name);
    }
    SetTaskStep(s, p, idx, ui::AiTaskState::Paused,
                run->pause_folder ? 0.0f : static_cast<float>(p.moved) / static_cast<float>((std::max<size_t>)(p.moves, 1)),
                run->pause_folder ? nullptr : &desc);
    AiBlockView& e = c.Add(AiBlockKind::Error, Now());
    e.tone = ui::AiErrTone::Warn;
    e.icon = run->pause_lock ? ui::AiIcon::Lock : ui::AiIcon::Warn;
    e.title = Text(run->pause_lock ? I::AiTkPausedLock : I::AiTkPausedFail);
    if (run->pause_folder)
        e.sub = Text(I::AiTkPausedFolderSub);
    else
        e.sub = Fill(Fill(Fill(Fill(Text(I::AiTkPausedSub), L"{d}", Num(static_cast<size_t>(idx))), L"{k}",
                               Num(static_cast<size_t>(idx) + 1)),
                          L"{m}", Num(p.moved)),
                     L"{n}", Num(p.moves));
    e.file = run->pause_name;
    if (run->pause_lock) {
        const ops::LockOwner& o = st.lock_owners.front();
        std::wstring app_name = o.app_name;
        if (app_name.empty() && !o.image_name.empty()) {
            app_name = o.image_name;
            if (app_name.size() > 4 && _wcsicmp(app_name.c_str() + app_name.size() - 4, L".exe") == 0)
                app_name.resize(app_name.size() - 4);
        }
        e.file_note = app_name.empty() ? Text(I::AiTkInUseAny) : Fill(Text(I::AiTkInUse), L"{app}", app_name);
    } else {
        std::wstring why = !st.failures.empty() ? st.failures.front().reason : st.last_error;
        if (why.size() > 28) why = why.substr(0, 27) + L"\u2026";
        e.file_note = why;
    }
    auto btn = [](ui::AiErrAct act, ui::AiIcon icon, I text, bool primary) {
        ui::AiErrButton b;
        b.act = act;
        b.icon = icon;
        b.text = Text(text);
        b.primary = primary;
        return b;
    };
    e.actions.push_back(btn(ui::AiErrAct::TaskRetry, ui::AiIcon::Retry, I::AiAsstRetry, run->pause_folder));
    if (!run->pause_folder) e.actions.push_back(btn(ui::AiErrAct::TaskSkip, ui::AiIcon::Skip, I::AiTkBtnSkip, true));
    e.actions.push_back(btn(ui::AiErrAct::TaskUndo, ui::AiIcon::Undo, I::AiTkBtnUndo, false));
    c.pause_id = e.id;
    AiChatSetChip(c, ui::StatusAiState::Waiting, Text(I::AiTkChipPaused));
    if (!s.aiAssistant.open) c.unread = true;
    c.on_task_act = [run](AppState& s2, ui::AiErrAct act) {
        AiChat& c2 = Chat(s2);
        TaskPlan& p2 = *run->p;
        AiBlockView* card = c2.Find(c2.pause_id);
        const std::wstring name = run->pause_name;
        if (card) {
            card->done = true;
            card->icon = act == ui::AiErrAct::TaskSkip ? ui::AiIcon::Skip
                         : act == ui::AiErrAct::TaskRetry ? ui::AiIcon::Retry : ui::AiIcon::Undo;
            card->title = act == ui::AiErrAct::TaskUndo ? Text(I::AiTkChoseUndo)
                                                        : Fill(Text(act == ui::AiErrAct::TaskSkip ? I::AiTkChoseSkip
                                                                                                    : I::AiTkChoseRetry),
                                                               L"{name}", name);
        }
        c2.pause_id = 0;
        c2.on_task_act = nullptr;
        const uint64_t now = Now();
        if (now > run->paused_at) {
            c2.turn_start += now - run->paused_at;   // waiting is not model time
            p2.started += now - run->paused_at;      // nor the task's own time
        }
        run->paused = false;
        if (Agent(s2).generation != run->gen) return;
        if (act == ui::AiErrAct::TaskUndo) {
            TaskUndo(s2, run->p, run);
            return;
        }
        if (run->pause_folder) {
            run->pause_folder = false;
            run->dest_sent = false;
            run->folder_next = 0;
            run->last_change = now;
            SetTaskStep(s2, p2, p2.step_folders, ui::AiTaskState::Running, 0.0f);
        } else if (act == ui::AiErrAct::TaskSkip) {
            p2.items[run->cur].st = TaskPlan::Item::St::Skipped;
            ++p2.skipped;
            if (run->pause_lock) p2.skipped_locked = true;
            run->cur = SIZE_MAX;
            if (AiBlockView* b = TaskCard(s2, p2); b && p2.step_move >= 0) {
                ui::AiTaskStepView& sv = b->task[static_cast<size_t>(p2.step_move)];
                sv.tag = Fill(Text(I::AiTkTagSkipped), L"{n}", Num(p2.skipped));
                sv.tag_warn = true;
            }
            const std::wstring d = MovedOf(p2);
            SetTaskStep(s2, p2, p2.step_move, ui::AiTaskState::Running,
                        static_cast<float>(p2.moved + p2.skipped) / static_cast<float>((std::max<size_t>)(p2.moves, 1)), &d);
        } else {
            const size_t i = run->cur;
            run->cur = SIZE_MAX;
            SubmitTaskMove(s2, *run, i);
            const std::wstring d = MovedOf(p2);
            SetTaskStep(s2, p2, p2.step_move, ui::AiTaskState::Running,
                        static_cast<float>(p2.moved) / static_cast<float>((std::max<size_t>)(p2.moves, 1)), &d);
        }
        PumpTask(s2, run, 120);
        Redraw(s2);
    };
}

void StartTaskZip(AppState& s, std::shared_ptr<TaskRun> run) {
    TaskPlan& p = *run->p;
    run->phase = TaskRun::Phase::Zip;
    run->zip_sources.clear();
    run->zip_names.clear();
    std::unordered_set<std::wstring> used;
    for (const auto& it : p.items) {
        if (it.st == TaskPlan::Item::St::Skipped) continue;
        const std::wstring path = !p.move || it.already ? it.src : it.now;
        if (path.empty() || !Exists(path)) continue;
        std::wstring leaf = Leaf(path);
        std::wstring entry = it.group.empty() ? leaf : it.group + L"/" + leaf;
        // Two files of one name from different folders (zip only).
        for (int k = 2; !used.insert(ui::AiListLower(entry)).second && k < 1000; ++k) {
            const size_t dot = leaf.rfind(L'.');
            const std::wstring n = dot == std::wstring::npos
                                       ? leaf + L" (" + std::to_wstring(k) + L")"
                                       : leaf.substr(0, dot) + L" (" + std::to_wstring(k) + L")" + leaf.substr(dot);
            entry = it.group.empty() ? n : it.group + L"/" + n;
        }
        run->zip_sources.push_back(path);
        run->zip_names.push_back(entry);
    }
    if (run->zip_sources.empty()) {
        p.zip_error = Text(I::AiTkZipNothing);
        run->phase = TaskRun::Phase::Done;
        return;
    }
    const std::wstring d = Fill(Text(I::AiTkZipping), L"{p}", L"0");
    SetTaskStep(s, p, p.step_zip, ui::AiTaskState::Running, 0.0f, &d);
    TaskChip(s, p, p.step_zip, Text(I::AiTkChipZip), 0.0f);
    auto job = std::make_shared<AiAgent::Job>();
    Agent(s).job = job;
    run->zip_job = job;
    run->zip_read = std::make_shared<std::atomic<uint64_t>>(0);
    run->zip_total = std::make_shared<std::atomic<uint64_t>>(0);
    run->zip_res = std::make_shared<app::ZipCompressResult>();
    run->zip_started = Now();
    std::thread([job, read = run->zip_read, total = run->zip_total, res = run->zip_res, sources = run->zip_sources,
                 names = run->zip_names, zip = p.zip_path] {
        *res = app::CompressToZip(sources, zip, &job->cancelled,
                                  [read, total](uint64_t r, uint64_t t) {
                                      read->store(r);
                                      total->store(t);
                                  },
                                  &names);
        job->done.store(true);
    }).detach();
}

// One tick of the running task; returns the next delay or -1.
int TickTask(AppState& s, std::shared_ptr<TaskRun> run) {
    if (Agent(s).generation != run->gen || run->paused) return -1;
    TaskPlan& p = *run->p;
    const uint64_t now = Now();
    if (run->phase == TaskRun::Phase::Folders) {
        if (!p.dest_exists && !run->dest_sent) {
            SubmitTaskFolder(s, *run, p.dest);
            run->dest_sent = true;
            run->last_change = now;
            return 60;
        }
        const bool dest_ready = IsDirectory(p.dest);
        if (dest_ready && !run->navigated) {
            run->navigated = true;
            NavigateTo(s, p.dest);
            ui::AiListOverlay& o = ListOverlay(p.dest);
            o.new_badge = Text(I::AiAsstNewBadge);
            o.new_folders.clear();
            o.leaving.clear();
            o.target.clear();
            o.targets.clear();
            o.bumps.clear();
            o.dests.clear();
            for (const auto& f : p.folders) o.targets.insert(ui::AiListLower(f.name));
            run->next_at = now + 300;
        }
        if (dest_ready && p.move) {
            while (run->folder_next < p.folders.size() && p.folders[run->folder_next].exists) ++run->folder_next;
            if (run->folder_next < p.folders.size() && now >= run->next_at) {
                const TaskPlan::Folder& f = p.folders[run->folder_next++];
                SubmitTaskFolder(s, *run, f.path);
                if (ui::AiListOverlay* o = OverlayOf(p.dest)) o->new_folders[ui::AiListLower(f.name)] = now;
                run->next_at = now + 120;
                run->last_change = now;
            }
        }
        size_t ready = dest_ready && !p.dest_exists ? 1 : 0;
        size_t want = p.dest_exists ? 0 : 1;
        if (p.move)
            for (const auto& f : p.folders)
                if (!f.exists) {
                    ++want;
                    if (IsDirectory(f.path)) ++ready;
                }
        if (p.step_folders >= 0) {
            const std::wstring d = Fill(Fill(Text(I::AiTkCreating), L"{k}", Num(ready)), L"{n}", Num(want));
            SetTaskStep(s, p, p.step_folders, ui::AiTaskState::Running,
                        want ? static_cast<float>(ready) / static_cast<float>(want) : 1.0f, &d);
            TaskChip(s, p, p.step_folders, {}, want ? static_cast<float>(ready) / static_cast<float>(want) : 1.0f);
        }
        const bool fed = dest_ready && (!p.move || run->folder_next >= p.folders.size());
        if (fed && ready >= want) {
            if (p.step_folders >= 0) {
                const std::wstring d = Fill(Text(I::AiTkFoldersDone), L"{n}", Num(want));
                SetTaskStep(s, p, p.step_folders, ui::AiTaskState::Done, 1.0f, &d);
            }
            run->phase = p.moves ? TaskRun::Phase::Moves : TaskRun::Phase::Zip;
            run->next_at = now + 380;
            if (run->phase == TaskRun::Phase::Zip) {
                StartTaskZip(s, run);
                return 60;
            }
            const std::wstring d = MovedOf(p);
            SetTaskStep(s, p, p.step_move, ui::AiTaskState::Running, 0.0f, &d);
            TaskChip(s, p, p.step_move, Fill(Fill(Text(I::AiTkChipOf), L"{k}", L"0"), L"{n}", Num(p.moves)), 0.0f);
            return 380;
        }
        // Nothing for four seconds with the queue idle: a folder failed.
        if (!s.ops.Status().active && now - run->last_change > 4000) {
            run->pause_folder = true;
            run->pause_name = !dest_ready ? Leaf(p.dest) : std::wstring();
            if (run->pause_name.empty())
                for (const auto& f : p.folders)
                    if (!f.exists && !IsDirectory(f.path)) {
                        run->pause_name = f.name;
                        break;
                    }
            PauseTask(s, run, s.ops.Status());
            Redraw(s);
            return -1;
        }
        return 60;
    }
    if (run->phase == TaskRun::Phase::Moves) {
        if (run->cur != SIZE_MAX) {
            TaskPlan::Item& it = p.items[run->cur];
            if (!Exists(it.src)) {
                it.st = TaskPlan::Item::St::Moved;
                it.now = LandedPath(*run, it);
                ++p.moved;
                run->cur = SIZE_MAX;
                const std::wstring d = MovedOf(p);
                const float prog = static_cast<float>(p.moved + p.skipped) / static_cast<float>(p.moves);
                SetTaskStep(s, p, p.step_move, ui::AiTaskState::Running, prog, &d);
                TaskChip(s, p, p.step_move,
                         Fill(Fill(Text(I::AiTkChipOf), L"{k}", Num(p.moved)), L"{n}", Num(p.moves)), prog);
                run->next_at = now + run->stagger;
            } else {
                const ops::OpStatus st = s.ops.Status();
                const bool failed = !st.active && st.task_id == run->cur_id && st.phase == ops::OpPhase::Failed;
                const bool lost = !st.active && st.task_id != run->cur_id && now - run->cur_at > 3000;
                const bool quiet = !st.active && st.task_id == run->cur_id && now - run->cur_at > 1500;
                if (failed || lost || quiet) {
                    run->pause_name = it.name;
                    PauseTask(s, run, st.task_id == run->cur_id ? st : ops::OpStatus{});
                    Redraw(s);
                    return -1;
                }
                return 40;
            }
        }
        while (run->next < p.items.size() && (p.items[run->next].already)) ++run->next;
        if (run->next >= p.items.size()) {
            const std::wstring d = Fill(Text(I::AiTkMoveDone), L"{n}", Num(p.moved));
            SetTaskStep(s, p, p.step_move, ui::AiTaskState::Done, 1.0f, &d);
            if (p.step_zip >= 0) {
                StartTaskZip(s, run);
                return run->phase == TaskRun::Phase::Zip ? 300 : 0;
            }
            run->phase = TaskRun::Phase::Done;
            return 0;
        }
        if (now < run->next_at) return static_cast<int>(run->next_at - now);
        SubmitTaskMove(s, *run, run->next++);
        return 30;
    }
    if (run->phase == TaskRun::Phase::Zip) {
        const uint64_t total = run->zip_total->load(), read = run->zip_read->load();
        const float prog = total ? static_cast<float>(read) / static_cast<float>(total) : 0.0f;
        const std::wstring d = Fill(Text(I::AiTkZipping), L"{p}", Num(static_cast<size_t>(prog * 100.0f + 0.5f)));
        SetTaskStep(s, p, p.step_zip, ui::AiTaskState::Running, prog, &d);
        TaskChip(s, p, p.step_zip, Text(I::AiTkChipZip), prog);
        if (!run->zip_job->done.load() || now - run->zip_started < 500) return 60;
        Agent(s).job.reset();
        const app::ZipCompressResult& r = *run->zip_res;
        p.zip_ok = r.ok;
        if (r.ok) {
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (GetFileAttributesExW(app::ai::Win32Path(p.zip_path).c_str(), GetFileExInfoStandard, &fa))
                p.zip_size = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
            s.ops.RecordExternalCreate(p.zip_path, p.undo_group, Fill(Text(I::AiTkUndoLabel), L"{label}", p.label));
            if (ui::AiListOverlay* o = OverlayOf(p.dest)) o->new_folders[ui::AiListLower(p.zip_name)] = now;
            const std::wstring dd = p.zip_name + L" \u00B7 " + AiChatFormatSize(p.zip_size);
            SetTaskStep(s, p, p.step_zip, ui::AiTaskState::Done, 1.0f, &dd);
        } else {
            p.zip_error = r.error.empty() ? Text(I::AiTkZipNothing) : r.error;
            const std::wstring dd = Fill(Text(I::AiTkZipFailed), L"{reason}", p.zip_error);
            SetTaskStep(s, p, p.step_zip, ui::AiTaskState::Paused, 0.0f, &dd);
        }
        run->phase = TaskRun::Phase::Done;
        return 450;   // let the zip row land
    }
    FinishTask(s, run);
    return -1;
}

void PumpTask(AppState& s, std::shared_ptr<TaskRun> run, uint32_t delay) {
    AiChatPush(s, delay, [run](AppState& st) {
        const int next = TickTask(st, run);
        if (next >= 0) Redraw(st);
        return next;
    });
}

json::Value TaskResultForModel(const TaskPlan& p) {
    json::Value out = json::Value::Object();
    out.set("ok", p.moved > 0 || p.zip_ok);
    out.set("found", static_cast<uint64_t>(p.items.size()));
    out.set("dest", Utf8(p.dest));
    out.set("folders_created", static_cast<uint64_t>(p.new_folders));
    out.set("moved", static_cast<uint64_t>(p.moved));
    out.set("already_in_place", static_cast<uint64_t>(p.items.size() - p.moves));
    if (p.skipped) {
        json::Value names = json::Value::Array();
        for (const auto& it : p.items)
            if (it.st == TaskPlan::Item::St::Skipped) names.push(Utf8(it.name));
        out.set("skipped", std::move(names));
        out.set("skipped_reason", p.skipped_locked ? "in use by another program; the user chose to skip"
                                                   : "could not be moved; the user chose to skip");
    }
    if (p.clashes) out.set("renamed_for_clash", static_cast<uint64_t>(p.clashes));
    if (!p.zip_name.empty()) {
        json::Value z = json::Value::Object();
        z.set("ok", p.zip_ok);
        z.set("path", Utf8(p.zip_path));
        if (p.zip_ok) z.set("size", Utf8(AiChatFormatSize(p.zip_size)));
        else z.set("error", Utf8(p.zip_error));
        out.set("zip", std::move(z));
    }
    if (p.ignored) out.set("ignored", static_cast<uint64_t>(p.ignored));
    out.set("undo", "The user can undo the whole task with 撤销全部 on the result card in Pulse.");
    return out;
}

void FinishTask(AppState& s, std::shared_ptr<TaskRun> run) {
    std::shared_ptr<TaskPlan> plan = run->p;
    TaskPlan& p = *plan;
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    g_task_ops.clear();
    if (AiBlockView* b = TaskCard(s, p)) {
        b->collapsed = true;
        b->meta = p.skipped ? Fill(Fill(Text(I::AiTkPlanMetaSkip), L"{n}", Num(p.steps)), L"{k}", Num(p.skipped))
                            : Fill(Text(I::AiTkPlanMetaDone), L"{n}", Num(p.steps));
    }
    AiBlockView& r = c.Add(AiBlockKind::Result, Now());
    r.title = Text(I::AiTkDoneTitle);
    wchar_t secs[16];
    swprintf_s(secs, L"%.1f", static_cast<double>(Now() - p.started) / 1000.0);
    r.sub = Fill(Fill(Text(I::AiTkDoneSub), L"{folder}", p.dest_label), L"{t}", secs);
    r.lines.push_back(Line(false, Fill(Text(p.from_selection ? I::AiTkLnSel : I::AiTkLnFound), L"{noun}", p.noun),
                           L"{n}", Num(p.items.size())));
    if (p.new_folders) {
        const bool subs = p.dest_exists && p.by != TaskPlan::By::None;
        r.lines.push_back(Line(false,
                               Text(!subs ? I::AiTkLnFolders
                                    : p.by == TaskPlan::By::Month ? I::AiTkLnFoldersMonth
                                    : p.by == TaskPlan::By::Year ? I::AiTkLnFoldersYear : I::AiTkLnFoldersType),
                               L"{n}", Num(p.new_folders)));
    }
    if (p.moves) {
        if (p.skipped)
            r.lines.push_back(Line(true,
                                   Fill(Fill(Text(I::AiTkLnSkipped), L"{k}", Num(p.skipped)), L"{reason}",
                                        Text(p.skipped_locked ? I::AiTkReasonLock : I::AiTkReasonFail)),
                                   L"{n}", Num(p.moved)));
        else
            r.lines.push_back(Line(false, Text(I::AiTkLnMoved), L"{n}", Num(p.moved)));
    }
    if (!p.zip_name.empty()) {
        if (p.zip_ok)
            r.lines.push_back(Line(false, Fill(Text(I::AiTkLnZip), L"{size}", AiChatFormatSize(p.zip_size)), L"{name}",
                                   p.zip_name));
        else
            r.lines.push_back(Line(true, Fill(Text(I::AiTkLnZipFail), L"{reason}", p.zip_error), L"{name}", p.zip_name));
    }
    r.deny = Text(I::AiTkUndoAll);
    r.accept = Text(I::AiAsstShowInPulse);
    c.result_id = r.id;
    // 在 Pulse 中查看: the folder, with the zip selected and flashing.
    c.on_show = [plan](AppState& st) {
        NavigateTo(st, plan->dest);
        if (!plan->zip_ok) return;
        if (app::Tab* tab = ActiveTab(st); tab && SameDir(tab->current_path, plan->dest)) {
            tab->pending_selected_names = {plan->zip_name};
            tab->pending_selected_name = plan->zip_name;
            tab->pending_selection_revision = tab->selection_revision;
        }
        ui::AiListOverlay& o = ListOverlay(plan->dest);
        o.flash[ui::AiListLower(plan->zip_name)] = Now() + 200;
        o.end_ms = Now() + 2500;
    };
    c.on_undo = [plan](AppState& st) { TaskUndo(st, plan, nullptr); };
    c.peek_text = Fill(Fill(Text(I::AiTkPeek), L"{n}", Num(p.moved)), L"{folder}", p.dest_label);
    c.peek_undo = p.moved > 0 || p.zip_ok;
    if (ui::AiListOverlay* o = OverlayOf(p.dest)) o->end_ms = Now() + 6000;
    ToolOutcome out;
    out.data = TaskResultForModel(p);
    AddToolResult(a, run->call_id, out);
    ++a.call_index;
    AiChatSetChip(c, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
    AiChatPush(s, 350, [](AppState& st) { NextTool(st); return -1; });
}

// 撤销全部 on the result card, or 撤销已做的 on the pause card (abort_run).
void TaskUndo(AppState& s, std::shared_ptr<TaskPlan> plan, std::shared_ptr<TaskRun> abort_run) {
    AiChat& c = Chat(s);
    g_task_ops.clear();
    const bool nothing = plan->moved == 0 && !plan->zip_ok && plan->new_folders == 0;
    const auto outcome = nothing ? ops::OpsManager::GroupUndo::Started : s.ops.UndoGroup(plan->undo_group);
    if (outcome != ops::OpsManager::GroupUndo::Started) {
        AiBlockView& note = c.Add(AiBlockKind::Note, Now());
        const bool pending = outcome == ops::OpsManager::GroupUndo::Pending;
        note.text = Text(pending ? I::AiAsstUndoBusy : I::AiAsstUndoGone);
        if (!abort_run && pending) c.on_undo = [plan](AppState& st) { TaskUndo(st, plan, nullptr); };
        if (abort_run) {
            AiAgent& a = Agent(s);
            AddToolResult(a, abort_run->call_id,
                          ToolOutcome::Fail("stopped", "The user stopped the task in Pulse after a file could not be "
                                                       "moved, and the undo could not run; some files were moved."));
            ++a.call_index;
            NextTool(s);
        }
        Redraw(s);
        return;
    }
    AiChatSetChip(c, ui::StatusAiState::Busy, Text(I::AiAsstChipUndoing), 0.5f);
    c.peek_undo = false;
    AiChatHidePeek(s);
    if (ui::AiListOverlay* o = OverlayOf(plan->dest)) {
        const uint64_t now = Now();
        o->new_folders.clear();
        o->bumps.clear();
        o->leaving.clear();
        for (const auto& f : plan->folders)
            if (!f.exists) o->leaving[ui::AiListLower(f.name)] = now;
        if (plan->zip_ok) o->leaving[ui::AiListLower(plan->zip_name)] = now;
        o->end_ms = now + 4000;
    }
    auto started = std::make_shared<uint64_t>(Now());
    AiChatPush(s, 300, [plan, started, abort_run](AppState& s3) {
        size_t back = 0;
        for (const auto& it : plan->items)
            if (it.st == TaskPlan::Item::St::Moved && Exists(it.src)) ++back;
        if (back < plan->moved && !(Now() - *started > 3000 && !s3.ops.Status().active) && Now() - *started < 600000)
            return 150;
        AiChat& c3 = Chat(s3);
        if (AiBlockView* b = TaskCard(s3, *plan)) {
            b->collapsed = true;
            b->meta = Fill(Text(I::AiTkPlanMetaUndone), L"{n}", Num(plan->steps));
        }
        if (!abort_run) {
            if (AiBlockView* rr = c3.Find(c3.result_id)) {
                rr->done = true;
                rr->title = Text(I::AiAsstUndone);
                rr->sub = Fill(Text(I::AiTkUndoneSub), L"{n}", Num(back));
            }
        }
        AiBlockView& note = c3.Add(AiBlockKind::Note, Now());
        note.text = Text(abort_run ? I::AiTkAborted : I::AiTkUndoNote);
        AiChatSetChip(c3, ui::StatusAiState::Done, Text(I::AiAsstUndone));
        if (abort_run) {
            AiAgent& a = Agent(s3);
            if (a.generation == abort_run->gen) {
                AddToolResult(a, abort_run->call_id,
                              ToolOutcome::Fail("user_undid", "A file could not be moved, the task paused and the user "
                                                              "chose to undo it in Pulse: everything done so far was "
                                                              "reversed and nothing changed. Do not run it again unless "
                                                              "asked."));
                ++a.call_index;
                AiChatSetChip(c3, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
                NextTool(s3);
            }
        } else if (c3.agent) {
            c3.agent->pending_note = L"(The user clicked 撤销全部 in Pulse: the last run_task was reversed; every file "
                                     L"is back where it was and the new folders and zip are gone.)";
        }
        Redraw(s3);
        return -1;
    });
}

void RunTask(AppState& s, std::shared_ptr<TaskPlan> plan, const std::string& call_id) {
    AiAgent& a = Agent(s);
    a.organized = true;
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    plan->undo_group = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    plan->started = Now();
    g_task_ops.clear();
    auto run = std::make_shared<TaskRun>();
    run->p = plan;
    run->call_id = call_id;
    run->gen = a.generation;
    run->last_change = Now();
    run->stagger = plan->moves <= 60 ? (std::max)(20u, (std::min)(190u, static_cast<uint32_t>(3000 / (std::max<size_t>)(plan->moves, 1)))) : 0u;
    if (ui::AiListOverlay* o = g_list.get()) {
        o->dest_end = Now();
        o->end_ms = Now() + 500;
    }
    if (plan->step_folders >= 0) {
        SetTaskStep(s, *plan, plan->step_folders, ui::AiTaskState::Running, 0.0f);
    }
    PumpTask(s, run, 200);
}

void ToolTask(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
    AiAgent& a = Agent(s);
    const auto fail = [&](const char* code, std::string message) {
        AddToolResult(a, call.id, ToolOutcome::Fail(code, std::move(message)));
        ++a.call_index;
        NextTool(s);
    };
    auto plan = std::make_shared<TaskPlan>();
    plan->label = json::Wide(args.get_string("label"));
    if (plan->label.size() > 30) plan->label = plan->label.substr(0, 30);
    plan->noun = json::Wide(args.get_string("noun"));
    if (plan->noun.size() > 12 || plan->noun.empty()) plan->noun = Text(I::AiTkFiles);
    plan->dest = ResolveTaskDest(json::Wide(args.get_string("dest")));
    if (plan->dest.empty() || fs::IsVirtualPath(plan->dest) || !ValidDestParts(plan->dest))
        return fail("invalid_args", "dest must be an absolute folder path or start with Documents, Downloads, Desktop, "
                                    "Pictures, Videos or Music, e.g. Documents\\\\Invoices");
    const std::string by = args.get_string("group_by");
    plan->by = by == "month" ? TaskPlan::By::Month : by == "year" ? TaskPlan::By::Year
             : by == "type" ? TaskPlan::By::Type : TaskPlan::By::None;
    if (const json::Value* m = args.find("move"); m && m->is_bool()) plan->move = m->as_bool();
    plan->zip_name = json::Wide(args.get_string("zip"));
    while (!plan->zip_name.empty() && std::iswspace(plan->zip_name.back())) plan->zip_name.pop_back();
    if (!plan->zip_name.empty() && !ValidName(plan->zip_name))
        return fail("invalid_args", "zip must be a plain file name such as Invoices-2026.zip");
    if (!plan->move && plan->zip_name.empty()) return fail("invalid_args", "with move false, give zip");
    if (!plan->zip_name.empty() && !app::ZipCompressAvailable())
        return fail("unsupported", "Zip files cannot be made on this PC (the archive component is missing).");
    // Places the task never takes files from.
    std::vector<std::wstring> guarded;
    for (const KNOWNFOLDERID* id : {&FOLDERID_Windows, &FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86,
                                    &FOLDERID_ProgramData, &FOLDERID_LocalAppData, &FOLDERID_RoamingAppData})
        if (std::wstring g = KnownFolder(*id); !g.empty()) guarded.push_back(std::move(g));
    for (const std::wstring& g : guarded)
        if (UnderPath(plan->dest, g)) return fail("invalid_args", "dest is inside a system or program folder");
    std::vector<std::wstring> type_names;
    for (I id : {I::AiTkTypeDocs, I::AiTkTypeImages, I::AiTkTypeVideos, I::AiTkTypeAudio, I::AiTkTypeArchives,
                 I::AiTkTypeCode, I::AiTkTypeOther, I::AiTkTypeOther})
        type_names.push_back(Text(id));
    const std::string call_id = call.id;
    const uint64_t gen = a.generation;

    // After the files are known: the plan on a worker, then the card.
    auto build = [plan, guarded, type_names, call_id, gen](AppState& st, std::vector<std::wstring> paths, uint64_t ms) {
        plan->find_ms = ms;
        auto list = std::make_shared<std::vector<std::wstring>>(std::move(paths));
        RunJob(st, [plan, list, guarded, type_names](const std::atomic<bool>&) {
                   return BuildTaskPlan(*plan, *list, guarded, type_names);
               },
               [plan, call_id, gen](AppState& s2, const ToolOutcome& out) {
                   AiAgent& a2 = Agent(s2);
                   if (a2.generation != gen) return;
                   AiChat& c2 = Chat(s2);
                   if (!out.ok) {
                       EndStep(s2, a2, false, Fill(Text(I::AiAsstStepFailed), L"{reason}", json::Wide(out.error.message)));
                       AddToolResult(a2, call_id, out);
                       ++a2.call_index;
                       AiChatPush(s2, 250, [](AppState& s3) { NextTool(s3); return -1; });
                       return;
                   }
                   TaskPlan& p = *plan;
                   p.dest_label = ResultFolderLabel(p.dest);
                   // Where the files are: 下载 8 · 桌面 3 · 文档 3.
                   std::vector<std::pair<std::wstring, size_t>> where;
                   for (const auto& it : p.items) {
                       const std::wstring l = ResultFolderLabel(app::ai::ParentFolder(it.src));
                       auto f = std::find_if(where.begin(), where.end(), [&](const auto& w) { return w.first == l; });
                       if (f == where.end()) where.push_back({l, 1});
                       else ++f->second;
                   }
                   std::stable_sort(where.begin(), where.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
                   for (size_t i = 0; i < where.size() && i < 3; ++i)
                       p.where += (i ? L" \u00B7 " : L"") + where[i].first + L" " + Num(where[i].second);
                   if (where.size() > 3) p.where += L" \u2026";
                   EndStep(s2, a2, true,
                           Fill(Fill(Fill(Text(I::AiTkFindDone), L"{n}", Num(p.items.size())), L"{where}", p.where),
                                L"{t}", Secs(p.find_ms)));
                   if (AiBlockView* sb = Block(s2, a2.steps_id)) {
                       sb->label = Fill(Fill(Text(p.from_selection ? I::AiTkStepsSel : I::AiTkStepsFound), L"{label}",
                                             p.label),
                                        L"{n}", Num(p.items.size()));
                       sb->collapsed = true;
                       sb->open = false;
                   }
                   a2.steps_id = 0;
                   // 任务计划.
                   AiBlockView& b = c2.Add(AiBlockKind::Task, Now());
                   p.card_id = b.id;
                   b.label = Text(I::AiTkPlanLabel);
                   {
                       ui::AiTaskStepView v;
                       v.title = p.from_selection ? Text(I::AiTkStepSel) : Fill(Text(I::AiTkStepFind), L"{label}", p.label);
                       v.tag = Text(I::AiTkTagReadDone);
                       v.desc = Fill(Fill(Text(I::AiTkFindDesc), L"{n}", Num(p.items.size())), L"{where}", p.where);
                       v.state = ui::AiTaskState::Done;
                       b.task.push_back(std::move(v));
                   }
                   if (p.new_folders) {
                       ui::AiTaskStepView v;
                       const bool subs = p.by != TaskPlan::By::None && p.new_folders > (p.dest_exists ? 0u : 1u);
                       v.title = Text(!subs ? I::AiTkStepFolder
                                      : p.by == TaskPlan::By::Month ? I::AiTkStepFoldersMonth
                                      : p.by == TaskPlan::By::Year ? I::AiTkStepFoldersYear : I::AiTkStepFoldersType);
                       v.arg = p.dest_label;
                       v.desc = Fill(Fill(Text(I::AiTkFoldersDesc), L"{n}", Num(p.new_folders)), L"{names}",
                                     FolderList(p, true, 6));
                       p.step_folders = static_cast<int>(b.task.size());
                       b.task.push_back(std::move(v));
                   }
                   if (p.moves) {
                       ui::AiTaskStepView v;
                       v.title = Fill(Text(I::AiTkStepMove), L"{n}", Num(p.moves));
                       if (p.step_folders < 0) v.arg = p.dest_label;
                       v.desc = p.by == TaskPlan::By::Month ? Text(I::AiTkMoveDescMonth)
                              : p.by == TaskPlan::By::Year ? Text(I::AiTkMoveDescYear)
                              : p.by == TaskPlan::By::Type ? Text(I::AiTkMoveDescType)
                                                           : Fill(Text(I::AiTkMoveDescNone), L"{folder}", p.dest_label);
                       if (p.clashes) v.desc += Fill(Text(I::AiTkMoveClash), L"{n}", Num(p.clashes));
                       p.step_move = static_cast<int>(b.task.size());
                       b.task.push_back(std::move(v));
                   }
                   if (!p.zip_name.empty()) {
                       ui::AiTaskStepView v;
                       v.title = Text(I::AiTkStepZip);
                       v.arg = p.zip_name;
                       v.desc = Fill(Text(p.move ? I::AiTkZipDesc : I::AiTkZipDescStay), L"{folder}", p.dest_label);
                       p.step_zip = static_cast<int>(b.task.size());
                       b.task.push_back(std::move(v));
                   }
                   p.steps = b.task.size();
                   b.meta = Fill(Text(I::AiTkPlanMeta), L"{n}", Num(p.steps));
                   if (p.new_folders) b.sums.push_back(Sum(ui::AiIcon::FolderPlus, I::AiTkSumFolders, p.new_folders));
                   if (p.moves) b.sums.push_back(Sum(ui::AiIcon::Move, I::AiTkSumMove, p.moves));
                   if (!p.zip_name.empty()) b.sums.push_back(Sum(ui::AiIcon::Package, I::AiTkSumZip, 1));
                   b.safe = Text(I::AiTkSafe);
                   // The rows of the search show where each file goes.
                   if (app::Tab* tab = ActiveTab(s2); tab && Shown(s2, tab->current_path) && p.move) {
                       ui::AiListOverlay& o = ListOverlay(tab->current_path);
                       o.dests.clear();
                       for (const auto& it : p.items) {
                           if (it.already) continue;
                           bool fresh = it.group.empty() ? !p.dest_exists : false;
                           for (const auto& f : p.folders)
                               if (f.name == it.group) fresh = !f.exists;
                           o.dests[ui::AiListLower(it.name)] = {it.group.empty() ? Leaf(p.dest) : it.group,
                                                                fresh ? ui::AiListOverlay::kDestAccent
                                                                      : static_cast<uint8_t>(ui::AiTint::Neutral)};
                       }
                       o.dest_start = Now() + 500;
                       o.dest_end = 0;
                   }
                   // The confirmation; the turn waits here for the user.
                   AiChatPush(s2, 650, [plan, call_id, gen](AppState& s4) {
                       AiAgent& ag = Agent(s4);
                       if (ag.generation != gen) return -1;
                       AiChat& ch = Chat(s4);
                       TaskPlan& tp = *plan;
                       AiChatSetChip(ch, ui::StatusAiState::Waiting, Text(I::AiAsstChipWaiting));
                       if (!s4.aiAssistant.open) ch.unread = true;
                       AiBlockView& cf = ch.Add(AiBlockKind::Confirm, Now());
                       cf.title = Text(I::AiTkConfirmTitle);
                       std::wstring what;
                       const std::wstring sep = Text(I::AiTkListSep);
                       if (tp.new_folders) what += Fill(Text(I::AiTkSumFolders), L"{n}", Num(tp.new_folders));
                       if (tp.moves) what += (what.empty() ? L"" : sep) + Fill(Text(I::AiTkSumMove), L"{n}", Num(tp.moves));
                       if (!tp.zip_name.empty()) what += (what.empty() ? L"" : sep) + Fill(Text(I::AiTkSumZip), L"{n}", L"1");
                       cf.sub = Fill(Text(I::AiTkConfirmSub), L"{what}", what);
                       cf.hint = Text(I::AiTkConfirmHint);
                       cf.hint_icon = ui::AiIcon::Undo;
                       cf.deny = Text(I::AiAsstDeny);
                       cf.accept = Text(I::AiTkRun);
                       ch.confirm_id = cf.id;
                       ch.on_confirm = [plan, call_id, gen](AppState& s3, bool accept) {
                           AiChat& c3 = Chat(s3);
                           AiAgent& a3 = Agent(s3);
                           if (AiBlockView* card = c3.Find(c3.confirm_id)) {
                               const uint64_t now = Now();
                               if (now > card->born_ms) c3.turn_start += now - card->born_ms;
                               card->done = true;
                               card->title = Text(accept ? I::AiTkConfirmed : I::AiTkDenied);
                           }
                           c3.confirm_id = 0;
                           c3.on_confirm = nullptr;
                           if (a3.generation != gen) return;
                           if (!accept) {
                               if (AiBlockView* pb = TaskCard(s3, *plan)) pb->collapsed = true;
                               if (g_list) {
                                   g_list->dest_end = Now();
                                   g_list->end_ms = Now() + 500;
                               }
                               AddToolResult(a3, call_id,
                                             ToolOutcome::Fail("user_denied",
                                                               "The user declined this task in Pulse; nothing changed. "
                                                               "Do not propose the same task again; ask what they would "
                                                               "like instead."));
                               ++a3.call_index;
                               AiChatSetChip(c3, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
                               NextTool(s3);
                               Redraw(s3);
                               return;
                           }
                           RunTask(s3, plan, call_id);
                           Redraw(s3);
                       };
                       Redraw(s4);
                       return -1;
                   });
               });
    };

    const json::Value* find = args.find("find");
    if (find && find->is_object()) {
        app::AdvancedSearchSpec spec;
        ToolOutcome error;
        AddStep(s, a, ui::AiIcon::Search, Text(I::AiQStep), plan->label, Text(I::AiQStepRun));
        if (!app::ai::SearchSpecFromArgs(*find, spec, error) || !s.appPrefs.search_enabled) {
            if (!s.appPrefs.search_enabled && error.ok)
                error = ToolOutcome::Fail("unsupported", "Search is turned off in Pulse's settings, so nothing can be found by name.");
            EndStep(s, a, false, Fill(Text(I::AiAsstStepFailed), L"{reason}", json::Wide(error.error.message)));
            AddToolResult(a, call.id, error);
            ++a.call_index;
            AiChatPush(s, 250, [](AppState& s2) { NextTool(s2); return -1; });
            return;
        }
        const std::wstring path = app::MakeSearchPath(app::CompileSearchQuery(spec));
        AiChatSetChip(Chat(s), ui::StatusAiState::Busy, Text(I::AiQChipSearching), 0.4f);
        NavigateTo(s, path);
        AwaitSearch(s, path, [build, call_id, gen](AppState& st, const app::Tab* tab, uint64_t ms) {
            AiAgent& ag = Agent(st);
            if (ag.generation != gen) return;
            if (!tab) {
                EndStep(st, ag, false, Fill(Text(I::AiAsstStepFailed), L"{reason}", Text(I::AiQNoAnswer)));
                AddToolResult(ag, call_id, ToolOutcome::Fail("timeout", "Pulse's search did not finish.", true));
                ++ag.call_index;
                AiChatPush(st, 250, [](AppState& s2) { NextTool(s2); return -1; });
                return;
            }
            std::vector<std::wstring> paths;
            if (tab->search_entries)
                for (const fs::DirEntry& e : *tab->search_entries)
                    if (!e.is_dir) paths.push_back(e.full_path);
            if (static_cast<size_t>(tab->search_total) > (tab->search_entries ? tab->search_entries->size() : 0)) {
                EndStep(st, ag, false, Fill(Text(I::AiAsstStepFailed), L"{reason}", Text(I::AiTkTooMany)));
                AddToolResult(ag, call_id, ToolOutcome::Fail("too_many", "The search found more files than Pulse "
                                                                         "loaded; narrow the conditions."));
                ++ag.call_index;
                AiChatPush(st, 250, [](AppState& s2) { NextTool(s2); return -1; });
                return;
            }
            build(st, std::move(paths), ms);
        });
        return;
    }
    // The selection of the active tab.
    app::Tab* tab = ActiveTab(s);
    std::vector<std::wstring> paths;
    if (tab && tab->snapshot && !fs::IsVirtualPath(tab->current_path)) paths = SelectedFullPaths(*tab);
    if (paths.empty())
        return fail("no_files", "Nothing is selected in Pulse. Pass find with search conditions, or ask the user to "
                                "select the files first.");
    plan->from_selection = true;
    AddStep(s, a, ui::AiIcon::Rows, Text(I::AiTkStepSel), {}, Text(I::AiQStepRun));
    build(s, std::move(paths), 0);
}

// ---------------------------------------------------------- space (D) --
// disk_usage and find_cleanup (plans/ai/Pulse_AI助手_第二批扩展.html, flow
// D): where a drive's space went, then what in the user's own folders can
// go to the Recycle Bin, after the user ticks and confirms it in Pulse.

constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;

double Gb(uint64_t bytes) { return static_cast<double>(bytes) / kGiB; }

std::wstring Gb1(uint64_t bytes) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f", Gb(bytes));
    // A few megabytes must not read as nothing (0.0 GB).
    if (bytes > 0 && wcscmp(buf, L"0.0") == 0) return L"<0.1";
    return buf;
}

// Fills "{gb} GB"; under 1 GB the whole "{gb} GB" becomes MB / KB, so a small
// batch (duplicates) reads 13.5 MB instead of <0.1 GB.
std::wstring FillGb(std::wstring t, uint64_t bytes) {
    if (bytes >= static_cast<uint64_t>(kGiB)) return Fill(t, L"{gb}", Gb1(bytes));
    wchar_t buf[32];
    if (bytes >= 1024ull * 1024) swprintf_s(buf, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else swprintf_s(buf, L"%llu KB", static_cast<unsigned long long>((bytes + 1023) / 1024));
    const size_t at = t.find(L"{gb} GB");
    if (at != std::wstring::npos) return t.replace(at, 7, buf);
    return Fill(t, L"{gb}", Gb1(bytes));
}

// 221.0 -> 221 (the big used figure).
std::wstring GbTrim(uint64_t bytes) {
    std::wstring v = Gb1(bytes);
    if (v.size() > 2 && v.compare(v.size() - 2, 2, L".0") == 0) v.resize(v.size() - 2);
    return v;
}

// 1,337 (zh / en) - the design's INT().
std::wstring Int(uint64_t n) {
    std::wstring d = std::to_wstring(n), out;
    for (size_t i = 0; i < d.size(); ++i) {
        if (i && (d.size() - i) % 3 == 0) out += L',';
        out += d[i];
    }
    return out;
}

double Round1(uint64_t bytes) { return std::round(Gb(bytes) * 10.0) / 10.0; }

wchar_t DriveArg(const json::Value& args) {
    const std::string d = args.get_string("drive");
    wchar_t c = d.empty() ? L'C' : static_cast<wchar_t>(std::towupper(static_cast<wchar_t>(static_cast<unsigned char>(d[0]))));
    return c >= L'A' && c <= L'Z' ? c : 0;
}

struct SpaceDisk {
    app::ai::DiskUsage du;
    uint32_t card_id = 0;
};

struct SpaceClean {
    wchar_t drive = L'C';
    app::ai::CleanupScan scan;            // categories sorted, items biggest first
    std::vector<std::vector<bool>> on;    // per category, per item
    uint32_t card_id = 0, confirm_id = 0, list_base = 0;
    std::string call_id;
    uint64_t gen = 0;
    app::ai::BinState bin;
    // The batch once it went to the bin.
    std::vector<std::wstring> sources;
    std::vector<uint64_t> source_bytes;
    std::vector<size_t> source_cat;
    std::vector<bool> moved;
    uint64_t moved_bytes = 0;
    size_t moved_n = 0;
    FILETIME since{};
    uint64_t undo_group = 0;
    uint32_t result_id = 0;
    uint64_t free_before = 0, total = 0;
    uint64_t bin_items_before = 0;
    bool dup = false;                     // find_duplicates' copies (flow E)
};

std::shared_ptr<SpaceDisk> g_space_disk;     // the latest disk card
std::shared_ptr<SpaceClean> g_space_clean;   // the latest cleanup

ui::AiIcon CleanIcon(app::ai::CleanKind k) {
    using K = app::ai::CleanKind;
    switch (k) {
    case K::Installer: return ui::AiIcon::Install;
    case K::Extracted: return ui::AiIcon::Package;
    case K::Copy: return ui::AiIcon::Copy;
    case K::Temp: return ui::AiIcon::Temp;
    default: return ui::AiIcon::Video;
    }
}

ui::AiTint CleanTint(app::ai::CleanKind k) {
    using K = app::ai::CleanKind;
    switch (k) {
    case K::Installer: return ui::AiTint::App;
    case K::Extracted: return ui::AiTint::Archive;
    case K::Copy: return ui::AiTint::Document;
    case K::Temp: return ui::AiTint::Image;
    default: return ui::AiTint::Video;
    }
}

I CleanTitleId(app::ai::CleanKind k) {
    using K = app::ai::CleanKind;
    switch (k) {
    case K::Installer: return I::AiClCatInstaller;
    case K::Extracted: return I::AiClCatExtracted;
    case K::Copy: return I::AiClCatCopy;
    case K::Temp: return I::AiClCatTemp;
    default: return I::AiClCatVideo;
    }
}

const char* CleanKey(app::ai::CleanKind k) {
    using K = app::ai::CleanKind;
    switch (k) {
    case K::Installer: return "installers";
    case K::Extracted: return "extracted_archives";
    case K::Copy: return "duplicate_downloads";
    case K::Temp: return "temp_files";
    default: return "old_videos";
    }
}

// One file's reason (the list tab's 原因 column).
std::wstring CleanReason(app::ai::CleanKind k, const app::ai::CleanItem& it) {
    using K = app::ai::CleanKind;
    switch (k) {
    case K::Installer: return Fill(Text(I::AiClWhyInstaller), L"{app}", it.arg);
    case K::Extracted: return Fill(Text(I::AiClWhyExtracted), L"{folder}", Leaf(it.arg));
    case K::Copy: return Fill(Text(I::AiClWhyCopy), L"{name}", Leaf(it.arg));
    case K::Temp: return Fill(Text(I::AiClWhyTemp), L"{n}", Num(it.days));
    default: return Fill(Text(I::AiClWhyVideo), L"{n}", Num(it.days));
    }
}

// Where a category's files are: 下载, or 下载 · 桌面.
std::wstring CleanWhere(const app::ai::CleanCategory& c) {
    std::vector<std::pair<std::wstring, size_t>> where;
    for (const auto& it : c.items) {
        const std::wstring l = FolderLabel(app::ai::ParentFolder(it.path));
        auto f = std::find_if(where.begin(), where.end(), [&](const auto& w) { return w.first == l; });
        if (f == where.end()) where.push_back({l, 1});
        else ++f->second;
    }
    std::stable_sort(where.begin(), where.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
    std::wstring out;
    for (size_t i = 0; i < where.size() && i < 2; ++i) out += (i ? L" \u00B7 " : L"") + where[i].first;
    return out;
}

void DiskSegments(ui::AiDiskView& d, const app::ai::DiskUsage& du, uint64_t in_bin) {
    const double t = du.total ? static_cast<double>(du.total) : 1.0;
    const uint64_t users = du.users > in_bin ? du.users - in_bin : 0;
    d.seg[0] = static_cast<float>(du.system / t);
    d.seg[1] = static_cast<float>(du.programs / t);
    d.seg[2] = static_cast<float>(users / t);
    d.seg[3] = static_cast<float>(du.other / t);
    d.seg[4] = static_cast<float>(in_bin / t);
    d.legend_recycle = Fill(Text(I::AiDkLegRecycle), L"{gb}", Gb1(in_bin));
}

void FillDiskCard(AiBlockView& b, const app::ai::DiskUsage& du) {
    b.label = Fill(Text(I::AiDkTitle), L"{d}", std::wstring(1, du.drive));
    b.meta = du.indexed ? Fill(Text(I::AiDkMetaIndex), L"{t}", Secs(du.ms)) : Text(I::AiDkMetaNoIndex);
    b.hint_icon = du.indexed ? ui::AiIcon::Bolt : ui::AiIcon::Count;
    ui::AiDiskView& d = b.disk;
    const uint64_t used = du.total - (std::min)(du.free, du.total);
    d.used_gb = d.used_from = static_cast<float>(Gb(used));
    d.free_gb = d.free_from = static_cast<float>(Gb(du.free));
    d.used_of = Fill(Text(I::AiDkUsedOf), L"{total}", GbTrim(du.total));
    d.free_unit = Text(I::AiDkFreeUnit);
    d.legend[0] = Fill(Text(I::AiDkLegSys), L"{gb}", Gb1(du.system));
    d.legend[1] = Fill(Text(I::AiDkLegProg), L"{gb}", Gb1(du.programs));
    std::wstring user = Fill(Fill(Text(I::AiDkLegUser), L"{name}", du.user_name), L"{gb}", Gb1(du.users));
    for (size_t at; (at = user.find(L"  ")) != std::wstring::npos;) user.erase(at, 1);
    d.legend[2] = user;
    d.legend[3] = Fill(Text(I::AiDkLegOther), L"{gb}", Gb1(du.other));
    d.legend_free = Text(I::AiDkLegFree);
    DiskSegments(d, du, 0);
    d.top_label = Text(I::AiDkTop);
    const uint64_t biggest = du.top.empty() ? 1 : (std::max<uint64_t>)(du.top.front().bytes, 1);
    for (const app::ai::DiskTop& t : du.top) {
        ui::AiDiskRowView r;
        r.file = t.is_file;
        r.name = Leaf(t.path);
        using P = app::ai::DiskPlace;
        switch (t.place) {
        case P::Profile:
            r.name = FolderLabel(t.path);
            r.sub = Fill(Text(I::AiDkSubUser), L"{name}", du.user_name);
            break;
        case P::AppData: {
            r.name = L"AppData \u203A " + Leaf(t.path);
            for (const auto& [name, bytes] : t.parts)
                r.sub += (r.sub.empty() ? L"" : L" \u00B7 ") + name + L" " + Gb1(bytes);
            break;
        }
        case P::System: r.sys = true; r.sub = Text(I::AiDkSubSystem); break;
        case P::Programs: r.sys = true; r.sub = Text(I::AiDkSubPrograms); break;
        case P::Hibernate: r.sys = true; r.sub = Text(I::AiDkSubHiber); break;
        case P::Pagefile: r.sys = true; r.sub = Text(I::AiDkSubPage); break;
        default: {
            // Where it sits, as a trail like 用户 › SS: 本地磁盘 (C:) › Games
            // rather than a bare C:\ for things at the root.
            const std::wstring parent = app::ai::ParentFolder(path::StripExtendedPathPrefix(t.path));
            if (parent.size() >= 3 && parent[1] == L':' && parent[2] == L'\\') {
                r.sub = Fill(Text(I::AiDkTitle), L"{d}", std::wstring(1, static_cast<wchar_t>(std::towupper(parent[0]))));
                for (size_t at = 3; at < parent.size();) {
                    const size_t next = (std::min)(parent.find(L'\\', at), parent.size());
                    if (next > at) r.sub += L" \u203A " + parent.substr(at, next - at);
                    at = next + 1;
                }
            } else {
                r.sub = parent;
            }
            break;
        }
        }
        r.value = Gb1(t.bytes);
        r.frac = static_cast<float>(static_cast<double>(t.bytes) / static_cast<double>(biggest));
        d.rows.push_back(std::move(r));
    }
    d.grow_ms = Now() + 60;
}

json::Value DiskJson(const app::ai::DiskUsage& du) {
    json::Value o = json::Value::Object();
    o.set("drive", Utf8(std::wstring(1, du.drive) + L":"));
    o.set("total_gb", Round1(du.total));
    o.set("free_gb", Round1(du.free));
    o.set("used_gb", Round1(du.total - (std::min)(du.free, du.total)));
    o.set("system_gb", Round1(du.system));
    o.set("programs_gb", Round1(du.programs));
    o.set("users_gb", Round1(du.users));
    o.set("other_gb", Round1(du.other));
    o.set("recycle_bin_gb", Round1(du.recycle_bytes));
    o.set("recycle_bin_items", du.recycle_items);
    json::Value top = json::Value::Array();
    for (const app::ai::DiskTop& t : du.top) {
        json::Value e = json::Value::Object();
        e.set("path", Utf8(t.path));
        e.set("gb", Round1(t.bytes));
        using P = app::ai::DiskPlace;
        const bool sys = t.place == P::System || t.place == P::Programs || t.place == P::Hibernate || t.place == P::Pagefile;
        e.set("kind", t.place == P::Hibernate ? "hibernation_file" : t.place == P::Pagefile ? "page_file"
                      : t.place == P::System ? "windows" : t.place == P::Programs ? "programs"
                      : t.place == P::AppData ? "app_data" : t.place == P::Profile ? "user_folder" : "other");
        if (sys) e.set("note", "system: do not suggest deleting");
        if (!t.parts.empty()) {
            json::Value parts = json::Value::Array();
            for (const auto& [name, bytes] : t.parts) {
                json::Value p = json::Value::Object();
                p.set("name", Utf8(name));
                p.set("gb", Round1(bytes));
                parts.push(std::move(p));
            }
            e.set("biggest", std::move(parts));
        }
        top.push(std::move(e));
    }
    o.set("biggest", std::move(top));
    if (!du.indexed)
        o.set("note", "Pulse's index has no folder sizes for this drive (文件夹大小 is off or not ready), so only the totals are known.");
    o.set("shown", "Pulse shows this as a card: do not repeat the numbers as a list; one or two sentences on what stands out.");
    return o;
}

void ToolDiskUsage(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
    AiAgent& a = Agent(s);
    const wchar_t drive = DriveArg(args);
    if (!drive) {
        AddToolResult(a, call.id, ToolOutcome::Fail("invalid_args", "drive must be a letter such as C"));
        ++a.call_index;
        NextTool(s);
        return;
    }
    AddStep(s, a, ui::AiIcon::Hdd, Text(I::AiDkStep), std::wstring(1, drive) + L":", Text(I::AiDkStepRun));
    AiChatSetChip(Chat(s), ui::StatusAiState::Busy, Text(I::AiDkChipMeasuring), 0.3f);
    auto disk = std::make_shared<SpaceDisk>();
    const std::string call_id = call.id;
    const uint64_t gen = a.generation;
    RunJob(s, [disk, drive](const std::atomic<bool>& cancel) {
               if (!app::ai::MeasureDisk(drive, disk->du, cancel))
                   return ToolOutcome::Fail("not_found", "That drive does not exist or is not a local disk.");
               ToolOutcome out;
               out.data = DiskJson(disk->du);
               return out;
           },
           [disk, call_id, gen](AppState& st, const ToolOutcome& out) {
               AiAgent& ag = Agent(st);
               if (ag.generation != gen) return;
               AiChat& c = Chat(st);
               if (!out.ok) {
                   EndStep(st, ag, false, Fill(Text(I::AiAsstStepFailed), L"{reason}", json::Wide(out.error.message)));
               } else {
                   const app::ai::DiskUsage& du = disk->du;
                   const uint64_t used = du.total - (std::min)(du.free, du.total);
                   EndStep(st, ag, true,
                           Fill(Fill(Fill(Text(du.indexed ? I::AiDkStepDone : I::AiDkStepDonePlain), L"{used}", GbTrim(used)),
                                     L"{total}", GbTrim(du.total)),
                                L"{t}", Secs(du.ms)));
                   AiBlockView& b = c.Add(AiBlockKind::Disk, Now());
                   FillDiskCard(b, du);
                   disk->card_id = b.id;
                   g_space_disk = disk;
                   AiChatSetChip(c, ui::StatusAiState::Busy, Text(I::AiDkChipDone));
               }
               AddToolResult(ag, call_id, out);
               ++ag.call_index;
               AiChatPush(st, 350, [](AppState& s2) { NextTool(s2); return -1; });
           });
}

// The selection's totals.
struct CleanPick {
    uint64_t files = 0, bytes = 0, items = 0;
    std::vector<uint64_t> cat_files, cat_bytes;
};

// upto: only the first categories (the rows landed so far).
CleanPick Picked(const SpaceClean& sc, size_t upto = SIZE_MAX) {
    CleanPick p;
    p.cat_files.assign(sc.scan.cats.size(), 0);
    p.cat_bytes.assign(sc.scan.cats.size(), 0);
    for (size_t c = 0; c < sc.scan.cats.size() && c < upto; ++c)
        for (size_t i = 0; i < sc.scan.cats[c].items.size(); ++i)
            if (sc.on[c][i]) {
                const app::ai::CleanItem& it = sc.scan.cats[c].items[i];
                p.cat_files[c] += it.files;
                p.cat_bytes[c] += it.bytes;
                p.files += it.files;
                p.bytes += it.bytes;
                ++p.items;
            }
    return p;
}

AiBlockView* CleanCard(AppState& s, const SpaceClean& sc) { return Chat(s).Find(sc.card_id); }

// The card's rows, sum and the confirmation follow the checkboxes.
// A row shows what is ticked in it; an unticked one its whole size, grey.
void SyncClean(AppState& s, SpaceClean& sc, bool bump, size_t upto = SIZE_MAX) {
    const CleanPick all = Picked(sc);
    const CleanPick p = Picked(sc, upto);
    const uint64_t now = Now();
    if (AiBlockView* b = CleanCard(s, sc)) {
        for (size_t c = 0; c < sc.scan.cats.size() && c < b->clean.rows.size(); ++c) {
            ui::AiCleanRowView& r = b->clean.rows[c];
            const bool any = all.cat_files[c] > 0;
            const std::wstring n = Int(any ? all.cat_files[c] : sc.scan.cats[c].files);
            const std::wstring g = Gb1(any ? all.cat_bytes[c] : sc.scan.cats[c].bytes);
            if (bump && (n != r.count || g != r.size)) r.bump_ms = now;
            r.count = n;
            r.size = g;
            r.on = any;
            r.tag = sc.scan.cats[c].default_on || r.on ? std::wstring() : Text(I::AiClTagLook);
            for (size_t f = 0; f < r.files.size(); ++f) r.files[f].on = sc.on[c][f];
        }
        const std::wstring n = Int(p.files), g = Gb1(p.bytes);
        if (bump && (n != b->clean.sum_n || g != b->clean.sum_gb)) b->clean.sum_bump_ms = now;
        b->clean.sum_n = n;
        b->clean.sum_gb = g;
    }
    AiChat& ch = Chat(s);
    if (AiBlockView* cf = ch.Find(sc.confirm_id); cf && !cf->done && upto == SIZE_MAX) {
        cf->title = Fill(Text(I::AiClConfirmTitle), L"{n}", Int(p.files));
        std::wstring what;
        for (size_t c = 0; c < sc.scan.cats.size(); ++c)
            if (p.cat_files[c]) what += (what.empty() ? L"" : Text(I::AiTkListSep)) + Text(CleanTitleId(sc.scan.cats[c].kind));
        cf->sub = Fill(Fill(Text(I::AiClConfirmSub), L"{gb}", Gb1(p.bytes)), L"{what}", what);
        const app::ai::BinState& bin = sc.bin;
        const bool fits = bin.available && bin.used_bytes + p.bytes <= bin.max_bytes;
        cf->hint2_warn = !fits;
        cf->hint2 = !bin.available ? Text(I::AiClBinOff)
                  : Fill(Fill(Text(fits ? I::AiClBinOk : I::AiClBinFull), L"{max}", Gb1(bin.max_bytes)), L"{used}",
                         Gb1(bin.used_bytes));
        cf->accept_off = !fits || p.items == 0;
    }
    // An open 查看 tab keeps its selection in step.
    if (app::Tab* tab = ActiveTab(s)) {
        std::wstring kind, rest;
        if (app::ParsePulsePath(tab->current_path, &kind, &rest) && kind == L"ai-list") {
            const uint32_t id = static_cast<uint32_t>(wcstoul(rest.c_str(), nullptr, 10));
            if (id >= sc.list_base && id < sc.list_base + sc.scan.cats.size() && tab->snapshot) {
                const size_t c = id - sc.list_base;
                // By path: the tab skips files that are gone.
                const auto& items = sc.scan.cats[c].items;
                std::vector<int> picked;
                for (size_t e = 0; e < tab->EntryCount(); ++e) {
                    const fs::DirEntry entry = tab->EntryAt(e);
                    const std::wstring& path = entry.full_path;
                    for (size_t i = 0; i < items.size() && i < sc.on[c].size(); ++i) {
                        if (_wcsicmp(items[i].path.c_str(), path.c_str()) != 0) continue;
                        if (sc.on[c][i]) picked.push_back(static_cast<int>(e));
                        break;
                    }
                }
                tab->ClearSelection();
                if (!picked.empty()) tab->SelectIndices(picked);
            }
        }
    }
}

void OpenCleanList(AppState& s, SpaceClean& sc, size_t c) {
    const app::ai::CleanCategory& cat = sc.scan.cats[c];
    std::vector<app::ai::AiListRow> rows;
    for (size_t i = 0; i < cat.items.size(); ++i) {
        const app::ai::CleanItem& it = cat.items[i];
        app::ai::AiListRow r;
        r.path = it.path;
        r.reason = CleanReason(cat.kind, it);
        r.bytes = it.bytes;
        r.mtime = it.mtime;
        r.dir = it.dir;
        r.on = sc.on[c][i];
        rows.push_back(std::move(r));
    }
    const uint32_t id = sc.list_base + static_cast<uint32_t>(c);
    app::ai::PutAiList(id, Fill(Text(I::AiClListTab), L"{cat}", Text(CleanTitleId(cat.kind))), std::move(rows));
    NavigateTo(s, L"pulse:ai-list:" + std::to_wstring(id));
}

void CleanClick(AppState& s, ui::AiPanelPart part, int row, int file) {
    std::shared_ptr<SpaceClean> sc = g_space_clean;
    if (!sc || row < 0 || static_cast<size_t>(row) >= sc->scan.cats.size()) return;
    const size_t c = static_cast<size_t>(row);
    std::vector<bool>& on = sc->on[c];
    if (part == ui::AiPanelPart::CleanCheck) {
        const bool any = std::find(on.begin(), on.end(), true) != on.end();
        std::fill(on.begin(), on.end(), !any);
    } else if (part == ui::AiPanelPart::CleanFile) {
        if (file >= 0 && static_cast<size_t>(file) < on.size()) on[static_cast<size_t>(file)] = !on[static_cast<size_t>(file)];
    } else if (part == ui::AiPanelPart::CleanLink) {
        if (AiBlockView* b = CleanCard(s, *sc); b && c < b->clean.rows.size()) {
            ui::AiCleanRowView& r = b->clean.rows[c];
            const bool open = !r.open;
            for (auto& other : b->clean.rows) other.open = false;
            r.open = open && !r.files.empty();
            if (open) OpenCleanList(s, *sc, c);
        }
        return;
    }
    SyncClean(s, *sc, true);
}

json::Value CleanJson(const SpaceClean& sc) {
    json::Value o = json::Value::Object();
    json::Value cats = json::Value::Array();
    for (const app::ai::CleanCategory& c : sc.scan.cats) {
        json::Value e = json::Value::Object();
        e.set("kind", CleanKey(c.kind));
        e.set("files", c.files);
        e.set("gb", Round1(c.bytes));
        e.set("checked_by_default", c.default_on);
        json::Value names = json::Value::Array();
        for (size_t i = 0; i < c.items.size() && i < 3; ++i) names.push(Utf8(Leaf(c.items[i].path)));
        e.set("examples", std::move(names));
        cats.push(std::move(e));
    }
    o.set("categories", std::move(cats));
    return o;
}

ui::AiMiniDiskView MiniDisk(wchar_t drive, uint64_t total, uint64_t free, uint64_t in_bin) {
    ui::AiMiniDiskView m;
    m.on = true;
    m.label = Fill(Text(I::AiClMiniFree), L"{d}", std::wstring(1, drive));
    m.value = m.value_from = static_cast<float>(Gb(free));
    m.unit = L"GB";
    const double t = total ? static_cast<double>(total) : 1.0;
    const uint64_t used = total - (std::min)(free, total);
    m.used = static_cast<float>((used > in_bin ? used - in_bin : 0) / t);
    m.recycle = m.recycle_from = static_cast<float>(in_bin / t);
    return m;
}

bool DriveSpace(wchar_t drive, uint64_t& total, uint64_t& free) {
    const wchar_t root[] = {drive, L':', L'\\', 0};
    ULARGE_INTEGER avail{}, tot{}, fr{};
    if (!GetDiskFreeSpaceExW(root, &avail, &tot, &fr)) return false;
    total = tot.QuadPart;
    free = fr.QuadPart;
    return true;
}

void SettleRecycle(AppState& s, std::shared_ptr<SpaceClean> sc);
void AskPurge(AppState& s, std::shared_ptr<SpaceClean> sc);

// 撤销: one group undo, then the card turns grey once the files are back.
// Files behind one moved path: a temp folder counts what is inside it.
// Both sides normalised: Recycle Bin entries spell folders their own way.
uint64_t CleanFilesOf(const SpaceClean& sc, const std::wstring& path) {
    const std::wstring want = fs::NormalizePath(path);
    for (const app::ai::CleanCategory& cat : sc.scan.cats)
        for (const app::ai::CleanItem& it : cat.items)
            if (_wcsicmp(fs::NormalizePath(it.path).c_str(), want.c_str()) == 0)
                return it.files ? static_cast<uint64_t>(it.files) : 1;
    return 1;
}

// A 查看 tab lists files by path; reload it once they moved or came back.
void RefreshCleanLists(AppState& s) { AiAssistantRefreshLists(s); }

std::function<void(AppState&)> MakeCleanUndo(std::shared_ptr<SpaceClean> sc) {
    return [sc](AppState& st) {
        AiChat& c2 = Chat(st);
        const auto outcome = st.ops.UndoGroup(sc->undo_group);
        if (outcome != ops::OpsManager::GroupUndo::Started) {
            AiBlockView& note = c2.Add(AiBlockKind::Note, Now());
            const bool pending = outcome == ops::OpsManager::GroupUndo::Pending;
            note.text = Text(pending ? I::AiAsstUndoBusy : I::AiAsstUndoGone);
            if (pending) c2.on_undo = MakeCleanUndo(sc);
            Redraw(st);
            return;
        }
        c2.on_purge = nullptr;
        AiChatSetChip(c2, ui::StatusAiState::Busy, Text(I::AiAsstChipUndoing), 0.5f);
        c2.peek_undo = false;
        AiChatHidePeek(st);
        auto started = std::make_shared<uint64_t>(Now());
        AiChatPush(st, 300, [sc, started](AppState& s3) {
            size_t back = 0;
            uint64_t back_files = 0;   // a temp folder counts its files, as the result card did
            for (size_t i = 0; i < sc->sources.size(); ++i) {
                if (!sc->moved[i] || !Exists(sc->sources[i])) continue;
                ++back;
                uint64_t n = 1;
                for (const auto& it : sc->scan.cats[sc->source_cat[i]].items)
                    if (it.path == sc->sources[i]) { n = it.files; break; }
                back_files += n;
            }
            if (back < sc->moved_n && !(Now() - *started > 3000 && !s3.ops.Status().active) && Now() - *started < 600000)
                return 150;
            AiChat& c3 = Chat(s3);
            if (AiBlockView* rr = c3.Find(sc->result_id)) {
                rr->done = true;
                rr->title = Text(I::AiAsstUndone);
                rr->sub = Fill(Text(sc->dup ? I::AiDupUndoneSub : I::AiClUndoneSub), L"{n}", Int(back_files));
                rr->mini.on = false;
            }
            if (g_space_disk && g_space_disk->du.drive == sc->drive)
                if (AiBlockView* db = c3.Find(g_space_disk->card_id)) DiskSegments(db->disk, g_space_disk->du, 0);
            AiBlockView& note = c3.Add(AiBlockKind::Note, Now());
            note.text = Text(I::AiClUndoNote);
            AiChatSetChip(c3, ui::StatusAiState::Done, Text(I::AiAsstUndone));
            RefreshCleanLists(s3);
            if (c3.agent)
                c3.agent->pending_note = sc->dup
                    ? L"(The user clicked Undo in Pulse: the duplicate copies find_duplicates moved to the Recycle Bin are back where they were.)"
                    : L"(The user clicked Undo in Pulse: the files find_cleanup moved to the Recycle Bin are back where they were.)";
            Redraw(s3);
            return -1;
        });
    };
}

void RunRecycle(AppState& s, std::shared_ptr<SpaceClean> sc) {
    AiAgent& a = Agent(s);
    const CleanPick p = Picked(*sc);
    for (size_t c = 0; c < sc->scan.cats.size(); ++c)
        for (size_t i = 0; i < sc->scan.cats[c].items.size(); ++i)
            if (sc->on[c][i]) {
                sc->sources.push_back(sc->scan.cats[c].items[i].path);
                sc->source_bytes.push_back(sc->scan.cats[c].items[i].bytes);
                sc->source_cat.push_back(c);
            }
    sc->moved.assign(sc->sources.size(), false);
    DriveSpace(sc->drive, sc->total, sc->free_before);
    sc->bin_items_before = sc->bin.items;
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    sc->undo_group = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    // $I records carry the deletion time; allow for a coarse clock.
    const uint64_t since = sc->undo_group - 20000000ull;
    sc->since.dwLowDateTime = static_cast<DWORD>(since);
    sc->since.dwHighDateTime = static_cast<DWORD>(since >> 32);
    a.organized = true;
    AddStep(s, a, ui::AiIcon::Trash, Text(I::AiClRecStep),
            Fill(Fill(Text(I::AiClRecArg), L"{n}", Int(p.files)), L"{gb}", Gb1(p.bytes)), {});
    if (AiBlockView* sb = Block(s, a.steps_id)) {
        sb->label = Fill(Text(I::AiClRecSteps), L"{n}", Int(p.files));
        sb->open = true;
    }
    ops::OpRequest r;
    r.type = ops::OpType::RecycleDelete;
    r.sources = sc->sources;
    r.undo_group = sc->undo_group;
    r.undo_label = Text(I::AiClUndoLabel);
    s.ops.Submit(std::move(r));
    struct Watch { uint64_t started = 0, last_change = 0; size_t gone = 0; };
    auto w = std::make_shared<Watch>();
    w->started = w->last_change = Now();
    AiChatPush(s, 150, [sc, w](AppState& st) {
        AiAgent& ag = Agent(st);
        if (ag.generation != sc->gen) return -1;
        const uint64_t now = Now();
        size_t gone = 0;
        std::wstring next;
        for (size_t i = 0; i < sc->sources.size(); ++i) {
            if (!sc->moved[i] && !Exists(sc->sources[i])) sc->moved[i] = true;
            if (sc->moved[i]) ++gone;
            else if (next.empty()) {
                next = Leaf(sc->sources[i]);
            }
        }
        if (gone != w->gone) {
            w->gone = gone;
            w->last_change = now;
        }
        const float prog = sc->sources.empty() ? 1.0f : static_cast<float>(gone) / static_cast<float>(sc->sources.size());
        if (ui::AiStepView* step = LastStep(st, ag)) {
            step->progress = prog;
            step->sub = Fill(Fill(Fill(Text(I::AiClRecSub), L"{k}", Int(gone)), L"{n}", Int(sc->sources.size())), L"{name}",
                             next);
        }
        AiChatSetChip(Chat(st), ui::StatusAiState::Busy,
                      Fill(Text(I::AiClChipRec), L"{p}", Num(static_cast<size_t>(prog * 100.0f + 0.5f))), prog);
        const bool idle = !st.ops.Status().active;
        if (gone >= sc->sources.size() || (idle && now - w->last_change > 2500) || now - w->started > 600000) {
            SettleRecycle(st, sc);
            return -1;
        }
        return 150;
    });
}

void SettleRecycle(AppState& s, std::shared_ptr<SpaceClean> sc) {
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    RefreshCleanLists(s);
    std::vector<uint64_t> cat_files(sc->scan.cats.size(), 0), cat_bytes(sc->scan.cats.size(), 0), cat_left(sc->scan.cats.size(), 0);
    uint64_t files = 0;
    size_t left = 0;
    for (size_t i = 0; i < sc->sources.size(); ++i) {
        const size_t ci = sc->source_cat[i];
        uint64_t n = 1;
        for (const auto& it : sc->scan.cats[ci].items)
            if (it.path == sc->sources[i]) { n = it.files; break; }
        if (sc->moved[i]) {
            ++sc->moved_n;
            sc->moved_bytes += sc->source_bytes[i];
            cat_files[ci] += n;
            cat_bytes[ci] += sc->source_bytes[i];
            files += n;
        } else {
            ++left;
            ++cat_left[ci];
        }
    }
    EndStep(s, a, sc->moved_n > 0,
            Fill(Fill(Text(I::AiClRecDone), L"{n}", Int(files)), L"{gb}", Gb1(sc->moved_bytes)) +
                (left ? Fill(Text(I::AiClRecSkip), L"{k}", Int(left)) : std::wstring()));
    if (AiBlockView* sb = Block(s, a.steps_id)) {
        sb->collapsed = true;
        sb->open = false;
    }
    a.steps_id = 0;
    // The disk card: the user part gives the batch to the hatched bin part.
    if (g_space_disk && g_space_disk->du.drive == sc->drive)
        if (AiBlockView* db = c.Find(g_space_disk->card_id)) DiskSegments(db->disk, g_space_disk->du, sc->moved_bytes);
    if (AiBlockView* cb = CleanCard(s, *sc)) {
        for (auto& r : cb->clean.rows) r.open = false;
    }
    if (sc->moved_n) {
        AiBlockView& r = c.Add(AiBlockKind::Result, Now());
        r.title = Fill(Text(I::AiClResTitle), L"{n}", Int(files));
        r.sub = Fill(Text(I::AiClResSub), L"{gb}", Gb1(sc->moved_bytes));
        for (size_t ci = 0; ci < sc->scan.cats.size(); ++ci) {
            if (!cat_files[ci]) continue;
            ui::AiResultLine ln;
            ln.before = Text(CleanTitleId(sc->scan.cats[ci].kind)) + L" ";
            ln.bold = Int(cat_files[ci]);
            ln.after = Fill(Text(I::AiClResLineAfter), L"{gb}", Gb1(cat_bytes[ci]));
            r.lines.push_back(std::move(ln));
        }
        if (left) {
            ui::AiResultLine ln;
            ln.warn = true;
            ln.before = Fill(Text(I::AiClResSkipped), L"{k}", Int(left));
            r.lines.push_back(std::move(ln));
        }
        {
            ui::AiResultLine ln;
            ln.warn = true;
            ln.before = Fill(Text(I::AiClResStill), L"{d}", std::wstring(1, sc->drive));
            r.lines.push_back(std::move(ln));
        }
        uint64_t total = sc->total, free = sc->free_before;
        DriveSpace(sc->drive, total, free);
        r.mini = MiniDisk(sc->drive, total, free, sc->moved_bytes);
        r.mini.recycle_from = 0.0f;
        r.mini.count_ms = Now() + 200;
        r.mini.leg_used = Text(I::AiClMiniUsed);
        r.mini.leg_recycle = Fill(Text(I::AiDkLegRecycle), L"{gb}", Gb1(sc->moved_bytes)) + L" GB";
        r.mini.leg_free = Text(I::AiDkLegFree);
        r.deny = Text(I::AiAsstUndo);
        r.purge = Text(I::AiClPurge);
        sc->result_id = r.id;
        c.result_id = r.id;
        c.on_show = nullptr;
        c.on_undo = MakeCleanUndo(sc);
        c.on_purge = [sc](AppState& st) { AskPurge(st, sc); };
        c.peek_text = Fill(Fill(Text(I::AiClPeek), L"{n}", Int(files)), L"{gb}", Gb1(sc->moved_bytes));
        c.peek_undo = true;
    }
    AiChatSetChip(c, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
    ToolOutcome out;
    out.data = json::Value::Object();
    out.data.set("ok", sc->moved_n > 0);
    out.data.set("moved_files", files);
    out.data.set("moved_gb", Round1(sc->moved_bytes));
    out.data.set("not_moved", static_cast<uint64_t>(left));
    json::Value cats = json::Value::Array();
    for (size_t ci = 0; ci < sc->scan.cats.size(); ++ci) {
        if (!cat_files[ci]) continue;
        json::Value e = json::Value::Object();
        e.set("kind", CleanKey(sc->scan.cats[ci].kind));
        e.set("files", cat_files[ci]);
        e.set("gb", Round1(cat_bytes[ci]));
        cats.push(std::move(e));
    }
    out.data.set("moved", std::move(cats));
    out.data.set("recycle_bin_items_before", sc->bin_items_before);
    out.data.set("note", "The files are in the Recycle Bin, so they still take space on the drive and free space has not "
                         "changed yet. The user can undo with the card's Undo button, or free the space with its "
                         "\xE6\xB0\xB8\xE4\xB9\x85\xE5\x88\xA0\xE9\x99\xA4\xE8\xBF\x99\xE4\xBA\x9B button, which deletes only "
                         "this batch and leaves the rest of the Recycle Bin alone. Say so briefly.");
    if (left) out.data.set("not_moved_note", "Some files did not move (in use or no permission).");
    AddToolResult(a, sc->call_id, out);
    ++a.call_index;
    AiChatPush(s, 350, [](AppState& s2) { NextTool(s2); return -1; });
    Redraw(s);
}

// 永久删除这些…: a red confirmation, then only this batch leaves the bin.
void AskPurge(AppState& s, std::shared_ptr<SpaceClean> sc) {
    AiChat& ch = Chat(s);
    if (ch.busy || ch.confirm_id) return;
    const std::vector<app::ai::BinHit> hits = [&] {
        std::vector<std::wstring> moved;
        for (size_t i = 0; i < sc->sources.size(); ++i)
            if (sc->moved[i]) moved.push_back(sc->sources[i]);
        return app::ai::FindInBin(moved, sc->since);
    }();
    if (hits.empty()) {
        AiBlockView& note = ch.Add(AiBlockKind::Note, Now());
        note.text = Text(I::AiClPurgeMissing);
        Redraw(s);
        return;
    }
    uint64_t bytes = 0, hit_files = 0;
    for (const auto& h : hits) {
        bytes += h.bytes;
        hit_files += CleanFilesOf(*sc, h.original);
    }
    const app::ai::BinState bin = app::ai::ReadBin(sc->drive);
    const uint64_t others = bin.items > hits.size() ? bin.items - hits.size() : 0;
    AiChatSetChip(ch, ui::StatusAiState::Waiting, Text(I::AiAsstChipWaiting));
    AiBlockView& cf = ch.Add(AiBlockKind::Confirm, Now());
    cf.danger = true;
    cf.title = Fill(Text(I::AiClPurgeTitle), L"{n}", Int(hit_files));
    cf.sub = Fill(FillGb(Text(I::AiClPurgeSub), bytes), L"{k}", Int(others));
    cf.hint = Text(I::AiClPurgeHint);
    cf.hint_icon = ui::AiIcon::Warn;
    cf.deny = Text(I::AiAsstDeny);
    cf.accept = Text(I::AiClPurgeAccept);
    ch.confirm_id = cf.id;
    auto shared_hits = std::make_shared<std::vector<app::ai::BinHit>>(hits);
    ch.on_confirm = [sc, shared_hits, others, hit_files](AppState& s3, bool accept) {
        AiChat& c3 = Chat(s3);
        if (AiBlockView* card = c3.Find(c3.confirm_id)) {
            card->done = true;
            card->title = accept ? Fill(Text(I::AiClPurgeConfirmed), L"{n}", Int(hit_files)) : Text(I::AiClPurgeDenied);
        }
        c3.confirm_id = 0;
        c3.on_confirm = nullptr;
        if (!accept) {
            AiChatSetChip(c3, ui::StatusAiState::Done, Text(I::AiClChipInBin));
            Redraw(s3);
            return;
        }
        AiChatSetChip(c3, ui::StatusAiState::Busy, Text(I::AiClChipPurge), 0.6f);
        ops::OpRequest r;
        r.type = ops::OpType::RealDelete;
        for (const auto& h : *shared_hits) {
            r.sources.push_back(h.payload);
            if (!h.index.empty()) r.sources.push_back(h.index);
        }
        s3.ops.Submit(std::move(r));
        auto started = std::make_shared<uint64_t>(Now());
        AiChatPush(s3, 200, [sc, shared_hits, others, started](AppState& s4) {
            size_t left = 0;
            uint64_t freed = 0, freed_files = 0;
            for (const auto& h : *shared_hits) {
                if (Exists(h.payload)) {
                    ++left;
                    continue;
                }
                freed += h.bytes;
                freed_files += CleanFilesOf(*sc, h.original);
            }
            if (left && !(Now() - *started > 2500 && !s4.ops.Status().active) && Now() - *started < 600000) return 150;
            recycle_empty::NotifyShell();
            AiChat& c4 = Chat(s4);
            uint64_t total = sc->total, free = sc->free_before;
            DriveSpace(sc->drive, total, free);
            // The earlier card can no longer undo.
            if (AiBlockView* old = c4.Find(sc->result_id)) {
                old->gone = true;
                old->sub = Text(I::AiClGoneSub);
            }
            if (c4.result_id == sc->result_id) {
                c4.on_undo = nullptr;
                c4.on_purge = nullptr;
            }
            AiBlockView& r2 = c4.Add(AiBlockKind::Result, Now());
            r2.freed = true;
            r2.title = FillGb(Text(I::AiClFreedTitle), freed);
            r2.sub = Fill(Fill(Text(I::AiClFreedSub), L"{n}", Int(freed_files)), L"{k}", Int(others));
            const uint64_t free_was = free > freed ? free - freed : 0;
            r2.mini = MiniDisk(sc->drive, total, free, 0);
            r2.mini.value_from = static_cast<float>(Gb(free_was));
            r2.mini.from = Fill(Text(I::AiClMiniFrom), L"{gb}", Gb1(free_was));
            r2.mini.recycle_from = static_cast<float>(Gb(freed) / (std::max)(Gb(total), 0.001));
            r2.mini.recycle = 0.0f;
            r2.mini.count_ms = Now() + 120;
            r2.mini.leg_used = Fill(Text(I::AiClMiniUsedN), L"{gb}", Gb1(total - (std::min)(free, total)));
            r2.mini.leg_free = Fill(Text(I::AiClMiniFreeN), L"{gb}", Gb1(free));
            sc->result_id = 0;
            // The disk card: the hatch goes, the figures count to the new ones.
            if (g_space_disk && g_space_disk->du.drive == sc->drive)
                if (AiBlockView* db = c4.Find(g_space_disk->card_id)) {
                    app::ai::DiskUsage& du = g_space_disk->du;
                    ui::AiDiskView& d = db->disk;
                    d.used_from = d.used_gb;
                    d.free_from = d.free_gb;
                    du.users = du.users > freed ? du.users - freed : 0;
                    du.free = free;
                    DiskSegments(d, du, 0);
                    d.used_gb = static_cast<float>(Gb(total - (std::min)(free, total)));
                    d.free_gb = static_cast<float>(Gb(free));
                    d.free_ok = true;
                    d.count_ms = Now() + 120;
                }
            AiChatSetChip(c4, ui::StatusAiState::Done,
                          Fill(Fill(Text(I::AiClChipFreed), L"{d}", std::wstring(1, sc->drive)), L"{gb}", Gb1(free)));
            if (c4.agent)
                c4.agent->pending_note = L"(In Pulse the user permanently deleted the " + Int(freed_files) +
                                         (sc->dup ? L" duplicate copies that find_duplicates" : L" files that find_cleanup") +
                                         L" had moved to the Recycle Bin; " +
                                         std::wstring(1, sc->drive) + L": now has " + Gb1(free) + L" GB free.)";
            Redraw(s4);
            return -1;
        });
        Redraw(s3);
    };
    Redraw(s);
}

void ToolFindCleanup(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
    AiAgent& a = Agent(s);
    const wchar_t drive = DriveArg(args);
    if (!drive) {
        AddToolResult(a, call.id, ToolOutcome::Fail("invalid_args", "drive must be a letter such as C"));
        ++a.call_index;
        NextTool(s);
        return;
    }
    const app::ai::SpaceRoots roots = app::ai::CurrentSpaceRoots();
    AddStep(s, a, ui::AiIcon::Search, Text(I::AiClStep), FolderLabel(roots.profile), Text(I::AiClStage0));
    AiChatSetChip(Chat(s), ui::StatusAiState::Busy, Text(I::AiClChipFinding), 0.5f);
    auto sc = std::make_shared<SpaceClean>();
    sc->drive = drive;
    sc->call_id = call.id;
    sc->gen = a.generation;
    auto progress = std::make_shared<app::ai::CleanProgress>();
    auto job = std::make_shared<AiAgent::Job>();
    a.job = job;
    std::thread([job, sc, progress, drive] {
        app::ai::FindCleanup(drive, sc->scan, *progress, job->cancelled);
        job->done.store(true);
    }).detach();
    const uint64_t started = Now();
    AiChatPush(s, 40, [sc, job, progress, started](AppState& st) {
        AiAgent& ag = Agent(st);
        if (ag.generation != sc->gen) {
            job->cancelled.store(true);
            return -1;
        }
        if (!job->done.load() || Now() - started < 900) {
            if (ui::AiStepView* step = LastStep(st, ag)) {
                const int stage = std::clamp(progress->stage.load(), 0, 4);
                static constexpr I kStage[] = {I::AiClStage0, I::AiClStage1, I::AiClStage2, I::AiClStage3, I::AiClStage4};
                step->sub = Text(kStage[stage]);
                step->progress = std::clamp((static_cast<float>(stage) + progress->fraction.load()) / 5.0f, 0.02f, 1.0f);
            }
            return 60;
        }
        ag.job.reset();
        AiChat& c = Chat(st);
        app::ai::CleanupScan& scan = sc->scan;
        for (auto& cat : scan.cats)
            std::stable_sort(cat.items.begin(), cat.items.end(),
                             [](const app::ai::CleanItem& x, const app::ai::CleanItem& y) { return x.bytes > y.bytes; });
        uint64_t files = 0, bytes = 0;
        for (const auto& cat : scan.cats) {
            files += cat.files;
            bytes += cat.bytes;
        }
        if (scan.cats.empty()) {
            EndStep(st, ag, true, Text(I::AiClStepNone));
            ToolOutcome out;
            out.data = CleanJson(*sc);
            out.data.set("note", "Nothing in the user's Downloads, Temp or Videos is safe to clean. Say so; do not suggest "
                                 "deleting system or program files.");
            AddToolResult(ag, sc->call_id, out);
            ++ag.call_index;
            AiChatPush(st, 350, [](AppState& s2) { NextTool(s2); return -1; });
            return -1;
        }
        EndStep(st, ag, true,
                Fill(Fill(Fill(Text(I::AiClStepDone), L"{k}", Num(scan.cats.size())), L"{n}", Int(files)), L"{gb}",
                     Gb1(bytes)));
        if (AiBlockView* sb = Block(st, ag.steps_id)) {
            sb->label = Fill(Text(I::AiClStepsLabel), L"{k}", Num(scan.cats.size()));
            sb->collapsed = true;
            sb->open = false;
        }
        ag.steps_id = 0;
        // 清理建议.
        sc->on.clear();
        for (const auto& cat : scan.cats) sc->on.emplace_back(cat.items.size(), cat.default_on);
        AiBlockView& b = c.Add(AiBlockKind::Clean, Now());
        sc->card_id = b.id;
        sc->list_base = b.id * 8;
        b.label = Text(I::AiClTitle);
        b.meta = Text(I::AiClMeta);
        ui::AiCleanView& cv = b.clean;
        cv.unit_n = Text(I::AiClUnitN);
        cv.unit_gb = L"GB";
        cv.sum_pre = Text(I::AiClSumPre);
        cv.sum_mid = Text(I::AiClSumMid);
        cv.sum_unit = L"GB";
        cv.safe = Text(I::AiClSafe);
        cv.keep = Text(I::AiClKeep);
        cv.note = Text(I::AiClNote);
        if (g_space_disk && g_space_disk->du.drive == sc->drive)
            for (const auto& t : g_space_disk->du.top)
                if (t.place == app::ai::DiskPlace::Hibernate && t.bytes > 2 * kGiB)
                    cv.note += Fill(Text(I::AiClNoteHiber), L"{gb}", Gb1(t.bytes));
        const uint64_t now = Now();
        for (size_t ci = 0; ci < scan.cats.size(); ++ci) {
            const app::ai::CleanCategory& cat = scan.cats[ci];
            ui::AiCleanRowView r;
            r.icon = CleanIcon(cat.kind);
            r.tint = CleanTint(cat.kind);
            r.title = Text(CleanTitleId(cat.kind));
            r.link = Text(I::AiClView);
            const std::wstring where = CleanWhere(cat);
            switch (cat.kind) {
            case app::ai::CleanKind::Installer: r.desc = Fill(Text(I::AiClDescInstaller), L"{where}", where); break;
            case app::ai::CleanKind::Extracted: r.desc = Fill(Text(I::AiClDescExtracted), L"{where}", where); break;
            case app::ai::CleanKind::Copy: r.desc = Fill(Text(I::AiClDescCopy), L"{where}", where); break;
            case app::ai::CleanKind::Temp: r.desc = Text(I::AiClDescTemp); break;
            default: r.desc = Fill(Text(I::AiClDescVideo), L"{where}", where); break;
            }
            for (size_t i = 0; i < cat.items.size() && i < 5; ++i) {
                const app::ai::CleanItem& it = cat.items[i];
                ui::AiCleanFileView f;
                f.name = Leaf(it.path);
                f.size = it.bytes >= 1024ull * 1024 * 1024 ? Gb1(it.bytes) + L" GB"
                                                           : std::to_wstring((it.bytes + 512 * 1024) / (1024 * 1024)) + L" MB";
                if (it.dir) {
                    f.icon = ui::AiIcon::FolderFilled;
                    f.tint = ui::AiTint::Neutral;
                } else {
                    AiChatGroupLook({f.name}, f.icon, f.tint);
                }
                r.files.push_back(std::move(f));
            }
            if (cat.items.size() > 5) r.more = Fill(Text(I::AiClMore), L"{n}", Int(cat.items.size() - 5));
            r.in_ms = now + 120 + 200 * ci;
            cv.rows.push_back(std::move(r));
        }
        sc->bin = app::ai::ReadBin(sc->drive);
        g_space_clean = sc;
        c.clean_id = b.id;
        c.on_clean = [](AppState& s2, ui::AiPanelPart part, int row, int file) { CleanClick(s2, part, row, file); };
        // The rows land one by one and the sum counts up with them.
        SyncClean(st, *sc, false, 0);
        for (size_t ci = 0; ci < scan.cats.size(); ++ci) {
            AiChatPush(st, ci == 0 ? 120 : 200, [sc, ci](AppState& s2) {
                if (Agent(s2).generation != sc->gen) return -1;
                SyncClean(s2, *sc, true, ci + 1);
                Redraw(s2);
                return -1;
            });
        }
        // The confirmation; the turn waits here while the user ticks.
        AiChatPush(st, 700, [sc](AppState& s4) {
            AiAgent& a4 = Agent(s4);
            if (a4.generation != sc->gen) return -1;
            AiChat& ch = Chat(s4);
            const CleanPick p = Picked(*sc);
            AiChatSetChip(ch, ui::StatusAiState::Waiting,
                          Fill(Text(I::AiClChipReady), L"{gb}", Gb1(p.bytes)));
            if (!s4.aiAssistant.open) ch.unread = true;
            AiBlockView& cf = ch.Add(AiBlockKind::Confirm, Now());
            cf.tag = Text(I::AiClTagDelete);
            cf.hint = Text(I::AiClConfirmHint);
            cf.hint_icon = ui::AiIcon::Undo;
            cf.deny = Text(I::AiAsstDeny);
            cf.accept = Text(I::AiClAccept);
            sc->confirm_id = cf.id;
            ch.confirm_id = cf.id;
            SyncClean(s4, *sc, false);
            ch.on_confirm = [sc](AppState& s3, bool accept) {
                AiChat& c3 = Chat(s3);
                AiAgent& a3 = Agent(s3);
                const CleanPick pk = Picked(*sc);
                if (AiBlockView* card = c3.Find(c3.confirm_id)) {
                    const uint64_t now2 = Now();
                    if (now2 > card->born_ms) c3.turn_start += now2 - card->born_ms;
                    card->done = true;
                    card->tag.clear();
                    card->hint2.clear();
                    card->title = accept ? Fill(Text(I::AiClConfirmed), L"{n}", Int(pk.files)) : Text(I::AiClDenied);
                }
                c3.confirm_id = 0;
                c3.on_confirm = nullptr;
                // The card's checkboxes are settled now.
                c3.clean_id = 0;
                c3.on_clean = nullptr;
                if (a3.generation != sc->gen) return;
                if (!accept) {
                    if (AiBlockView* cb = CleanCard(s3, *sc)) {
                        cb->collapsed = true;
                        for (auto& r : cb->clean.rows) r.open = false;
                    }
                    AddToolResult(a3, sc->call_id,
                                  ToolOutcome::Fail("user_denied", "The user declined the cleanup in Pulse; nothing was "
                                                                   "moved. Do not offer the same cleanup again; ask what "
                                                                   "they would like instead."));
                    ++a3.call_index;
                    AiChatSetChip(c3, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
                    NextTool(s3);
                    Redraw(s3);
                    return;
                }
                RunRecycle(s3, sc);
                Redraw(s3);
            };
            Redraw(s4);
            return -1;
        });
        Redraw(st);
        return -1;
    });
}

// ------------------------------------------------- find_duplicates (E) --
// plans/ai/Pulse_AI助手_第二批扩展.html flow E: the local funnel, the groups
// with what each keeps and why, one confirmation, then the copies go to the
// Recycle Bin behind DuplicateCleanupGuard (keepers locked and re-checked).

struct DupRun {
    app::ai::DupScan scan;
    bool newest = false;
    std::vector<std::wstring> labels;     // per root (下载, 图片, …)
    std::wstring where;                   // 图片、下载
    std::vector<size_t> shown;            // groups on the card (at most 4)
    uint32_t funnel_id = 0, card_id = 0, confirm_id = 0, list_id = 0;
    std::string call_id;
    uint64_t gen = 0;
    uint64_t started = 0, run_started = 0;
    wchar_t drive = L'C';                 // where most of the copies are
    app::ai::BinState bin;
    size_t skipped = 0;                   // groups the pre-check left alone
    std::vector<size_t> moved_root;       // per root: copies moved
};

std::shared_ptr<DupRun> g_dup;

std::wstring SizeShort(uint64_t b) {
    wchar_t buf[32];
    if (b >= kGiB) swprintf_s(buf, L"%.1f GB", Gb(b));
    else if (b >= 1024ull * 1024) swprintf_s(buf, L"%.1f MB", static_cast<double>(b) / (1024.0 * 1024.0));
    else swprintf_s(buf, L"%llu KB", static_cast<unsigned long long>((b + 1023) / 1024));
    return buf;
}

std::wstring Secs1(uint64_t ms) {
    wchar_t buf[24];
    // Under 0.1 s show two decimals (at least 0.01) so a fast pass never reads 0.0.
    if (ms < 100) swprintf_s(buf, L"%.2f", static_cast<double>((std::max<uint64_t>)(ms, 10)) / 1000.0);
    else swprintf_s(buf, L"%.1f", static_cast<double>(ms) / 1000.0);
    return buf;
}

std::wstring DupDate(uint64_t ticks) {
    FILETIME ft{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)}, lt{};
    SYSTEMTIME st{}, now{};
    if (!FileTimeToLocalFileTime(&ft, &lt) || !FileTimeToSystemTime(&lt, &st)) return {};
    GetLocalTime(&now);
    const std::wstring t = Text(st.wYear == now.wYear ? I::AiDupDate : I::AiDupDateY);
    return Fill(Fill(Fill(t, L"{y}", Num(st.wYear)), L"{m}", Num(st.wMonth)), L"{d}", Num(st.wDay));
}

FILETIME DupFt(uint64_t ticks) { return FILETIME{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)}; }

std::wstring DupRootLabel(const app::ai::DupRoot& r) {
    static const std::pair<const wchar_t*, I> kNames[] = {
        {L"downloads", I::AiDupRootDownloads}, {L"pictures", I::AiDupRootPictures}, {L"desktop", I::AiDupRootDesktop},
        {L"documents", I::AiDupRootDocuments}, {L"videos", I::AiDupRootVideos},     {L"music", I::AiDupRootMusic}};
    for (const auto& [key, id] : kNames)
        if (r.key == key) return Text(id);
    return FolderLabel(r.path);
}

size_t DupKeeper(const app::ai::DupGroup& g) {
    for (size_t i = 0; i < g.files.size(); ++i)
        if (g.files[i].keep) return i;
    return 0;
}

// A stable group title: the clean-named copy (no (1) / 副本), else the first;
// it does not follow the keeper when the user picks another one.
std::wstring DupGroupName(const app::ai::DupGroup& g) {
    for (const app::ai::DupFile& f : g.files)
        if (f.clean) return Leaf(f.path);
    return g.files.empty() ? std::wstring() : Leaf(g.files[0].path);
}

struct DupTotals {
    size_t groups = 0, kept = 0, extra = 0, both = 0, acting = 0;
    uint64_t bytes = 0;
    size_t first_both = SIZE_MAX;
};

DupTotals DupCount(const DupRun& run) {
    DupTotals t;
    t.groups = run.scan.groups.size();
    for (size_t i = 0; i < run.scan.groups.size(); ++i) {
        const app::ai::DupGroup& g = run.scan.groups[i];
        t.kept += g.Kept();
        t.extra += g.Extra();
        t.bytes += g.ExtraBytes();
        if (g.Extra()) ++t.acting;
        if (g.both) {
            ++t.both;
            if (t.first_both == SIZE_MAX) t.first_both = i;
        }
    }
    return t;
}

std::wstring DupWhyText(const app::ai::DupGroup& g) {
    using W = app::ai::DupWhy;
    switch (g.why) {
    case W::Organized: return Text(I::AiDupWhy1);
    case W::CleanName: return Text(I::AiDupWhy2);
    case W::Newest: return Text(I::AiDupWhy3New);
    case W::Both: return Fill(Text(I::AiDupWhyBoth), L"{k}", Num(g.Kept()));
    case W::Picked: return Text(I::AiDupWhyPicked);
    default: return Text(I::AiDupWhy3);
    }
}

void FillDupGroup(const DupRun& run, size_t gi, ui::AiDupGroupView& v) {
    const app::ai::DupGroup& g = run.scan.groups[gi];
    if (g.files.empty()) return;
    v.name = DupGroupName(g);
    AiChatGroupLook({v.name}, v.icon, v.tint);
    if (v.icon == ui::AiIcon::Folder) v.icon = ui::AiIcon::File;   // a group is a file, never a folder
    v.meta = Fill(Fill(Text(I::AiDupGroupMeta), L"{k}", Num(g.files.size())), L"{size}", SizeShort(g.files[0].bytes));
    v.both = g.both;
    v.why = DupWhyText(g);
    std::vector<ui::AiDupFileView> files;
    for (size_t i = 0; i < g.files.size() && i < 5; ++i) {
        const app::ai::DupFile& f = g.files[i];
        ui::AiDupFileView fv;
        fv.dir = app::ai::DupDirLabel(run.scan, f, f.root < run.labels.size() ? run.labels[f.root] : std::wstring());
        fv.name = Leaf(f.path);
        fv.date = DupDate(f.mtime);
        fv.keep = f.keep;
        fv.swap_ms = i < v.files.size() ? v.files[i].swap_ms : 0;
        files.push_back(std::move(fv));
    }
    v.files = std::move(files);
    v.more = g.files.size() > 5 ? Fill(Text(I::AiDupFileMore), L"{n}", Num(g.files.size() - 5)) : std::wstring();
}

// The 重复文件 tab: every group under its header, 保留 / 副本 in 状态, the
// copies selected.
void PutDupList(const DupRun& run) {
    std::vector<app::ai::AiListRow> rows;
    for (size_t gi = 0; gi < run.scan.groups.size() && gi < 254; ++gi) {
        const app::ai::DupGroup& g = run.scan.groups[gi];
        const std::wstring label = DupGroupName(g);
        for (const app::ai::DupFile& f : g.files) {
            app::ai::AiListRow r;
            r.path = f.path;
            r.reason = Text(f.keep ? I::AiDupKeep : I::AiDupExtra);
            r.bytes = f.bytes;
            r.mtime = DupFt(f.mtime);
            r.on = !f.keep;
            r.group = static_cast<uint8_t>(gi + 1);
            r.group_label = label;
            rows.push_back(std::move(r));
        }
    }
    app::ai::PutAiList(run.list_id, Fill(Text(I::AiDupListTab), L"{g}", Int(run.scan.groups.size())), std::move(rows),
                       Text(I::AiDupColumn));
}

std::wstring DupListPath(const DupRun& run) { return L"pulse:ai-list:" + std::to_wstring(run.list_id); }

// Reloads the open tab in place (keeps its scroll).
void RefreshDupList(AppState& s, const DupRun& run) {
    PutDupList(run);
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->current_path != DupListPath(run)) return;
    const float sy = tab->scroll_y;
    RefreshPath(s, tab->current_path);
    if (app::Tab* again = ActiveTab(s)) again->scroll_y = sy;
}

void SyncDup(AppState& s, DupRun& run, bool bump) {
    const DupTotals t = DupCount(run);
    AiChat& ch = Chat(s);
    const uint64_t now = Now();
    if (AiBlockView* b = ch.Find(run.card_id)) {
        b->meta = Fill(Fill(Text(I::AiDupMeta), L"{n}", Int(t.extra)), L"{size}", SizeShort(t.bytes));
        ui::AiDupView& dv = b->dup;
        dv.rules = {Text(I::AiDupRule1), Text(I::AiDupRule2), Text(run.newest ? I::AiDupRule3New : I::AiDupRule3)};
        for (size_t i = 0; i < run.shown.size() && i < dv.groups.size(); ++i) FillDupGroup(run, run.shown[i], dv.groups[i]);
        const std::wstring size = SizeShort(t.bytes);
        const size_t sp = size.find(L' ');
        std::vector<std::pair<std::wstring, bool>> sum = {
            {Text(I::AiDupSumPre) + L" ", false},       {Int(t.kept), true},
            {L" " + Text(I::AiDupSumMid) + L" ", false}, {Int(t.extra), true},
            {L" " + Text(I::AiDupSumMid2) + L" ", false}, {size.substr(0, sp), true},
            {sp == std::wstring::npos ? std::wstring() : size.substr(sp), false}};
        if (bump && sum != dv.sum) dv.sum_bump_ms = now;
        dv.sum = std::move(sum);
    }
    if (AiBlockView* cf = ch.Find(run.confirm_id); cf && !cf->done) {
        cf->title = Fill(Text(I::AiDupConfirmTitle), L"{n}", Int(t.extra));
        const std::wstring size = SizeShort(t.bytes);
        if (t.both == 0) {
            cf->sub = Fill(Fill(Text(I::AiDupConfirmSub), L"{size}", size), L"{g}", Int(t.acting));
        } else {
            const app::ai::DupGroup& g = run.scan.groups[t.first_both];
            const std::wstring name = DupGroupName(g);
            cf->sub = t.both == 1
                ? Fill(Fill(Fill(Fill(Text(I::AiDupConfirmSubBoth), L"{size}", size), L"{g}", Int(t.acting)), L"{name}", name),
                       L"{k}", Num(g.Kept()))
                : Fill(Fill(Fill(Fill(Text(I::AiDupConfirmSubBothN), L"{size}", size), L"{g}", Int(t.acting)), L"{name}", name),
                       L"{b}", Num(t.both));
        }
        uint64_t on_drive = 0;
        for (const auto& g : run.scan.groups)
            for (const auto& f : g.files)
                if (!f.keep && !f.path.empty() && towupper(f.path[0]) == run.drive) on_drive += f.bytes;
        const app::ai::BinState& bin = run.bin;
        const bool fits = bin.available && bin.used_bytes + on_drive <= bin.max_bytes;
        cf->hint2_warn = !fits;
        cf->hint2 = !bin.available ? Text(I::AiClBinOff)
                  : Fill(Fill(Text(fits ? I::AiClBinOk : I::AiClBinFull), L"{max}", Gb1(bin.max_bytes)), L"{used}",
                         Gb1(bin.used_bytes));
        cf->accept_off = !fits || t.extra == 0;
        if (cf->accept_off == false) AiChatSetChip(ch, ui::StatusAiState::Waiting, Fill(Text(I::AiDupChipReady), L"{n}", Int(t.extra)));
    }
}

void DupClick(AppState& s, ui::AiPanelPart part, int g, int f) {
    std::shared_ptr<DupRun> run = g_dup;
    if (!run) return;
    AiBlockView* b = Chat(s).Find(run->card_id);
    if (!b) return;
    if (part == ui::AiPanelPart::DupMore) {
        PutDupList(*run);
        NavigateTo(s, DupListPath(*run));
        return;
    }
    if (part == ui::AiPanelPart::DupRule) {
        run->newest = !run->newest;
        for (app::ai::DupGroup& grp : run->scan.groups) app::ai::ApplyKeepRules(grp, run->newest);
        b->dup.rule_ms = Now();
    } else if (part == ui::AiPanelPart::DupFile) {
        if (g < 0 || static_cast<size_t>(g) >= run->shown.size() || static_cast<size_t>(g) >= b->dup.groups.size()) return;
        app::ai::DupGroup& grp = run->scan.groups[run->shown[static_cast<size_t>(g)]];
        if (f < 0 || static_cast<size_t>(f) >= grp.files.size()) return;
        if (grp.files[static_cast<size_t>(f)].keep && grp.Kept() == 1) return;   // already the one
        app::ai::PickKeeper(grp, static_cast<size_t>(f));
        for (ui::AiDupFileView& fv : b->dup.groups[static_cast<size_t>(g)].files) fv.swap_ms = Now();
    }
    SyncDup(s, *run, true);
    RefreshDupList(s, *run);
}

json::Value DupJson(const DupRun& run, size_t limit) {
    json::Value groups = json::Value::Array();
    for (size_t gi = 0; gi < run.scan.groups.size() && gi < limit; ++gi) {
        const app::ai::DupGroup& g = run.scan.groups[gi];
        json::Value e = json::Value::Object();
        e.set("name", Utf8(DupGroupName(g)));
        json::Value kept = json::Value::Array(), extra = json::Value::Array();
        for (const app::ai::DupFile& f : g.files) {
            json::Value fe = json::Value::Object();
            fe.set("path", Utf8(f.path));
            fe.set("date", Utf8(DupDate(f.mtime)));
            (f.keep ? kept : extra).push(std::move(fe));
        }
        e.set("kept", std::move(kept));
        e.set("copies", std::move(extra));
        e.set("why", Utf8(DupWhyText(g)));
        groups.push(std::move(e));
    }
    return groups;
}

void SettleDupRecycle(AppState& s, std::shared_ptr<DupRun> run, std::shared_ptr<SpaceClean> sc) {
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    size_t left = 0;
    std::vector<size_t> per_root(run->scan.roots.size(), 0);
    for (size_t i = 0; i < sc->sources.size(); ++i) {
        if (sc->moved[i]) {
            ++sc->moved_n;
            sc->moved_bytes += sc->source_bytes[i];
        } else {
            ++left;
        }
    }
    // Per root, for the result line (下载 49 份 · 图片 6 份).
    for (const app::ai::DupGroup& g : run->scan.groups)
        for (const app::ai::DupFile& f : g.files)
            for (size_t i = 0; i < sc->sources.size(); ++i)
                if (sc->moved[i] && sc->sources[i] == f.path && f.root < per_root.size()) ++per_root[f.root];
    RefreshDupList(s, *run);
    const std::wstring size = SizeShort(sc->moved_bytes);
    EndStep(s, a, sc->moved_n > 0,
            Fill(Fill(Fill(Text(I::AiDupRecDone), L"{n}", Int(sc->moved_n)), L"{size}", size), L"{t}",
                 Secs1(Now() - run->run_started)) +
                (left ? Fill(Text(I::AiClRecSkip), L"{k}", Int(left)) : std::wstring()));
    if (AiBlockView* sb = Block(s, a.steps_id)) {
        sb->collapsed = true;
        sb->open = false;
    }
    a.steps_id = 0;
    if (AiBlockView* db = c.Find(run->card_id)) {
        db->dup.settled = sc->moved_n > 0;
        if (sc->moved_n) db->collapsed = true;
    }
    const DupTotals t = DupCount(*run);
    // Only groups that lost a copy count: a skipped or 都留 group is not "kept".
    size_t groups_moved = 0, kept_moved = 0;
    for (const app::ai::DupGroup& g : run->scan.groups) {
        bool any = false;
        for (const app::ai::DupFile& f : g.files)
            for (size_t i = 0; i < sc->sources.size() && !any; ++i)
                if (sc->moved[i] && sc->sources[i] == f.path) any = true;
        if (!any) continue;
        ++groups_moved;
        kept_moved += g.Kept();
    }
    if (sc->moved_n) {
        AiBlockView& r = c.Add(AiBlockKind::Result, Now());
        r.title = Fill(Text(I::AiDupResTitle), L"{n}", Int(sc->moved_n));
        r.sub = Fill(Fill(Text(I::AiDupResSub), L"{g}", Int(groups_moved)), L"{size}", size);
        {
            ui::AiResultLine ln;
            bool first = true;
            for (size_t ri = 0; ri < per_root.size(); ++ri) {
                if (!per_root[ri]) continue;
                const std::wstring label = ri < run->labels.size() ? run->labels[ri] : std::wstring();
                if (first) {
                    ln.before = label + L" ";
                    ln.bold = Int(per_root[ri]);
                    ln.after = L" " + Text(I::AiDupResUnit);
                    first = false;
                } else {
                    ln.after += L" \u00B7 " + label + L" " + Int(per_root[ri]) + L" " + Text(I::AiDupResUnit);
                }
            }
            if (!first) r.lines.push_back(std::move(ln));
        }
        {
            ui::AiResultLine ln;
            ln.before = Fill(Text(I::AiDupResKept), L"{n}", Int(kept_moved));
            r.lines.push_back(std::move(ln));
        }
        if (run->skipped || left) {
            ui::AiResultLine ln;
            ln.warn = true;
            ln.before = run->skipped ? Fill(Text(I::AiDupResSkipped), L"{g}", Int(run->skipped))
                                     : Fill(Text(I::AiClResSkipped), L"{k}", Int(left));
            r.lines.push_back(std::move(ln));
        }
        {
            ui::AiResultLine ln;
            ln.warn = true;
            ln.before = Fill(Text(I::AiDupResStill), L"{size}", size);
            r.lines.push_back(std::move(ln));
        }
        r.deny = Text(I::AiAsstUndo);
        r.purge = Text(I::AiClPurge);
        sc->result_id = r.id;
        c.result_id = r.id;
        c.on_show = nullptr;
        c.on_undo = MakeCleanUndo(sc);
        c.on_purge = [sc](AppState& st) { AskPurge(st, sc); };
        c.peek_text = Fill(Fill(Text(I::AiDupPeek), L"{n}", Int(sc->moved_n)), L"{size}", size);
        c.peek_undo = true;
    } else {
        AiBlockView& note = c.Add(AiBlockKind::Note, Now());
        note.text = Text(I::AiDupGuardNote);
    }
    AiChatSetChip(c, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
    ToolOutcome out;
    out.data = json::Value::Object();
    out.data.set("ok", sc->moved_n > 0);
    out.data.set("moved_copies", static_cast<uint64_t>(sc->moved_n));
    out.data.set("moved_size", Utf8(size));
    out.data.set("groups", static_cast<uint64_t>(t.groups));
    out.data.set("groups_where_all_copies_stay", static_cast<uint64_t>(t.both));
    out.data.set("kept_files", static_cast<uint64_t>(kept_moved));
    json::Value where = json::Value::Object();
    for (size_t ri = 0; ri < per_root.size(); ++ri)
        if (per_root[ri] && ri < run->labels.size()) where.set(Utf8(run->labels[ri]), static_cast<uint64_t>(per_root[ri]));
    out.data.set("moved_by_folder", std::move(where));
    out.data.set("examples", DupJson(*run, 6));
    if (sc->moved_n) {
        out.data.set("note", "The copies are in the Recycle Bin, so they still take space until it is emptied. The user can "
                             "undo with the card's Undo button, or free the space with its "
                             "\xE6\xB0\xB8\xE4\xB9\x85\xE5\x88\xA0\xE9\x99\xA4\xE8\xBF\x99\xE4\xBA\x9B button, which deletes "
                             "only this batch. Say so briefly and name a group where all copies stayed, if any."
                             + std::string(run->skipped ? " groups_skipped_because_files_changed: a file in those groups "
                                                          "changed after the compare, so Pulse left the whole group "
                                                          "untouched; say that plainly (not that its copies were kept on "
                                                          "purpose) and offer to compare again."
                                                        : ""));
    } else {
        out.data.set("note", "Pulse moved nothing: files changed after the compare, so the guard stopped. Suggest running "
                             "find_duplicates again.");
    }
    if (run->skipped) out.data.set("groups_skipped_because_files_changed", static_cast<uint64_t>(run->skipped));
    if (left) out.data.set("not_moved", static_cast<uint64_t>(left));
    AddToolResult(a, sc->call_id, out);
    ++a.call_index;
    AiChatPush(s, 350, [](AppState& s2) { NextTool(s2); return -1; });
    Redraw(s);
}

void RunDupRecycle(AppState& s, std::shared_ptr<DupRun> run) {
    AiAgent& a = Agent(s);
    const DupTotals t = DupCount(*run);
    run->run_started = Now();
    a.organized = true;
    AddStep(s, a, ui::AiIcon::Trash, Text(I::AiClRecStep),
            Fill(Fill(Text(I::AiDupRecArg), L"{n}", Int(t.extra)), L"{size}", SizeShort(t.bytes)), Text(I::AiDupRecVerify));
    if (AiBlockView* sb = Block(s, a.steps_id)) {
        sb->label = Fill(Text(I::AiDupRecSteps), L"{n}", Int(t.extra));
        sb->open = true;
    }
    AiChatSetChip(Chat(s), ui::StatusAiState::Busy, Text(I::AiDupChipVerify), 0.1f);
    // Pre-check each group on a worker: the guard is all or nothing, so a
    // group whose files changed since the compare is dropped here.
    auto ok = std::make_shared<std::vector<char>>(run->scan.groups.size(), 0);
    auto job = std::make_shared<AiAgent::Job>();
    a.job = job;
    std::thread([run, ok, job] {
        for (size_t gi = 0; gi < run->scan.groups.size(); ++gi) {
            if (job->cancelled.load()) break;
            const app::ai::DupGroup& g = run->scan.groups[gi];
            if (!g.Extra()) continue;
            const app::ai::DupFile& keeper = g.files[DupKeeper(g)];
            bool same = true;
            for (const app::ai::DupFile& f : g.files)
                if (!f.keep && !app::ai::DupStillSame(keeper, f, job->cancelled)) {
                    same = false;
                    break;
                }
            (*ok)[gi] = same ? 1 : 0;
        }
        job->done.store(true);
    }).detach();
    AiChatPush(s, 60, [run, ok, job](AppState& st) {
        AiAgent& ag = Agent(st);
        if (ag.generation != run->gen) {
            job->cancelled.store(true);
            return -1;
        }
        if (!job->done.load()) return 60;
        ag.job.reset();
        auto sc = std::make_shared<SpaceClean>();
        sc->dup = true;
        sc->drive = run->drive;
        sc->call_id = run->call_id;
        sc->gen = run->gen;
        sc->bin = run->bin;
        sc->scan.cats.resize(1);
        app::ai::CleanCategory& cat = sc->scan.cats[0];
        cat.kind = app::ai::CleanKind::Copy;
        ops::OpRequest r;
        r.type = ops::OpType::RecycleDelete;
        r.duplicate_cleanup = true;
        run->skipped = 0;
        for (size_t gi = 0; gi < run->scan.groups.size(); ++gi) {
            const app::ai::DupGroup& g = run->scan.groups[gi];
            if (!g.Extra()) continue;
            if (!(*ok)[gi]) {
                ++run->skipped;
                continue;
            }
            const app::ai::DupFile& keeper = g.files[DupKeeper(g)];
            ops::DuplicateCleanupGroup cg;
            cg.keeper = {keeper.path, keeper.bytes, keeper.mtime};
            for (const app::ai::DupFile& f : g.files) {
                if (f.keep) continue;
                cg.extras.push_back({f.path, f.bytes, f.mtime});
                r.sources.push_back(f.path);
                app::ai::CleanItem it;
                it.path = f.path;
                it.bytes = f.bytes;
                it.files = 1;
                it.mtime = DupFt(f.mtime);
                cat.items.push_back(it);
                cat.files += 1;
                cat.bytes += f.bytes;
                sc->sources.push_back(f.path);
                sc->source_bytes.push_back(f.bytes);
                sc->source_cat.push_back(0);
            }
            r.duplicate_groups.push_back(std::move(cg));
        }
        sc->moved.assign(sc->sources.size(), false);
        DriveSpace(sc->drive, sc->total, sc->free_before);
        sc->bin_items_before = sc->bin.items;
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        sc->undo_group = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        const uint64_t since = sc->undo_group - 20000000ull;
        sc->since.dwLowDateTime = static_cast<DWORD>(since);
        sc->since.dwHighDateTime = static_cast<DWORD>(since >> 32);
        if (sc->sources.empty()) {
            SettleDupRecycle(st, run, sc);
            return -1;
        }
        r.undo_group = sc->undo_group;
        r.undo_label = Text(I::AiDupUndoLabel);
        st.ops.Submit(std::move(r));
        struct Watch { uint64_t started = 0, last_change = 0; size_t gone = 0; };
        auto w = std::make_shared<Watch>();
        w->started = w->last_change = Now();
        AiChatPush(st, 150, [run, sc, w](AppState& s2) {
            AiAgent& a2 = Agent(s2);
            if (a2.generation != sc->gen) return -1;
            const uint64_t now = Now();
            size_t gone = 0;
            for (size_t i = 0; i < sc->sources.size(); ++i) {
                if (!sc->moved[i] && !Exists(sc->sources[i])) sc->moved[i] = true;
                if (sc->moved[i]) ++gone;
            }
            if (gone != w->gone) {
                w->gone = gone;
                w->last_change = now;
            }
            const float prog = static_cast<float>(gone) / static_cast<float>(sc->sources.size());
            if (ui::AiStepView* step = LastStep(s2, a2)) {
                step->progress = prog;
                step->sub = Fill(Fill(Text(I::AiDupRecSub), L"{k}", Int(gone)), L"{n}", Int(sc->sources.size()));
            }
            AiChatSetChip(Chat(s2), ui::StatusAiState::Busy,
                          Fill(Text(I::AiClChipRec), L"{p}", Num(static_cast<size_t>(prog * 100.0f + 0.5f))), prog);
            const bool idle = !s2.ops.Status().active;
            if (gone >= sc->sources.size() || (idle && now - w->last_change > 2500) || now - w->started > 600000) {
                SettleDupRecycle(s2, run, sc);
                return -1;
            }
            return 150;
        });
        Redraw(st);
        return -1;
    });
}

// The card, the tab and the confirmation once the funnel is done.
void ShowDupGroups(AppState& s, std::shared_ptr<DupRun> run) {
    AiChat& c = Chat(s);
    if (AiBlockView* fb = c.Find(run->funnel_id)) fb->collapsed = true;
    AiBlockView& b = c.Add(AiBlockKind::Dup, Now());
    run->card_id = b.id;
    run->list_id = b.id * 8;
    b.label = Fill(Text(I::AiDupTitle), L"{g}", Int(run->scan.groups.size()));
    ui::AiDupView& dv = b.dup;
    dv.change = Text(I::AiDupChange);
    dv.both_tag = Text(I::AiDupBoth);
    dv.keep_tag = Text(I::AiDupKeep);
    dv.extra_tag = Text(I::AiDupExtra);
    dv.safe = Text(I::AiDupSafe);
    // Three groups that act plus the first 都留 one, in list order.
    std::vector<size_t> pick;
    size_t first_both = SIZE_MAX;
    for (size_t i = 0; i < run->scan.groups.size(); ++i) {
        if (run->scan.groups[i].both) {
            if (first_both == SIZE_MAX) first_both = i;
        } else if (pick.size() < 3) {
            pick.push_back(i);
        }
    }
    if (first_both != SIZE_MAX) pick.push_back(first_both);
    for (size_t i = 0; i < run->scan.groups.size() && pick.size() < 4; ++i)
        if (std::find(pick.begin(), pick.end(), i) == pick.end()) pick.push_back(i);
    std::sort(pick.begin(), pick.end());
    run->shown = pick;
    const uint64_t now = Now();
    for (size_t i = 0; i < pick.size(); ++i) {
        ui::AiDupGroupView v;
        v.in_ms = now + 120 + 170 * i;
        dv.groups.push_back(std::move(v));
    }
    if (run->scan.groups.size() > pick.size()) {
        dv.more = Fill(Text(I::AiDupMore), L"{n}", Int(run->scan.groups.size() - pick.size()));
        dv.more_link = Text(I::AiDupMoreLink);
    }
    SyncDup(s, *run, false);
    c.dup_id = b.id;
    c.on_dup = [](AppState& s2, ui::AiPanelPart part, int g, int f) { DupClick(s2, part, g, f); };
    AiChatSetChip(c, ui::StatusAiState::Done, Fill(Text(I::AiDupChipDone), L"{g}", Int(run->scan.groups.size())));
    // The tab opens once the groups landed; the confirmation a second later.
    AiChatPush(s, static_cast<uint32_t>(120 + 170 * pick.size()), [run](AppState& s2) {
        if (Agent(s2).generation != run->gen) return -1;
        PutDupList(*run);
        NavigateTo(s2, DupListPath(*run));
        return -1;
    });
    AiChatPush(s, 1000, [run](AppState& s4) {
        AiAgent& a4 = Agent(s4);
        if (a4.generation != run->gen) return -1;
        AiChat& ch = Chat(s4);
        if (!s4.aiAssistant.open) ch.unread = true;
        AiBlockView& cf = ch.Add(AiBlockKind::Confirm, Now());
        cf.tag = Text(I::AiClTagDelete);
        cf.hint = Text(I::AiDupLockHint);
        cf.hint_icon = ui::AiIcon::Lock;
        cf.hint1 = Text(I::AiDupUndoHint);
        cf.hint1_icon = ui::AiIcon::Undo;
        cf.deny = Text(I::AiAsstDeny);
        cf.accept = Text(I::AiClAccept);
        run->confirm_id = cf.id;
        ch.confirm_id = cf.id;
        AiChatSetChip(ch, ui::StatusAiState::Waiting, Text(I::AiAsstChipWaiting));
        SyncDup(s4, *run, false);
        ch.on_confirm = [run](AppState& s3, bool accept) {
            AiChat& c3 = Chat(s3);
            AiAgent& a3 = Agent(s3);
            const DupTotals t = DupCount(*run);
            if (AiBlockView* card = c3.Find(c3.confirm_id)) {
                const uint64_t now2 = Now();
                if (now2 > card->born_ms) c3.turn_start += now2 - card->born_ms;
                card->done = true;
                card->tag.clear();
                card->hint2.clear();
                card->title = accept ? Fill(Text(I::AiDupConfirmed), L"{n}", Int(t.extra)) : Text(I::AiDupDenied);
            }
            c3.confirm_id = 0;
            c3.on_confirm = nullptr;
            c3.dup_id = 0;
            c3.on_dup = nullptr;
            if (a3.generation != run->gen) return;
            if (!accept) {
                if (AiBlockView* db = c3.Find(run->card_id)) db->collapsed = true;
                ToolOutcome out = ToolOutcome::Fail("user_denied", "The user declined moving the duplicate copies in "
                                                                   "Pulse; nothing was moved. Do not offer it again; ask "
                                                                   "what they would like instead.");
                AddToolResult(a3, run->call_id, out);
                ++a3.call_index;
                AiChatSetChip(c3, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
                NextTool(s3);
                Redraw(s3);
                return;
            }
            RunDupRecycle(s3, run);
            Redraw(s3);
        };
        Redraw(s4);
        return -1;
    });
    Redraw(s);
}

void ToolFindDuplicates(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
    AiAgent& a = Agent(s);
    std::vector<std::wstring> names;
    if (const json::Value* f = args.find("folders"); f && f->is_array())
        for (size_t i = 0; i < f->size() && i < 8; ++i)
            if (f->at(i).is_string()) names.push_back(json::Wide(f->at(i).as_string()));
    std::wstring current;
    if (app::Tab* tab = ActiveTab(s); tab && !fs::IsVirtualPath(tab->current_path)) current = NormDir(tab->current_path);
    auto run = std::make_shared<DupRun>();
    std::string error;
    if (!app::ai::ResolveDupRoots(names, current, run->scan.roots, error)) {
        AddToolResult(a, call.id, ToolOutcome::Fail("invalid_args", error));
        ++a.call_index;
        NextTool(s);
        return;
    }
    run->newest = args.get_string("prefer") == "newest";
    run->call_id = call.id;
    run->gen = a.generation;
    run->started = Now();
    for (const app::ai::DupRoot& r : run->scan.roots) {
        run->labels.push_back(DupRootLabel(r));
        run->where += (run->where.empty() ? L"" : Text(I::AiTkListSep)) + run->labels.back();
    }
    AddStep(s, a, ui::AiIcon::Duplicate, Text(I::AiDupStep), run->where, Text(I::AiDupStepSub));
    AiChat& c = Chat(s);
    AiChatSetChip(c, ui::StatusAiState::Busy, Fill(Text(I::AiDupChipRun), L"{i}", L"1"), 0.05f);
    // 比对过程.
    {
        AiBlockView& fb = c.Add(AiBlockKind::Funnel, Now());
        run->funnel_id = fb.id;
        fb.label = Text(I::AiDupFunnel);
        fb.funnel.foot = Text(I::AiDupFunnelFoot);
        const I labels[] = {I::AiDupS1, I::AiDupS2, I::AiDupS3};
        const I subs[] = {I::AiDupS1Sub, I::AiDupS2Sub, I::AiDupS3Sub};
        for (int i = 0; i < 3; ++i) {
            ui::AiFunnelStageView st;
            st.label = Text(labels[i]);
            st.sub = i == 0 ? Fill(Text(I::AiDupS1Run), L"{n}", L"0") : Text(subs[i]);
            st.unit = Text(I::AiDupUnit);
            st.count_dur = i == 2 ? 1300 : 650;
            fb.funnel.stages.push_back(std::move(st));
        }
        fb.funnel.stages[0].state = ui::AiFunnelState::Run;
        fb.funnel.stages[0].state_ms = Now();
    }
    auto progress = std::make_shared<app::ai::DupProgress>();
    auto job = std::make_shared<AiAgent::Job>();
    a.job = job;
    std::thread([job, run, progress] {
        app::ai::FindDuplicates(run->scan, 1024, *progress, job->cancelled);
        if (run->newest)
            for (app::ai::DupGroup& g : run->scan.groups) app::ai::ApplyKeepRules(g, true);
        job->done.store(true);
    }).detach();
    // The rows follow the scan; each holds its count-up before the next.
    struct Pace { int ds = 0, phase = 0; uint64_t t = 0; };
    auto pace = std::make_shared<Pace>();
    pace->t = Now();
    AiChatPush(s, 40, [run, job, progress, pace](AppState& st) {
        AiAgent& ag = Agent(st);
        if (ag.generation != run->gen) {
            job->cancelled.store(true);
            return -1;
        }
        AiChat& ch = Chat(st);
        AiBlockView* fb = ch.Find(run->funnel_id);
        if (!fb || fb->funnel.stages.size() != 3) return -1;
        const uint64_t now = Now();
        const int reached = job->done.load() ? 3 : progress->stage.load();
        ui::AiFunnelStageView& row = fb->funnel.stages[static_cast<size_t>(pace->ds)];
        const app::ai::DupScan& scan = run->scan;
        if (pace->phase == 0) {
            if (pace->ds == 0) row.sub = Fill(Text(I::AiDupS1Run), L"{n}", Int(progress->files.load()));
            else if (reached == pace->ds && progress->todo.load())
                row.sub = Fill(Fill(Text(I::AiDupSRun), L"{k}", Int(progress->done.load())), L"{n}", Int(progress->todo.load()));
            if (reached > pace->ds && now - pace->t >= 300) {
                const uint64_t base = (std::max)(scan.size_matches, uint64_t{1});
                const auto bar = [&](uint64_t v) {
                    return v ? (std::max)(0.04f, static_cast<float>(std::pow(static_cast<double>(v) / static_cast<double>(base), 0.6)))
                             : 0.0f;
                };
                if (pace->ds == 0) {
                    row.value = scan.size_matches;
                    row.sub = Fill(Text(I::AiDupS1Sub), L"{n}", Int(scan.total_files));
                    row.time = Fill(Text(I::AiDupSec), L"{t}", Secs1(scan.ms[0]));
                    row.bar = scan.size_matches ? 1.0f : 0.0f;
                } else if (pace->ds == 1) {
                    row.value = scan.head_matches;
                    row.sub = Text(I::AiDupS2Sub);
                    row.time = Fill(Text(I::AiDupSec), L"{t}", Secs1(scan.ms[1]));
                    row.bar = bar(scan.head_matches);
                } else {
                    row.value = scan.dup_files;
                    row.unit = Fill(Text(I::AiDupUnitGroups), L"{g}", Int(scan.groups.size()));
                    row.sub = Text(I::AiDupS3Sub);
                    row.time = Fill(Fill(Text(I::AiDupSecRead), L"{t}", Secs1(scan.ms[2])), L"{size}", SizeShort(scan.read_bytes));
                    row.bar = bar(scan.dup_files);
                }
                row.count_ms = now;
                pace->phase = 1;
                pace->t = now;
            }
        } else if (pace->phase == 1) {
            if (now - pace->t >= row.count_dur) {
                row.state = ui::AiFunnelState::Ok;
                row.state_ms = now;
                pace->phase = 2;
                pace->t = now;
            }
        } else if (now - pace->t >= 160) {
            ++pace->ds;
            pace->phase = 0;
            pace->t = now;
            if (pace->ds < 3) {
                fb->funnel.stages[static_cast<size_t>(pace->ds)].state = ui::AiFunnelState::Run;
                fb->funnel.stages[static_cast<size_t>(pace->ds)].state_ms = now;
            }
        }
        if (pace->ds < 3) {
            const float prog = (static_cast<float>(pace->ds) + (pace->phase ? 0.9f : 0.3f)) / 3.0f;
            if (ui::AiStepView* step = LastStep(st, ag)) step->progress = prog;
            AiChatSetChip(ch, ui::StatusAiState::Busy, Fill(Text(I::AiDupChipRun), L"{i}", Num(static_cast<size_t>(pace->ds + 1))), prog);
            Redraw(st);
            return 40;
        }
        // Done.
        ag.job.reset();
        const uint64_t total_ms = scan.ms[0] + scan.ms[1] + scan.ms[2];
        fb->meta = Fill(Text(I::AiDupFunnelMeta), L"{t}", Secs1(total_ms));
        const bool none = scan.groups.empty();
        EndStep(st, ag, true,
                none ? Fill(Fill(Text(I::AiDupStepNone), L"{n}", Int(scan.total_files)), L"{t}", Secs1(total_ms))
                     : Fill(Fill(Fill(Text(I::AiDupStepDone), L"{g}", Int(scan.groups.size())), L"{n}", Int(scan.dup_files)),
                            L"{t}", Secs1(total_ms)));
        if (AiBlockView* sb = Block(st, ag.steps_id)) {
            sb->label = none ? Fill(Text(I::AiDupStepsNone), L"{where}", run->where)
                             : Fill(Fill(Text(I::AiDupSteps), L"{where}", run->where), L"{g}", Int(scan.groups.size()));
            sb->collapsed = true;
            sb->open = false;
        }
        ag.steps_id = 0;
        if (none) {
            AiChatSetChip(ch, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
            ToolOutcome out;
            out.data = json::Value::Object();
            out.data.set("groups", static_cast<uint64_t>(0));
            out.data.set("files_compared", scan.total_files);
            out.data.set("note", "No two files have the same content in these folders. Say so in one sentence.");
            AddToolResult(ag, run->call_id, out);
            ++ag.call_index;
            AiChatPush(st, 350, [](AppState& s2) { NextTool(s2); return -1; });
            Redraw(st);
            return -1;
        }
        // The Recycle Bin of the drive with most of the copies.
        std::map<wchar_t, uint64_t> per_drive;
        for (const auto& g : scan.groups)
            for (const auto& f : g.files)
                if (!f.keep && !f.path.empty()) per_drive[static_cast<wchar_t>(towupper(f.path[0]))] += f.bytes;
        uint64_t most = 0;
        for (const auto& [d, n] : per_drive)
            if (n > most) {
                most = n;
                run->drive = d;
            }
        run->bin = app::ai::ReadBin(run->drive);
        g_dup = run;
        AiChatSetChip(ch, ui::StatusAiState::Done, Fill(Text(I::AiDupChipDone), L"{g}", Int(scan.groups.size())));
        Redraw(st);
        AiChatPush(st, 800, [run](AppState& s2) {
            if (Agent(s2).generation != run->gen) return -1;
            ShowDupGroups(s2, run);
            return -1;
        });
        return -1;
    });
}

// --------------------------------------------------------- run_actions --
// P3 (plans/ai/P3_AI接入动作表.html), grown from the third batch's
// control_pulse (flow F): the assistant runs actions of the action table
// (actions/action_registry.h) by id, in order. View, organize and create
// actions run at once; programs and system actions ask first (one confirm
// card for the call); moving and deleting files stays with the flow tools
// (P4). Each action is a step; the result card undoes the call as one group
// and shows the user's own keys (or what to type in the palette). The call
// stops at the first step that fails; what was done stays done and undoable.

struct CtlRun {
    std::string call_id;
    uint64_t gen = 0;
    std::vector<json::Value> queue;     // the actions; a layout's panes add opens
    size_t index = 0;
    // 撤销, in reverse order. Each checks that what it restores still exists.
    std::vector<std::function<void(AppState&)>> undo;
    std::vector<ui::AiKeyRowView> keys;
    std::vector<ui::AiResultLine> lines;
    json::Value report = json::Value::Array();
    size_t ok = 0;
    // What kinds were done (title, undo text).
    bool layout = false, open = false, window = false, other = false;
    size_t organize = 0;
    std::vector<std::wstring> window_names;   // the new windows
    HWND last_window = nullptr;               // the newest of them: the next cascades from it
    std::vector<std::wstring> back_parts;     // 桌面回到右栏 / D 盘窗口已关闭
    std::vector<std::wstring> organize_kinds; // 托盘 / 标签 / 收藏夹
    bool single_back = false;                 // a pane left: the window is one pane again
    app::LayoutPreset first_layout = app::LayoutPreset::Single;
    bool has_first_layout = false;
    std::wstring single_title;                // one plain action: its done line
    bool merged = false;
    // P3.
    bool locked = false, deny_delete = false;   // a locked setting: the lock card
    std::wstring fail_title, fail_arg, fail_why; // the step that failed
    std::vector<std::wstring> not_run;          // titles of the steps after it
    size_t waited = SIZE_MAX;                   // the step whose wait is over
    bool touched = false;                       // a file was made or opened: no 文件没动 line
    size_t undo_steps = 0;                      // steps 撤销 reverses (a selection is not one)
};

const wchar_t* PresetName(app::LayoutPreset p) {
    switch (p) {
    case app::LayoutPreset::Single: return l10n::Get(I::AiCtlLayoutSingle).c_str();
    case app::LayoutPreset::TwoVertical: return l10n::Get(I::AiCtlLayoutTwoV).c_str();
    case app::LayoutPreset::TwoHorizontal: return l10n::Get(I::AiCtlLayoutTwoH).c_str();
    case app::LayoutPreset::Three: return l10n::Get(I::AiCtlLayoutThree).c_str();
    case app::LayoutPreset::FourGrid: return l10n::Get(I::AiCtlLayoutFour).c_str();
    default: return l10n::Get(I::AiCtlLayoutCustom).c_str();
    }
}

// The panes in screen order, numbered as get_state numbers them.
std::vector<app::Pane*> ShownPanes(AppState& s) {
    if (!Root(s)) return {};
    return app::actions::PanesOnScreen(LiveLayout(s));
}

bool PaneAlive(AppState& s, const app::Pane* p) {
    if (!p) return false;
    for (const auto& owned : Panes(s))
        if (owned.get() == p) return true;
    return false;
}

// 左栏 / 右栏, 上栏 / 下栏, 第 3 栏; empty with one pane.
std::wstring PanePos(AppState& s, size_t i) {
    const size_t n = ShownPanes(s).size();
    if (n < 2) return {};
    const app::LayoutPreset p = LayoutOf(s);
    if (n == 2 && p == app::LayoutPreset::TwoVertical) return Text(i == 0 ? I::AiCtlPaneLeft : I::AiCtlPaneRight);
    if (n == 2 && p == app::LayoutPreset::TwoHorizontal) return Text(i == 0 ? I::AiCtlPaneTop : I::AiCtlPaneBottom);
    return Fill(Text(I::AiCtlPaneN), L"{n}", Num(i + 1));
}

size_t PaneIndex(AppState& s, const app::Pane* p) {
    const auto v = ShownPanes(s);
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] == p) return i;
    return 0;
}

std::wstring PlaceLabel(const std::wstring& path) {
    if (path.empty()) return Text(I::AiCtlThisPc);
    return FolderLabel(path);
}

// 左栏 下载 · 右栏 桌面
std::wstring PanesLine(AppState& s) {
    std::wstring out;
    const auto v = ShownPanes(s);
    for (size_t i = 0; i < v.size(); ++i) {
        const app::Tab* t = v[i]->ActiveTab();
        if (!out.empty()) out += L" \u00B7 ";
        out += PanePos(s, i) + L" " + PlaceLabel(t ? t->current_path : std::wstring());
    }
    return out;
}

// The keys now bound to `action` (a range action: its `position`th chord).
std::vector<std::wstring> KeysFor(app::KeyAction action, int position = -1) {
    const app::Keymap& km = app::ActiveKeymap();
    if (position >= 0) {
        const auto chords = km.RangeChords(action, km.RangeMods(action));
        if (static_cast<size_t>(position) < chords.size()) return app::ChordCaps(chords[static_cast<size_t>(position)]);
        return {};
    }
    const auto& chords = km.Bindings(action);
    return chords.empty() ? std::vector<std::wstring>() : app::ChordCaps(chords.front());
}

void AddKeys(CtlRun& run, std::wstring label, std::vector<std::wstring> caps) {
    if (caps.empty() || run.keys.size() >= 4) return;
    for (const auto& k : run.keys)
        if (k.label == label) return;
    run.keys.push_back({std::move(label), std::move(caps)});
}

// A folder the model named: absolute, a drive (D:), or a known folder.
std::wstring CtlFolder(const std::string& utf8) {
    std::wstring p = ResolveTaskDest(json::Wide(utf8));
    if (p.empty() || !IsDirectory(p)) return {};
    return p;
}

std::vector<std::wstring> CtlItems(AppState& s, const json::Value& act) {
    std::vector<std::wstring> out;
    if (const json::Value* v = act.find("items"); v && v->is_array()) {
        for (size_t i = 0; i < v->size() && out.size() < 500; ++i) {
            if (!v->at(i).is_string()) continue;
            std::wstring p = json::Wide(v->at(i).as_string());
            if (p.empty() || fs::IsVirtualPath(p)) continue;
            if (const std::wstring known = ResolveTaskDest(p); !known.empty()) p = known;
            if (Exists(p)) out.push_back(NormDir(p));
        }
        return out;
    }
    app::Tab* tab = ActiveTab(s);
    if (tab && tab->snapshot && !fs::IsVirtualPath(tab->current_path)) out = SelectedFullPaths(*tab);
    return out;
}

struct CtlStep {
    ui::AiIcon icon = ui::AiIcon::Window;
    std::wstring title, arg, sub;
    bool ok = false;
    std::string error;               // back to the model when !ok
    app::Pane* wait = nullptr;       // the sub is the item count once it loads
    bool skip = false;               // nothing to do (already so): no step
    std::wstring why;                // !ok: the reason the card shows (找不到这个文件夹)
    json::Value data;                // more for the model (a selection's report)
};

// Runs `fn` with `p` focused, then gives the focus back.
template <class Fn>
void WithPane(AppState& s, app::Pane* p, Fn&& fn) {
    app::Pane* before = s.pane;
    if (p && p != s.pane) FocusPane(s, p);
    fn();
    if (before && before != s.pane && PaneAlive(s, before)) FocusPane(s, before);
}

// The windows of one call cascade from each other instead of all opening on
// the same spot next to this one.
HWND CtlOpenWindow(AppState& s, CtlRun& run, WindowInit init) {
    RECT last{};
    MONITORINFO mi{sizeof(mi)};
    if (run.last_window && IsWindow(run.last_window) && !IsZoomed(run.last_window) && !IsIconic(run.last_window) &&
        GetWindowRect(run.last_window, &last) &&
        GetMonitorInfoW(MonitorFromWindow(run.last_window, MONITOR_DEFAULTTONEAREST), &mi))
        init.rect = app::CascadeWindowRect(last, mi.rcWork, static_cast<int>(std::lround(32.0f * (s.scale > 0.0f ? s.scale : 1.0f))));
    const HWND h = OpenPulseWindow(s, init);
    if (h) run.last_window = h;
    return h;
}

CtlStep CtlLayout(AppState& s, CtlRun& run, const json::Value& act) {
    CtlStep st;
    st.icon = ui::AiIcon::Split;
    st.title = Text(I::AiCtlStepLayout);
    // get_state's names; control_pulse's old ones still work.
    static const struct { const char* id; app::LayoutPreset p; int key; } kMap[] = {
        {"single", app::LayoutPreset::Single, 0},       {"side_by_side", app::LayoutPreset::TwoVertical, 1},
        {"stacked", app::LayoutPreset::TwoHorizontal, -1}, {"three", app::LayoutPreset::Three, 2},
        {"four", app::LayoutPreset::FourGrid, 3},       {"two_vertical", app::LayoutPreset::TwoVertical, 1},
        {"two_horizontal", app::LayoutPreset::TwoHorizontal, -1},
    };
    const std::string want = act.get_string("layout");
    const auto* m = std::find_if(std::begin(kMap), std::end(kMap), [&](const auto& e) { return want == e.id; });
    if (m == std::end(kMap)) {
        st.error = "unknown layout";
        return st;
    }
    st.arg = PresetName(m->p);
    const app::LayoutPreset before = LayoutOf(s);
    app::Pane* focus = s.pane;
    const app::LayoutTab* lt = s.window_tabs.Active();
    if (before != m->p) {
        ApplyLayoutPreset(s, m->p);
        if (LayoutOf(s) != m->p) {
            st.error = "this tab cannot be split (a settings page)";
            st.why = Text(I::AiActWhyFailed);
            return st;
        }
        if (before != app::LayoutPreset::Custom)
            run.undo.push_back([before, focus, lt](AppState& st2) {
                if (st2.window_tabs.Active() != lt) return;
                if (PaneAlive(st2, focus)) FocusPane(st2, focus);
                ApplyLayoutPreset(st2, before);
            });
        if (!run.has_first_layout) {
            run.first_layout = before;
            run.has_first_layout = true;
        }
    }
    st.ok = true;
    st.sub = PanesLine(s);
    run.layout = true;
    AddKeys(run, PresetName(m->p), m->key >= 0 ? KeysFor(app::KeyAction::Layout, m->key) : std::vector<std::wstring>());
    if (ShownPanes(s).size() > 1)
        AddKeys(run, Text(ShownPanes(s).size() == 2 ? I::AiCtlKeyNextPane2 : I::AiCtlKeyNextPane), KeysFor(app::KeyAction::NextPane));
    // panes: one open per named folder, right after this step.
    if (const json::Value* v = act.find("panes"); v && v->is_array()) {
        std::vector<json::Value> opens;
        for (size_t i = 0; i < v->size() && i < 4; ++i) {
            if (!v->at(i).is_string() || v->at(i).as_string().empty()) continue;
            json::Value args = json::Value::Object();
            args.set("path", v->at(i).as_string());
            args.set("pane", static_cast<uint64_t>(i + 1));
            json::Value o = json::Value::Object();
            o.set("action", "nav.go");
            o.set("args", std::move(args));
            opens.push_back(std::move(o));
        }
        run.queue.insert(run.queue.begin() + static_cast<std::ptrdiff_t>(run.index + 1), opens.begin(), opens.end());
    }
    return st;
}

CtlStep CtlOpen(AppState& s, CtlRun& run, const json::Value& act) {
    CtlStep st;
    st.icon = ui::AiIcon::Folder;
    const std::wstring path = CtlFolder(act.get_string("path"));
    std::vector<app::Pane*> picked;
    std::string pane_error;
    app::Pane* p = s.pane;
    if (const json::Value* v = act.find("pane"); v && !(v->is_string() && v->as_string() == "all")) {
        if (!app::actions::PanesForArg(s, v, picked, pane_error)) {
            st.title = Text(I::AiCtlStepOpen);
            st.arg = json::Wide(act.get_string("path"));
            st.error = pane_error;
            st.why = Text(I::AiActWhyFailed);
            return st;
        }
        p = picked.front();
    }
    const std::wstring pos = PanePos(s, PaneIndex(s, p));
    st.title = pos.empty() ? Text(I::AiCtlStepOpen) : Fill(Text(I::AiCtlStepOpenIn), L"{pane}", pos);
    if (path.empty()) {
        st.arg = json::Wide(act.get_string("path"));
        st.error = "no such folder";
        st.why = Text(I::AiActWhyNoFolder);
        return st;
    }
    st.arg = PlaceLabel(path);
    if (_wcsicmp(NormDir(path).c_str(), KnownFolder(FOLDERID_Desktop).c_str()) == 0) st.icon = ui::AiIcon::Desktop;
    app::Tab* tab = p ? p->ActiveTab() : nullptr;
    if (!tab) {
        st.error = "no pane";
        return st;
    }
    if (SameDir(tab->current_path, path)) {
        st.skip = true;
        st.ok = true;
        return st;
    }
    const std::wstring old = tab->current_path;
    WithPane(s, p, [&] { NavigateTo(s, path); });
    const std::wstring now = tab->current_path;
    run.undo.push_back([p, old, now](AppState& st2) {
        if (!PaneAlive(st2, p) || !p->ActiveTab() || p->ActiveTab()->current_path != now) return;
        WithPane(st2, p, [&] { NavigateTo(st2, old); });
    });
    st.ok = true;
    st.wait = p;
    run.open = true;
    return st;
}

CtlStep CtlNewWindow(AppState& s, CtlRun& run, const json::Value& act) {
    CtlStep st;
    st.icon = ui::AiIcon::NewWindow;
    st.title = Text(I::AiCtlStepNewWindow);
    std::wstring path;
    if (!act.get_string("path").empty()) {
        path = CtlFolder(act.get_string("path"));
        if (path.empty()) {
            st.arg = json::Wide(act.get_string("path"));
            st.error = "no such folder";
            st.why = Text(I::AiActWhyNoFolder);
            return st;
        }
    } else if (const app::Tab* tab = ActiveTab(s)) {
        path = tab->current_path;
    }
    st.arg = PlaceLabel(path);
    const HWND h = CtlOpenWindow(s, run, WindowInit{path});
    if (!h) {
        st.error = "the window could not be opened";
        return st;
    }
    AppCore* core = &s.core;
    run.undo.push_back([h, core](AppState& st) {
        if (!core->windows.Lookup(h)) return;
        ShowWindow(h, SW_HIDE);
        // Undo takes back a window the assistant opened: it is not one the
        // user closed, so no 已关闭窗口 entry and no 恢复 toast.
        const uint64_t seq = core->closed_windows.LatestSeq();
        DestroyWindow(h);
        if (core->closed_windows.LatestSeq() != seq) {
            core->closed_windows.TakeLatest();
            st.notification_toast.DismissFor(st.hwnd, WM_REOPEN_CLOSED_WINDOW);
        }
    });
    st.ok = true;
    st.sub = Fill(Text(I::AiCtlNewWindowN), L"{n}", Num(s.core.windows.Count()));
    run.window = true;
    run.window_names.push_back(st.arg);
    run.back_parts.push_back(Fill(Text(I::AiCtlBackClosed), L"{name}", st.arg));
    AddKeys(run, Text(I::AiCtlKeyNewWindow), KeysFor(app::KeyAction::NewWindow));
    return st;
}

// The tab (tab_to_window) or one pane of the split (pane_to_window) moves
// to a window of its own; 撤销 puts it back where it was.
CtlStep CtlToWindow(AppState& s, CtlRun& run, const json::Value& act, bool pane_only) {
    CtlStep st;
    st.icon = ui::AiIcon::NewWindow;
    AppCore* core = &s.core;
    AppState* self = &s;
    auto panes = ShownPanes(s);
    if (!pane_only && s.window_tabs.items.size() < 2 && panes.size() >= 2) pane_only = true;   // the focused pane
    if (pane_only) {
        int64_t n = 0;
        app::Pane* p = s.pane;
        if (const json::Value* v = act.find("pane"); v && v->get_int(n, 1, 4) && static_cast<size_t>(n) <= panes.size())
            p = panes[static_cast<size_t>(n - 1)];
        const std::wstring pos = PanePos(s, PaneIndex(s, p));
        const std::wstring label = PlaceLabel(p && p->ActiveTab() ? p->ActiveTab()->current_path : std::wstring());
        st.title = Text(I::AiCtlStepTabToWindow);
        st.arg = label;
        if (panes.size() < 2 || !p) {
            st.error = "the window shows one pane; use tab_to_window or new_window";
            return st;
        }
        app::LayoutTab* into = s.window_tabs.Active();
        auto origin = std::make_shared<app::PaneOrigin>();
        const std::optional<size_t> tab_index = SplitPaneIntoTab(s, p, *origin);
        if (!tab_index) {
            st.error = "this pane cannot leave the split";
            return st;
        }
        WindowInit init;
        if (p->ActiveTab()) init.path = p->ActiveTab()->current_path;
        init.adopt_from = &s;
        init.adopt_index = *tab_index;
        const HWND h = CtlOpenWindow(s, run, init);
        // The split it left shows again here.
        for (size_t i = 0; i < s.window_tabs.items.size(); ++i)
            if (s.window_tabs.items[i].get() == into) {
                if (s.window_tabs.active != i) SwitchTab(s, i);
                break;
            }
        if (!h) {
            st.error = "the window could not be opened";
            return st;
        }
        run.undo.push_back([h, core, self, into, origin](AppState&) {
            AppState* moved = core->windows.Lookup(h);
            if (!moved || moved->window_tabs.items.size() != 1) return;
            ShowWindow(h, SW_HIDE);
            if (ReturnTabAsPane(*moved, 0, *self, into, *origin, TransferSource::Closing)) DestroyWindow(h);
            else ShowWindow(h, SW_SHOWNORMAL);
        });
        st.ok = true;
        st.sub = Fill(Text(I::AiCtlNewWindowN), L"{n}", Num(s.core.windows.Count()));
        if (ShownPanes(s).size() == 1) run.single_back = true;
        run.window = true;
        run.window_names.push_back(label);
        run.back_parts.push_back(Fill(Fill(Text(I::AiCtlBackPane), L"{name}", label), L"{pane}", pos));
        AddKeys(run, Text(I::AiCtlKeyTabToWindow), KeysFor(app::KeyAction::TabToWindow));
        return st;
    }
    auto& tabs = s.window_tabs;
    const size_t index = tabs.active;
    const std::wstring label = PlaceLabel(ActiveTab(s) ? ActiveTab(s)->current_path : std::wstring());
    st.title = Text(I::AiCtlStepTabToWindow);
    st.arg = label;
    if (tabs.items.size() < 2 || index >= tabs.items.size()) {
        st.error = "the window has one tab; use new_window, or pane_to_window for a pane";
        return st;
    }
    WindowInit init;
    if (const app::Tab* view = ActiveTab(s)) init.path = view->current_path;
    init.adopt_from = &s;
    init.adopt_index = index;
    const size_t before = tabs.items.size();
    const HWND h = CtlOpenWindow(s, run, init);
    if (!h || s.window_tabs.items.size() == before) {
        st.error = "the window could not be opened";
        return st;
    }
    run.undo.push_back([h, core, self, index](AppState&) {
        AppState* moved = core->windows.Lookup(h);
        if (!moved || moved->window_tabs.items.size() != 1) return;
        ShowWindow(h, SW_HIDE);
        if (MoveLayoutTab(*moved, 0, *self, index, TransferSource::Closing)) DestroyWindow(h);
        else ShowWindow(h, SW_SHOWNORMAL);
    });
    st.ok = true;
    st.sub = Fill(Text(I::AiCtlNewWindowN), L"{n}", Num(s.core.windows.Count()));
    run.window = true;
    run.window_names.push_back(label);
    run.back_parts.push_back(Fill(Text(I::AiCtlBackTab), L"{name}", label));
    AddKeys(run, Text(I::AiCtlKeyTabToWindow), KeysFor(app::KeyAction::TabToWindow));
    return st;
}

const wchar_t* ViewName(ui::ViewMode m) {
    static const I kNames[] = {I::AiCtlViewXl, I::AiCtlViewL, I::AiCtlViewM, I::AiCtlViewS,
                               I::AiCtlViewList, I::AiCtlViewDetails, I::AiCtlViewTiles, I::AiCtlViewContent};
    const int i = ui::ViewModeIndex(m);
    return l10n::Get(kNames[i >= 0 && i < 8 ? i : 5]).c_str();
}

// A result line from a sentence with `key` in it: the value goes in bold.
ui::AiResultLine SplitLine(const std::wstring& text, std::wstring_view key, const std::wstring& value) {
    ui::AiResultLine ln;
    const size_t at = key.empty() ? std::wstring::npos : text.find(key);
    if (at == std::wstring::npos) {
        ln.before = text;
        return ln;
    }
    ln.before = text.substr(0, at);
    ln.bold = value;
    ln.after = text.substr(at + key.size());
    return ln;
}

void AddLine(CtlRun& run, ui::AiResultLine ln) {
    run.single_title = ln.before + ln.bold + ln.after;
    run.lines.push_back(std::move(ln));
}

// One plain action (view, sort, bars, windows) done: its line.
void PlainDone(CtlRun& run, ui::AiResultLine ln) {
    run.other = true;
    AddLine(run, std::move(ln));
}

// The panes a view / sort / group step changes (the "pane" argument; "all"
// is every pane on screen). False: st has the error.
bool StepPanes(AppState& s, const json::Value& act, CtlStep& st, std::vector<app::Pane*>& out) {
    std::string error;
    if (app::actions::PanesForArg(s, act.find("pane"), out, error)) {
        std::erase_if(out, [](app::Pane* p) { return !p || !p->ActiveTab(); });
        if (!out.empty()) return true;
        error = "no tab";
    }
    st.error = error;
    st.why = Text(I::AiActWhyFailed);
    return false;
}

// "全部 2 栏" after the argument when the step changed more than one pane.
void AllPanesArg(CtlStep& st, size_t n) {
    if (n > 1) st.arg += L" \u00B7 " + Fill(Text(I::AiActAllPanes), L"{n}", Num(n));
}

CtlStep CtlView(AppState& s, CtlRun& run, const json::Value& act) {
    CtlStep st;
    st.icon = ui::AiIcon::Rows;
    st.title = Text(I::AiCtlStepView);
    static const char* kIds[] = {"extra_large_icons", "large_icons", "medium_icons", "small_icons",
                                 "list", "details", "tiles", "content"};
    const std::string want = act.get_string("view", act.get_string("mode"));
    int idx = -1;
    for (int i = 0; i < 8; ++i)
        if (want == kIds[i]) idx = i;
    if (idx < 0) {
        st.error = "unknown view";
        st.why = Text(I::AiActWhyFailed);
        return st;
    }
    std::vector<app::Pane*> panes;
    if (!StepPanes(s, act, st, panes)) return st;
    const ui::ViewMode mode = ui::ViewModeFromIndex(idx);
    st.arg = ViewName(mode);
    AllPanesArg(st, panes.size());
    st.ok = true;
    st.sub = Fill(Text(I::AiCtlViewDone), L"{view}", ViewName(mode));
    AddKeys(run, ViewName(mode), KeysFor(app::KeyAction::ViewMode, idx));
    bool changed = false;
    for (app::Pane* p : panes) {
        const ui::ViewMode old = p->ActiveTab()->view_mode;
        if (old == mode) continue;
        WithPane(s, p, [&] { ChooseViewMode(s, mode); });
        run.undo.push_back([p, old](AppState& st2) {
            if (PaneAlive(st2, p)) WithPane(st2, p, [&] { ChooseViewMode(st2, old); });
        });
        changed = true;
    }
    if (!changed) {
        st.skip = true;
        return st;
    }
    PlainDone(run, SplitLine(Text(I::AiCtlLineView), L"{view}", ViewName(mode)));
    return st;
}

CtlStep CtlSort(AppState& s, CtlRun& run, const json::Value& act) {
    CtlStep st;
    st.icon = ui::AiIcon::Sliders;
    st.title = Text(I::AiCtlStepSort);
    static const struct { const char* id; ui::SortColumn col; I name; } kCols[] = {
        {"name", ui::SortColumn::Name, I::AiCtlSortName},   {"modified", ui::SortColumn::Mtime, I::AiCtlSortModified},
        {"type", ui::SortColumn::Type, I::AiCtlSortType},   {"size", ui::SortColumn::Size, I::AiCtlSortSize},
        {"created", ui::SortColumn::Created, I::AiCtlSortCreated},
    };
    const std::string want = act.get_string("by", act.get_string("sort"));
    const auto* m = std::find_if(std::begin(kCols), std::end(kCols), [&](const auto& e) { return want == e.id; });
    if (m == std::end(kCols)) {
        st.error = "unknown sort";
        st.why = Text(I::AiActWhyFailed);
        return st;
    }
    std::vector<app::Pane*> panes;
    if (!StepPanes(s, act, st, panes)) return st;
    const bool desc = act.get_bool("desc", m->col == ui::SortColumn::Mtime || m->col == ui::SortColumn::Size ||
                                               m->col == ui::SortColumn::Created);
    // The direction in the column's own words: 新的在前, 大的在前, A → Z.
    const bool dated = m->col == ui::SortColumn::Mtime || m->col == ui::SortColumn::Created;
    const std::wstring dir_text = Text(dated ? (desc ? I::AiActSortNew : I::AiActSortOld)
                                       : m->col == ui::SortColumn::Size ? (desc ? I::AiActSortBig : I::AiActSortSmall)
                                                                        : (desc ? I::AiActSortZA : I::AiActSortAZ));
    const std::wstring by = Text(m->name);
    st.arg = by + L" \u00B7 " + dir_text;
    AllPanesArg(st, panes.size());
    st.ok = true;
    st.sub = Text(I::AiCtlApplied);
    const ui::SortDirection dir = desc ? ui::SortDirection::Desc : ui::SortDirection::Asc;
    bool changed = false;
    for (app::Pane* p : panes) {
        const app::Tab* tab = p->ActiveTab();
        if (tab->sort_column == m->col && tab->sort_direction == dir) continue;
        const ui::SortColumn old_col = tab->sort_column;
        const ui::SortDirection old_dir = tab->sort_direction;
        WithPane(s, p, [&] { SetSort(s, m->col, dir); });
        run.undo.push_back([p, old_col, old_dir](AppState& st2) {
            if (PaneAlive(st2, p)) WithPane(st2, p, [&] { SetSort(st2, old_col, old_dir); });
        });
        changed = true;
    }
    if (!changed) {
        st.skip = true;
        return st;
    }
    const std::wstring which = panes.size() == 2 ? Text(I::AiActBothPanes)
                                                 : Fill(Text(I::AiActAllPanes), L"{n}", Num(panes.size()));
    PlainDone(run, SplitLine(Fill(Fill(Text(panes.size() > 1 ? I::AiActLineSortAll : I::AiActLineSort), L"{panes}", which),
                                  L"{dir}", dir_text),
                             L"{by}", by));
    return st;
}

CtlStep CtlGroup(AppState& s, CtlRun& run, const json::Value& act) {
    CtlStep st;
    st.icon = ui::AiIcon::Rows;
    st.title = Text(I::AiCtlStepGroup);
    static const struct { const char* id; int v; I name; } kBy[] = {
        {"none", 0, I::AiCtlGroupNone}, {"name", 1, I::AiCtlSortName}, {"date", 2, I::AiCtlGroupDate},
        {"type", 3, I::AiCtlSortType},  {"size", 4, I::AiCtlSortSize},
    };
    const std::string want = act.get_string("by", act.get_string("group"));
    const auto* m = std::find_if(std::begin(kBy), std::end(kBy), [&](const auto& e) { return want == e.id; });
    if (m == std::end(kBy)) {
        st.error = "unknown group";
        st.why = Text(I::AiActWhyFailed);
        return st;
    }
    std::vector<app::Pane*> panes;
    if (!StepPanes(s, act, st, panes)) return st;
    st.arg = Text(m->name);
    AllPanesArg(st, panes.size());
    st.ok = true;
    st.sub = Text(I::AiCtlApplied);
    bool changed = false;
    for (app::Pane* p : panes) {
        const int old = p->ActiveTab()->group_by;
        if (old == m->v) continue;
        WithPane(s, p, [&] { SetGroupBy(s, m->v); });
        run.undo.push_back([p, old](AppState& st2) {
            if (PaneAlive(st2, p)) WithPane(st2, p, [&] { SetGroupBy(st2, old); });
        });
        changed = true;
    }
    if (!changed) {
        st.skip = true;
        return st;
    }
    PlainDone(run, m->v == 0 ? SplitLine(Text(I::AiCtlLineGroupOff), {}, {})
                             : SplitLine(Text(I::AiCtlLineGroup), L"{by}", Text(m->name)));
    return st;
}

// sidebar / favorites_bar: show (true) or hide.
CtlStep CtlBar(AppState& s, CtlRun& run, const json::Value& act, bool favorites) {
    CtlStep st;
    st.icon = favorites ? ui::AiIcon::Star : ui::AiIcon::Dock;
    st.title = Text(favorites ? I::AiCtlStepFavBar : I::AiCtlStepSidebar);
    const bool show = act.get_bool("show", true);
    st.arg = Text(favorites ? (show ? I::AiCtlShow : I::AiCtlHide) : (show ? I::AiCtlExpand : I::AiCtlCollapse));
    st.ok = true;
    st.sub = Text(I::AiCtlApplied);
    AddKeys(run, Text(favorites ? I::AiCtlKeyFavBar : I::AiCtlKeySidebar),
            KeysFor(favorites ? app::KeyAction::FavoritesBar : app::KeyAction::Sidebar));
    const auto shown = [favorites](AppState& st2) {
        return favorites ? st2.appPrefs.favorites_bar : !st2.appPrefs.sidebar_collapsed;
    };
    if (shown(s) == show) {
        st.skip = true;
        return st;
    }
    if (favorites) ToggleFavoritesBar(s);
    else ToggleSidebarCollapsed(s);
    run.undo.push_back([favorites, show, shown](AppState& st2) {
        if (shown(st2) != show) return;
        if (favorites) ToggleFavoritesBar(st2);
        else ToggleSidebarCollapsed(st2);
    });
    PlainDone(run, SplitLine(Text(favorites ? (show ? I::AiCtlFavBarShown : I::AiCtlFavBarHidden)
                                            : (show ? I::AiCtlSidebarShown : I::AiCtlSidebarHidden)),
                             {}, {}));
    return st;
}

CtlStep CtlTray(AppState& s, CtlRun& run, const json::Value& act) {
    CtlStep st;
    st.icon = ui::AiIcon::Tray;
    st.title = Text(I::AiCtlStepTray);
    const std::vector<std::wstring> paths = CtlItems(s, act);
    st.arg = Fill(Text(I::AiCtlItems), L"{n}", Num(paths.size()));
    if (paths.empty()) {
        st.error = "no items (select some, or pass full paths)";
        st.why = Text(I::AiActWhyNoItems);
        return st;
    }
    std::vector<uint64_t> before;
    for (const auto& b : s.tray.batches())
        for (const auto& it : b.items) before.push_back(it.id);
    StagePaths(s, paths, std::nullopt);
    std::vector<uint64_t> added;
    size_t total = 0;
    for (const auto& b : s.tray.batches())
        for (const auto& it : b.items) {
            ++total;
            if (std::find(before.begin(), before.end(), it.id) == before.end()) added.push_back(it.id);
        }
    st.ok = true;
    st.sub = Fill(Text(I::AiCtlTrayNow), L"{n}", Num(total));
    if (!added.empty())
        run.undo.push_back([added](AppState& st2) { RemoveTrayItems(st2, added); });
    ++run.organize;
    run.organize_kinds.push_back(Text(I::AiCtlKindTray));
    AddLine(run, SplitLine(Text(I::AiCtlLineTray), L"{items}", st.arg));
    AddKeys(run, Text(I::AiCtlKeyTray), KeysFor(app::KeyAction::StageToTray));
    return st;
}

CtlStep CtlTag(AppState& s, CtlRun& run, const json::Value& act) {
    CtlStep st;
    st.icon = ui::AiIcon::Tag;
    st.title = Text(I::AiCtlStepTag);
    std::wstring name = json::Wide(act.get_string("tag"));
    while (!name.empty() && std::iswspace(name.back())) name.pop_back();
    while (!name.empty() && std::iswspace(name.front())) name.erase(0, 1);
    const std::vector<std::wstring> paths = CtlItems(s, act);
    st.arg = name;
    if (name.empty() || name.size() > 40) {
        st.error = "a tag name is needed";
        return st;
    }
    if (paths.empty()) {
        st.error = "no items (select some, or pass full paths)";
        st.why = Text(I::AiActWhyNoItems);
        return st;
    }
    app::TagId id;
    for (const auto& t : s.places.tags)
        if (_wcsicmp(t.name.c_str(), name.c_str()) == 0) {
            id = t.id;
            name = t.name;
        }
    bool created = false;
    if (id.empty()) {
        const std::vector<uint32_t>& palette = TagColorPalette(s);
        id = s.places.CreateTag(name, palette.empty() ? 0xEF4444u : palette[s.places.tags.size() % palette.size()]);
        created = !id.empty();
        st.arg = Fill(Text(I::AiCtlTagNew), L"{name}", name);
    }
    const int index = s.places.FindTagIndex(id);
    if (index < 0) {
        st.error = "the tag could not be made";
        return st;
    }
    std::vector<std::wstring> fresh;
    for (const auto& p : paths)
        if (!s.places.PathHasTag(p, index)) fresh.push_back(p);
    if (!fresh.empty() && !ToggleTagForSelection(s, id, fresh)) {
        if (created) DeleteTagAndRefreshViews(s, id);
        st.error = "these items cannot take tags";
        return st;
    }
    run.undo.push_back([id, fresh, created](AppState& st2) {
        if (!st2.places.FindTag(id)) return;
        if (!fresh.empty() && st2.places.GetSelectionState(id, fresh) == app::TagSelectionState::All)
            ToggleTagForSelection(st2, id, fresh);
        if (created && st2.places.PathsForTag(id).empty()) DeleteTagAndRefreshViews(st2, id);
    });
    st.ok = true;
    const std::wstring on = Fill(Text(I::AiCtlTagOn), L"{n}", Num(paths.size()));
    st.sub = created ? Fill(Text(I::AiCtlTagNewDone), L"{name}", name) + L" \u00B7 " + on : on;
    ++run.organize;
    run.organize_kinds.push_back(Text(I::AiCtlKindTag));
    AddLine(run, SplitLine(Fill(Text(created ? I::AiCtlLineTagNew : I::AiCtlLineTag), L"{n}", Num(paths.size())),
                           L"{name}", name));
    return st;
}

CtlStep CtlFavorite(AppState& s, CtlRun& run, const json::Value& act) {
    CtlStep st;
    st.icon = ui::AiIcon::Star;
    st.title = Text(I::AiCtlStepFavorite);
    std::vector<std::wstring> paths;
    if (act.find("items")) paths = CtlItems(s, act);
    else if (const std::wstring one = json::Wide(act.get_string("path")); !one.empty()) {
        std::wstring p = ResolveTaskDest(one);
        if (p.empty()) p = one;
        if (Exists(p)) paths.push_back(NormDir(p));
    } else if (const app::Tab* tab = ActiveTab(s); tab && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path))
        paths.push_back(tab->current_path);
    if (paths.empty()) {
        st.error = "nothing to add (pass full paths)";
        st.why = Text(I::AiActWhyNoItems);
        return st;
    }
    std::wstring names;
    for (size_t i = 0; i < paths.size() && i < 3; ++i) names += (names.empty() ? L"" : Text(I::AiTkListSep)) + PlaceLabel(paths[i]);
    if (paths.size() > 3) names += Fill(Text(I::AiCtlAndMore), L"{n}", Num(paths.size()));
    st.arg = names;
    std::vector<std::wstring> added;
    for (const auto& p : paths) {
        if (s.places.IsStarred(p)) continue;
        if (ToggleStarred(s, p, IsDirectory(p) ? app::PlaceItemKind::Folder : app::PlaceItemKind::File)) added.push_back(p);
    }
    if (!added.empty())
        run.undo.push_back([added](AppState& st2) {
            for (const auto& p : added)
                if (st2.places.IsStarred(p)) ToggleStarred(st2, p, app::PlaceItemKind::Unknown);
        });
    st.ok = true;
    st.sub = Text(I::AiCtlFavIn) + L" \u203A " + names;
    ++run.organize;
    run.organize_kinds.push_back(Text(I::AiCtlKindFav));
    AddLine(run, SplitLine(Text(I::AiCtlLineFav), L"{name}", names));
    return st;
}

CtlStep CtlMerge(AppState& s, CtlRun& run) {
    CtlStep st;
    st.icon = ui::AiIcon::Tiles;
    st.title = Text(I::AiCtlStepMerge);
    const size_t before = s.core.windows.Count();
    if (before < 2 || !MergeAllWindows(s)) {
        st.error = "there is only one window";
        st.why = Text(I::AiActWhyOneWindow);
        return st;
    }
    st.ok = true;
    st.sub = Fill(Text(I::AiCtlMerged), L"{n}", Num(before - s.core.windows.Count()));
    run.merged = true;
    PlainDone(run, SplitLine(st.sub, {}, {}));
    AddKeys(run, Text(I::AiCtlKeyMerge), KeysFor(app::KeyAction::MergeWindows));
    return st;
}

CtlStep CtlOverview(AppState& s, CtlRun& run) {
    CtlStep st;
    st.icon = ui::AiIcon::Tiles;
    st.title = Text(I::AiCtlStepOverview);
    if (!s.overview.open) ToggleWindowOverview(s);
    st.ok = true;
    st.sub = Text(I::AiCtlApplied);
    PlainDone(run, SplitLine(Text(I::AiCtlLineOverview), {}, {}));
    AddKeys(run, Text(I::AiCtlStepOverview), KeysFor(app::KeyAction::Overview));
    return st;
}

// ---- P3: steps beyond the third batch's ----------------------------------

// The caps of a shown shortcut ("Ctrl+Shift+O" → Ctrl / Shift / O).
std::vector<std::wstring> CapsOf(const std::wstring& shortcut) {
    std::vector<std::wstring> caps;
    size_t at = 0;
    while (at < shortcut.size()) {
        size_t end = shortcut.find(L'+', at + 1);   // "Ctrl++" keeps its plus
        if (end == std::wstring::npos) end = shortcut.size();
        caps.push_back(shortcut.substr(at, end - at));
        at = end + 1;
    }
    return caps;
}

// No key for it: the card says what to type in the command palette, one
// row for the whole call (也可以自己输入 >详情面板 >文件夹大小).
void AddPaletteKey(CtlRun& run, const std::wstring& title) {
    const std::wstring cap = L">" + title;
    for (auto& k : run.keys)
        if (k.palette) {
            if (k.caps.size() < 3 && std::find(k.caps.begin(), k.caps.end(), cap) == k.caps.end()) k.caps.push_back(cap);
            return;
        }
    if (run.keys.size() >= 4) return;
    ui::AiKeyRowView row{Text(I::AiActPalette), {cap}};
    row.palette = true;
    run.keys.push_back(std::move(row));
}

// select.items: P2's selector on one pane, like the user's own selection
// (not part of the undo). Waits for the listing first (CtlNext).
CtlStep CtlSelect(AppState& s, CtlRun& run, const json::Value& act) {
    CtlStep st;
    st.icon = ui::AiIcon::CheckCircle;
    st.title = Text(I::AiActStepSelect);
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    app::actions::Selector sel;
    std::string error;
    if (!app::actions::ParseSelector(act, app::actions::FileTimeTicks(ft), sel, error)) {
        st.error = error;
        st.why = Text(I::AiActWhyFailed);
        return st;
    }
    const app::actions::SelectorMatch m = app::actions::ResolveSelector(s, sel);
    const size_t index = m.pane ? PaneIndex(s, m.pane) : 0;
    const std::wstring pos = m.pane ? PanePos(s, index) : std::wstring();
    const app::Tab* tab = m.pane ? m.pane->ActiveTab() : nullptr;
    st.arg = !pos.empty() ? pos : PlaceLabel(tab ? tab->current_path : std::wstring());
    st.data = app::actions::ApplySelection(s, m);
    if (!m.reason.empty()) {
        st.error = m.reason;
        st.why = Text(m.reason == "loading" ? I::AiActWhyLoading
                      : m.reason == "no_match" || m.reason == "filter_hides" || m.reason == "unknown_tag"
                          ? I::AiActWhyNoMatch
                          : I::AiActWhyFailed);
        return st;
    }
    st.ok = true;
    const std::wstring items = Fill(Text(I::AiCtlItems), L"{n}", Num(m.matched.size()));
    st.sub = m.bytes > 0 ? items + L" \u00B7 " + AiChatFormatSize(m.bytes) : items;
    run.other = true;
    AddLine(run, pos.empty() ? SplitLine(Fill(Text(I::AiActLineSelect), L"{items}", items), items, items)
                             : SplitLine(Fill(Fill(Text(I::AiActLineSelectIn), L"{pane}", pos), L"{items}", items),
                                         items, items));
    return st;
}

CtlStep CtlSelectNone(AppState& s, CtlRun& run) {
    CtlStep st;
    st.icon = ui::AiIcon::CheckCircle;
    st.title = Text(I::ActSelectNone);
    app::Tab* tab = ActiveTab(s);
    st.ok = true;
    st.sub = Text(I::AiCtlApplied);
    if (!tab || tab->SelectedCount() == 0) {
        st.skip = true;
        return st;
    }
    tab->ClearSelection();
    run.other = true;
    AddLine(run, SplitLine(Text(I::AiActLineClear), {}, {}));
    return st;
}

// The file nav.open opens: a full path, a name in the open folder, or the
// one selected item.
std::wstring OpenTarget(AppState& s, const json::Value& act) {
    std::wstring p = json::Wide(act.get_string("path"));
    const app::Tab* tab = ActiveTab(s);
    if (p.empty()) {
        if (tab && tab->snapshot && tab->SelectedCount() == 1) {
            const auto sel = SelectedFullPaths(*tab);
            if (!sel.empty()) return sel.front();
        }
        return {};
    }
    if (const std::wstring known = ResolveTaskDest(p); !known.empty()) return known;
    if (Exists(p)) return p;
    if (tab && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path) &&
        p.find_first_of(L"\\/:") == std::wstring::npos) {
        const std::wstring in = NormDir(tab->current_path) + L"\\" + p;
        if (Exists(in)) return in;
    }
    return {};
}

std::wstring LeafName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

CtlStep CtlOpenFile(AppState& s, CtlRun& run, const json::Value& act) {
    CtlStep st;
    st.icon = ui::AiIcon::Open;
    st.title = Text(I::AiCtlStepOpen);
    const std::wstring path = OpenTarget(s, act);
    if (path.empty()) {
        st.arg = json::Wide(act.get_string("path"));
        st.error = "no such file (pass a full path, a name in the open folder, or select one item)";
        st.why = Text(I::AiActWhyNoFile);
        return st;
    }
    st.arg = LeafName(path);
    if (IsDirectory(path)) {
        json::Value go = json::Value::Object();
        go.set("path", Utf8(path));
        return CtlOpen(s, run, go);
    }
    OpenPath(s, path);
    st.ok = true;
    st.sub = Text(I::AiCtlApplied);
    run.other = run.touched = true;
    AddLine(run, SplitLine(Fill(Text(I::AiActLineOpened), L"{name}", st.arg), st.arg, st.arg));
    return st;
}

// file.new_folder with a name: made in the open folder at once (a free name:
// 报销 (2) when 报销 exists); 撤销 removes it while it is still empty.
CtlStep CtlNewFolder(AppState& s, CtlRun& run, const json::Value& act) {
    CtlStep st;
    st.icon = ui::AiIcon::FolderPlus;
    st.title = Text(I::NewFolder);
    std::wstring name = json::Wide(act.get_string("name"));
    while (!name.empty() && (std::iswspace(name.back()) || name.back() == L'.')) name.pop_back();
    while (!name.empty() && std::iswspace(name.front())) name.erase(0, 1);
    st.arg = name;
    const app::Tab* tab = ActiveTab(s);
    if (name.empty() || name.size() > 120 || name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
        st.error = "a folder name is needed (no \\ / : * ? \" < > |)";
        st.why = Text(I::AiActWhyFailed);
        return st;
    }
    if (!tab || tab->current_path.empty() || fs::IsVirtualPath(tab->current_path)) {
        st.error = "the active tab shows no folder";
        st.why = Text(I::ActWhyNotFolder);
        return st;
    }
    const std::wstring base = NormDir(tab->current_path) + L"\\";
    std::wstring made = name;
    for (int n = 2; Exists(base + made) && n < 100; ++n) made = name + L" (" + std::to_wstring(n) + L")";
    const std::wstring full = base + made;
    if (Exists(full) || !CreateDirectoryW(full.c_str(), nullptr)) {
        st.error = "the folder could not be made";
        st.why = Text(I::AiActWhyFailed);
        return st;
    }
    run.undo.push_back([full](AppState&) { RemoveDirectoryW(full.c_str()); });   // fails unless empty
    run.touched = true;
    st.ok = true;
    st.arg = made;
    st.sub = Text(I::AiCtlApplied);
    st.data = json::Value::Object();
    st.data.set("path", Utf8(full));
    run.other = true;
    AddLine(run, SplitLine(Fill(Text(I::AiActLineNewFolder), L"{name}", made), made, made));
    AddKeys(run, Text(I::NewFolder), KeysFor(app::KeyAction::NewFolder));
    return st;
}

// A setting's value from the model: a switch takes true / false, a choice
// its key (numbers such as 7 may come as numbers). -1: invalid.
int SettingIndex(const app::actions::SetDef& d, const json::Value* val) {
    int index = -1;
    if (app::actions::IsSwitch(d)) {
        if (val && val->is_bool()) index = val->as_bool() ? 1 : 0;
        else if (val && val->is_string()) {
            const std::string& t = val->as_string();
            index = t == "true" || t == "on" ? 1 : t == "false" || t == "off" ? 0 : -1;
        }
    } else if (val) {
        std::string key = val->is_string() ? val->as_string()
                          : val->is_number() ? std::to_string(static_cast<int>(val->as_number())) : std::string();
        if (key == "mica-alt") key = "mica_alt";
        for (size_t k = 0; k < d.choices.size(); ++k)
            if (d.choices[k].key && key == d.choices[k].key) index = static_cast<int>(k);
    }
    return index;
}

bool LockedSettingKey(std::string_view id) {
    return id == "assistant_permissions" || id == "always_allow" || id == "connections" || id == "api_key";
}

// settings.set among other steps: one line old → new; a call of only
// settings gets the settings card instead (ToolChangeSettings).
CtlStep CtlSetting(AppState& s, CtlRun& run, const json::Value& act) {
    CtlStep st;
    st.icon = ui::AiIcon::Gear;
    st.title = Text(I::AiSetStep);
    const std::string id = act.get_string("key", act.get_string("id"));
    st.arg = json::Wide(id);
    if (LockedSettingKey(id)) {
        run.locked = true;
        st.error = "locked: only the user can change this in Pulse's settings; Pulse showed them why";
        st.why = Text(I::AiActWhyLocked);
        return st;
    }
    const app::actions::SetDef* d = app::actions::FindSetting(id);
    if (!d) {
        st.error = "unknown setting";
        st.why = Text(I::AiActWhyUnknown);
        return st;
    }
    st.arg = Text(d->label);
    const int index = SettingIndex(*d, act.find("value"));
    if (index < 0) {
        st.error = "invalid value";
        st.why = Text(I::AiActWhyFailed);
        return st;
    }
    if (id == "confirm_delete" && index == 0) {
        run.locked = run.deny_delete = true;
        st.error = "locked: the delete confirmation can never be turned off by the assistant; Pulse showed the user why";
        st.why = Text(I::AiActWhyLocked);
        return st;
    }
    const int from = d->get(s);
    const int raw_theme = s.appPrefs.theme_mode;
    const int want = app::actions::IsSwitch(*d) ? index : d->choices[static_cast<size_t>(index)].value;
    st.ok = true;
    if (from == want) {
        st.skip = true;
        st.sub = app::actions::SettingText(*d, from);
        return st;
    }
    app::actions::ApplySetting(s, *d, index);
    const int to = d->get(s);
    if (to != want) {
        if (to != from) app::actions::ApplySetting(s, *d, app::actions::ChoiceIndex(*d, from));
        st.ok = false;
        st.error = "Pulse could not change it";
        st.why = Text(I::AiActWhyFailed);
        return st;
    }
    const std::wstring old_text = app::actions::SettingText(*d, from), new_text = app::actions::SettingText(*d, to);
    st.sub = old_text + L" \u2192 " + new_text;
    if (d->flags & app::actions::kSetSys) st.sub += L" \u00B7 " + Text(I::AiSetSys);
    st.data = json::Value::Object();
    st.data.set("from", Utf8(old_text));
    st.data.set("to", Utf8(new_text));
    if (d->flags & app::actions::kSetSys) st.data.set("affects_windows", true);
    run.undo.push_back([d, from, to, raw_theme](AppState& st2) {
        if (d->get(st2) != to) return;
        const int idx = app::actions::ChoiceIndex(*d, from);
        if (idx >= 0) app::actions::ApplySetting(st2, *d, idx);
        if (std::string_view(d->id) == "theme" && raw_theme < 0) {
            st2.appPrefs.theme_mode = raw_theme;   // the legacy session theme stays legacy
            st2.appPrefs.Save();
        }
    });
    run.other = true;
    const std::wstring label = Text(d->label);
    AddLine(run, SplitLine(Fill(Fill(Fill(Text(I::AiActLineValue), L"{title}", label), L"{from}", old_text), L"{to}",
                                new_text),
                           new_text, new_text));
    return st;
}

// Any other row of the action table: what its menu row or palette row does.
// "choice" picks one of its choices (title or position); "show" (or "on")
// sets an on / off row. A switch or a choice undoes; the rest do not.
CtlStep CtlGeneric(AppState& s, CtlRun& run, const std::string& id, const json::Value& act) {
    CtlStep st;
    st.icon = ui::AiIcon::Bolt;
    const auto views = app::actions::ListActions(s);
    const auto find = [&](const std::vector<app::actions::ActionView>& list) -> const app::actions::ActionView* {
        for (const auto& v : list)
            if (v.id == id && v.key.empty()) return &v;
        return nullptr;
    };
    const app::actions::ActionView* v = find(views);
    if (!v) {
        st.title = Display(id);
        st.error = "unknown action (find_actions lists them)";
        st.why = Text(I::AiActWhyUnknown);
        return st;
    }
    st.title = v->title;
    if (!v->enabled) {
        st.error = "not available now: " + Utf8(v->why);
        st.why = v->why;
        return st;
    }
    int choice = -1;
    if (const json::Value* c = act.find("choice")) {
        int64_t n = 0;
        if (c->get_int(n, 0, static_cast<int64_t>(v->choices.size()) - 1)) choice = static_cast<int>(n);
        else if (c->is_string()) {
            const std::wstring want = json::Wide(c->as_string());
            for (size_t i = 0; i < v->choices.size() && choice < 0; ++i)
                if (_wcsicmp(v->choices[i].text.c_str(), want.c_str()) == 0) choice = static_cast<int>(i);
            for (size_t i = 0; i < v->choices.size() && choice < 0; ++i)
                if (!want.empty() && v->choices[i].text.find(want) != std::wstring::npos) choice = static_cast<int>(i);
        }
        if (choice < 0) {
            st.error = "no such choice (find_actions lists them)";
            st.why = Text(I::AiActWhyFailed);
            return st;
        }
        st.arg = v->choices[static_cast<size_t>(choice)].text;
    } else if (!v->choices.empty()) {
        std::string labels;
        for (const auto& c2 : v->choices) labels += (labels.empty() ? "" : " | ") + Utf8(c2.text);
        st.error = "choose one with \"choice\": " + labels;
        st.why = Text(I::AiActWhyFailed);
        return st;
    }
    const std::wstring on = Text(I::AiSetOn), off = Text(I::AiSetOff);
    const bool is_switch = v->value == on || v->value == off;
    const json::Value* show = act.find("show");
    if (!show) show = act.find("on");
    if (is_switch && show && show->is_bool() && (v->value == on) == show->as_bool()) {
        st.ok = true;
        st.skip = true;
        st.sub = v->value;
        return st;
    }
    const std::wstring before = v->value, title = v->title, shortcut = v->shortcut;
    const int index = v->index;
    const auto old_choice = [&]() -> int {
        for (size_t i = 0; i < v->choices.size(); ++i)
            if (v->choices[i].radio) return static_cast<int>(i);
        return -1;
    }();
    if (!app::actions::RunAction(s, index, choice)) {
        st.error = "Pulse could not run it now";
        st.why = Text(I::AiActWhyFailed);
        return st;
    }
    const auto after_views = app::actions::ListActions(s);
    const app::actions::ActionView* now = find(after_views);
    const std::wstring after = now ? now->value : std::wstring();
    st.ok = true;
    if (!before.empty() && !after.empty() && before != after) {
        st.sub = before + L" \u2192 " + after;
        AddLine(run, SplitLine(Fill(Fill(Fill(Text(I::AiActLineValue), L"{title}", title), L"{from}", before), L"{to}", after),
                               after, after));
        st.data = json::Value::Object();
        st.data.set("from", Utf8(before));
        st.data.set("to", Utf8(after));
        if (is_switch)
            run.undo.push_back([index, id, after](AppState& st2) {
                for (const auto& w : app::actions::ListActions(st2))
                    if (w.index == index && w.id == id && w.value == after) app::actions::RunAction(st2, index);
            });
        else if (old_choice >= 0)
            run.undo.push_back([index, id, after, old_choice](AppState& st2) {
                for (const auto& w : app::actions::ListActions(st2))
                    if (w.index == index && w.id == id && w.value == after) app::actions::RunAction(st2, index, old_choice);
            });
    } else {
        st.sub = Text(I::AiCtlApplied);
        AddLine(run, SplitLine(st.arg.empty() ? title : title + L" \u00B7 " + st.arg, {}, {}));
    }
    run.other = true;
    if (!shortcut.empty()) AddKeys(run, title, CapsOf(shortcut));
    else AddPaletteKey(run, title);
    return st;
}

void CtlNext(AppState& s, std::shared_ptr<CtlRun> run);

void CtlFinish(AppState& s, std::shared_ptr<CtlRun> run) {
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    const size_t undoable = run->undo.size();
    if (AiBlockView* b = Block(s, a.steps_id)) {
        b->label = Fill(Text(I::AiCtlStepsLabel), L"{n}", Num(run->ok));
        if (run->window && !run->layout && !run->other && !run->organize)
            b->label = Fill(Text(I::AiCtlStepsWindows), L"{n}", Num(run->window_names.size()));
        else if ((run->layout || run->open) && !run->window && !run->other && !run->organize)
            b->label = Fill(Text(I::AiCtlStepsLayout), L"{n}", Num(run->ok));
        b->collapsed = true;
        b->open = false;
    }
    a.steps_id = 0;
    const bool failed = !run->fail_title.empty();
    ToolOutcome out;
    out.data = json::Value::Object();
    out.data.set("ok", run->ok > 0 && !failed);
    out.data.set("done", static_cast<uint64_t>(run->ok));
    if (failed) out.data.set("stopped", "at the step that failed; later steps were not run, earlier ones stay done");
    out.data.set("steps", std::move(run->report));
    if (run->locked) {
        // A locked setting: why, and where the user changes it (third batch).
        AiBlockView& e = c.Add(AiBlockKind::Error, Now());
        e.tone = ui::AiErrTone::Warn;
        e.icon = ui::AiIcon::Lock;
        e.title = Text(run->deny_delete ? I::AiSetDenyT : I::AiSetLockT);
        e.sub = Text(run->deny_delete ? I::AiSetDenyS : I::AiSetLockS);
        e.hint = Text(I::AiSetLockHint);
        e.hint_icon = ui::AiIcon::Shield;
        e.pills = {Text(I::AiSetPillPerm), Text(I::AiSetPillAllow), Text(I::AiSetPillConn)};
    }
    // Only a call that changed something gets a card (all "already so": the
    // model hears it, nothing to show or undo).
    const bool changed = !run->undo.empty() || run->merged || run->window || !run->lines.empty();
    if (run->ok > 0 && changed && !(failed && run->locked && run->lines.empty())) {
        AiBlockView& r = c.Add(AiBlockKind::Result, Now());
        const size_t things = run->organize + (run->other ? 1 : 0) + (run->window ? 1 : 0) + ((run->layout || run->open) ? 1 : 0);
        // The title: what the turn was about.
        if (failed) {
            // 做好了 1 件，1 件没做成: what was done, the step that failed, what was not done.
            r.partial = true;
            r.title = Fill(Fill(Text(I::AiActPartial), L"{ok}", Num(run->ok)), L"{bad}", Num(1));
            r.sub = Text(I::AiActPartialSub);
            r.lines = run->lines;
            ui::AiResultLine bad = SplitLine(Fill(Fill(Fill(Text(I::AiActFailLine), L"{title}", run->fail_title), L"{why}",
                                                       run->fail_why),
                                                  L"{arg}", run->fail_arg),
                                             run->fail_arg, run->fail_arg);
            bad.warn = true;
            r.lines.push_back(std::move(bad));
            for (const auto& t : run->not_run) {
                ui::AiResultLine ln = SplitLine(Fill(Text(I::AiActNotRun), L"{title}", t), {}, {});
                ln.mute = true;
                r.lines.push_back(std::move(ln));
            }
        } else if (run->window && !run->other && !run->organize) {
            r.title = Fill(Text(I::AiCtlResWindows), L"{n}", Num(run->window_names.size()));
            for (const auto& n : run->window_names) r.sub += (r.sub.empty() ? L"" : L" \u00B7 ") + n;
            if (run->single_back) r.sub += Text(I::AiCtlResMainSingle);
        } else if ((run->layout || run->open) && !run->other && !run->organize) {
            const app::LayoutPreset p = LayoutOf(s);
            const I t = p == app::LayoutPreset::Single          ? I::AiCtlResSingle
                        : p == app::LayoutPreset::TwoVertical   ? I::AiCtlResTwoV
                        : p == app::LayoutPreset::TwoHorizontal ? I::AiCtlResTwoH
                        : p == app::LayoutPreset::Three         ? I::AiCtlResThree
                        : p == app::LayoutPreset::FourGrid      ? I::AiCtlResFour
                                                                : I::AiCtlResLayout;
            r.title = run->layout ? Text(t) : Text(I::AiCtlResOpened);
            r.sub = PanesLine(s);
        } else if (things == 1 && run->lines.size() == 1) {
            r.title = run->single_title;
            if (run->organize) r.sub = Text(I::AiCtlResOrganizeSub);
        } else {
            r.lines = run->lines;
            if (run->layout) {
                // 布局 单栏 → 左右两栏：下载 | 文档, first: the rest happened in it.
                const app::LayoutPreset now_layout = LayoutOf(s);
                std::wstring folders;
                for (app::Pane* p : ShownPanes(s)) {
                    const app::Tab* t = p->ActiveTab();
                    folders += (folders.empty() ? L"" : L" | ") + PlaceLabel(t ? t->current_path : std::wstring());
                }
                const std::wstring to = PresetName(now_layout);
                const std::wstring from = run->has_first_layout && run->first_layout != now_layout
                                              ? std::wstring(PresetName(run->first_layout))
                                              : std::wstring();
                std::wstring text = Fill(Fill(Text(from.empty() ? I::AiActLineLayoutTo : I::AiActLineLayout), L"{from}", from),
                                         L"{panes}", folders);
                r.lines.insert(r.lines.begin(), SplitLine(Fill(text, L"{to}", to), to, to));
            }
            r.title = Fill(Text(I::AiCtlResThings), L"{n}",
                           Num(r.lines.size() + (run->window ? 1 : 0) + ((run->open && !run->layout) ? 1 : 0)));
            if (run->organize && !run->other && !run->window && !run->layout && !run->open) r.sub = Text(I::AiCtlResOrganizeSub);
            else if (!run->touched) r.sub = Text(I::AiActResSub);
        }
        // Keys only where they help: layout, windows, view; the organize
        // lines say enough.
        if (!failed && (run->layout || run->window || run->other)) r.keys = run->keys;
        if (!failed && run->window && run->keys.size() < 4) {
            ui::AiKeyRowView merge{Text(I::AiCtlKeyMerge), KeysFor(app::KeyAction::MergeWindows)};
            bool have = false;
            for (const auto& k : r.keys) have |= k.label == merge.label;
            if (!have && !merge.caps.empty()) r.keys.push_back(std::move(merge));
        }
        if (undoable) r.deny = Text(undoable > 1 && run->lines.size() > 1 ? I::AiCtlUndoAll : I::AiAsstUndo);
        if (run->window) {
            r.accept = Text(I::AiCtlOverview);
            r.accept_icon = ui::AiIcon::Tiles;
        }
        c.result_id = r.id;
        c.on_show = run->window ? std::function<void(AppState&)>([](AppState& st) {
            if (!st.overview.open) ToggleWindowOverview(st);
        })
                                : std::function<void(AppState&)>();
        if (undoable) {
            c.on_undo = [run](AppState& st) {
                HideRenameOverlay(st, true);   // a rename field must not stay over a moved pane
                for (auto it = run->undo.rbegin(); it != run->undo.rend(); ++it) (*it)(st);
                AiChat& c2 = Chat(st);
                c2.peek_undo = false;
                AiChatHidePeek(st);
                if (AiBlockView* rr = c2.Find(c2.result_id)) {
                    rr->done = true;
                    std::wstring t;
                    if (run->window) {
                        for (const auto& p : run->back_parts) t += (t.empty() ? L"" : Text(I::AiCtlBackSep)) + p;
                    } else if ((run->layout || run->open) && !run->other && !run->organize) {
                        t = run->has_first_layout ? Fill(Text(I::AiCtlUndoneLayout), L"{layout}", PresetName(run->first_layout))
                                                  : Text(I::AiCtlUndoneOpen);
                    } else if (run->organize && !run->other) {
                        std::wstring kinds;
                        for (size_t i = 0; i < run->organize_kinds.size(); ++i) {
                            if (std::find(run->organize_kinds.begin(), run->organize_kinds.begin() + static_cast<std::ptrdiff_t>(i),
                                          run->organize_kinds[i]) != run->organize_kinds.begin() + static_cast<std::ptrdiff_t>(i))
                                continue;
                            kinds += (kinds.empty() ? L"" : Text(I::AiTkListSep)) + run->organize_kinds[i];
                        }
                        t = Fill(Text(run->organize_kinds.size() > 1 ? I::AiCtlUndoneKinds : I::AiCtlUndoneKind), L"{kinds}", kinds);
                    } else {
                        t = Text(I::AiCtlUndoneAll);
                    }
                    rr->title = Text(I::AiAsstUndone);
                    rr->sub = t;   // the panel hides lines / keys / warn of a done card
                }
                AiBlockView& note = c2.Add(AiBlockKind::Note, Now());
                note.text = run->window && !run->other && !run->organize ? Text(I::AiCtlNoteWindows)
                            : (run->layout || run->open) && !run->other && !run->organize && !run->window
                                ? Text(I::AiCtlNoteLayout)
                            : run->undo_steps <= 1 ? Text(I::AiActNoteOne)
                                                   : Fill(Text(I::AiCtlNoteThings), L"{n}", Num(run->undo_steps));
                AiChatSetChip(c2, ui::StatusAiState::Done, Text(I::AiAsstUndone));
                if (c2.agent)
                    c2.agent->pending_note =
                        L"(The user clicked Undo in Pulse: the changes of the last run_actions call were reversed; a selection stays as it was.)";
                Redraw(st);
            };
        } else {
            c.on_undo = nullptr;
        }
        c.peek_text = r.title;
        c.peek_undo = undoable > 0;
        AiChatSetChip(c, ui::StatusAiState::Busy, r.title, 0.95f);
    }
    AddToolResult(a, run->call_id, out);
    ++a.call_index;
    NextTool(s);
}

// The step's action id; control_pulse's "do" names still work.
std::string StepAction(const json::Value& step) {
    std::string id = step.get_string("action");
    if (!id.empty()) return id;
    static const std::pair<const char*, const char*> kOld[] = {
        {"layout", "window.layout"},      {"open", "nav.go"},           {"new_window", "window.new"},
        {"tab_to_window", "tab.to_window"}, {"pane_to_window", "pane.to_window"}, {"merge_windows", "window.merge"},
        {"window_overview", "window.overview"}, {"view", "view.mode"},  {"sort", "view.sort"},
        {"group", "view.group"},          {"sidebar", "sidebar.toggle"}, {"favorites_bar", "favbar.toggle"},
        {"tray", "tray.add"},             {"tag", "tag.toggle"},        {"favorite", "fav.add"},
    };
    const std::string old = step.get_string("do");
    for (const auto& [from, to] : kOld)
        if (old == from) return to;
    return old;
}

// Its arguments: "args", or (control_pulse) the step itself.
const json::Value& StepArgs(const json::Value& step) {
    const json::Value* args = step.find("args");
    return args && args->is_object() ? *args : step;
}

// The title a step shows when it was not done (排序：没有做).
std::wstring StepTitle(AppState& s, const std::string& id) {
    if (id == "settings.set") return Text(I::AiSetStep);
    if (id == "nav.go") return Text(I::AiCtlStepOpen);
    for (const auto& v : app::actions::ListActions(s))
        if (v.id == id && v.key.empty()) return v.title;
    return Display(id);
}

CtlStep CtlRunStep(AppState& s, CtlRun& run, const std::string& id, const json::Value& act) {
    if (!app::actions::FindAction(id)) {
        CtlStep st;
        st.title = Display(id);
        st.error = "unknown action (find_actions lists them)";
        st.why = Text(I::AiActWhyUnknown);
        return st;
    }
    const app::actions::Policy policy = app::actions::AiPolicy(id);
    if (policy.gate == app::actions::Gate::Later || policy.gate == app::actions::Gate::Locked) {
        CtlStep st;
        st.title = StepTitle(s, id);
        st.icon = policy.gate == app::actions::Gate::Locked ? ui::AiIcon::Lock : ui::AiIcon::Move;
        st.error = policy.gate == app::actions::Gate::Later
                       ? "moving, renaming and deleting files is not done by run_actions: use organize_files, "
                         "rename_files, run_task, find_cleanup or find_duplicates (they show the user a plan)"
                       : "locked: only the user can do this";
        st.why = Text(policy.gate == app::actions::Gate::Later ? I::AiActWhyLater : I::AiActWhyLocked);
        return st;
    }
    if (id == "window.layout") return CtlLayout(s, run, act);
    if (id == "nav.go") return CtlOpen(s, run, act);
    if (id == "window.new") return CtlNewWindow(s, run, act);
    if (id == "tab.to_window") return CtlToWindow(s, run, act, false);
    if (id == "pane.to_window") return CtlToWindow(s, run, act, true);
    if (id == "window.merge") return CtlMerge(s, run);
    if (id == "window.overview") return CtlOverview(s, run);
    if (id == "view.mode") return CtlView(s, run, act);
    if (id == "view.sort") return CtlSort(s, run, act);
    if (id == "view.group") return CtlGroup(s, run, act);
    if (id == "sidebar.toggle") return CtlBar(s, run, act, false);
    if (id == "favbar.toggle") return CtlBar(s, run, act, true);
    if (id == "tray.add") return CtlTray(s, run, act);
    if (id == "tag.toggle") return CtlTag(s, run, act);
    if (id == "fav.add") return CtlFavorite(s, run, act);
    if (id == "select.items") return CtlSelect(s, run, act);
    if (id == "select.clear") return CtlSelectNone(s, run);
    if (id == "nav.open") return CtlOpenFile(s, run, act);
    if (id == "settings.set") return CtlSetting(s, run, act);
    if (id == "file.new_folder" && act.find("name")) return CtlNewFolder(s, run, act);
    if (id == "tab.new" && !act.get_string("path").empty()) {
        CtlStep st;
        st.icon = ui::AiIcon::Plus;
        st.title = Text(I::TooltipNewTab);
        const std::wstring path = CtlFolder(act.get_string("path"));
        st.arg = path.empty() ? json::Wide(act.get_string("path")) : PlaceLabel(path);
        if (path.empty()) {
            st.error = "no such folder";
            st.why = Text(I::AiActWhyNoFolder);
            return st;
        }
        OpenTabAt(s, path);
        st.ok = true;
        st.sub = Text(I::AiCtlApplied);
        run.other = true;
        AddLine(run, SplitLine(Fill(Text(I::AiActLineOpened), L"{name}", st.arg), st.arg, st.arg));
        AddKeys(run, Text(I::TooltipNewTab), KeysFor(app::KeyAction::NewTab));
        return st;
    }
    return CtlGeneric(s, run, id, act);
}

// A wait the step asks for ("wait"), or the listing a selection needs.
bool StepWait(const json::Value& step, const std::string& id, app::actions::WaitSpec& spec) {
    const json::Value& args = StepArgs(step);
    if (const json::Value* w = step.find("wait")) {
        json::Value obj = json::Value::Object();
        if (w->is_object()) obj = *w;
        else if (w->is_string()) obj.set("wait", w->as_string());
        if (!obj.find("pane"))
            if (const json::Value* pane = args.find("pane"); pane && !(pane->is_string() && pane->as_string() == "all"))
                obj.set("pane", *pane);
        std::string error;
        return app::actions::ParseWait(obj, spec, error);
    }
    if (id != "select.items") return false;
    spec = {};
    spec.kind = app::actions::WaitKind::ListingLoaded;
    spec.timeout_ms = 5000;
    int64_t n = 0;
    if (const json::Value* pane = args.find("pane")) {
        if (pane->is_string() && pane->as_string() == "target") spec.pane = -1;
        else if (pane->get_int(n, 1, 4)) spec.pane = static_cast<int>(n);
    }
    return true;
}

void CtlNext(AppState& s, std::shared_ptr<CtlRun> run) {
    AiAgent& a = Agent(s);
    if (a.generation != run->gen) return;
    while (run->index < run->queue.size()) {
        const json::Value step = run->queue[run->index];
        const std::string what = StepAction(step);
        if (run->waited != run->index) {
            run->waited = run->index;
            app::actions::WaitSpec spec;
            json::Value ready;
            if (StepWait(step, what, spec) && !app::actions::WaitReady(s, spec, ready)) {
                AppState* self = &s;
                app::actions::WaitFor(s, spec, [self, run] { return Agent(*self).generation != run->gen; },
                                      [run](AppState& s2, json::Value) { CtlNext(s2, run); });
                return;
            }
        }
        const size_t undo_before = run->undo.size();
        CtlStep st = CtlRunStep(s, *run, what, StepArgs(step));
        // A layout's folders are part of the layout line, not things of their own.
        if (run->undo.size() > undo_before && !(what == "nav.go" && run->layout)) ++run->undo_steps;
        if (!st.ok && st.why.empty()) st.why = Text(I::AiActWhyFailed);
        json::Value rep = json::Value::Object();
        rep.set("action", what);
        rep.set("ok", st.ok);
        if (!st.ok) rep.set("error", st.error);
        else if (st.wait) rep.set("result", Utf8(L"opened " + st.arg));
        else if (!st.sub.empty()) rep.set("result", Utf8(st.sub));
        if (st.skip) rep.set("already", true);   // nothing to do: it was so already
        if (!st.data.is_null())
            for (size_t k = 0; k < st.data.keys().size(); ++k) rep.set(st.data.keys()[k], st.data.items()[k]);
        run->report.push(std::move(rep));
        ++run->index;
        if (st.ok) ++run->ok;
        if (!st.ok) {
            // Stop here: later steps often build on this one.
            run->fail_title = st.title;
            run->fail_arg = st.arg;
            run->fail_why = st.why;
            json::Value skipped = json::Value::Array();
            for (size_t k = run->index; k < run->queue.size(); ++k) {
                const std::string id = StepAction(run->queue[k]);
                run->not_run.push_back(StepTitle(s, id));
                skipped.push(id);
            }
            if (skipped.size()) {
                json::Value rep2 = json::Value::Object();
                rep2.set("not_run", std::move(skipped));
                run->report.push(std::move(rep2));
            }
            run->index = run->queue.size();
        }
        if (st.skip) continue;
        AddStep(s, a, st.icon, st.title.empty() ? Display(what) : st.title, st.arg, {});
        AiChatSetChip(Chat(s), ui::StatusAiState::Busy, st.title,
                      static_cast<float>(run->index) / static_cast<float>(run->queue.size() + 1));
        Redraw(s);
        const uint64_t started = Now();
        AiChatPush(s, 450, [run, st, started](AppState& s2) {
            AiAgent& ag = Agent(s2);
            if (ag.generation != run->gen) return -1;
            std::wstring sub = st.ok ? st.sub : st.why;
            if (st.wait) {
                // The folder's item count once it has loaded (1.5 s at most).
                const app::Tab* t = PaneAlive(s2, st.wait) ? st.wait->ActiveTab() : nullptr;
                if (t && t->loading && Now() - started < 1500) return 60;
                sub = t && !t->loading ? Fill(Text(I::AiCtlItemsIn), L"{n}", Num(t->EntryCount())) : Text(I::AiCtlApplied);
            }
            EndStep(s2, ag, st.ok, sub);
            CtlNext(s2, run);
            return -1;
        });
        return;
    }
    CtlFinish(s, run);
}

void ToolChangeSettings(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args);

// A step that asks first: a program or script (nav.open), or a System action.
struct AskStep {
    size_t index = SIZE_MAX;
    std::string id;
    std::wstring target;   // nav.open: the file
    size_t count = 0;      // how many steps of the call ask
};

AskStep FirstAsk(AppState& s, const std::vector<json::Value>& queue) {
    AskStep ask;
    for (size_t i = 0; i < queue.size(); ++i) {
        const std::string id = StepAction(queue[i]);
        const std::wstring target = id == "nav.open" ? OpenTarget(s, StepArgs(queue[i])) : std::wstring();
        if (app::actions::AiPolicy(id, target).gate != app::actions::Gate::Confirm) continue;
        ++ask.count;
        if (ask.index != SIZE_MAX) continue;
        ask.index = i;
        ask.id = id;
        ask.target = target;
    }
    return ask;
}

void ToolRunActions(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
    AiAgent& a = Agent(s);
    auto run = std::make_shared<CtlRun>();
    run->call_id = call.id;
    run->gen = a.generation;
    if (const json::Value* v = args.find("steps"); v && v->is_array())
        for (size_t i = 0; i < v->size() && i < 12; ++i)
            if (v->at(i).is_object()) run->queue.push_back(v->at(i));
    if (run->queue.empty()) {
        AddToolResult(a, call.id, ToolOutcome::Fail("invalid_args", "steps is empty"));
        ++a.call_index;
        NextTool(s);
        return;
    }
    // Only settings: the settings card of the third batch (old -> new, 显示).
    bool only_settings = true;
    for (const auto& step : run->queue) only_settings = only_settings && StepAction(step) == "settings.set";
    if (only_settings) {
        json::Value changes = json::Value::Array();
        for (const auto& step : run->queue) {
            const json::Value& sa = StepArgs(step);
            json::Value ch = json::Value::Object();
            ch.set("id", sa.get_string("key", sa.get_string("id")));
            if (const json::Value* val = sa.find("value")) ch.set("value", *val);
            changes.push(std::move(ch));
        }
        json::Value wrapped = json::Value::Object();
        wrapped.set("changes", std::move(changes));
        ToolChangeSettings(s, call, wrapped);
        return;
    }
    HideRenameOverlay(s, true);   // panes and windows are about to move: end a rename like a click elsewhere
    FoldSteps(s, a);
    const AskStep ask = FirstAsk(s, run->queue);
    if (ask.index == SIZE_MAX) {
        CtlNext(s, run);
        return;
    }
    // One confirm card for the call, before any step runs.
    AiChat& ch = Chat(s);
    AiChatSetChip(ch, ui::StatusAiState::Waiting, Text(I::AiAsstChipWaiting));
    AiBlockView& cf = ch.Add(AiBlockKind::Confirm, Now());
    const bool program = ask.id == "nav.open";
    const std::wstring name = program ? LeafName(ask.target) : StepTitle(s, ask.id);
    cf.tag_amber = true;
    if (program) {
        cf.title = Text(I::AiActRunTitle);
        cf.tag = Text(I::AiActTagExternal);
        cf.sub = Text(I::AiActRunSub);
        cf.cf_icon = ui::AiIcon::Package;
        cf.file = name;
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        const size_t slash = ask.target.find_last_of(L"\\/");
        const std::wstring folder = slash == std::wstring::npos ? std::wstring() : FolderLabel(ask.target.substr(0, slash));
        if (GetFileAttributesExW(ask.target.c_str(), GetFileExInfoStandard, &fa))
            cf.file_note = AiChatFormatSize((static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow) +
                           (folder.empty() ? L"" : L" \u00B7 " + folder);
        cf.deny = Text(I::AiActRunDeny);
        cf.accept = Text(I::AiActRunAccept);
    } else {
        cf.title = Fill(Text(I::AiActSysTitle), L"{title}", name);
        cf.tag = Text(I::AiSetSys);
        cf.sub = Text(I::AiActSysSub);
        cf.cf_icon = ui::AiIcon::WinSettings;
        cf.deny = Text(I::AiAsstDeny);
        cf.accept = Text(I::AiActSysAccept);
    }
    cf.hint = Text(I::AiActConfirmHint);
    cf.hint_icon = ui::AiIcon::Shield;
    if (run->queue.size() > 1) {
        cf.hint1 = Fill(Text(I::AiActMoreSteps), L"{n}", Num(run->queue.size() - 1));
        cf.hint1_icon = ui::AiIcon::Tasks;
    }
    ch.confirm_id = cf.id;
    ch.on_confirm = [run, program, name](AppState& s3, bool accept) {
        AiChat& c3 = Chat(s3);
        if (AiBlockView* card = c3.Find(c3.confirm_id)) {
            card->done = true;
            card->title = accept ? Fill(Text(program ? I::AiActConfirmedRun : I::AiActConfirmedSys),
                                        program ? L"{name}" : L"{title}", name)
                                 : Fill(Text(program ? I::AiActDeniedRun : I::AiActDenied),
                                        program ? L"{name}" : L"{title}", name);
        }
        c3.confirm_id = 0;
        c3.on_confirm = nullptr;
        AiAgent& a3 = Agent(s3);
        if (a3.generation != run->gen) return;
        if (!accept) {
            AiChatSetChip(c3, ui::StatusAiState::Done, Text(I::AiActWhyDeclined));
            ToolOutcome out;
            out.data = json::Value::Object();
            out.data.set("ok", false);
            out.data.set("declined", true);
            out.data.set("message", "The user declined in Pulse; no step of this call was done.");
            AddToolResult(a3, run->call_id, out);
            ++a3.call_index;
            Redraw(s3);
            NextTool(s3);
            return;
        }
        AiChatSetChip(c3, ui::StatusAiState::Busy, Text(I::AiCtlApplied), 0.2f);
        CtlNext(s3, run);
    };
    Redraw(s);
}

// find_actions: the action table for what the common list lacks; domain
// "settings" lists every setting with its value (read_settings before P3).
json::Value SettingJson(const app::actions::SetDef& d, int v);

void ToolFindActions(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
    AiAgent& a = Agent(s);
    const std::string domain = args.get_string("domain");
    const std::wstring query = json::Wide(args.get_string("query"));
    const auto views = app::actions::ListActions(s);
    json::Value rows = json::Value::Array();
    std::wstring names;
    size_t found = 0;
    const auto add = [&](const app::actions::ActionView& v) {
        json::Value row = app::actions::ActionJson(v);
        if (!v.key.empty())
            if (const app::actions::SetDef* d = app::actions::FindSetting(v.key)) row.set("value", SettingJson(*d, d->get(s)));
        rows.push(std::move(row));
        if (found < 3) names += (names.empty() ? L"" : Text(I::AiTkListSep)) + v.title;
        ++found;
    };
    if (domain == "settings") {
        for (const auto& v : views)
            if (!v.key.empty()) add(v);
    } else {
        for (const app::actions::ActionView* v : app::actions::FindActions(views, query, 8)) add(*v);
    }
    ToolOutcome out;
    out.data = json::Value::Object();
    out.data.set("actions", std::move(rows));
    if (!found) out.data.set("note", "nothing matched; try other words (Chinese or English) or domain settings");
    AddStep(s, a, ui::AiIcon::Search, Text(I::AiActStepFind), domain == "settings" ? Text(I::AiSetStep) : query, {});
    AddToolResult(a, call.id, out);
    const std::wstring sub = !found       ? Text(I::AiActFoundNone)
                             : found > 3 ? Fill(Fill(Text(I::AiActFound), L"{n}", Num(found)), L"{names}",
                                                names + Text(I::AiTkListSep) + L"\u2026")
                                         : Fill(Fill(Text(I::AiActFound), L"{n}", Num(found)), L"{names}", names);
    const uint64_t gen = a.generation;
    AiChatPush(s, 350, [gen, sub, found](AppState& st) {
        AiAgent& ag = Agent(st);
        if (ag.generation != gen) return -1;
        EndStep(st, ag, found > 0, sub);
        ++ag.call_index;
        NextTool(st);
        return -1;
    });
    Redraw(s);
}

// ---- Third batch, flow S: Pulse's settings ----------------------------------
// The rows live in actions/action_settings.cpp (the command palette lists them too).
using app::actions::ApplySetting;
using app::actions::ChoiceIndex;
using app::actions::FindSetting;
using app::actions::IsSwitch;
using app::actions::kSetSys;
using app::actions::SetChoice;
using app::actions::SetDef;
using app::actions::SettingText;
using app::actions::SettingsPageTitle;

// The value as the model sees it.
json::Value SettingJson(const SetDef& d, int v) {
    if (IsSwitch(d)) return json::Value(v != 0);
    for (const SetChoice& c : d.choices)
        if (c.key && c.value == v) return json::Value(std::string(c.key));
    return json::Value(static_cast<double>(v));   // a value no choice has (an old row height)
}


struct SetChange {
    const SetDef* def = nullptr;
    int from = 0, to = 0;          // prefs values
    int theme_raw = 0;             // theme: prefs.theme_mode before (-1: the legacy session theme)
};

void ToolChangeSettings(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    json::Value report = json::Value::Array();
    std::vector<SetChange> done;
    bool deny_delete = false, deny_own = false;
    const json::Value* list = args.find("changes");
    if (!list || !list->is_array() || list->size() == 0) {
        AddToolResult(a, call.id, ToolOutcome::Fail("invalid_args", "changes is empty"));
        ++a.call_index;
        NextTool(s);
        return;
    }
    HideRenameOverlay(s, true);
    FoldSteps(s, a);
    for (size_t i = 0; i < list->size() && i < 24; ++i) {
        const json::Value& ch = list->at(i);
        if (!ch.is_object()) continue;
        const json::Value* idv = ch.find("id");
        const json::Value* val = ch.find("value");
        const std::string id = idv && idv->is_string() ? idv->as_string() : std::string();
        json::Value rep = json::Value::Object();
        rep.set("id", id);
        auto fail = [&](const char* why) {
            rep.set("ok", false);
            rep.set("error", why);
            report.push(std::move(rep));
        };
        if (id == "assistant_permissions" || id == "always_allow" || id == "connections" || id == "api_key") {
            deny_own = true;
            fail("locked: only the user can change this in Pulse's settings; Pulse showed them why");
            continue;
        }
        const SetDef* d = FindSetting(id);
        if (!d) { fail("unknown setting"); continue; }
        // The value: a switch takes true / false, a choice its key (numbers
        // such as 7 may come as numbers).
        int index = -1;
        if (IsSwitch(*d)) {
            if (val && val->is_bool()) index = val->as_bool() ? 1 : 0;
            else if (val && val->is_string()) {
                const std::string& t = val->as_string();
                index = t == "true" || t == "on" ? 1 : t == "false" || t == "off" ? 0 : -1;
            }
        } else if (val) {
            std::string key = val->is_string() ? val->as_string()
                              : val->is_number() ? std::to_string(static_cast<int>(val->as_number())) : std::string();
            if (key == "mica-alt") key = "mica_alt";
            for (size_t k = 0; k < d->choices.size(); ++k)
                if (d->choices[k].key && key == d->choices[k].key) index = static_cast<int>(k);
        }
        if (index < 0) { fail("invalid value"); continue; }
        if (id == "confirm_delete" && index == 0) {
            deny_delete = true;
            fail("locked: the delete confirmation can never be turned off by the assistant; Pulse showed the user why");
            continue;
        }
        SetChange sc;
        sc.def = d;
        sc.from = d->get(s);
        sc.theme_raw = s.appPrefs.theme_mode;
        const int want = IsSwitch(*d) ? index : d->choices[static_cast<size_t>(index)].value;
        if (sc.from == want) {
            rep.set("ok", true);
            rep.set("already", true);
            rep.set("value", SettingJson(*d, want));
            report.push(std::move(rep));
            continue;
        }
        ApplySetting(s, *d, index);
        sc.to = d->get(s);
        if (sc.to != want) {
            // The setter refused (a hidden prerequisite, a failed registry write).
            if (sc.to != sc.from) ApplySetting(s, *d, ChoiceIndex(*d, sc.from));
            fail("Pulse could not change it");
            continue;
        }
        rep.set("ok", true);
        rep.set("from", SettingJson(*d, sc.from));
        rep.set("to", SettingJson(*d, sc.to));
        if (d->flags & kSetSys) rep.set("affects_windows", true);
        report.push(std::move(rep));
        done.push_back(sc);
    }
    ToolOutcome out;
    out.data = json::Value::Object();
    out.data.set("ok", !done.empty());
    out.data.set("changes", std::move(report));
    if (deny_delete || deny_own) {
        AiBlockView& e = c.Add(AiBlockKind::Error, Now());
        e.tone = ui::AiErrTone::Warn;
        e.icon = ui::AiIcon::Lock;
        e.title = Text(deny_delete ? I::AiSetDenyT : I::AiSetLockT);
        e.sub = Text(deny_delete ? I::AiSetDenyS : I::AiSetLockS);
        e.hint = Text(I::AiSetLockHint);
        e.hint_icon = ui::AiIcon::Shield;
        e.pills = {Text(I::AiSetPillPerm), Text(I::AiSetPillAllow), Text(I::AiSetPillConn)};
    }
    if (done.empty()) {
        AddToolResult(a, call.id, out);
        ++a.call_index;
        Redraw(s);
        NextTool(s);
        return;
    }
    // The pages, in the order of the changes.
    std::vector<int> pages;
    for (const SetChange& sc : done)
        if (std::find(pages.begin(), pages.end(), sc.def->page) == pages.end()) pages.push_back(sc.def->page);
    std::wstring page_names;
    for (int p : pages) page_names += (page_names.empty() ? L"" : Text(I::AiTkListSep)) + SettingsPageTitle(p);
    std::vector<std::wstring> marks;
    for (const SetChange& sc : done) marks.push_back(Text(sc.def->label));
    s.settings.SetAssistantMarks(marks);
    AddStep(s, a, ui::AiIcon::Gear, Text(I::AiSetStep),
            Fill(Fill(Text(I::AiSetStepArg), L"{page}", page_names), L"{n}", Num(done.size())), {});
    AiChatSetChip(c, ui::StatusAiState::Busy, Text(I::AiSetStep), 0.5f);
    Redraw(s);
    const uint64_t gen = a.generation;
    const std::string call_id = call.id;
    auto changes = std::make_shared<std::vector<SetChange>>(std::move(done));
    AiChatPush(s, 350, [gen, call_id, changes, pages, page_names, out](AppState& s2) mutable {
        AiAgent& ag = Agent(s2);
        if (ag.generation != gen) return -1;
        EndStep(s2, ag, true, Text(I::AiSetApplied));
        const std::wstring title =
            changes->size() == 1 ? Text(I::AiSetResTitle1) : Fill(Text(I::AiSetResTitle), L"{n}", Num(changes->size()));
        // The folded steps say what the turn did, like the card (改了 2 项设置).
        const auto steps_id = ag.steps_id;
        FoldSteps(s2, ag);
        if (AiBlockView* sb = Block(s2, steps_id)) sb->label = title;
        AiChat& c2 = Chat(s2);
        AiBlockView& r = c2.Add(AiBlockKind::Result, Now());
        r.title = title;
        r.sub = Fill(Text(I::AiSetResSub), L"{page}", page_names);
        bool fm_on = false;
        for (const SetChange& sc : *changes) {
            ui::AiChangeView v;
            v.name = Text(sc.def->label);
            v.from = SettingText(*sc.def, sc.from);
            v.to = SettingText(*sc.def, sc.to);
            if (sc.def->flags & kSetSys) v.tag = Text(I::AiSetSys);
            r.changes.push_back(std::move(v));
            fm_on |= std::string_view(sc.def->id) == "open_folders_with_pulse" && sc.to != 0;
        }
        if (fm_on) r.warn = Text(I::AiSetWarnFm);
        r.deny = Text(I::AiAsstUndo);
        r.accept = Text(I::AiSetShow);
        r.accept_icon = ui::AiIcon::Gear;
        c2.result_id = r.id;
        const int first_page = pages.front();
        c2.on_show = [first_page](AppState& st) { OpenSettingsTab(st, first_page); };
        c2.on_undo = [changes](AppState& st) {
            bool fm_back = false;
            for (auto it = changes->rbegin(); it != changes->rend(); ++it) {
                const SetDef& d = *it->def;
                const int now = d.get(st);
                if (now == it->from) continue;
                const int idx = ChoiceIndex(d, it->from);
                if (idx >= 0) ApplySetting(st, d, idx);
                else {
                    // A size off the choices (a custom row height): put the number back.
                    const std::string_view id = d.id;
                    if (id == "row_height") st.appPrefs.row_height = it->from;
                    else if (id == "font_size") st.appPrefs.ui_font_scale = it->from;
                    else if (id == "staging_icon_size") st.appPrefs.tray_icon_size = it->from;
                    st.appPrefs.Save();
                    ApplySettingsEffects(st, app::SettingsEffect::RowHeight);
                    ApplySettingsEffects(st, app::SettingsEffect::TrayDeckIcon);
                }
                if (std::string_view(d.id) == "theme" && it->theme_raw < 0) {
                    st.appPrefs.theme_mode = it->theme_raw;   // the legacy session theme stays legacy
                    st.appPrefs.Save();
                }
                fm_back |= std::string_view(d.id) == "open_folders_with_pulse" && it->to != 0;
            }
            st.settings.SetAssistantMarks({});
            AiChat& c3 = Chat(st);
            c3.peek_undo = false;
            AiChatHidePeek(st);
            if (AiBlockView* rr = c3.Find(c3.result_id)) {
                rr->done = true;
                rr->title = Text(I::AiAsstUndone);
                std::wstring t = changes->size() == 1 ? Text(I::AiSetUndone1)
                                 : changes->size() == 2 ? Text(I::AiSetUndone2)
                                                        : Fill(Text(I::AiSetUndoneN), L"{n}", Num(changes->size()));
                if (fm_back) t += Text(I::AiSetUndoneFm);
                rr->sub = t;
            }
            AiBlockView& note = c3.Add(AiBlockKind::Note, Now());
            note.text = Text(I::AiSetNote);
            AiChatSetChip(c3, ui::StatusAiState::Done, Text(I::AiAsstUndone));
            if (c3.agent)
                c3.agent->pending_note =
                    L"(The user clicked Undo in Pulse: every setting of the last run_actions call is back as it was.)";
            Redraw(st);
        };
        AddToolResult(ag, call_id, out);
        ++ag.call_index;
        Redraw(s2);
        NextTool(s2);
        return -1;
    });
}

void NextTool(AppState& s) {
    AiAgent& a = Agent(s);
    if (a.call_index >= a.calls.size()) {
        a.calls.clear();
        a.call_index = 0;
        SendRound(s);
        return;
    }
    const app::ai::ModelToolCall& call = a.calls[a.call_index];
    ++a.tool_count;
    json::Value args;
    if (!json::Parse(call.arguments, args) || !args.is_object()) {
        AddToolResult(a, call.id, ToolOutcome::Fail("invalid_args", "arguments are not a JSON object"));
        ++a.call_index;
        NextTool(s);
        return;
    }
    if (call.name == "get_state") ToolGetState(s, call);
    else if (call.name == "search_files") ToolSearch(s, call, args);
    else if (call.name == "list_folder") ToolListFolder(s, call, std::move(args));
    else if (call.name == "get_properties") ToolGetProperties(s, call, args);
    else if (call.name == "organize_files") ToolOrganize(s, call, args);
    else if (call.name == "rename_files") ToolRename(s, call, args);
    else if (call.name == "run_task") ToolTask(s, call, args);
    else if (call.name == "disk_usage") ToolDiskUsage(s, call, args);
    else if (call.name == "find_cleanup") ToolFindCleanup(s, call, args);
    else if (call.name == "find_duplicates") ToolFindDuplicates(s, call, args);
    else if (call.name == "find_actions") ToolFindActions(s, call, args);
    else if (call.name == "run_actions") ToolRunActions(s, call, args);
    else {
        AddToolResult(a, call.id, ToolOutcome::Fail("unknown_tool", "no tool named " + call.name));
        ++a.call_index;
        NextTool(s);
    }
    Redraw(s);
}

// ---------------------------------------------------------------- stream --

// Error card A (plans/ai/Pulse_AI助手_错误与快捷键.html): red needs the user,
// amber passes on its own. "DeepSeek" only for the official service; a custom
// address is named by its host.
using ui::AiErrAct;
using ui::AiErrTone;
using ui::AiIcon;

ui::AiErrButton Btn(AiErrAct act, AiIcon icon, I text, bool primary) {
    ui::AiErrButton b;
    b.act = act;
    b.icon = icon;
    b.text = Text(text);
    b.primary = primary;
    return b;
}

bool Official() { return AiAgentBaseUrl() == AiAgentDefaultBaseUrl(); }
std::wstring ServiceName() { return Official() ? std::wstring(L"DeepSeek") : AiAgentHost(); }

AiChatError CutError() {
    AiChatError e;
    e.cut = true;
    e.tone = AiErrTone::Warn;
    e.icon = AiIcon::Cloud;
    e.title = Text(I::AiErrCutT);
    e.sub = Text(I::AiErrEarlyS);
    e.buttons = {Btn(AiErrAct::Retry, AiIcon::Retry, I::AiAsstRetry, true)};
    return e;
}

AiChatError BuildError(const ModelEvent& ev) {
    AiChatError e;
    const std::wstring name = ServiceName();
    const ui::AiErrButton retry = Btn(AiErrAct::Retry, AiIcon::Retry, I::AiAsstRetry, false);
    const ui::AiErrButton retry_pri = Btn(AiErrAct::Retry, AiIcon::Retry, I::AiAsstRetry, true);
    switch (ev.http_status) {
    case 401:
    case 403:
        e.tone = AiErrTone::Err;
        e.icon = AiIcon::Key;
        e.title = Text(I::AiErrAuthT);
        e.sub = Fill(Text(I::AiErrAuthS), L"{name}", name);
        e.buttons = {Btn(AiErrAct::ManageConn, AiIcon::Gear, I::AiErrBtnManage, true), retry};
        return e;
    case 402:
        e.tone = AiErrTone::Err;
        e.icon = AiIcon::Wallet;
        e.peek_title = Text(I::AiErrBalanceT);
        if (Official()) {
            e.title = Text(I::AiErrBalanceOfficialT);
            e.sub = Text(I::AiErrBalanceS);
            e.buttons = {Btn(AiErrAct::TopUp, AiIcon::Open, I::AiErrBtnTopUp, true), retry};
        } else {
            e.title = Text(I::AiErrBalanceT);
            e.sub = Fill(Text(I::AiErrBalanceHostS), L"{host}", AiAgentHost());
            e.buttons = {retry_pri};
        }
        return e;
    case 429:
        // No automatic retry: it would only keep sending refused requests.
        e.tone = AiErrTone::Warn;
        e.icon = AiIcon::Clock;
        e.title = Text(I::AiErrRateT);
        e.sub = Fill(Text(I::AiErrRateS), L"{name}", name);
        e.buttons = {retry_pri};
        return e;
    case 0:
        if (ev.error_type == "incomplete") return CutError();
        e.tone = AiErrTone::Warn;
        e.icon = AiIcon::WifiOff;
        e.title = Fill(Text(I::AiErrNetT), L"{host}", AiAgentHost());
        e.sub = Fill(Text(I::AiErrNetS), L"{code}", std::to_wstring(ev.win32_error));
        e.peek_title = Text(I::AiPeekNet);
        e.buttons = {retry_pri, Btn(AiErrAct::ManageConn, AiIcon::Gear, I::AiErrBtnManage, false)};
        return e;
    default:
        break;
    }
    const std::wstring code = std::to_wstring(ev.http_status);
    if (ev.http_status >= 500) {
        e.tone = AiErrTone::Warn;
        e.icon = AiIcon::Cloud;
        e.title = Fill(Fill(Text(I::AiErrServerT), L"{name}", name), L"{code}", code);
        e.sub = Text(I::AiErrServerS);
        e.peek_title = Text(I::AiPeekServer);
        e.buttons = {retry_pri};
        return e;
    }
    // Refused (400, 404, 422 …): the service's words in the grey box.
    std::wstring detail = json::Wide(ev.error_message);
    if (detail.size() > 300) detail = detail.substr(0, 300) + L"\u2026";
    e.tone = AiErrTone::Err;
    e.icon = AiIcon::Ban;
    e.title = Fill(Text(I::AiErrBadT), L"{code}", code);
    if (!detail.empty()) e.sub = Text(I::AiErrBadS);
    e.detail = detail;
    if (ev.http_status == 404) e.hint = Text(I::AiErrBadHint404);
    else if (ev.http_status == 400 || ev.http_status == 422) e.hint = Text(I::AiErrBadHint);
    e.buttons = {Btn(AiErrAct::SwitchModel, AiIcon::Swap, I::AiErrBtnSwitch, true), retry};
    if (!detail.empty()) e.buttons.push_back(Btn(AiErrAct::CopyDetail, AiIcon::Copy, I::AiErrBtnCopy, false));
    return e;
}

AiChatError TooManyError() {
    AiChatError e;
    e.tone = AiErrTone::Warn;
    e.icon = AiIcon::AlertCircle;
    e.title = Text(I::AiErrTooManyT);
    e.sub = Text(I::AiErrTooManyS);
    e.buttons = {Btn(AiErrAct::Retry, AiIcon::Retry, I::AiAsstRetry, true)};
    return e;
}

AiChatError NoKeyError() {
    // Not an error: accent, one button.
    AiChatError e;
    e.tone = AiErrTone::Accent;
    e.icon = AiIcon::Plug;
    e.title = Text(I::AiErrNoKeyT);
    e.sub = Text(I::AiErrNoKeyS);
    e.buttons = {Btn(AiErrAct::Connect, AiIcon::Plug, I::AiErrBtnConnect, true)};
    return e;
}

void EndTurn(AppState& s) {
    AiAgent& a = Agent(s);
    FoldThinking(s, a);
    FoldSteps(s, a);
    a.stream.reset();
    const bool searched = a.pending_query != nullptr;
    const size_t found = a.pending_query ? a.pending_query->count : 0;
    AddQueryCard(s, a);
    AiChatAddActs(s, a.tool_count);
    AiChatFinish(s, true);
    if (searched) AiChatSetChip(Chat(s), ui::StatusAiState::Done, Fill(Text(I::AiQChipFound), L"{n}", Num(found)));
}

void Fail(AppState& s, AiChatError error) {
    AiAgent& a = Agent(s);
    FoldThinking(s, a);
    if (AiBlockView* t = Block(s, a.text_id)) t->streaming = false;
    a.text_id = 0;
    // The model never saw a complete turn: forget it so a retry starts clean
    // (an organize that already ran is remembered through the note).
    if (a.history.size() > a.turn_history) {
        json::Value kept = json::Value::Array();
        for (size_t i = 0; i < a.turn_history; ++i) kept.push(a.history.at(i));
        a.history = std::move(kept);
    }
    if (a.organized && a.pending_note.empty())
        a.pending_note = L"(Earlier in this conversation organize_files ran and moved items; the turn then failed.)";
    for (AiBlockView& b : Chat(s).blocks)
        for (ui::AiStepView& st : b.steps)
            if (st.state == ui::AiStepState::Running) { st.state = ui::AiStepState::Failed; st.progress = -1.0f; }
    FoldSteps(s, a);
    a.stream.reset();
    a.pending_query.reset();
    // The card ends the turn; no acts row under a failed one.
    AiChatFail(s, std::move(error));
}

// One drained event batch. Returns false when the round ended.
bool Pump(AppState& s) {
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    if (!a.stream) return false;
    std::vector<ModelEvent> events;
    a.stream->Drain(events);
    for (ModelEvent& ev : events) {
        switch (ev.kind) {
        case ModelEvent::Kind::Reasoning:
            a.reasoning += ev.text;
            if (AiBlockView* b = Block(s, a.think_id)) b->text = json::Wide(a.reasoning);
            break;
        case ModelEvent::Kind::Content: {
            if (a.think_id) FoldThinking(s, a);
            FoldSteps(s, a);
            a.content += ev.text;
            AiBlockView* t = Block(s, a.text_id);
            if (!t) {
                AiBlockView& nt = c.Add(AiBlockKind::Text, Now());
                nt.streaming = true;
                a.text_id = nt.id;
                t = &nt;
            }
            t->text = Display(a.content);
            break;
        }
        case ModelEvent::Kind::Finished: {
            FoldThinking(s, a);
            if (AiBlockView* t = Block(s, a.text_id)) {
                t->streaming = false;
                t->text = Display(a.content);
            }
            a.text_id = 0;
            json::Value msg = json::Value::Object();
            msg.set("role", "assistant");
            msg.set("content", ev.content);
            if (!ev.reasoning.empty()) msg.set("reasoning_content", ev.reasoning);
            if (!ev.tool_calls.empty()) {
                json::Value calls = json::Value::Array();
                for (const auto& tc : ev.tool_calls) {
                    json::Value call = json::Value::Object();
                    call.set("id", tc.id);
                    call.set("type", "function");
                    json::Value fn = json::Value::Object();
                    fn.set("name", tc.name);
                    fn.set("arguments", tc.arguments);
                    call.set("function", std::move(fn));
                    calls.push(std::move(call));
                }
                msg.set("tool_calls", std::move(calls));
            }
            a.history.push(std::move(msg));
            a.stream.reset();
            if (ev.tool_calls.empty()) {
                EndTurn(s);
            } else {
                a.calls = std::move(ev.tool_calls);
                a.call_index = 0;
                NextTool(s);
            }
            return false;
        }
        case ModelEvent::Kind::Failed:
            Fail(s, BuildError(ev));
            return false;
        }
    }
    if (a.stream && a.stream->Ended()) {
        Fail(s, CutError());
        return false;
    }
    return true;
}

void SendRound(AppState& s) {
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    if (++a.rounds > kMaxRounds) {
        Fail(s, TooManyError());
        return;
    }
    std::wstring key;
    if (!app::ai::LoadModelKey(key)) {
        Fail(s, NoKeyError());
        return;
    }
    a.reasoning.clear();
    a.content.clear();
    a.text_id = 0;
    if (s.aiAssistant.effort > 0) {
        AiBlockView& b = c.Add(AiBlockKind::Thinking, Now());
        b.running = true;
        b.label = Text(I::AiAsstThinking);
        b.meta = L"\u00B7 " + AiChatEffortName(s);
        a.think_id = b.id;
        c.think_id = b.id;
        c.think_start = Now();
    }
    app::ai::ModelEndpoint endpoint;
    endpoint.base_url = AiAgentBaseUrl();
    endpoint.key = std::move(key);
    a.stream = app::ai::ModelStream::Start(std::move(endpoint), Body(s, a));
    const uint64_t gen = a.generation;
    AiChatPush(s, 30, [gen](AppState& st) {
        if (Agent(st).generation != gen) return -1;
        const bool more = Pump(st);
        Redraw(st);
        return more ? 30 : -1;
    });
}

} // namespace

// ----------------------------------------------------------------- public --

namespace {
uint64_t g_key_checked = 0;
bool g_key_present = false;
bool g_base_loaded = false;
std::wstring g_base_url;
} // namespace

bool AiAgentConnected() {
    const uint64_t now = GetTickCount64();
    if (!g_key_checked || now - g_key_checked > 2000) {
        g_key_present = app::ai::HasModelKey();
        g_key_checked = now;
    }
    return g_key_present;
}

void AiAgentKeyChanged() {
    g_key_checked = 0;
    g_base_loaded = false;
}

std::wstring AiAgentBaseUrl() {
    if (!g_base_loaded) {
        g_base_loaded = true;
        const std::string url = app::ai::LoadSettings().assistant.base_url;
        g_base_url = url.empty() ? std::wstring(kBaseUrl) : json::Wide(url);
    }
    return g_base_url;
}

std::wstring AiAgentDefaultBaseUrl() { return kBaseUrl; }

std::wstring AiAgentHost() { return app::ai::ModelHost(AiAgentBaseUrl()); }

void AiAgentStart(AppState& s, const std::wstring& prompt, bool with_context) {
    AiAgent& a = Agent(s);
    ++a.generation;
    a.turn_history = a.history.size();
    a.rounds = 0;
    a.tool_count = 0;
    a.organized = false;
    a.think_id = a.text_id = a.steps_id = 0;
    a.read_folder.clear();
    a.read_files = 0;
    a.pending_query.reset();
    a.searched = 0;
    a.did_search = false;
    a.calls.clear();
    a.call_index = 0;
    std::wstring content = prompt;
    if (with_context) {
        if (app::Tab* tab = ActiveTab(s); tab && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path)) {
            content += L"\n\n[Pulse: the open folder is " + tab->current_path + L" (\u201C" +
                       FolderLabel(tab->current_path) + L"\u201D, " + Num(tab->EntryCount()) + L" items)]";
        }
    }
    if (!a.pending_note.empty()) {
        content = a.pending_note + L"\n\n" + content;
        a.pending_note.clear();
    }
    json::Value user = json::Value::Object();
    user.set("role", "user");
    user.set("content", Utf8(content));
    a.history.push(std::move(user));
    SendRound(s);
}

void AiAgentStop(AppState& s) {
    AiChat& c = Chat(s);
    if (!c.agent) return;
    AiAgent& a = *c.agent;
    ++a.generation;
    if (a.stream) a.stream->Cancel();
    a.stream.reset();
    if (a.job) a.job->cancelled.store(true);
    a.job.reset();
    if (g_flush_sort) {
        auto flush = std::move(g_flush_sort);
        g_flush_sort = nullptr;
        flush(s);
    }
    EndOverlay(400);
    if (a.history.size() > a.turn_history) {
        json::Value kept = json::Value::Array();
        for (size_t i = 0; i < a.turn_history; ++i) kept.push(a.history.at(i));
        a.history = std::move(kept);
    }
    if (a.organized && a.pending_note.empty())
        a.pending_note = L"(Earlier in this conversation organize_files moved items before the user stopped the turn.)";
    a.think_id = a.text_id = a.steps_id = 0;
    a.pending_query.reset();
    a.calls.clear();
    a.call_index = 0;
}

void AiListFlashRows(const std::wstring& folder, const std::vector<std::wstring>& names) {
    if (names.empty() || !ui::motion::SystemAnimationsEnabled()) return;
    // The assistant is busy in another folder: its overlay wins.
    if (g_list && !SameDir(g_list->folder, folder) && !g_list->end_ms) return;
    const bool live = g_list && SameDir(g_list->folder, folder) && !g_list->end_ms;
    const uint64_t prev_end = g_list && SameDir(g_list->folder, folder) ? g_list->end_ms : 0;
    ui::AiListOverlay& o = ListOverlay(folder);
    const uint64_t start = ui::motion::NowMs() + 120;   // after the dialog's exit
    uint64_t at = start;
    for (const std::wstring& name : names) {
        o.flash[ui::AiListLower(name)] = at;
        at += 30;
    }
    // ListOverlay reopened it: close again once the last flash is over, but
    // an overlay the assistant is still using stays open ended.
    if (!live) o.end_ms = (std::max)(prev_end, at + 1500);
}

std::shared_ptr<const ui::AiListOverlay> AiListOverlayFor(const std::wstring& folder) {
    if (!g_list || !SameDir(g_list->folder, folder)) return nullptr;
    if (g_list->end_ms && ui::motion::NowMs() >= g_list->end_ms) {
        g_list.reset();
        return nullptr;
    }
    return g_list;
}

bool AiAssistantOwnsTask(uint64_t task_id) { return task_id != 0 && g_task_ops.count(task_id) != 0; }

void AiAssistantRefreshLists(AppState& s) {
    std::vector<std::wstring> paths;
    ForEachPane(s, [&](app::Pane& pane) {
        const app::Tab* tab = pane.ActiveTab();
        std::wstring kind, rest;
        if (tab && app::ParsePulsePath(tab->current_path, &kind, &rest) && kind == L"ai-list" &&
            std::find(paths.begin(), paths.end(), tab->current_path) == paths.end())
            paths.push_back(tab->current_path);
    });
    for (const std::wstring& path : paths) RefreshPath(s, path);
}

} // namespace pulse
