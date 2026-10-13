#pragma once

// System (Explorer-wide) context menu entries: read-only enumeration (S1) and
// enable/disable (S2). The disable markers are the ones ContextMenuManager
// writes, so items disabled by either tool show up correctly in the other.

#include <windows.h>

#include <string>
#include <string_view>
#include <vector>

namespace pulse::app::sysctx {

enum class Kind { Verb, ShellEx, DragDrop, ShellNew, SendTo, OpenWith };
enum class Root { User, Machine, File };

// Where HKCR's two layers live. Tests point both at a scratch key under HKCU.
struct Roots {
    HKEY user = HKEY_CURRENT_USER;
    std::wstring user_classes = L"Software\\Classes";
    HKEY machine = HKEY_LOCAL_MACHINE;
    std::wstring machine_classes = L"Software\\Classes";
    std::wstring sendto_dir;     // empty: FOLDERID_SendTo
    // Under `user`; holds <.ext>\UserChoice. Empty: ignore user choices.
    std::wstring file_exts = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts";
    bool resolve_strings = true; // SHLoadIndirectString / CLSID names
    bool check_owner = true;     // TrustedInstaller lookups (slow on big scans)
};

enum class SceneMode { Menu, DragDrop, ShellNew, SendTo, OpenWith };

struct Scene {
    std::wstring id;                       // "file", "folder", "ext:.png", ...
    std::vector<std::wstring> class_keys;  // relative to the classes root
    SceneMode mode = SceneMode::Menu;
};

const std::vector<Scene>& BuiltinScenes();
// ".png" -> SystemFileAssociations\.png plus the extension's ProgID.
Scene ExtensionScene(const Roots& roots, std::wstring_view ext);
Scene PerceivedScene(std::wstring_view type); // "image" -> SystemFileAssociations\image

struct Item {
    Kind kind = Kind::Verb;
    Root root = Root::User;
    std::wstring scene;
    std::wstring class_key;  // "Directory\\Background"
    std::wstring key_name;   // verb / handler key name / extension / file name
    std::wstring key_path;   // current location relative to the classes root
    std::wstring display;    // resolved menu text or handler name
    std::wstring clsid;
    std::wstring file;       // resolved exe/dll when it exists
    std::wstring command;
    std::wstring icon;       // raw Icon value
    bool enabled = true;
    bool dash_marker = false;      // -ContextMenuHandlers / -ShellNew
    bool velocity_hidden = false;  // HideBasedOnVelocityId
    bool legacy_disable = false;
    bool programmatic_only = false;
    bool command_flags_hidden = false;
    bool no_open_with = false;
    bool extended = false;         // Shift only
    bool is_protected = false;
    bool trusted_installer = false;
    bool missing = false;          // command/handler points at a file that is gone
};

std::vector<Item> Enumerate(const Roots& roots, const Scene& scene);

// ---- S2: enable / disable -------------------------------------------------

enum class Action { Disable, Enable };

// Verb markers an op adds (Disable) or removes (Enable). PlanOp records the
// exact set so Inverse() undoes a change byte for byte.
enum : unsigned {
    kMarkVelocity = 1,      // HideBasedOnVelocityId
    kMarkLegacy = 2,        // LegacyDisable
    kMarkProgrammatic = 4,  // ProgrammaticAccessOnly
    kMarkFlags = 8,         // CommandFlags bit 0x8
};

struct Op {
    Action action = Action::Disable;
    Kind kind = Kind::Verb;
    Root root = Root::User;
    std::wstring key_path;  // as reported by Item::key_path
    bool planned = false;
    unsigned mask = 0;      // verbs only, valid when planned
};

enum class OpResult { Ok, NotFound, Conflict, AccessDenied, Unsupported, Failed };

Op MakeOp(const Item& item, Action action);
// Reads the current verb values and fixes op.mask (no-op for other kinds).
void PlanOp(const Roots& roots, Op& op);
// Location of the item after `op` ran (shellex keys move between
// ContextMenuHandlers and -ContextMenuHandlers).
std::wstring KeyPathAfter(const Op& op);
Op Inverse(const Op& op);
OpResult Apply(const Roots& roots, const Op& op);
bool CanWriteWithoutElevation(const Op& op);

// Machine-wide batches go to pulse_integration.exe --system-ctx-apply <file>.
std::wstring SerializeBatch(const std::vector<Op>& ops);
// Rejects anything but the supported kinds/actions on keys under the classes
// root (no "..", no absolute paths, only shell/shellex locations).
bool ParseBatch(std::wstring_view text, std::vector<Op>& ops);
bool IsAllowedKeyPath(Kind kind, std::wstring_view key_path);

// The helper's report: one result per batch line, in batch order.
std::wstring SerializeResults(const std::vector<OpResult>& results);
bool ParseResults(std::wstring_view text, std::vector<OpResult>& results);
const wchar_t* ResultName(OpResult result);

// pulse_integration.exe --system-ctx-apply <batch> --result <file>: machine
// ops only. Returns the process exit code (0 all ok, 1 some failed, 2 bad
// input). Notifies the shell when anything changed.
int RunElevatedBatch(const std::wstring& batch_path, const std::wstring& result_path);

// .reg text (REGEDIT5) with the current contents of the keys the ops touch.
std::wstring ExportReg(const Roots& roots, const std::vector<Op>& ops);

// Apply Windows 10 1703+ velocity marker? (false on older systems / tests).
void SetVelocitySupportedForTesting(int supported); // -1 auto, 0 no, 1 yes

} // namespace pulse::app::sysctx
