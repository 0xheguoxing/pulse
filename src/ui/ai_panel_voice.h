// ai_panel_voice.h - 语音输入 (third batch, flow V in
// plans/ai/Pulse_AI助手_第三批_操控Pulse与语音.html): the 语音设置 menu over
// the mic's chevron (.vmenu) and the 语音识别连接 card at the panel's foot
// (.vc). AiPanelView draws them (ai_panel_voice.cpp); app/ai_assistant_voice.cpp
// fills these views and places the card's address and key EDITs over
// VoiceFieldRect().
#pragma once

#include "ai_panel_view.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pulse::ui {

struct AiVoiceMenuView {
    std::wstring heading;            // 语音识别
    std::wstring service;            // 硅基流动 / 还没有语音连接
    std::wstring service_sub;        // SenseVoiceSmall · 已连接
    bool connected = false;          // the ok dot (else a grey one)
    std::wstring manage;             // 管理连接 / 添加
    std::wstring mic_label;          // 麦克风
    std::wstring mic_name;           // 耳机 (USB Audio) / 没有找到
    bool mic_missing = false;        // the name in text3
    std::wstring pause_heading;      // 停顿多久算说完
    std::array<std::wstring, 3> pauses;   // 1 秒 · 2 秒 · 手动
    int pause = 0;
    std::wstring note;               // 只在点麦克风时录音…
};

enum class AiVoiceTest : uint8_t { None, Running, Ok, Bad };

struct AiVoiceCardView {
    std::wstring title;              // 添加语音识别连接 / 语音识别连接
    std::wstring sub;
    std::wstring service_label;      // 服务
    std::array<std::wstring, 3> services;
    int service = 0;
    std::wstring url_label;          // 地址
    std::wstring url_text;           // drawn when the EDIT is not there
    std::wstring url_placeholder;
    bool url_focused = false;
    bool url_edit = false;           // the EDIT shows the text
    std::wstring key_label;          // API key
    std::wstring key_note;           // 只存在本机 (lock)
    std::wstring key_text;           // sk-ylxq••••7f3a; drawn when the EDIT is not there
    std::wstring key_placeholder;    // 粘贴 API key
    bool key_focused = false;
    bool key_edit = false;
    std::wstring model_label;        // 模型
    std::wstring model_note;         // 已从服务读取
    std::wstring model;              // FunAudioLLM/SenseVoiceSmall (empty: the placeholder)
    std::wstring model_placeholder;  // 填好 key 后从服务读取
    bool model_ready = false;        // the dropdown opens
    AiVoiceTest test = AiVoiceTest::None;
    std::wstring test_text;          // 正在读取模型并测试识别… / 已连接 · 测试音频识别用了 0.6 秒
    std::wstring privacy;            // 只在你点麦克风时录音…
    std::wstring remove;             // 移除连接 (empty: not shown)
    std::wstring cancel, save;
    bool can_save = false;
};

// The view's voice state (private to ai_panel_view.cpp / ai_panel_voice.cpp).
struct AiPanelView::VoiceGeom {
    // The menu's pop (shown keeps the last view while it fades out).
    bool menu_shown = false;
    uint64_t menu_at = 0;
    std::shared_ptr<const AiVoiceMenuView> menu_view;
    D2D1_RECT_F menu_rect{};
    // The card's rise.
    bool card_shown = false;
    uint64_t card_at = 0;
    std::shared_ptr<const AiVoiceCardView> card_view;
    D2D1_RECT_F card_rect{};
    // Hit rects of the last frame, the card's and the menu's.
    struct Hit {
        D2D1_RECT_F rect;
        AiPanelHit hit;
    };
    std::vector<Hit> card_hits, menu_hits;
    D2D1_RECT_F url_text{}, key_text{};
    bool url_place = false, key_place = false;
    D2D1_RECT_F model_box{}, mic_box{};
    // The composer while listening: the indicator did not fit after the text.
    bool extra_line = false;
    // The waveform as drawn: each bar eases toward its level (.12 s).
    std::array<float, 14> shown{};
    uint64_t shown_at = 0;
};

} // namespace pulse::ui
