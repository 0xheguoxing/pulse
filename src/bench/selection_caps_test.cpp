// selection_caps_test.cpp - what a selection can do by what it is: drives get
// a rename and their own verbs but no cut / copy / delete, a server's shares and
// the Network view get no file commands, Del in the Network view only unpins.
#include "../app/selection_caps.h"

#include <cstdio>
#include <memory>
#include <vector>

// Nothing here is saved (PlacesCatalog::persist is off).
namespace pulse::app { std::wstring GetPulseDataDir() { return {}; } }

using namespace pulse;
using namespace pulse::app;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

fs::DirEntry Entry(const wchar_t* name, const wchar_t* full, uint8_t drive_type = 0, bool locked = false) {
    fs::DirEntry e;
    e.name = name;
    e.full_path = full;
    e.is_dir = true;
    e.attrs = FILE_ATTRIBUTE_DIRECTORY;
    e.drive_type = drive_type;
    e.drive_locked = locked;
    return e;
}

void Fill(Tab& tab, const wchar_t* path, std::vector<fs::DirEntry> entries) {
    tab.current_path = path;
    tab.SetSnapshot(std::make_shared<const std::vector<fs::DirEntry>>(std::move(entries)));
    tab.ClearSelection();
}

} // namespace

int main() {
    Check(IsUncServerRoot(L"\\\\nas") && IsUncServerRoot(L"\\\\nas\\") && IsUncServerRoot(L"\\\\?\\UNC\\nas"),
          "\\\\server is a server root");
    Check(!IsUncServerRoot(L"\\\\nas\\share") && !IsUncServerRoot(L"C:\\") && !IsUncServerRoot(L"\\\\?\\C:\\x"),
          "a share, a drive or a long local path is not a server root");
    Check(IsUncShareRoot(L"\\\\nas\\share") && IsUncShareRoot(L"\\\\nas\\share\\"),
          "\\\\server\\share is a share root");
    Check(!IsUncShareRoot(L"\\\\nas") && !IsUncShareRoot(L"\\\\nas\\share\\dir") && !IsUncShareRoot(L"D:\\share"),
          "a server, a folder in a share or a local path is not a share root");

    PlacesCatalog places;
    places.persist = false;
    Tab tab;

    // Ordinary folder: everything, rename for one item only.
    Fill(tab, L"C:\\work", {Entry(L"a", L"C:\\work\\a"), Entry(L"b", L"C:\\work\\b")});
    Check(ComputeSelectionCaps(tab, &places).kind == SelectionKind::None, "nothing selected: no commands");
    tab.ToggleSelect(0);
    SelectionCaps caps = ComputeSelectionCaps(tab, &places);
    Check(caps.kind == SelectionKind::Files && caps.cut && caps.copy && caps.del && caps.rename && caps.tags,
          "one file: cut, copy, delete, rename, tags");
    tab.ToggleSelect(1);
    caps = ComputeSelectionCaps(tab, &places);
    Check(caps.cut && caps.copy && caps.del && !caps.rename, "two files: no single rename");

    // This PC: drives.
    Fill(tab, L"", {Entry(L"Win11 (C:)", L"C:\\", DRIVE_FIXED), Entry(L"USB (H:)", L"H:\\", DRIVE_REMOVABLE),
                    Entry(L"share (Y:)", L"Y:\\", DRIVE_REMOTE), Entry(L"Locked (L:)", L"L:\\", DRIVE_FIXED, true),
                    Entry(L"DVD (E:)", L"E:\\", DRIVE_CDROM)});
    tab.ToggleSelect(0);
    caps = ComputeSelectionCaps(tab, &places);
    Check(caps.kind == SelectionKind::Drives && !caps.cut && !caps.copy && !caps.del && caps.rename && !caps.tags,
          "local drive: label rename only, no tags");
    Check(!caps.any_removable && !caps.any_remote && !caps.any_locked, "local drive: no drive verbs");
    tab.ClearSelection();
    tab.ToggleSelect(1);
    caps = ComputeSelectionCaps(tab, &places);
    Check(caps.any_removable && caps.rename && !caps.del, "USB stick: eject and rename, no delete");
    tab.ClearSelection();
    tab.ToggleSelect(2);
    caps = ComputeSelectionCaps(tab, &places);
    Check(caps.any_remote && caps.rename && !caps.copy, "mapped drive: disconnect and rename, no copy");
    tab.ClearSelection();
    tab.ToggleSelect(3);
    caps = ComputeSelectionCaps(tab, &places);
    Check(caps.any_locked && caps.all_locked && !caps.rename, "locked drive: unlock, no rename");
    tab.ClearSelection();
    tab.ToggleSelect(4);
    caps = ComputeSelectionCaps(tab, &places);
    Check(caps.any_removable && !caps.rename, "optical drive: eject, no label rename");
    tab.ToggleSelect(1);
    caps = ComputeSelectionCaps(tab, &places);
    Check(caps.kind == SelectionKind::Drives && !caps.rename && !caps.del, "two drives: no rename, no delete");

    // A server's share list.
    Fill(tab, L"\\\\nas", {Entry(L"share", L"\\\\nas\\share")});
    tab.ToggleSelect(0);
    caps = ComputeSelectionCaps(tab, &places);
    Check(caps.kind == SelectionKind::NetworkShares && !caps.cut && !caps.copy && !caps.del && !caps.rename && !caps.tags,
          "share in \\\\server: no file commands or tags");

    // Inside a share: ordinary files again.
    Fill(tab, L"\\\\nas\\share", {Entry(L"dir", L"\\\\nas\\share\\dir")});
    tab.ToggleSelect(0);
    Check(ComputeSelectionCaps(tab, &places).kind == SelectionKind::Files, "folder inside a share: files");

    // Network view: Del unpins Pulse's own pins only.
    places.PinNetwork(L"\\\\nas\\pinned", L"");
    Fill(tab, L"pulse:networks", {Entry(L"pinned", L"\\\\nas\\pinned"), Entry(L"system", L"\\\\nas\\system")});
    tab.ToggleSelect(1);
    caps = ComputeSelectionCaps(tab, &places);
    Check(caps.kind == SelectionKind::NetworkPlaces && !caps.del && !caps.any_network_pin && !caps.rename && !caps.tags,
          "system network location: nothing to delete or tag");
    tab.ToggleSelect(0);
    caps = ComputeSelectionCaps(tab, &places);
    Check(caps.any_network_pin && caps.del && !caps.cut && !caps.copy, "pinned location: Del unpins, no copy");

    // Recycle Bin: permanent delete only.
    Fill(tab, L"pulse:recycle", {Entry(L"old", L"C:\\old")});
    tab.ToggleSelect(0);
    caps = ComputeSelectionCaps(tab, &places);
    Check(caps.kind == SelectionKind::Recycle && caps.del && !caps.cut && !caps.copy && !caps.rename && !caps.tags,
          "recycle item: delete only");

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
