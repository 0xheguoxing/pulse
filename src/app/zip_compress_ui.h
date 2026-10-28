// zip_compress_ui.h - 压缩 (toolbar tool): the selection into a .zip beside it.
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pulse {
struct AppState;
// Real files and folders of one folder are selected.
bool CanCompressSelection(AppState& s);
// Starts the archive on a thread; a snack with 取消 shows once it takes a
// moment, and the result replaces it. The archive gets selected (arrival_select.h).
void CompressSelection(AppState& s);
// Staging shelf: paths into dir\<base_name>.zip (" (2)" when taken). The
// caller draws `progress` (0..1); the result snack is the same. Returns the
// job id, 0 when nothing started.
uint64_t CompressPathsTo(AppState& s, std::vector<std::wstring> paths, const std::wstring& dir,
                         const std::wstring& base_name, std::shared_ptr<std::atomic<float>> progress);
// WM_ZIP_COMPRESS / WM_ZIP_CANCEL.
void HandleZipMessage(AppState& s, UINT message, WPARAM wparam, LPARAM lparam);
// Window closing: ask running archives to stop (each removes its temporary).
void CancelZipJobs(AppState& s);
}
