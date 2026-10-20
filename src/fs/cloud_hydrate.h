#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace pulse::fs {

enum class HydrateResult : uint8_t { Done, Cancelled, Failed };

// Reads `path` front to back, 1 MB at a time, so the sync engine brings the
// whole file onto this device - the same thing opening it in an app does.
// Only reads: nothing is written and no attribute is changed. `progress` gets
// the bytes read so far after every chunk. `cancel` is checked between
// chunks; a read still waiting on the provider can be ended from another
// thread with CancelSynchronousIo (reported as Cancelled once `cancel` is set).
HydrateResult HydrateByReading(const std::wstring& path, const std::atomic<bool>& cancel,
                               const std::function<void(uint64_t)>& progress,
                               DWORD* error = nullptr);

} // namespace pulse::fs
