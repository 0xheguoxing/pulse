#include "ai_question_specs.h"
#include "../common/localization.h"

namespace pulse::app::ai {
namespace {

std::wstring Fill(std::wstring text, std::wstring_view key, const std::wstring& value) {
    for (size_t at = text.find(key); at != std::wstring::npos; at = text.find(key, at + value.size()))
        text.replace(at, key.size(), value);
    return text;
}

std::wstring Text(l10n::StringId id) { return l10n::Get(id).c_str(); }

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
    // The dialog lists the first names and ends with "and N more" of total.
    spec.items = names;
    spec.items_total = total;
    spec.item_glyph = L"\xE8A5";
    spec.confirm_text = Text(I::AiCreateAllow);
    spec.secondary_text = Text(I::AiCreateAlways);
    spec.cancel_text = Text(I::AiDeny);
    spec.note = Text(I::AiCreateNote);
    spec.tone = ui::ConfirmTone::Question;
    spec.glyph = L"\xF012";
    return spec;
}

} // namespace pulse::app::ai
