#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

namespace pulse::app {

// 下载并预览 (C4b): the one explicit "bring this cloud file here" in Pulse.
// One download at a time for the whole process; starting another cancels the
// running one. The work is fs::HydrateByReading on its own thread.
enum CloudDownloadEvent : WPARAM {
    kCloudDownloadProgress = 0,   // throttled to ~8 per second
    kCloudDownloadDone = 1,
    kCloudDownloadFailed = 2,
    kCloudDownloadCancelled = 3,
};

// Posts `message` to `notify` with a CloudDownloadEvent and lParam = job id.
bool StartCloudDownload(const std::wstring& path, uint64_t size, HWND notify, UINT message);
// 0..100 while `path` is downloading, -1 otherwise.
int CloudDownloadPercent(const std::wstring& path);
// Path of the download a finished event belongs to (empty when unknown).
std::wstring CloudDownloadPathFor(LPARAM job);
void CancelCloudDownloads();

} // namespace pulse::app
