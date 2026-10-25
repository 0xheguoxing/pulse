// ai_panel_conn.h - 管理连接, the model menu unfolded into the connection
// card (plans/ai/Pulse_AI助手_管理连接.html, form B).
//
// AiPanelView draws it in the model menu's place, anchored to the same
// corner; the size eases between the menu and the card (220 ms) and follows
// the card while it grows (a test result, 高级). The key and address fields
// are EDITs app/ai_assistant_conn.cpp places over ConnFieldRect(); the card
// draws their frames, and their text whenever the EDIT is not there.
#pragma once

#include "ai_panel_view.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pulse::ui {

enum class AiConnStatus : uint8_t { Off, Ok, Bad };
enum class AiConnTest : uint8_t { None, Running, Ok, Bad };
enum class AiConnKey : uint8_t { Empty, Typing, Saved };

struct AiConnModelRow {
    std::wstring name;   // deepseek-flash
    std::wstring desc;   // 快，日常整理
    std::wstring tag;    // 默认 (empty: none)
};

struct AiConnView {
    std::wstring title;            // 管理连接
    std::wstring service;          // DeepSeek
    std::wstring service_sub;      // api.deepseek.com · OpenAI 格式
    AiConnStatus status = AiConnStatus::Off;
    std::wstring status_text;      // 未连接 / 已连接 / 密钥无效

    std::wstring key_label;        // API Key
    std::wstring key_link;         // 在哪里获取？
    AiConnKey key = AiConnKey::Empty;
    bool key_error = false;        // red frame (the last test failed)
    bool key_focused = false;
    bool key_edit = false;         // the EDIT shows the text (else the card draws it)
    bool key_revealed = false;     // the eye is on
    std::wstring key_mask;         // Saved: sk-…9832; Typing: ••••…9832 (or the text, revealed)
    std::wstring key_placeholder;  // 粘贴 sk- 开头的密钥
    std::wstring paste;            // 粘贴
    std::wstring change;           // 更换
    std::wstring key_help;

    AiConnTest test = AiConnTest::None;
    std::wstring test_title;       // 连接正常 / 密钥无效（401）/ 正在连接…
    std::wstring test_detail;      // " · 286 毫秒" after the bold title
    std::wstring test_sub;

    std::wstring models_heading;   // 可用模型
    std::vector<AiConnModelRow> models;   // shown after a good test

    bool advanced_open = false;
    std::wstring advanced;         // 高级
    std::wstring url_label;        // 接口地址
    std::wstring url_help;
    bool url_focused = false;
    bool url_edit = false;
    std::wstring url_text;         // drawn when the EDIT is not there
    std::wstring url_placeholder;  // https://api.deepseek.com

    std::wstring safe;             // 密钥用 Windows 加密…

    std::wstring remove;           // 移除密钥 (empty: hidden)
    std::wstring test_button;      // 测试连接
    std::wstring save_button;      // 保存 / 完成
    bool can_test = false;
    bool can_save = false;
};

// The view's per-card state (private to ai_panel_view.cpp / ai_panel_conn.cpp).
struct AiPanelView::ConnGeom {
    // The card last shown (drawn on while the menu fades out).
    std::shared_ptr<const AiConnView> view;
    // Morph: the shown box eases from `from` to the target size.
    bool card = false;             // the card (else the menu) is the target
    float menu_scroll = 0.0f;      // the menu's model list, when it is longer than its box
    float menu_scroll_max = 0.0f;
    // A short window: the card's body scrolls between its head and foot.
    float card_view_h = 0.0f;      // the card's shown height (px)
    float card_scroll = 0.0f;
    float card_scroll_max = 0.0f;  // 0: the whole card fits
    bool sized = false;
    float w = 0.0f, h = 0.0f;      // shown size (px)
    float from_w = 0.0f, from_h = 0.0f;
    float to_w = 0.0f, to_h = 0.0f;
    uint64_t at = 0;
    bool moving = false;
    float content = 1.0f;          // the content's fade-in, 0..1
    uint64_t content_at = 0;
    // Advanced chevron.
    bool adv_open = false;
    uint64_t adv_at = 0;
    // Hit rects of the last frame (px).
    struct Hit {
        D2D1_RECT_F rect;
        AiPanelPart part;
    };
    std::vector<Hit> hits;
    D2D1_RECT_F rect{};            // the card (when it is the target)
    D2D1_RECT_F key_text{}, url_text{};
    bool key_place = false, url_place = false;
    // Text formats the panel's set lacks.
    float fmt_scale = 0.0f, fmt_font_scale = 0.0f;
    IDWriteTextFormat* mono = nullptr;     // 13 Consolas
    IDWriteTextFormat* link = nullptr;     // 12 medium
    IDWriteTextFormat* tag = nullptr;      // 11 semibold
    void ReleaseFormats();
    ~ConnGeom() { ReleaseFormats(); }
};

} // namespace pulse::ui
