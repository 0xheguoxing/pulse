#pragma once
#include "elevated_transfer.h"
#include <memory>
namespace pulse::ops {
// STA worker only. Recycle uses ITransferSource::RecycleItem, never DeleteItem.
// Permanent deletion uses IFileOperation. Neither requests elevation or Shell UI.
// Only local fixed volumes have a Recycle Bin: on a share (\\server\share, a
// mapped drive) or a removable drive a delete is permanent, which Explorer
// confirms first. `volume_error`: the volume lookup failed (the path is not
// judged; the delete itself reports why).
bool RecycleBinAvailable(const std::wstring& path, DWORD* volume_error = nullptr);
// Shell objects one recycle task reuses from item to item: the Recycle Bin,
// the transfer source of the folder last recycled from and that folder's
// volume check. Same STA thread as the deletes; release before CoUninitialize.
class RecycleCache {
public:
    RecycleCache();
    ~RecycleCache();
    RecycleCache(const RecycleCache&) = delete;
    RecycleCache& operator=(const RecycleCache&) = delete;
    struct State;
    State& state() { return *state_; }
private:
    std::unique_ptr<State> state_;
};
ShellTransferResult DeleteWithCurrentToken(const std::vector<std::wstring>& sources,
    bool permanent, HWND owner, std::atomic<bool>& cancel, ShellTransferProgress progress = {},
    RecycleCache* cache = nullptr);
}
