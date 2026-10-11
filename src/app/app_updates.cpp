#include "app_updates.h"
#include "update_shutdown.h"
#include "app_state.h"
#include "app_internal.h"
#include "app_runtime.h"
#include "pulse_version.h"
#include "../common/localization.h"
#include "../common/runtime_log.h"
#include <cstdio>

namespace pulse {
namespace {
constexpr ULONGLONG kCheckInterval = 6ull * 60 * 60 * 1000;
void ShowInstallError(AppState& state) {
    using l10n::StringId;
    const auto message = state.update_install_error == ERROR_CANCELLED ? StringId::UpdateCancelled :
        state.update_install_error == ERROR_BUSY ? StringId::UpdateBusy : StringId::UpdateInstallFailed;
    state.notification_toast.Show(state.hwnd, l10n::Get(StringId::Update), l10n::Get(message));
}

void LaunchReadyUpdate(AppState& state) {
    const auto phase = state.update_installer.Progress().phase;
    if (phase != app::UpdatePhase::Ready && phase != app::UpdatePhase::WaitingOperations) return;
    DWORD error = ERROR_SUCCESS;
    const bool idle = app::RequestUpdateLaunch(state.ops, state.settings.migration_pending(), [&] {
        state.update_installer.Launch(state.hwnd, error);
    });
    if (!idle) state.update_installer.WaitForOperations();
    if (error) {
        state.update_install_error = error;
        state.update_installer.Stop();
        ShowInstallError(state);
    }
}
}

void CheckForUpdates(AppState& state) {
    if (state.update_installer.Progress().active()) return;
    if (state.update_checker.CheckAsync(ServiceWindow(state), WM_UPDATE_RESULT)) {
        state.update_result_ready = false;
        state.update_install_error = ERROR_SUCCESS;
        state.next_update_check = GetTickCount64() + kCheckInterval;
        InvalidateRect(state.hwnd, nullptr, FALSE);
    }
}

LRESULT CloseForUpdate(AppState& state) {
    const HWND window = state.hwnd;
    const auto operation_id = diagnostics::runtime::NextId();
    const auto started = GetTickCount64();
    diagnostics::runtime::Event("update_shutdown_request", {{"operation", operation_id}});
    bool save_failed = false;
    const LRESULT result = app::RequestUpdateShutdown(state.ops, state.settings.migration_pending(), [&] {
        diagnostics::runtime::Event("update_session_save_start", {{"operation", operation_id}});
        const bool saved = PrepareSessionForUpdate(state);
        diagnostics::runtime::Event("update_session_save_end", {{"operation", operation_id},
            {"ok", saved}, {"elapsed_ms", GetTickCount64() - started}});
        if (!saved) { save_failed = true; return false; }
        diagnostics::runtime::Event("update_shutdown_destroy", {{"operation", operation_id}});
        if (DestroyWindow(window)) return true;
        const DWORD error = GetLastError();
        diagnostics::runtime::Event("update_shutdown_destroy_failed", {{"operation", operation_id}, {"code", error}});
        state.updateSessionPrepared = false;
        return false;
    });
    diagnostics::runtime::Event("update_shutdown_result", {{"operation", operation_id},
        {"result", static_cast<uint64_t>(save_failed ? app::kUpdateShutdownSaveFailed : result)},
        {"elapsed_ms", GetTickCount64() - started}});
    return save_failed ? app::kUpdateShutdownSaveFailed : result;
}

void TickUpdates(AppState& state, unsigned long long now) {
    LaunchReadyUpdate(state);
    DWORD install_error = ERROR_SUCCESS;
    if (state.update_installer.TakeInstallResult(install_error)) {
        state.update_install_error = install_error;
        if (install_error) ShowInstallError(state);
        state.update_installer.Stop();
        InvalidateRect(state.hwnd, nullptr, FALSE);
    }
    // Snapshot reads are cheap; repaint at most 10 Hz, only while work is active and visible.
    // Bytes are never posted as individual window messages.
    if (!state.shot.active && state.update_installer.Progress().active() &&
        now >= state.next_update_progress_paint && IsWindowVisible(state.hwnd) && !IsIconic(state.hwnd)) {
        state.next_update_progress_paint = now + 100;
        InvalidateRect(state.hwnd, nullptr, FALSE);
    }
    if (!state.whatsNewVersion.empty() && now >= state.whatsNewAt &&
        IsWindowVisible(state.hwnd) && !IsIconic(state.hwnd)) {
        wchar_t title[160]{};
        swprintf_s(title, l10n::Get(l10n::StringId::UpdatedTitleFormat).c_str(),
                   state.whatsNewVersion.c_str());
        state.notification_toast.Show(state.hwnd, title, l10n::Get(l10n::StringId::UpdatedClick),
                                      false, WM_SHOW_RELEASE_NOTES);
        state.whatsNewVersion.clear();
    }
    if (state.shot.active || !app::UpdateChecker::Enabled() || !state.appPrefs.auto_check_updates ||
        now < state.next_update_check) return;
    if (state.update_installer.Progress().active() || state.update_checker.checking()) return;
    state.next_update_check = now + kCheckInterval;
    CheckForUpdates(state);
}

void NoteRunningVersion(AppState& state) {
    auto& prefs = state.appPrefs;
    if (!prefs.persist || state.shot.active || state.menushot) return;
    if (prefs.last_seen_version == PULSE_VERSION_STRING) return;
    // A fresh install has no app.json yet; only upgrades get the toast.
    if (prefs.had_file) {
        state.whatsNewVersion = PULSE_VERSION_STRING;
        state.whatsNewAt = GetTickCount64() + 2500;
    }
    prefs.last_seen_version = PULSE_VERSION_STRING;
    prefs.Save();
}

void ShowReleaseNotes(AppState& state) {
    OpenSettingsTab(state, 3);
    state.settingsReleaseExpanded = 0;
    const auto vm = BuildVm(state, false);
    const float w = static_cast<float>(state.compositor.Width());
    const float h = static_cast<float>(state.compositor.Height());
    state.settings.SetScroll(
        state.renderer.SettingsDestinationOffset(vm, static_cast<int>(l10n::StringId::ReleaseNotes), w, h),
        state.renderer.SettingsMaxScroll(vm, w, h));
    InvalidateRect(state.hwnd, nullptr, FALSE);
}

void InstallUpdate(AppState& state) {
    if (state.update_installer.installing()) return;
    if (state.update_installer.Progress().active()) {
        state.update_installer.Stop();
        state.update_install_error = ERROR_CANCELLED;
    } else if (state.update_result_ready && state.update_result.update_available) {
        state.update_install_error = ERROR_SUCCESS;
        if (!state.update_installer.Start(state.update_result, ServiceWindow(state), WM_UPDATE_DOWNLOADED))
            state.update_install_error = ERROR_GEN_FAILURE;
    }
    InvalidateRect(state.hwnd, nullptr, FALSE);
}

void CompleteUpdateCheck(AppState& state) {
    app::UpdateResult result;
    if (!state.update_checker.TakeResult(result)) return;
    state.update_result = std::move(result);
    state.update_result_ready = true;
    if (state.update_result.update_available && state.notified_update_version != state.update_result.version) {
        state.notified_update_version = state.update_result.version;
        wchar_t title[160]{};
        swprintf_s(title, l10n::Get(l10n::StringId::UpdateAvailableFormat).c_str(),
            state.update_result.version.c_str());
        state.notification_toast.Show(state.hwnd, title,
            l10n::Get(l10n::StringId::UpdateClickToInstall), true, WM_UPDATE_INSTALL);
    }
    InvalidateRect(state.hwnd, nullptr, FALSE);
}

void CompleteUpdateDownload(AppState& state) {
    DWORD error = ERROR_SUCCESS;
    if (!state.update_installer.TakeResult(error)) return;
    if (!error) {
        // Paint the verified/starting stage before ShellExecute can enter an elevation prompt.
        InvalidateRect(state.hwnd, nullptr, FALSE);
        UpdateWindow(state.hwnd);
        LaunchReadyUpdate(state);
        return;
    }
    state.update_install_error = error;
    if (error) {
        state.update_installer.Stop();
        ShowInstallError(state);
    }
    InvalidateRect(state.hwnd, nullptr, FALSE);
}
}
