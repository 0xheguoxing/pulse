// ai_assistant_chat.h - Pulse 助手's conversation: the blocks the panel shows,
// the status chip's words, and the driver that fills them.
//
// A2 drives it with a local demo that plays the design's script on the open
// folder (read only, nothing moves); A3 replaces the driver with the DeepSeek
// agent loop. Internal to src/app (not part of AppState's header).
#pragma once

#include "../ui/ai_panel_view.h"
#include "../ui/ui_view_model.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace pulse {
struct AppState;

struct AiChat {
    std::vector<ui::AiBlockView> blocks;
    uint32_t next_id = 1;
    bool busy = false;            // a turn runs (send shows stop)
    bool noted = false;           // the demo note is in this conversation
    std::wstring last_prompt;

    // The status bar chip while the assistant works (Idle: not ours).
    ui::StatusAiState chip = ui::StatusAiState::Idle;
    std::wstring chip_text;
    float chip_progress = -1.0f;
    bool unread = false;

    // Thinking block whose seconds count up (0: none).
    uint32_t think_id = 0;
    uint64_t think_start = 0;
    uint64_t turn_start = 0;

    // Driver: steps run in order, each after its delay; a step returns the
    // ms until it runs again (streaming) or -1 when it is done.
    struct Step {
        uint32_t delay = 0;
        std::function<int(AppState&)> run;
    };
    std::deque<Step> queue;
    uint64_t next_at = 0;
    // Paused at a confirmation card until the user answers.
    uint32_t confirm_id = 0;
    std::function<void(AppState&, bool)> on_confirm;
    // The latest result card's undo.
    uint32_t result_id = 0;
    std::function<void(AppState&)> on_undo;

    ui::AiBlockView* Find(uint32_t id);
    ui::AiBlockView& Add(ui::AiBlockKind kind, uint64_t now_ms);
};

AiChat& Chat(AppState& s);
// Starts a turn for `prompt`; `context` is the composer's folder chip text.
void AiChatSubmit(AppState& s, const std::wstring& prompt, const std::wstring& context);
void AiChatStop(AppState& s);
void AiChatReset(AppState& s);
// Conversation parts the panel reports (block index into blocks).
void AiChatActivate(AppState& s, ui::AiPanelPart part, int block);

} // namespace pulse
