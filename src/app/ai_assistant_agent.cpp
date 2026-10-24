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
#include "ai_tools_fs.h"
#include "app_internal.h"
#include "app_navigation.h"
#include "../common/json_value.h"
#include "../common/localization.h"
#include "../fs/fs_enum.h"
#include "../ui/ui_motion.h"
#include "../ui/ui_renderer.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <thread>

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
  "required":["folder","groups"]}}}
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
         "- To move items into folders call organize_files. Pulse shows the plan and asks the user itself, so do not "
         "ask for confirmation in text first, and never say anything was moved unless organize_files returned ok. "
         "If the user declines, do not propose the same plan again.\n"
         "- Before organize_files, write one short sentence that introduces the plan (what you found and how you "
         "will group it).\n"
         "- You cannot delete, rename, overwrite or read file contents.\n"
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
void Fail(AppState& s, const std::wstring& message);

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
        if (!a.read_folder.empty())
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
           [id, label](AppState& st, const ToolOutcome& out) {
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
               EndStep(st, ag, out.ok, sub);
               AiChatSetChip(Chat(st), ui::StatusAiState::Busy, Text(I::AiAsstThinking));
               AddToolResult(ag, id, out);
               ++ag.call_index;
               AiChatPush(st, 250, [](AppState& s2) { NextTool(s2); return -1; });
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

void RunOrganize(AppState& s, std::shared_ptr<OrganizePlan> plan, const std::string& call_id) {
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    a.organized = true;
    // One undo step for the whole sort, like 合并到文件夹.
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    plan->undo_group = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    const std::wstring undo_label = Text(I::AiAsstUndoLabel);
    for (const auto& g : plan->groups) {
        if (!g.exists) {
            ops::OpRequest create;
            create.type = ops::OpType::CreateFolder;
            create.sources.push_back(g.path);
            create.undo_group = plan->undo_group;
            create.undo_label = undo_label;
            s.ops.Submit(std::move(create));
        }
        ops::OpRequest move;
        move.type = ops::OpType::Move;
        for (const auto& name : g.items) move.sources.push_back(JoinPath(plan->folder, name));
        move.dest_dir = g.path;
        move.collision_policy = ops::CollisionPolicy::KeepBoth;
        move.undo_group = plan->undo_group;
        move.undo_label = undo_label;
        s.ops.Submit(std::move(move));
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
    };
    auto w = std::make_shared<Watch>();
    w->started = w->last_change = Now();
    const uint64_t gen = a.generation;
    AiChatPush(s, 200, [plan, call_id, w, gen](AppState& st2) {
        AiAgent& ag = Agent(st2);
        if (ag.generation != gen) return -1;
        size_t gone = 0;
        for (const auto& src : plan->sources)
            if (!Exists(src)) ++gone;
        const uint64_t now = Now();
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
        if (all || (idle && now - w->last_change > 2500) || now - w->started > 600000) {
            SettleOrganize(st2, plan, call_id, gone);
            return -1;
        }
        return 150;
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
    else if (call.name == "list_folder") ToolListFolder(s, call, std::move(args));
    else if (call.name == "get_properties") ToolGetProperties(s, call, args);
    else if (call.name == "organize_files") ToolOrganize(s, call, args);
    else {
        AddToolResult(a, call.id, ToolOutcome::Fail("unknown_tool", "no tool named " + call.name));
        ++a.call_index;
        NextTool(s);
    }
    Redraw(s);
}

// ---------------------------------------------------------------- stream --

std::wstring ErrorText(const ModelEvent& ev) {
    switch (ev.http_status) {
    case 401: return Text(I::AiAsstErrAuth);
    case 402: return Text(I::AiAsstErrBalance);
    case 429: return Text(I::AiAsstErrRate);
    case 0:
        if (ev.error_type == "incomplete") return Text(I::AiAsstErrEarly);
        return Fill(Fill(Text(I::AiAsstErrNetwork), L"{host}", AiAgentHost()), L"{code}",
                    std::to_wstring(ev.win32_error));
    default: break;
    }
    if (ev.http_status >= 500)
        return Fill(Text(I::AiAsstErrServer), L"{code}", std::to_wstring(ev.http_status));
    std::wstring detail = json::Wide(ev.error_message);
    if (detail.size() > 300) detail = detail.substr(0, 300) + L"\u2026";
    return Fill(Fill(Text(I::AiAsstErrBadRequest), L"{code}", std::to_wstring(ev.http_status)), L"{detail}", detail);
}

void EndTurn(AppState& s) {
    AiAgent& a = Agent(s);
    FoldThinking(s, a);
    FoldSteps(s, a);
    a.stream.reset();
    AiChatAddActs(s, a.tool_count);
    AiChatFinish(s, true);
}

void Fail(AppState& s, const std::wstring& message) {
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
    AiBlockView& t = Chat(s).Add(AiBlockKind::Text, Now());
    t.text = message;
    a.stream.reset();
    AiChatAddActs(s, a.tool_count);
    AiChatFinish(s, false);
    Redraw(s);
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
            Fail(s, ErrorText(ev));
            return false;
        }
    }
    if (a.stream && a.stream->Ended()) {
        Fail(s, Text(I::AiAsstErrEarly));
        return false;
    }
    return true;
}

void SendRound(AppState& s) {
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    if (++a.rounds > kMaxRounds) {
        Fail(s, Text(I::AiAsstErrTooMany));
        return;
    }
    std::wstring key;
    if (!app::ai::LoadModelKey(key)) {
        Fail(s, Text(I::AiAsstErrNoKey));
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
    endpoint.base_url = kBaseUrl;
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
} // namespace

bool AiAgentConnected() {
    const uint64_t now = GetTickCount64();
    if (!g_key_checked || now - g_key_checked > 2000) {
        g_key_present = app::ai::HasModelKey();
        g_key_checked = now;
    }
    return g_key_present;
}

void AiAgentKeyChanged() { g_key_checked = 0; }

std::wstring AiAgentHost() { return app::ai::ModelHost(kBaseUrl); }

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
    if (a.history.size() > a.turn_history) {
        json::Value kept = json::Value::Array();
        for (size_t i = 0; i < a.turn_history; ++i) kept.push(a.history.at(i));
        a.history = std::move(kept);
    }
    if (a.organized && a.pending_note.empty())
        a.pending_note = L"(Earlier in this conversation organize_files moved items before the user stopped the turn.)";
    a.think_id = a.text_id = a.steps_id = 0;
    a.calls.clear();
    a.call_index = 0;
}

} // namespace pulse
