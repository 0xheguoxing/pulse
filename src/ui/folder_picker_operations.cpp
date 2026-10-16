#include "folder_picker_operations.h"
#include "file_operation_dialog.h"
#include "../common/localization.h"

namespace pulse::ui {

PickerOperationResult RunPickerOperation(HWND owner, ops::OpRequest request,
                                        bool dark, D2D1_COLOR_F accent) {
    PickerOperationResult result;
    ops::OpsManager manager;
    FileOperationWindow progress;
    FileOperationCallbacks callbacks;
    callbacks.cancel = [&] { manager.CancelCurrent(); };
    callbacks.pause = [&] { manager.PauseCurrent(); };
    callbacks.resume = [&] { manager.ResumeCurrent(); };
    callbacks.retry_authorization = [&](uint64_t id) { manager.ResolveAuthorization(id, true); };
    callbacks.skip_authorization = [&](uint64_t id) { manager.ResolveAuthorization(id, false); };
    if (!progress.Create(owner, std::move(callbacks))) {
        result.error = l10n::Get(l10n::StringId::CouldNotOpenTheFileOperationWindow).c_str();
        return result;
    }
    progress.SetTheme(dark, accent);
    const bool enabled = owner && IsWindowEnabled(owner);
    if (enabled) EnableWindow(owner, FALSE);
    manager.SetUiWindow(owner);
    manager.Start({}, false);
    if (!manager.Submit(std::move(request))) {
        manager.Stop();
        progress.Destroy();
        if (enabled) EnableWindow(owner, TRUE);
        result.error = l10n::Get(l10n::StringId::ThisFileOperationIsNotSupported).c_str();
        return result;
    }
    const ULONGLONG started = GetTickCount64();
    bool quit = false;
    int exit_code = 0;
    uint64_t conflict_token = 0;
    for (;;) {
        const auto status = manager.Status();
        progress.Update(status);
        if (status.completed_ops && !status.active) {
            result.error = status.last_error;
            break;
        }
        if (status.active && (GetTickCount64() - started >= 250 ||
                              status.authorization != ops::AuthorizationState::None)) {
            if (!progress.IsVisible()) progress.Show();
        }
        if (quit) manager.CancelCurrent();
        if (const auto conflict = manager.PendingConflict();
            !quit && conflict && conflict->token != conflict_token) {
            conflict_token = conflict->token;
            progress.Hide();
            const auto choice = ShowFileConflictDialog(owner, *conflict, dark, accent);
            // Nested dialogs may enable their owner as they close.
            if (enabled) EnableWindow(owner, FALSE);
            manager.ResolveConflict(conflict->token, choice.choice, choice.apply_to_all);
        }
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                quit = true;
                exit_code = static_cast<int>(message.wParam);
                manager.CancelCurrent();
            } else {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 30, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    manager.Stop();
    result.completed = manager.DrainCompletions();
    progress.Destroy();
    if (enabled && IsWindow(owner)) { EnableWindow(owner, TRUE); SetActiveWindow(owner); }
    if (quit) PostQuitMessage(exit_code);
    return result;
}

} // namespace pulse::ui
