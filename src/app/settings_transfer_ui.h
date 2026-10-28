// settings_transfer_ui.h - Settings > 关于与诊断 > 备份与迁移: the questions,
// the export / import tasks and the restart around settings_transfer.h.
#pragma once

#include <string>

namespace pulse { struct AppState; }
namespace pulse::app { struct SettingsTask; struct SettingsTaskResult; }

namespace pulse {

// `pulse.exe --restart-after <pid>`: wait for that Pulse to end first, so a
// staged import is applied after it wrote its last files.
inline constexpr wchar_t kRestartAfterArgument[] = L"--restart-after";

// What the card shows.
struct SettingsTransferView {
    bool busy = false;        // an export / import task is running
    bool pending = false;     // an import waits for the next start
    bool has_backup = false;  // a pre-import backup exists
    std::wstring backup_time; // its time, "2026-10-21 23:40"
};
SettingsTransferView GetSettingsTransferView(const AppState& s);

// The card's buttons: 0 export, 1 import, 2 restore (or restart while pending).
void SettingsTransferAction(AppState& s, int action);
// Worker side of SettingsTaskKind::SettingsExport / SettingsImport.
bool RunSettingsTransferTask(const app::SettingsTask& task, std::wstring& error);
// UI side once such a task finished: toast, or the restart.
void FinishSettingsTransfer(AppState& s, const app::SettingsTaskResult& result);
// Starts a new Pulse that waits for this one, then exits this one.
bool RestartPulse(AppState& s);

// Startup, before anything reads the data dir: wait for the Pulse being
// replaced, then apply a staged import. The toast follows once a window exists.
void WaitForRestartingPulse(unsigned long process_id);
void ApplyStagedSettingsAtStartup();
void ShowSettingsTransferStartupToast(AppState& s);

} // namespace pulse
