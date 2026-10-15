#pragma once
#include <windows.h>

namespace pulse::index {
enum class UsnReadWait { Completed, Stopped, Failed, Removing };
// The OVERLAPPED and reusable read buffer must remain alive until completion,
// including cancellation and a failed wait. Also exercised with an isolated pipe.
// `removing`, when given, cancels the read so the volume handle can be closed.
inline UsnReadWait AwaitUsnRead(HANDLE file, HANDLE stop, HANDLE completed,
                              OVERLAPPED& operation, DWORD& received, DWORD& error,
                              HANDLE removing = nullptr) {
    HANDLE events[]{stop, completed, removing};
    const DWORD wait = WaitForMultipleObjects(removing ? 3 : 2, events, FALSE, INFINITE);
    if (wait == WAIT_OBJECT_0 + 1) {
        if (GetOverlappedResult(file, &operation, &received, FALSE)) return UsnReadWait::Completed;
        error = GetLastError();
    } else if (wait == WAIT_OBJECT_0 || wait == WAIT_OBJECT_0 + 2) {
        CancelIoEx(file, &operation);
        GetOverlappedResult(file, &operation, &received, TRUE);
        return wait == WAIT_OBJECT_0 ? UsnReadWait::Stopped : UsnReadWait::Removing;
    } else {
        error = wait == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
    }
    CancelIoEx(file, &operation);
    GetOverlappedResult(file, &operation, &received, TRUE);
    return UsnReadWait::Failed;
}
}
