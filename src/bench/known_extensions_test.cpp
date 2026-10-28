// 文件扩展名 off (ui/known_extensions.h): which names lose their extension.
// Reads HKEY_CLASSES_ROOT only; .txt and .lnk are registered on every
// Windows, the made-up extension is not.
#include "../ui/known_extensions.h"

#include <cstdio>

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

}  // namespace

int main() {
    using pulse::ui::HiddenExtensionLength;
    Check(!pulse::ui::HideKnownExtensions(), "extensions are shown by default");
    Check(HiddenExtensionLength(L"notes.txt", false) == 0, "nothing is hidden while extensions are shown");

    const uint64_t before = pulse::ui::KnownExtensionsRevision();
    pulse::ui::SetHideKnownExtensions(true);
    Check(pulse::ui::KnownExtensionsRevision() != before, "switching moves the revision (rows rebuild)");
    const uint64_t on = pulse::ui::KnownExtensionsRevision();
    pulse::ui::SetHideKnownExtensions(true);
    Check(pulse::ui::KnownExtensionsRevision() == on, "setting the same value keeps the revision");

    Check(HiddenExtensionLength(L"notes.txt", false) == 4, "a registered type loses its extension");
    Check(HiddenExtensionLength(L"NOTES.TXT", false) == 4, "the extension is matched without case");
    Check(HiddenExtensionLength(L"v1.2.final.txt", false) == 4, "only the last extension goes");
    Check(HiddenExtensionLength(L"Shortcut.lnk", false) == 4, "a shortcut loses .lnk, as in File Explorer");
    Check(HiddenExtensionLength(L"data.zzqxunregistered", false) == 0, "an unregistered type keeps its extension");
    Check(HiddenExtensionLength(L"archive.txt", true) == 0, "a folder keeps its whole name");
    Check(HiddenExtensionLength(L".txt", false) == 0, "a name that is only an extension is kept");
    Check(HiddenExtensionLength(L"README", false) == 0, "a name without a dot is kept");
    Check(HiddenExtensionLength(L"trailing.", false) == 0, "a trailing dot is kept");
    Check(HiddenExtensionLength(L"notes.txt", false) == 4, "a second lookup gives the same answer (cached)");

    pulse::ui::SetHideKnownExtensions(false);
    Check(HiddenExtensionLength(L"notes.txt", false) == 0, "switching back shows extensions again");

    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
