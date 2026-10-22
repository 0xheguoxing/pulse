#pragma once
#include "update_progress.h"
#include "../common/localization.h"
#include "../ui/ui_view_model.h"
#include <cwchar>

namespace pulse::app {
inline std::wstring UpdateProgressText(const UpdateProgress& progress) {
    using l10n::StringId;
    switch (progress.phase) {
    case UpdatePhase::Connecting: return l10n::Get(StringId::UpdateConnecting);
    case UpdatePhase::Verifying: return l10n::Get(StringId::UpdateVerifying);
    case UpdatePhase::WaitingOperations: return l10n::Get(StringId::UpdateWaitingOperations);
    case UpdatePhase::Ready:
    case UpdatePhase::Launching: return l10n::Get(StringId::UpdateLaunching);
    case UpdatePhase::Installing: return l10n::Get(StringId::UpdateInstallingStatus);
    case UpdatePhase::Downloading: {
        wchar_t text[256]{};
        constexpr double mib = 1024.0 * 1024.0;
        const int percent = progress.percent();
        if (percent >= 0)
            swprintf_s(text, l10n::Get(StringId::UpdateDownloadProgress).c_str(), percent,
                progress.received_bytes / mib, progress.total_bytes / mib);
        else
            swprintf_s(text, l10n::Get(StringId::UpdateDownloadUnknown).c_str(), progress.received_bytes / mib);
        return text;
    }
    default: return {};
    }
}
inline void ApplyUpdateStatus(ui::StatusBarView& status, const UpdateProgress& progress, bool operation_active) {
    // Search cancellation and active file operations retain their existing priority.
    if (!progress.active() || status.query_active || operation_active) return;
    status.task_is_update = true;
    status.task_text = UpdateProgressText(progress);
    status.task_progress = static_cast<float>(progress.percent());
}
}
