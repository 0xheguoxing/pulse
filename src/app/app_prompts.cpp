#include "../common/windows_compat.h"
#include "app_prompts.h"

#include "app_state.h"
#include "../common/localization.h"
#include "../ui/confirm_dialog.h"
#include "content_scope_mode.h"

#include <cwchar>
#include <string>

namespace pulse {
namespace {

std::wstring Text(l10n::StringId id) { return l10n::Get(id); }

ui::ConfirmChoice Ask(AppState& s, const ui::ConfirmDialogSpec& spec) {
    return ui::ShowConfirmDialogEx(s.hwnd, spec, s.darkMode, s.accentColor);
}

// Older prompt strings carry their own paragraph breaks.
std::wstring TrimBreaks(std::wstring text) {
    while (!text.empty() && (text.front() == L'\n' || text.front() == L'\r')) text.erase(0, 1);
    return text;
}

// Code-side Chinese, converted for the zh-TW UI phrase by phrase.
std::wstring Zh(const wchar_t* chinese, const wchar_t* english) {
    return l10n::IsChinese() ? l10n::HantText(chinese) : std::wstring(english);
}

} // namespace

int AskContentScope(AppState& s) {
    ui::ConfirmDialogSpec spec;
    spec.title = Zh(L"内容搜索范围", L"Content search scope");
    spec.message = Zh(L"内容搜索会在搜索时直接读取文件。只搜索个人文件夹更快，占用也更少；"
                      L"搜索所有驱动器能找到更多文件，但每次搜索要读的文件也更多。",
                      L"Content search reads files while you search. Searching your personal folders "
                      L"is faster and lighter; searching all drives finds more but reads far more files each time.");
    spec.items = app::UserContentFolders();
    spec.item_glyph = L"\xE8B7";  // Folder
    spec.note = Zh(L"指定了文件夹的搜索（如“仅当前文件夹”）不受影响。可随时在设置中更改。",
                   L"Searches in a specific folder (such as current folder only) are not affected. "
                   L"You can change this anytime in Settings.");
    spec.confirm_text = Zh(L"仅个人文件夹（推荐）", L"Personal folders (recommended)");
    spec.secondary_text = Zh(L"所有驱动器", L"All drives");
    spec.cancel_text = Zh(L"以后再说", L"Not now");
    spec.tone = ui::ConfirmTone::Question;
    spec.glyph = L"\xE721";  // Search
    spec.default_choice = ui::ConfirmChoice::Confirm;
    const auto choice = Ask(s, spec);
    return choice == ui::ConfirmChoice::Confirm ? 1 : choice == ui::ConfirmChoice::Secondary ? 2 : 0;
}

bool ConfirmClearRecent(AppState& s) {
    ui::ConfirmDialogSpec spec;
    spec.title = Text(l10n::StringId::ClearRecentTitle);
    spec.message = Text(l10n::StringId::ClearRecentPrompt);
    spec.confirm_text = Text(l10n::StringId::ClearAll);
    spec.danger = true;
    spec.default_choice = ui::ConfirmChoice::Cancel;
    return Ask(s, spec) == ui::ConfirmChoice::Confirm;
}

bool ConfirmClearDiagnostics(AppState& s) {
    ui::ConfirmDialogSpec spec;
    spec.title = Text(l10n::StringId::ClearDiagnostics);
    spec.message = Text(l10n::StringId::DiagnosticsClearConfirm);
    spec.confirm_text = Text(l10n::StringId::ClearDiagnostics);
    spec.danger = true;
    spec.default_choice = ui::ConfirmChoice::Cancel;
    return Ask(s, spec) == ui::ConfirmChoice::Confirm;
}

bool ConfirmDiagnosticsExport(AppState& s, bool& include_service, bool& include_dumps) {
    ui::ConfirmDialogSpec privacy;
    privacy.title = Text(l10n::StringId::DiagnosticsPrivacyTitle);
    privacy.message = l10n::Pick(L"导出脱敏错误事件、程序版本、服务状态和磁盘空间，供反馈问题。默认不包含文件路径、搜索内容或内存转储。若需要排查崩溃，可选择包含转储；转储可能包含私人信息。请检查生成的 ZIP 后再发送。", L"Export sanitized error events, application versions, service status and disk space. File paths, searches and memory dumps are excluded by default. For crash analysis, you can include dumps, which may contain private information. Review the ZIP before sharing.");
    privacy.confirm_text = l10n::Pick(L"导出普通诊断", L"Export basic diagnostics");
    privacy.secondary_text = l10n::Pick(L"同时包含崩溃转储", L"Include crash dumps");
    privacy.tone = ui::ConfirmTone::Warning;
    privacy.default_choice = ui::ConfirmChoice::Cancel;
    const auto privacy_choice = Ask(s, privacy);
    if (privacy_choice == ui::ConfirmChoice::Cancel) return false;
    include_dumps = privacy_choice == ui::ConfirmChoice::Secondary;

    ui::ConfirmDialogSpec service;
    service.title = Text(l10n::StringId::DiagnosticsPrivacyTitle);
    service.message = Text(l10n::StringId::DiagnosticsIncludeService);
    service.confirm_text = Text(l10n::StringId::DiagnosticsIncludeServiceYes);
    service.secondary_text = Text(l10n::StringId::DiagnosticsIncludeServiceNo);
    service.tone = ui::ConfirmTone::Question;
    service.glyph = L"\xE7EF";  // Admin (shield)
    service.default_choice = ui::ConfirmChoice::Secondary;
    const ui::ConfirmChoice choice = Ask(s, service);
    if (choice == ui::ConfirmChoice::Cancel) return false;
    include_service = choice == ui::ConfirmChoice::Confirm;
    return true;
}

bool AskRetryRecovery(AppState& s, size_t count, bool uncertain_destructive, bool duplicate_cleanup) {
    ui::ConfirmDialogSpec spec;
    spec.title = Text(l10n::StringId::RecoveryTitle);
    wchar_t message[512]{};
    swprintf_s(message, Text(l10n::StringId::RecoveryPromptFormat).c_str(), count);
    spec.message = message;
    if (uncertain_destructive) spec.note = TrimBreaks(Text(l10n::StringId::RecoveryDestructiveWarning));
    if (duplicate_cleanup) {
        if (!spec.note.empty()) spec.note += L"\n";
        spec.note += l10n::Pick(L"重复文件清理不会恢复，请重新扫描。",
                                L"Duplicate cleanup will not be resumed. Please scan again.");
    }
    spec.confirm_text = Text(l10n::StringId::RecoveryRetry);
    spec.cancel_text = Text(l10n::StringId::RecoveryDiscard);
    spec.tone = ui::ConfirmTone::Warning;
    spec.glyph = L"\xE777";  // UpdateRestore
    spec.default_choice = ui::ConfirmChoice::Cancel;
    return Ask(s, spec) == ui::ConfirmChoice::Confirm;
}

} // namespace pulse
