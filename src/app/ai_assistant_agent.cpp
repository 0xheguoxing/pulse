// ai_assistant_agent.cpp - Pulse 助手's agent loop on DeepSeek.
//
// One user turn is a series of rounds: stream a chat completion (thinking,
// then content or tool calls), run the tool calls in order, send their
// results back, until the model answers without tools. Everything lands in
// the same conversation blocks the design shows:
//
//   reasoning_content   -> the thinking block (folds into 已思考 N 秒)
//   get_context         -> step 查看当前窗口 (UI thread)
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
#include "ai_model_key.h"
#include "ai_model_stream.h"
#include "ai_search_spec.h"
#include "ai_settings.h"
#include "ai_tools_fs.h"
#include "app_commands.h"
#include "app_internal.h"
#include "app_navigation.h"
#include "app_ops_ui.h"
#include "batch_rename_ai.h"
#include "batch_rename_plan.h"
#include "batch_rename_presets.h"
#include "places.h"
#include "../common/json_value.h"
#include "../common/localization.h"
#include "../fs/fs_enum.h"
#include "../ui/ai_list_overlay.h"
#include "../ui/ui_motion.h"
#include "../ui/ui_renderer.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <propkey.h>
#include <propsys.h>
#include <shobjidl.h>

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
// it (CheckPathArg) or with it (the model echoes get_context).
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
{"type":"function","function":{"name":"get_context",
 "description":"The active Pulse tab: its folder (null for This PC or a virtual view such as a search), how many items it shows, and up to 200 selected paths.",
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
         "- When the request is about this folder, the selection or what is open, call get_context first; "
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
         "- You cannot delete, overwrite or read file contents.\n"
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

void ToolGetContext(AppState& s, const app::ai::ModelToolCall& call) {
    AiAgent& a = Agent(s);
    AddStep(s, a, ui::AiIcon::Window, Text(I::AiAsstStepWindow), {}, Text(I::AiAsstStepWindowRun));
    json::Value out = json::Value::Object();
    std::wstring sub;
    app::Tab* tab = ActiveTab(s);
    const std::wstring folder = tab ? tab->current_path : std::wstring();
    if (!tab || folder.empty() || fs::IsVirtualPath(folder)) {
        out.set("folder", nullptr);
        if (tab && !folder.empty()) out.set("view", Utf8(folder));
        else out.set("view", "This PC");
        sub = Text(I::AiAsstStepWindowNone);
    } else {
        out.set("folder", Utf8(folder));
        out.set("folder_display_name", Utf8(FolderLabel(folder)));
        out.set("items", static_cast<uint64_t>(tab->EntryCount()));
        std::vector<int> picked;
        if (tab->snapshot) picked = tab->SelectedIndices();
        std::sort(picked.begin(), picked.end());
        json::Value selected = json::Value::Array();
        for (size_t i = 0; i < picked.size() && i < 200; ++i)
            selected.push(Utf8(JoinPath(folder, tab->EntryAt(static_cast<size_t>(picked[i])).name)));
        out.set("selected", std::move(selected));
        out.set("selected_count", static_cast<uint64_t>(picked.size()));
        const std::wstring label = FolderLabel(folder);
        sub = picked.empty()
                  ? Fill(Fill(Text(I::AiAsstStepWindowDone), L"{folder}", label), L"{n}", Num(tab->EntryCount()))
                  : Fill(Fill(Fill(Text(I::AiAsstStepWindowSel), L"{folder}", label), L"{n}", Num(tab->EntryCount())),
                         L"{k}", Num(picked.size()));
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
    if (call.name == "get_context") ToolGetContext(s, call);
    else if (call.name == "search_files") ToolSearch(s, call, args);
    else if (call.name == "list_folder") ToolListFolder(s, call, std::move(args));
    else if (call.name == "get_properties") ToolGetProperties(s, call, args);
    else if (call.name == "organize_files") ToolOrganize(s, call, args);
    else if (call.name == "rename_files") ToolRename(s, call, args);
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

} // namespace pulse
