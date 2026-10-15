// drive_label.h — the name File Explorer gives a drive, without " (C:)".
#pragma once
#include <string>

namespace pulse::fs {

// A mapped network drive shows the label it was renamed to (the Shell keeps it
// under MountPoints2\_LabelFromReg), else "share (\\server)" like File
// Explorer -- never the label of the server's own volume. Any other drive
// shows its volume label, or `fallback` when it has none. Reads only local
// state (no network or media access). Defined in fs_enum.cpp.
std::wstring DriveLabel(wchar_t letter, const std::wstring& volume_label, const std::wstring& fallback);

}
