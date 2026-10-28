// ai_question_specs.h - What Pulse's questions for AI clients say (the
// confirm dialog specs). Apart from ai_confirm.cpp so the dialog gallery can
// draw them without the app.
#pragma once
#include "../ui/confirm_dialog.h"
#include <string>
#include <vector>

namespace pulse::app::ai {

// "Claude wants to connect to Pulse": 允许 / 只允许这次 (default) / 拒绝.
ui::ConfirmDialogSpec ConnectQuestionSpec(const std::wstring& client);
// "Claude wants to create a zip archive": `names` lists the items (at most
// five are shown, then "and N more" for the rest of `total`).
ui::ConfirmDialogSpec CreateZipQuestionSpec(const std::wstring& client, const std::vector<std::wstring>& names,
                                            size_t total, const std::wstring& archive_name,
                                            const std::wstring& folder);

} // namespace pulse::app::ai
