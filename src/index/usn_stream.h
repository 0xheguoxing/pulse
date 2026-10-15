#pragma once
#include "usn_packet_queue.h"
#include "usn_read_wait.h"
#include <winioctl.h>
#include <cfgmgr32.h>
#include <atomic>
#include <new>
#include <thread>
#include <string>
#pragma comment(lib, "cfgmgr32.lib")

namespace pulse::index {
// The collector owns its handle/cursor. Applying records, merging the index
// and writing cache files never hold its queue lock or delay the next read.
class UsnStream {
public:
    UsnStream(wchar_t letter, uint64_t journal, int64_t cursor, HANDLE signal)
        : UsnStream(signal) {
        thread_ = std::thread([this, letter, journal, cursor] { Read(letter, journal, cursor); });
    }
    ~UsnStream() {
        if (stop_) SetEvent(stop_);
        if (thread_.joinable()) thread_.join();
        for (HANDLE event : {stop_, removing_, released_, resumed_})
            if (event) CloseHandle(event);
    }
    bool Take(std::vector<BYTE>& records, int64_t& cursor) {
        bool more = false;
        const bool ok = packets_.Take(records, cursor, more);
        if (more) SetEvent(signal_);
        return ok;
    }
    DWORD Error() const { return packets_.Error(); }
    UsnPacketQueue::Stats Memory() const { return packets_.Memory(); }
private:
    friend struct UsnStreamTestAccess;
    explicit UsnStream(HANDLE signal) : signal_(signal), stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    void Failed(DWORD error) { packets_.Fail(error); SetEvent(signal_); }
    static HANDLE OpenJournalVolume(const std::wstring& path) {
        return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    }
    // Device removal (#251). An open volume handle vetoes Safely Remove and
    // eject unless it is closed on query-remove. The callback has the reader
    // close it; a cancelled removal resumes at the same cursor.
    HCMNOTIFICATION WatchRemoval(HANDLE volume) {
        if (!removing_ || !released_ || !resumed_) return nullptr;
        ResetEvent(removing_);
        ResetEvent(resumed_);
        removal_cancelled_ = false;
        CM_NOTIFY_FILTER filter{};
        filter.cbSize = sizeof(filter);
        filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEHANDLE;
        filter.u.DeviceHandle.hTarget = volume;
        HCMNOTIFICATION notification = nullptr;
        if (CM_Register_Notification(&filter, this, &UsnStream::RemovalCallback, &notification) != CR_SUCCESS)
            return nullptr;
        return notification;
    }
    void UnwatchRemoval(HCMNOTIFICATION& notification) {
        if (!notification) return;
        // Waits for a callback in flight; never called from the callback itself.
        SetEvent(released_);
        CM_Unregister_Notification(notification);
        notification = nullptr;
    }
    static DWORD CALLBACK RemovalCallback(HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action,
                                          PCM_NOTIFY_EVENT_DATA, DWORD) {
        auto* stream = static_cast<UsnStream*>(context);
        switch (action) {
        case CM_NOTIFY_ACTION_DEVICEQUERYREMOVE:
            ResetEvent(stream->released_);
            SetEvent(stream->removing_);
            WaitForSingleObject(stream->released_, 5000);
            break;
        case CM_NOTIFY_ACTION_DEVICEQUERYREMOVEFAILED:
            stream->removal_cancelled_ = true;
            SetEvent(stream->resumed_);
            break;
        case CM_NOTIFY_ACTION_DEVICEREMOVEPENDING:
        case CM_NOTIFY_ACTION_DEVICEREMOVECOMPLETE:
            SetEvent(stream->resumed_);
            break;
        default:
            break;
        }
        return ERROR_SUCCESS;
    }
    void Read(wchar_t letter, uint64_t journal, int64_t cursor) {
        if (!stop_) { Failed(ERROR_NOT_ENOUGH_MEMORY); return; }
        const auto path = L"\\\\.\\" + std::wstring(1, letter) + L":";
        HANDLE volume = OpenJournalVolume(path);
        if (volume == INVALID_HANDLE_VALUE) { Failed(GetLastError()); return; }
        HANDLE completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!completed) { const auto error = GetLastError(); CloseHandle(volume); Failed(error); return; }
        HCMNOTIFICATION removal = WatchRemoval(volume);
        READ_USN_JOURNAL_DATA_V0 request{};
        request.StartUsn = cursor; request.UsnJournalID = journal;
        request.ReasonMask = 0xffffffffu; request.BytesToWaitFor = 1;
        try {
            // This buffer is never moved into the queue and is reused only after
            // the previous I/O has completed (or cancellation has been drained).
            std::vector<BYTE> buffer(UsnPacketQueue::kReadBytes);
            while (WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0) {
                if (removing_ && WaitForSingleObject(removing_, 0) == WAIT_OBJECT_0) {
                    ResetEvent(removing_);
                    CloseHandle(volume);
                    volume = INVALID_HANDLE_VALUE;
                    SetEvent(released_);
                    HANDLE resume[]{stop_, resumed_};
                    if (WaitForMultipleObjects(2, resume, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;
                    UnwatchRemoval(removal);
                    // Gone: report it like a surprise removal; the volume topology takes over.
                    if (!removal_cancelled_) { Failed(ERROR_DEVICE_REMOVED); break; }
                    volume = OpenJournalVolume(path);
                    if (volume == INVALID_HANDLE_VALUE) { Failed(GetLastError()); break; }
                    removal = WatchRemoval(volume);
                    continue;
                }
                OVERLAPPED operation{}; operation.hEvent = completed; ResetEvent(completed);
                DWORD received = 0;
                bool ok = DeviceIoControl(volume, FSCTL_READ_USN_JOURNAL, &request, sizeof(request),
                    buffer.data(), static_cast<DWORD>(buffer.size()), &received, &operation) != FALSE;
                if (!ok && GetLastError() == ERROR_IO_PENDING) {
                    DWORD error = ERROR_SUCCESS;
                    const auto result = AwaitUsnRead(volume, stop_, completed, operation, received, error, removing_);
                    if (result == UsnReadWait::Stopped) break;
                    if (result == UsnReadWait::Removing) continue;
                    if (result == UsnReadWait::Failed) { Failed(error); break; }
                    ok = true;
                }
                if (!ok) { Failed(GetLastError()); break; }
                const bool queued = packets_.Push(buffer.data(), received);
                SetEvent(signal_);
                if (!queued) break;
                std::memcpy(&request.StartUsn, buffer.data(), sizeof(request.StartUsn));
            }
        } catch (const std::bad_alloc&) {
            Failed(ERROR_NOT_ENOUGH_MEMORY);
        }
        CloseHandle(completed);
        if (volume != INVALID_HANDLE_VALUE) CloseHandle(volume);
        UnwatchRemoval(removal);
    }
    HANDLE signal_, stop_;
    HANDLE removing_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual: AwaitUsnRead must not consume it
    HANDLE released_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE resumed_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::atomic<bool> removal_cancelled_{false};
    UsnPacketQueue packets_;
    std::thread thread_;
};
}
