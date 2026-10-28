#include "ai_tools_ui.h"
#include "ai_tools_fs.h"
#include "app_messages.h"
#include "app_navigation.h"
#include "app_runtime.h"
#include "app_state.h"
#include "actions/action_state.h"
#include "shell_window_sync.h"
#include "tray_reveal.h"
#include "../fs/fs_enum.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>

namespace pulse::app::ai {
namespace {

constexpr DWORD kUiWaitMs = 5000;
constexpr ULONGLONG kRevealSpacingMs = 500;   // at most two reveals a second

struct UiTask {
    std::function<void(AppState*)> work;
    std::mutex mu;
    std::condition_variable cv;
    bool running = false;
    bool done = false;
    bool abandoned = false;
};

// Runs `work` on the UI thread. False when Pulse has no host window, or the
// task was cancelled or not started in time (then it never runs).
bool RunOnUiThread(HWND host, std::function<void(AppState*)> work, const std::atomic<bool>& cancelled) {
    if (!host || !IsWindow(host)) return false;
    auto task = std::make_shared<UiTask>();
    task->work = std::move(work);
    auto* message = new std::shared_ptr<UiTask>(task);
    if (!PostMessageW(host, WM_AI_UI_TASK, 0, reinterpret_cast<LPARAM>(message))) {
        delete message;
        return false;
    }
    std::unique_lock lock(task->mu);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kUiWaitMs);
    while (!task->done && !task->running && !cancelled && std::chrono::steady_clock::now() < deadline)
        task->cv.wait_for(lock, std::chrono::milliseconds(50));
    if (task->running) {
        // Started: UI work is short and must not be left writing into our result.
        task->cv.wait(lock, [&] { return task->done; });
        return true;
    }
    if (!task->done) task->abandoned = true;
    return task->done;
}

std::wstring TrimmedFolder(std::wstring path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    return path;
}

bool SameFolderPath(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(TrimmedFolder(a).c_str(), TrimmedFolder(b).c_str()) == 0;
}

ToolOutcome NoWindow() {
    return ToolOutcome::Fail("unsupported", "Pulse has no window open right now; ask the user to open Pulse.", true);
}

// The same snapshot the assistant's get_state returns (action_state.h).
void ReadContext(AppState* s, json::Value& out) {
    if (!s) {
        out = json::Value::Object();
        out.set("window", nullptr);
        return;
    }
    out = actions::StateSnapshot(*s);
}

struct RevealJob {
    std::wstring client;
    std::wstring folder;
    std::vector<std::wstring> names;
};

// The tab each client's reveals use, while it exists (UI thread only).
struct RevealTab {
    HWND window = nullptr;
    const void* tab = nullptr;   // app::LayoutTab*, compared only
};
std::map<std::wstring, RevealTab>& RevealTabs() {
    static std::map<std::wstring, RevealTab> tabs;
    return tabs;
}

void RevealOnUi(AppState* s, const RevealJob& job, json::Value& out) {
    out = json::Value::Object();
    if (!s) return;
    RevealWindow(*s);
    auto& tabs = RevealTabs();
    size_t index = s->window_tabs.items.size();
    if (const auto known = tabs.find(job.client); known != tabs.end() && known->second.window == s->hwnd)
        for (size_t i = 0; i < s->window_tabs.items.size(); ++i)
            if (s->window_tabs.items[i].get() == known->second.tab) index = i;
    if (index < s->window_tabs.items.size()) {
        if (index != s->window_tabs.active) SwitchTab(*s, index);
        app::Tab* tab = ActiveTab(*s);
        if (tab && !SameFolderPath(tab->current_path, job.folder)) NavigateTo(*s, job.folder);
        out.set("tab", "reused");
    } else {
        const size_t before = s->window_tabs.items.size();
        OpenFolderTab(*s, job.folder);   // a tab already showing it, else a new one
        if (s->window_tabs.items.size() > before && s->window_tabs.active < s->window_tabs.items.size()) {
            // Only a tab opened for the client becomes its reveal tab, never one of the user's.
            tabs[job.client] = {s->hwnd, s->window_tabs.items[s->window_tabs.active].get()};
            out.set("tab", "new");
        } else {
            out.set("tab", "existing");
        }
    }
    app::Tab* tab = ActiveTab(*s);
    if (!tab) return;
    SelectNameInTab(*s, *tab, job.names.front());
    for (size_t i = 1; i < job.names.size(); ++i) AddNameToSelection(*s, *tab, job.names[i]);
    InvalidateRect(s->hwnd, nullptr, FALSE);
    out.set("shown", true);
}

} // namespace

bool PostAiUiTask(HWND host, std::function<void(AppState*)> work) {
    if (!host || !IsWindow(host)) return false;
    auto task = std::make_shared<UiTask>();
    task->work = std::move(work);
    auto* message = new std::shared_ptr<UiTask>(std::move(task));
    if (PostMessageW(host, WM_AI_UI_TASK, 0, reinterpret_cast<LPARAM>(message))) return true;
    delete message;
    return false;
}

bool RunOnAiUiThread(HWND host, std::function<void(AppState*)> work, const std::atomic<bool>& cancelled) {
    return RunOnUiThread(host, std::move(work), cancelled);
}

void HandleAiUiTask(AppState* target, LPARAM lparam) {
    std::unique_ptr<std::shared_ptr<UiTask>> holder(reinterpret_cast<std::shared_ptr<UiTask>*>(lparam));
    if (!holder || !*holder) return;
    UiTask& task = **holder;
    {
        std::lock_guard lock(task.mu);
        if (task.abandoned) return;
        task.running = true;
    }
    task.cv.notify_all();
    task.work(target);
    {
        std::lock_guard lock(task.mu);
        task.done = true;
    }
    task.cv.notify_all();
}

ToolOutcome GetContextTool(HWND host, const ToolCall& call) {
    auto result = std::make_shared<json::Value>();
    if (!RunOnUiThread(host, [result](AppState* s) { ReadContext(s, *result); }, call.cancelled))
        return call.cancelled ? ToolOutcome::Fail("cancelled", "The call was cancelled.")
                              : ToolOutcome::Fail("busy", "Pulse is busy; try again.", true);
    ToolOutcome out;
    out.data = std::move(*result);
    return out;
}

ToolOutcome RevealTool(HWND host, const ToolCall& call) {
    static std::atomic<ULONGLONG> last_reveal{0};
    const json::Value* paths = call.args.find("paths");
    if (!paths || !paths->is_array() || paths->size() == 0 || paths->size() > 200)
        return ToolOutcome::Fail("invalid_args", "paths must be an array of 1 to 200 paths");
    // Checked here, off the UI thread: the disk may be slow or a share offline.
    RevealJob job;
    job.client = call.client;
    json::Value not_shown = json::Value::Array();
    json::Value missing = json::Value::Array();
    for (const json::Value& arg : paths->items()) {
        std::wstring path;
        ToolOutcome error;
        if (!CheckPathArg(arg, "paths", path, error)) return error;
        const std::wstring folder = ParentFolder(path);
        if (folder.empty()) return ToolOutcome::Fail("invalid_args", "a drive or share root cannot be selected");
        if (GetFileAttributesW(Win32Path(path).c_str()) == INVALID_FILE_ATTRIBUTES) {
            missing.push(json::Utf8(path));
            continue;
        }
        if (job.folder.empty()) job.folder = folder;
        if (!SameFolderPath(job.folder, folder)) {
            not_shown.push(json::Utf8(path));
            continue;
        }
        job.names.push_back(path.substr(path.find_last_of(L'\\') + 1));
    }
    if (job.names.empty()) return ToolOutcome::Fail("not_found", "None of the paths exist.");
    const ULONGLONG now = GetTickCount64();
    ULONGLONG previous = last_reveal.load();
    if (now - previous < kRevealSpacingMs || !last_reveal.compare_exchange_strong(previous, now))
        return ToolOutcome::Fail("busy", "reveal can be called at most twice a second.", true);
    auto result = std::make_shared<json::Value>();
    auto shared_job = std::make_shared<RevealJob>(std::move(job));
    if (!RunOnUiThread(host, [result, shared_job](AppState* s) { RevealOnUi(s, *shared_job, *result); },
                       call.cancelled))
        return call.cancelled ? ToolOutcome::Fail("cancelled", "The call was cancelled.")
                              : ToolOutcome::Fail("busy", "Pulse is busy; try again.", true);
    if (!result->get_bool("shown")) return NoWindow();
    ToolOutcome out;
    out.data = std::move(*result);
    out.data.set("folder", json::Utf8(shared_job->folder));
    out.data.set("selected", static_cast<uint64_t>(shared_job->names.size()));
    if (not_shown.size()) out.data.set("not_shown", std::move(not_shown));   // other folders: one at a time
    if (missing.size()) out.data.set("missing", std::move(missing));
    return out;
}

} // namespace pulse::app::ai
