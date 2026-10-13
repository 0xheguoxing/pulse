// ctx_menu_timing.h - one record per right-click menu, for the before/after
// comparison of the context-menu work (docs: 右键菜单优化方案 §4.6).
//
// `grew` counts display rebuilds that changed the row count while the menu was
// on screen, either way: the visible "menu gets longer" the user reported, and
// a cached menu that shrinks to a partial answer and grows back. It must stay
// 0. `trace` lists every rebuild as ms-since-open:rows.
#pragma once
#include <cstdint>
#include <string>

namespace pulse::app {

struct CtxMenuTiming {
    std::wstring key;                // static verb key: .dwg, :folder, :drive, :bg
    bool background = false;
    uint32_t input_to_open_ms = 0;   // input message time -> popup requested
    uint32_t rows_open = 0;          // top-level rows painted in the first frame
    bool seeded = false;             // shell rows came from the per-type cache
    bool live_at_open = false;       // the host's final answer was in before open
    int32_t live_after_open_ms = -1; // final answer after open; -1 = not while open
    uint32_t refreshes = 0;          // display updates applied while open
    uint32_t grew = 0;               // updates that changed the row count
    std::string trace;               // "120:17 240:23" (capped, ASCII)
    uint32_t rows_final = 0;
    bool shell_command = false;      // the chosen row was an Explorer row
};

inline std::string CtxTimingKeyUtf8(const std::wstring& key) {
    std::string out;
    out.reserve(key.size());
    for (wchar_t ch : key) {
        // Keys are extensions or ":name" tags; anything else is escaped so the
        // record stays valid JSON whatever the registry holds.
        if (ch >= 0x20 && ch < 0x7F && ch != L'"' && ch != L'\\') {
            out.push_back(static_cast<char>(ch));
        } else {
            char buffer[8]{};
            const unsigned value = static_cast<unsigned>(ch) & 0xFFFFu;
            static const char* const hex = "0123456789abcdef";
            buffer[0] = '\\';
            buffer[1] = 'u';
            buffer[2] = hex[(value >> 12) & 0xF];
            buffer[3] = hex[(value >> 8) & 0xF];
            buffer[4] = hex[(value >> 4) & 0xF];
            buffer[5] = hex[value & 0xF];
            out.append(buffer, 6);
        }
    }
    return out;
}

// One JSON line (with the trailing newline) for the logs folder.
inline std::string FormatCtxMenuTiming(const CtxMenuTiming& t, uint64_t tick_ms) {
    auto flag = [](bool on) { return on ? "true" : "false"; };
    return std::string("{\"event\":\"ctx_menu\",\"tick_ms\":") + std::to_string(tick_ms) +
        ",\"key\":\"" + CtxTimingKeyUtf8(t.key) + "\",\"background\":" + flag(t.background) +
        ",\"input_to_open_ms\":" + std::to_string(t.input_to_open_ms) +
        ",\"rows_open\":" + std::to_string(t.rows_open) +
        ",\"seeded\":" + flag(t.seeded) +
        ",\"live_at_open\":" + flag(t.live_at_open) +
        ",\"live_after_open_ms\":" + std::to_string(t.live_after_open_ms) +
        ",\"refreshes\":" + std::to_string(t.refreshes) +
        ",\"grew\":" + std::to_string(t.grew) +
        ",\"rows_final\":" + std::to_string(t.rows_final) +
        ",\"trace\":\"" + t.trace + "\"" +
        ",\"shell_command\":" + flag(t.shell_command) + "}\n";
}

} // namespace pulse::app
