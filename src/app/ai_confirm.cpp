#include "ai_confirm.h"
#include "ai_tools_fs.h"
#include "ai_tools_ui.h"
#include "app_state.h"
#include "tray_reveal.h"
#include "../common/localization.h"
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace pulse::app::ai {
namespace {

constexpr size_t kListedItems = 5;

struct Question {
    ui::ConfirmDialogSpec spec;
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;        // answered (or found no window)
    bool no_window = false;
    bool abandoned = false;   // the asker gave up: do not show, or close
    HWND dialog = nullptr;
    ui::ConfirmChoice choice = ui::ConfirmChoice::Cancel;
};

std::mutex g_turn;                    // one question at a time
std::mutex g_open_mu;
std::weak_ptr<Question> g_open;       // the question whose dialog is up

std::wstring Fill(std::wstring text, std::wstring_view key, const std::wstring& value) {
    for (size_t at = text.find(key); at != std::wstring::npos; at = text.find(key, at + value.size()))
        text.replace(at, key.size(), value);
    return text;
}

std::wstring Text(l10n::StringId id) { return l10n::Get(id).c_str(); }

std::wstring LeafName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

void Show(AppState* s, const std::shared_ptr<Question>& q) {
    {
        std::lock_guard lock(q->mu);
        if (q->abandoned) return;
        if (!s || !s->hwnd) {
            q->no_window = true;
            q->done = true;
            q->cv.notify_all();
            return;
        }
    }
    ui::ConfirmDialogSpec spec = q->spec;
    std::weak_ptr<Question> weak = q;
    spec.on_open = [weak](HWND dialog) {
        const auto question = weak.lock();
        if (!question) return;
        std::lock_guard lock(question->mu);
        question->dialog = dialog;
        if (dialog && question->abandoned) PostMessageW(dialog, WM_CLOSE, 0, 0);
    };
    {
        std::lock_guard lock(g_open_mu);
        g_open = q;
    }
    // A question needs a visible Pulse to sit on (it may be in the tray).
    if (!IsWindowVisible(s->hwnd) || IsIconic(s->hwnd)) RevealWindow(*s);
    if (GetForegroundWindow() != s->hwnd) {
        // Pulse is behind the AI app: the dialog may not take the foreground.
        FLASHWINFO flash{sizeof(flash), s->hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0};
        FlashWindowEx(&flash);
    }
    const ui::ConfirmChoice choice = ui::ShowConfirmDialogEx(s->hwnd, spec, s->darkMode, s->accentColor);
    {
        std::lock_guard lock(g_open_mu);
        if (g_open.lock() == q) g_open.reset();
    }
    std::lock_guard lock(q->mu);
    q->choice = choice;
    q->done = true;
    q->cv.notify_all();
}

enum class Asked { Answered, NoWindow, Gone, Timeout };

Asked Ask(HWND host, const std::shared_ptr<Question>& q, const std::atomic<bool>* cancelled,
          const std::atomic<bool>& stopping, DWORD timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    auto gone = [&] { return stopping.load() || (cancelled && cancelled->load()); };
    std::unique_lock turn(g_turn, std::defer_lock);
    while (!turn.try_lock()) {
        if (gone()) return Asked::Gone;
        if (std::chrono::steady_clock::now() >= deadline) return Asked::Timeout;
        Sleep(50);
    }
    if (!PostAiUiTask(host, [q](AppState* s) { Show(s, q); })) return Asked::NoWindow;
    std::unique_lock lock(q->mu);
    Asked result = Asked::Answered;
    while (!q->done) {
        if (gone()) {
            result = Asked::Gone;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            result = Asked::Timeout;
            break;
        }
        q->cv.wait_for(lock, std::chrono::milliseconds(50));
    }
    if (!q->done) {
        q->abandoned = true;
        if (q->dialog) PostMessageW(q->dialog, WM_CLOSE, 0, 0);
        return result;
    }
    return q->no_window ? Asked::NoWindow : Asked::Answered;
}

} // namespace

ui::ConfirmDialogSpec ConnectQuestionSpec(const std::wstring& client) {
    using I = l10n::StringId;
    ui::ConfirmDialogSpec spec;
    spec.title = Fill(Text(I::AiConnectTitle), L"{client}", client);
    spec.message = Text(I::AiConnectMessage);
    spec.confirm_text = Text(I::AiConnectAllow);
    spec.secondary_text = Text(I::AiConnectSession);
    spec.cancel_text = Text(I::AiDeny);
    spec.note = Text(I::AiConnectNote);
    spec.tone = ui::ConfirmTone::Question;
    spec.glyph = L"\xE99A";
    // Enter alone does not let a new app in.
    spec.default_choice = ui::ConfirmChoice::Secondary;
    return spec;
}

ui::ConfirmDialogSpec CreateZipQuestionSpec(const std::wstring& client, const std::vector<std::wstring>& names,
                                            size_t total, const std::wstring& archive_name,
                                            const std::wstring& folder) {
    using I = l10n::StringId;
    ui::ConfirmDialogSpec spec;
    spec.title = Fill(Text(I::AiCreateZipTitle), L"{client}", client);
    spec.message = Fill(Fill(Text(I::AiCreateZipMessage), L"{name}", archive_name), L"{folder}", folder);
    for (size_t i = 0; i < names.size() && i < kListedItems; ++i) spec.items.push_back(names[i]);
    if (total > spec.items.size())
        spec.items.push_back(Fill(Text(I::AiCreateMore), L"{n}", std::to_wstring(total - spec.items.size())));
    spec.item_glyph = L"\xE8A5";
    spec.confirm_text = Text(I::AiCreateAllow);
    spec.secondary_text = Text(I::AiCreateAlways);
    spec.cancel_text = Text(I::AiDeny);
    spec.note = Text(I::AiCreateNote);
    spec.tone = ui::ConfirmTone::Question;
    spec.glyph = L"\xF012";
    return spec;
}

ConnectAnswer AskToConnect(HWND host, const std::wstring& client, const std::atomic<bool>& stopping,
                           DWORD timeout_ms) {
    auto q = std::make_shared<Question>();
    q->spec = ConnectQuestionSpec(client);
    if (Ask(host, q, nullptr, stopping, timeout_ms) != Asked::Answered) return ConnectAnswer::Deny;
    switch (q->choice) {
        case ui::ConfirmChoice::Confirm: return ConnectAnswer::Allow;
        case ui::ConfirmChoice::Secondary: return ConnectAnswer::Session;
        default: return ConnectAnswer::Deny;
    }
}

CreateAnswer AskToCreate(HWND host, const ConfirmRequest& request, const std::atomic<bool>& cancelled,
                         const std::atomic<bool>& stopping, DWORD timeout_ms) {
    auto q = std::make_shared<Question>();
    if (request.tool == "pulse.create_zip") {
        // The broker ran the tool's check first, so the plan is valid; its
        // name is what the archive will be called unless the folder changes.
        ZipPlan plan;
        ToolOutcome ignored;
        std::vector<std::wstring> names;
        if (PlanZip(request.args, plan, ignored)) {
            for (const std::wstring& source : plan.sources) {
                names.push_back(LeafName(source));
                if (names.size() >= kListedItems) break;
            }
        }
        q->spec = CreateZipQuestionSpec(request.client, names, plan.sources.size(), LeafName(plan.zip_path),
                                        request.folder);
    } else {
        using I = l10n::StringId;
        q->spec.title = request.client;
        q->spec.message = Fill(Text(I::AiCreateGenericMessage), L"{folder}", request.folder);
        q->spec.confirm_text = Text(I::AiCreateAllow);
        q->spec.secondary_text = Text(I::AiCreateAlways);
        q->spec.cancel_text = Text(I::AiDeny);
    }
    switch (Ask(host, q, &cancelled, stopping, timeout_ms)) {
        case Asked::Answered: break;
        case Asked::Timeout: return CreateAnswer::Timeout;
        default: return CreateAnswer::Deny;
    }
    switch (q->choice) {
        case ui::ConfirmChoice::Confirm: return CreateAnswer::Allow;
        case ui::ConfirmChoice::Secondary: return CreateAnswer::AlwaysHere;
        default: return CreateAnswer::Deny;
    }
}

bool BringAiQuestionToFront() {
    std::shared_ptr<Question> q;
    {
        std::lock_guard lock(g_open_mu);
        q = g_open.lock();
    }
    if (!q) return false;
    HWND dialog = nullptr;
    {
        std::lock_guard lock(q->mu);
        dialog = q->dialog;
    }
    if (!dialog || !IsWindow(dialog)) return false;
    if (IsIconic(GetWindow(dialog, GW_OWNER))) ShowWindow(GetWindow(dialog, GW_OWNER), SW_RESTORE);
    SetForegroundWindow(dialog);
    return true;
}

} // namespace pulse::app::ai
