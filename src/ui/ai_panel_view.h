// ai_panel_view.h — The Pulse 助手 panel (plans/ai/pulse_ai_assistant_v2.html).
//
// Drawn in the main window by MainRenderer. Docked (the default) it is a
// column right of the sidebar rail and the toolbar and panes move over;
// floating it is a card over the lower left of the window. The view owns the
// open/close motion and the geometry of the last frame (hit tests, where the
// app puts the composer's EDIT); everything it shows comes from the view model.
#pragma once
#include "ai_panel_icons.h"
#include "FluentTokens.h"

#include <d2d1_1.h>
#include <dwrite.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::ui {

class Compositor;

enum class AiPanelMode : uint8_t { Docked = 0, Floating = 1 };

// The design's file-type tints (icon tiles, plan groups, result chips).
enum class AiTint : uint8_t { Image, Document, App, Video, Archive, Neutral };

enum class AiSendState : uint8_t { Dim, Ready, Busy };

enum class AiPanelPart : uint8_t {
    None,
    Panel,          // anywhere else on the panel (swallows the click)
    NewChat,
    Mode,           // docked <-> floating
    Close,
    Suggestion,     // index
    ContextChip,
    ContextRemove,
    AddFiles,
    ModelButton,
    Send,
    Input,
    Menu,           // the model menu's background
    MenuModel,      // index
    MenuEffort,     // index 0..3
    MenuManage,
    Conversation,
    // Conversation parts; index = block index in AiPanelViewModel::blocks.
    ThinkToggle,
    StepsToggle,
    PlanToggle,
    ConfirmDeny,
    ConfirmAccept,
    ResultUndo,
    ResultShow,
    ActCopy,
    ActRetry,
    ActThumb,
};

struct AiPanelHit {
    AiPanelPart part = AiPanelPart::None;
    int index = -1;
    bool operator==(const AiPanelHit&) const = default;
};

struct AiSuggestionView {
    AiIcon icon = AiIcon::Move;
    AiTint tint = AiTint::Image;
    std::wstring title;
    std::wstring detail;
};

struct AiModelView {
    std::wstring name;
    std::wstring detail;
};

// One piece of the conversation (v2.css "messages" … "result card").
enum class AiBlockKind : uint8_t {
    User,       // bubble on the right, optional context chip above it
    Thinking,   // ✦ 思考中 / 已思考 4 秒 · 高, body scrolls while running
    Text,       // the answer: paragraphs, "- " bullets, **bold**
    Steps,      // tool timeline (card); collapses to one summary line
    Plan,       // 整理方案 card with groups
    Confirm,    // amber confirmation, decided → one quiet line
    Result,     // 已整理 … with folder tags, 撤销 / 在 Pulse 中查看
    Acts,       // copy / retry / thumb + meta under a finished answer
    Working,    // ✦ shimmer line while waiting for the next thing
    Note,       // centred line between rules (你撤销了这次整理)
};

enum class AiStepState : uint8_t { Running, Done, Failed };

struct AiStepView {
    AiIcon icon = AiIcon::Search;
    std::wstring title;
    std::wstring arg;                 // grey pill after the title
    std::wstring sub;
    AiStepState state = AiStepState::Running;
    float progress = -1.0f;           // 0..1 shows the bar
    uint64_t born_ms = 0;             // msg-in
    uint64_t done_ms = 0;             // the check pops
};

struct AiGroupView {
    AiIcon icon = AiIcon::Image;
    AiTint tint = AiTint::Image;
    std::wstring name;
    std::wstring count;               // 3 个
    std::wstring list;                // a、b、c (ellipsized)
    std::wstring badge;               // 新建 (empty: none)
    uint64_t in_ms = 0;               // 0: not shown yet (keeps its space)
};

struct AiTagView {
    AiTint tint = AiTint::Neutral;
    std::wstring text;                // 图片 3
};

struct AiBlockView {
    AiBlockKind kind = AiBlockKind::Text;
    uint32_t id = 0;                  // stable while the block lives (motion)
    uint64_t born_ms = 0;             // msg-in; 0 = no entrance
    std::wstring text;                // User / Text / Working / Note
    std::wstring context;             // User: chip above the bubble
    bool streaming = false;           // Text: caret after the last character
    bool running = false;             // Thinking / Working
    bool open = false;                // Thinking / Steps (collapsed) expanded
    bool collapsed = false;           // Steps: summary line; Plan: header only
    std::wstring label;               // Thinking header; Steps summary; Plan title
    std::wstring meta;                // Thinking " · 高"; Plan / Acts meta
    std::wstring seconds;             // Thinking: "3 秒" while running
    std::wstring title, sub, hint;    // Confirm / Result
    std::vector<AiStepView> steps;
    std::vector<AiGroupView> groups;
    std::vector<AiTagView> tags;
    std::wstring deny, accept;        // Confirm buttons; Result: undo, show
    bool done = false;                // Confirm decided; Result undone
};

// The status bar chip (left end): "✦ AI" plus what the assistant is doing.
enum class AiChipState : uint8_t { Idle, Busy, Waiting, Done };

struct AiStatusChipView {
    std::wstring label;               // AI
    std::wstring text;                // 思考中 / 读取中 9/12 / 等你确认 / 完成
    bool words = false;               // room for `text`
    AiChipState state = AiChipState::Idle;
    float progress = -1.0f;           // 0..1 shows the small bar
    bool open = false;
    bool unread = false;
    bool hover = false;
    bool pressed = false;
    bool dark = false;
};

// Chip geometry shared by layout and drawing (DIPs scaled): padding 0 10 0 8,
// spark 15, gap 6, bar 38.
float AiStatusChipWidth(float label_w, float text_w, bool words, bool progress, float scale);

struct AiPanelViewModel {
    bool dark = false;
    bool high_contrast = false;
    // Header.
    std::wstring title;               // Pulse 助手
    // Empty state.
    bool empty = true;
    std::wstring greeting;            // 早上好，想整理点什么？
    std::wstring intro;
    std::vector<AiSuggestionView> suggestions;
    std::wstring safe_note;
    // Conversation (shown when !empty).
    std::vector<AiBlockView> blocks;
    // Composer.
    bool context_visible = false;
    std::wstring context_text;        // 下载 · 12 项
    bool composer_focused = false;
    bool composer_empty = true;
    int composer_lines = 1;           // the EDIT's line count (input grows to 6)
    float composer_line_px = 0.0f;    // the EDIT's line height (0: the design's 21)
    // While the model menu covers the input the EDIT hides and its text is
    // drawn here instead.
    bool draw_composer_text = false;
    std::wstring composer_text;
    std::wstring placeholder;
    std::wstring add_files;
    std::wstring model_label;         // deepseek-flash
    std::wstring effort_label;        // " · 高" (empty when thinking is off)
    AiSendState send = AiSendState::Dim;
    // Model menu.
    bool menu_open = false;
    std::wstring model_heading, effort_heading;
    std::vector<AiModelView> models;
    int model = 0;
    std::array<std::wstring, 4> effort_names;
    std::wstring effort_detail;       // the selected effort's line
    int effort = 2;
    bool connected = false;
    std::wstring connection_text;     // 已连接 api.deepseek.com / 未连接
    std::wstring manage_text;         // 管理连接
    // Pointer.
    AiPanelHit hover;
    AiPanelHit pressed;
};

class AiPanelView {
public:
    AiPanelView() = default;
    AiPanelView(const AiPanelView&) = delete;
    AiPanelView& operator=(const AiPanelView&) = delete;
    ~AiPanelView();

    // Opens, closes or moves the panel; `animate` is ignored when the system
    // turns animations off. The docked width and the floating fade each run
    // to their own target, so switching modes crosses over.
    void SetState(bool open, AiPanelMode mode, bool animate);
    bool IsOpen() const noexcept { return open_; }
    // New chat: the empty state's spark breathes again.
    void Rewake(uint64_t now_ms);
    AiPanelMode Mode() const noexcept { return mode_; }
    // Something of the panel is on screen (open, or still closing).
    bool Visible() const noexcept { return dock_value_ > 0.0f || float_value_ > 0.0f; }
    bool FloatingVisible() const noexcept { return float_value_ > 0.0f; }

    // Once per frame before layout: samples the motion. True while it runs.
    bool Tick(uint64_t now_ms);
    bool Animating() const noexcept { return dock_anim_ || float_anim_ || conv_anim_; }
    // Decorative motion of the open panel (the breathing spark, spinners).
    bool WantsDecorFrame() const noexcept { return (open_ && (decor_ || conv_decor_)) || chip_decor_; }

    // Width of the docked column this frame, in px (0 when floating or closed).
    float DockWidth(float window_width, float rail_width, float scale) const;
    // The full docked width the column grows to, in px.
    static float DockTargetWidth(float window_width, float rail_width, float scale);

    // `column`: docked column this frame (may be narrower than the content
    // while it opens). `floating_area`: the area a floating card sits in
    // (main area between title bar and status bar).
    // `column_content_width`: the full docked width the content is laid out
    // at (the column clips it while it grows).
    void Draw(Compositor* compositor, const AiPanelViewModel& vm, const Theme& theme,
              const D2D1_RECT_F& column, float column_content_width, const D2D1_RECT_F& floating_area,
              float scale, uint64_t now_ms);

    // Conversation scrolling (px). Follows the newest block until the user
    // scrolls up; scrolling back to the end follows again.
    void ScrollBy(float dy);
    void ScrollToEnd();
    bool Following() const noexcept { return follow_; }

    // Last drawn geometry.
    AiPanelHit HitTest(float x, float y) const;
    bool Contains(float x, float y) const;
    D2D1_RECT_F InputRect() const noexcept { return input_rect_; }
    // The composer's input is visible and settled (place the EDIT there).
    bool InputPlaced() const noexcept { return input_placed_; }
    D2D1_COLOR_F CardColor() const noexcept { return card_color_; }
    D2D1_COLOR_F TextColor() const noexcept { return text_color_; }
    D2D1_COLOR_F PlaceholderColor() const noexcept { return text3_color_; }
    D2D1_RECT_F ModelButtonRect() const noexcept { return model_button_; }

    // The status bar chip; its motion (spinning spark, amber ring) asks for
    // decorative frames like the open panel does.
    void DrawStatusChip(Compositor* compositor, const Theme& theme, const AiStatusChipView& chip,
                        const D2D1_RECT_F& rect, float scale, uint64_t now_ms);

    void ReleaseDeviceResources();

    struct Palette;   // colors of one frame (ai_panel_view.cpp)

private:
    struct Formats {
        float scale = 0.0f;
        float font_scale = 0.0f;
        IDWriteTextFormat* title = nullptr;     // 14.5 semibold
        IDWriteTextFormat* greeting = nullptr;  // 21 semibold
        IDWriteTextFormat* body = nullptr;      // 13.5
        IDWriteTextFormat* body13 = nullptr;    // 13
        IDWriteTextFormat* body13b = nullptr;   // 13 semibold
        IDWriteTextFormat* caption = nullptr;    // 12
        IDWriteTextFormat* small125 = nullptr;  // 12.5
        IDWriteTextFormat* small125b = nullptr; // 12.5 semibold
        IDWriteTextFormat* model = nullptr;     // 12.5 medium
        IDWriteTextFormat* heading = nullptr;   // 11.5 semibold
        IDWriteTextFormat* body_b = nullptr;    // 13.5 semibold
        IDWriteTextFormat* caption_b = nullptr; // 12 semibold
        IDWriteTextFormat* badge = nullptr;     // 10.5 semibold
        IDWriteTextFormat* meta = nullptr;      // 11.5
        void Release();
    };

    void EnsureFormats(Compositor* compositor, float scale);
    void DrawContent(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal,
                     const D2D1_RECT_F& panel, float scale, uint64_t now_ms);
    void DrawHeader(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal,
                    const D2D1_RECT_F& panel, float scale);
    void DrawEmptyState(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal,
                        const D2D1_RECT_F& area, float scale, uint64_t now_ms);
    void DrawConversation(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal,
                          const D2D1_RECT_F& area, float scale, uint64_t now_ms);
    float ComposerHeight(const AiPanelViewModel& vm, float scale) const;
    void DrawComposer(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal,
                      const D2D1_RECT_F& box, float scale, uint64_t now_ms);
    void DrawModelMenu(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal,
                       float scale, uint64_t now_ms);

    bool open_ = false;
    AiPanelMode mode_ = AiPanelMode::Docked;
    bool animate_ = true;
    // Docked width and floating fade, 0..1, each with its own spring.
    float dock_value_ = 0.0f, dock_from_ = 0.0f;
    float float_value_ = 0.0f, float_from_ = 0.0f;
    bool dock_anim_ = false, float_anim_ = false;
    uint64_t dock_start_ = 0, float_start_ = 0;
    uint64_t opened_at_ = 0;
    // Model menu pop.
    bool menu_shown_ = false;
    uint64_t menu_at_ = 0;
    bool decor_ = false;
    bool chip_decor_ = false;
    bool chip_waiting_ = false;
    uint64_t chip_since_ = 0;
    bool EnsureBrush(ID2D1DeviceContext* dc);

    // Geometry of the last frame (px).
    bool drawn_ = false;
    D2D1_RECT_F panel_rect_{};     // clipped to what is visible
    D2D1_RECT_F head_rect_{};
    D2D1_RECT_F new_button_{}, mode_button_{}, close_button_{};
    std::vector<D2D1_RECT_F> suggestion_rects_;
    D2D1_RECT_F composer_rect_{};
    D2D1_RECT_F context_chip_{}, context_remove_{};
    D2D1_RECT_F input_rect_{};
    bool input_placed_ = false;
    D2D1_RECT_F add_files_{}, model_button_{}, send_button_{};
    D2D1_RECT_F menu_rect_{};
    std::vector<D2D1_RECT_F> menu_model_rects_;
    std::array<D2D1_RECT_F, 4> menu_effort_rects_{};
    D2D1_RECT_F menu_manage_{};
    D2D1_RECT_F conversation_rect_{};
    D2D1_COLOR_F card_color_{1, 1, 1, 1};
    D2D1_COLOR_F text_color_{0, 0, 0, 1};
    D2D1_COLOR_F text3_color_{0.5f, 0.5f, 0.5f, 1};

    // Conversation motion and scrolling.
    struct Tween {
        float from = 0.0f, to = 0.0f;
        uint64_t start = 0;
        uint32_t ms = 0;
        bool used = false;
    };
    // Eases the value under `key` toward `target` over `ms`; keys are block
    // id * 8 + slot. Unused keys are dropped after each frame.
    float TweenTo(uint64_t key, float target, uint32_t ms, uint64_t now_ms);
    std::vector<std::pair<uint64_t, Tween>> tweens_;
    bool conv_anim_ = false;    // tweens, entrances, scrolling: full rate
    bool conv_decor_ = false;   // spinners, caret, shimmer: decorative rate
    float scroll_ = 0.0f;       // px from the top of the content
    float scroll_from_ = 0.0f, scroll_to_ = 0.0f;
    uint64_t scroll_start_ = 0;
    bool scroll_anim_ = false;
    bool follow_ = true;
    float content_h_ = 0.0f, view_h_ = 0.0f;
    struct ConvHit {
        D2D1_RECT_F rect;
        AiPanelHit hit;
    };
    std::vector<ConvHit> conv_hits_;

    Formats formats_;
    ID2D1SolidColorBrush* brush_ = nullptr;
    ID2D1DeviceContext* brush_dc_ = nullptr;
};

} // namespace pulse::ui
