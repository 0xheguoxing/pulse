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
#include <memory>
#include <string>
#include <vector>

namespace pulse::ui {

class Compositor;
struct AiConnView;   // 管理连接 (ai_panel_conn.h)

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
    ErrAction,      // index = block index * 4 + button (AiBlockView::actions)
    CutRetry,       // the 重试 link on the 回答中途断开了 strip
    // Search conditions card (first batch, flow A).
    QueryChipRemove,   // index = block index * 16 + chip (the chip's ×)
    QueryAdd,          // + 条件
    FollowItem,        // index = block index * 8 + item (接着做 chips)
    // Rename plan card and its confirmation (first batch, flow B).
    RenameToggle,      // the card's header (folds the card)
    RenameMore,        // 还有 4 个 / 收起
    ConfirmExtra,      // 在批量重命名中调整… on the confirmation
    TaskToggle,        // 任务计划 card header (flow C)
    // Second batch, flow D (space).
    DiskToggle,        // 本地磁盘 card header
    CleanToggle,       // 清理建议 card header
    CleanCheck,        // index = block index * 32 + row (the category's box)
    CleanLink,         // index = block index * 32 + row (查看)
    CleanFile,         // index = (block index * 32 + row) * 32 + file (a file's box)
    ResultPurge,       // 永久删除这些… on a result card
    // 管理连接 card (the model menu unfolded).
    ConnCard,       // the card's background (swallows the click)
    ConnBack,
    ConnClose,
    ConnLink,       // 在哪里获取？
    ConnPaste,
    ConnEye,
    ConnChange,
    ConnAdvanced,
    ConnTest,
    ConnSave,
    ConnRemove,
    ConnKeyField,
    ConnUrlField,
    // The bubble above the chip once the panel is closed.
    PeekCard,
    PeekClose,
    PeekUndo,
    PeekOpen,
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
    Error,      // the turn failed: tile + title + sub (+ detail, hint) + buttons
    Cut,        // amber strip under a partial answer: 回答中途断开了 · 重试
    Query,      // 搜索条件 card: condition chips, count, where the results are
    Follow,     // 接着做: chips that put a next step into the composer
    Rename,     // 重命名方案 card: rule, old → new rows, checks, foot
    Task,       // 任务计划 card: numbered steps (they are the progress), summary
    Disk,       // 本地磁盘 (C:) card: used / free, segmented bar, the biggest places
    Clean,      // 清理建议 card: categories with boxes, reasons, 查看, the total
};

// Error card tone: Err needs the user (red, red border), Warn is temporary
// (amber), Accent is not an error (没连接模型).
enum class AiErrTone : uint8_t { Err, Warn, Accent };

// What an error card button does (app/ai_assistant_chat.cpp).
enum class AiErrAct : uint8_t { Retry, ManageConn, TopUp, SwitchModel, CopyDetail, Connect,
                               // A paused task (flow C): 重试 / 跳过它继续 / 撤销已做的.
                               TaskRetry, TaskSkip, TaskUndo };

struct AiErrButton {
    AiErrAct act = AiErrAct::Retry;
    AiIcon icon = AiIcon::Retry;
    std::wstring text;
    bool primary = false;
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
    std::wstring bold;                // Query locations: the count after the name
};

// A search condition on the Query card: 名称 含「发票」 ×.
struct AiQueryChipView {
    std::wstring key;                 // 名称 (accent, semibold)
    std::wstring value;               // 含「发票」
    bool fixed = false;               // no × (所有位置, the last condition)
    uint64_t in_ms = 0;               // pops in (0: no entrance)
    uint64_t out_ms = 0;              // removed: shrinks and fades, then goes
};

// 接着做: one chip that fills the composer.
struct AiFollowView {
    AiIcon icon = AiIcon::Package;
    std::wstring text;                // 打包这 7 个
}; 

// 重命名方案 (flow B). The rule as chips (tokens), literal text (mono) and
// plain words (换成, 开头加 in text3).
struct AiRuleToken {
    std::wstring text;
    bool token = false;
    bool word = false;
};

// One old → new row; the new stem in parts (token parts in accent).
struct AiRenameRow {
    std::wstring old_name;
    std::vector<AiRuleToken> parts;
    std::wstring ext;                 // ".jpg" (shown in text3)
    bool warn = false;                // amber dot: no date taken
    bool sample = false;              // shown while the list is folded
    uint64_t in_ms = 0;               // slides in (0: not yet)
};

struct AiCheckView {
    std::wstring text;
    bool warn = false;
};

// 任务计划 (flow C): one numbered step. The card is the task's progress.
enum class AiTaskState : uint8_t { Todo, Running, Done, Paused };

struct AiTaskStepView {
    std::wstring title;               // 新建月份文件夹
    std::wstring arg;                 // grey pill: 文档 › 发票
    std::wstring tag;                 // 只读 · 已完成 / 跳过 1 个
    bool tag_warn = false;            // amber tag (else ok on accent-soft)
    std::wstring desc;                // 6 个：2026-01、02…
    AiTaskState state = AiTaskState::Todo;
    float progress = 0.0f;            // the bar, while running or paused
    uint64_t state_ms = 0;            // the circle's colour change
};

// The summary line under the steps: icon, then before **bold** after.
struct AiTaskSumView {
    AiIcon icon = AiIcon::FolderPlus;
    std::wstring before, bold, after;
};

// 任务完成: one line per step (warn = amber, a step that skipped items).
struct AiResultLine {
    bool warn = false;
    std::wstring before, bold, after;
};

// 本地磁盘 (C:) card (second batch, flow D). Numbers in GB; the used and
// free figures count from *_from to their value over .9 s from count_ms.
struct AiDiskRowView {
    bool file = false;                // hiberfil.sys: the file icon
    bool sys = false;                 // grey bar, 不动 in the sub line
    std::wstring name;                // 下载 / AppData › Local
    std::wstring sub;                 // 用户 › SS / 系统 · 不动
    std::wstring value;               // 31.2
    float frac = 0.0f;                // bar width against the biggest row
};

struct AiDiskView {
    float used_gb = 0.0f, free_gb = 0.0f;
    float used_from = 0.0f, free_from = 0.0f;
    uint64_t count_ms = 0;
    bool free_ok = false;             // after a purge the free figure turns ok
    std::wstring used_of;             // GB 已用 / 238 GB
    std::wstring free_unit;           // GB 可用
    float seg[5]{};                   // share of the drive: 系统 程序 用户 其他 回收站中
    std::wstring legend[4];           // 系统 52.2 …
    std::wstring legend_recycle;      // 回收站中 11.7 (shown while seg[4] > 0)
    std::wstring legend_free;         // 可用 (the free figure follows)
    std::wstring top_label;           // 最占地方的
    std::vector<AiDiskRowView> rows;
    uint64_t grow_ms = 0;             // the bar grows, then the rows come in
};

// The small disk box on a result card: 「C 盘可用 17.0 GB」 (or 「17.0 GB →
// 28.7 GB」 counting), the used / recycled bar and its legend.
struct AiMiniDiskView {
    bool on = false;
    std::wstring label;               // C 盘可用
    std::wstring from;                // 17.0 GB → (empty: no arrow)
    float value = 0.0f, value_from = 0.0f;
    uint64_t count_ms = 0;
    std::wstring unit;                // GB
    float used = 0.0f;                // share of the drive in use, without the bin part
    float recycle = 0.0f, recycle_from = 0.0f;   // hatched part, tweens from recycle_from
    std::wstring leg_used, leg_recycle, leg_free;
};

// 清理建议 card: one category per row.
struct AiCleanFileView {
    std::wstring name;                // DockerDesktopInstaller.exe
    std::wstring size;                // 612 MB
    bool on = true;                   // off: grey, 留着
    AiIcon icon = AiIcon::File;
    AiTint tint = AiTint::Neutral;
};

struct AiCleanRowView {
    AiIcon icon = AiIcon::Install;
    AiTint tint = AiTint::App;
    std::wstring title;               // 装过的安装包
    std::wstring tag;                 // 看一眼 (amber), may be empty
    std::wstring count, size;         // 23 · 6.8 (个 / GB come from the card)
    std::wstring desc;                // 下载 · 对应的程序都已经装好了
    std::wstring link;                // 查看
    bool on = true;
    bool open = false;                // the file list below is unfolded
    std::vector<AiCleanFileView> files;
    std::wstring more;                // 还有 18 个 · 右边已列出全部
    uint64_t in_ms = 0;               // fades in
    uint64_t bump_ms = 0;             // the numbers changed
};

struct AiCleanView {
    std::vector<AiCleanRowView> rows;
    std::wstring unit_n, unit_gb;     // 个 / GB after a row's numbers
    std::wstring sum_pre, sum_mid, sum_unit;   // 选中 … 个 · … GB
    std::wstring sum_n, sum_gb;
    uint64_t sum_bump_ms = 0;
    std::wstring safe;                // 移到回收站，可撤销
    std::wstring note;                // 不碰系统文件和程序目录。…
    std::wstring keep;                // 留着 (a file left out)
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
    // Error: tone, tile icon, the service's own words (mono box) and buttons.
    AiErrTone tone = AiErrTone::Err;
    AiIcon icon = AiIcon::AlertCircle;
    std::wstring detail;
    std::vector<AiErrButton> actions;
    uint64_t gone_ms = 0;             // Error / Cut: fading out (retry)
    // Query: title 搜索条件, meta "Pulse 索引 · 0.02 秒", tags = locations,
    // label = "个文件 · 4.2 MB" after the count, hint = 本机重新搜索… (shown
    // from re_ms), add = 条件. Follow: label 接着做 and the chips.
    std::vector<AiQueryChipView> chips;
    std::vector<AiFollowView> follow;
    std::wstring count;               // 7
    std::wstring add;
    uint64_t bump_ms = 0;             // the count changed (bump)
    uint64_t re_ms = 0;               // 0: no rerun note
    // Confirm: a link button on the left (在批量重命名中调整…) and the line
    // that opens under the card while it is hovered.
    std::wstring extra, extra_tip;
    // Result: the icon of the second button (在 Pulse 中查看 / 在批量重命名中打开).
    AiIcon accept_icon = AiIcon::Open;
    // Rename: label 重命名方案, meta 8 个文件, rule_label 规则 + rule, hint
    // 扩展名不变, rows (folded: the sample ones, then `more`; open: all,
    // then `less`), checks, sub = the foot line. `collapsed` folds the card.
    std::wstring rule_label, more, less;
    std::vector<AiRuleToken> rule;
    std::vector<AiRenameRow> rows;
    std::vector<AiCheckView> checks;
    // Task: label 任务计划, meta 4 步 · 第 1 步已完成, the steps, the summary
    // and `safe` (不删除 · 不覆盖, shield in ok). Result: `lines` under the
    // title. Error (a paused task): `file` + `file_note` (.e-f row); `done`
    // folds it to the chosen line. Confirm: the hint's icon.
    std::vector<AiTaskStepView> task;
    std::vector<AiTaskSumView> sums;
    std::wstring safe;
    std::vector<AiResultLine> lines;
    std::wstring file, file_note;
    AiIcon hint_icon = AiIcon::Eye;
    // Second batch, flow D. Disk: label 本地磁盘 (C:), meta Pulse 索引 · 0.4
    // 秒. Clean: label 清理建议, meta 只读分析…. Confirm: `tag` (删除类, red)
    // after the title, `hint2` a second hint (ok check, or amber warn when
    // `hint2_warn`), `danger` the red card, `accept_off` a disabled primary.
    // Result: `mini` the disk box, `purge` a third (red) button, `gone` (the
    // batch was deleted for good: no buttons, no disk box, no warn lines),
    // `freed` (no buttons; the ok icon).
    AiDiskView disk;
    AiCleanView clean;
    std::wstring tag;
    std::wstring hint2;
    bool hint2_warn = false;
    bool danger = false;
    bool accept_off = false;
    AiMiniDiskView mini;
    std::wstring purge;
    bool gone = false;
    bool freed = false;
};

// The status bar chip (left end): "✦ AI" plus what the assistant is doing.
enum class AiChipState : uint8_t { Idle, Busy, Waiting, Done, Error, Warn };

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

// .peek: above the status bar chip when the panel is closed and the
// assistant finished (or needs you); 撤销 and 打开对话.
struct AiPeekView {
    bool on = false;
    std::wstring title;               // Pulse 助手 · 完成
    std::wstring text;                // 已把 12 个文件整理到 5 个新文件夹：…
    std::wstring undo;                // 撤销 (empty: no button)
    std::wstring open;                // 打开对话
    // A failed turn: the spark takes the tone; `undo` is then 重试 (primary,
    // retry icon) or 连接模型.
    bool failed = false;
    AiErrTone tone = AiErrTone::Err;
};

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
    // 管理连接: non-null while the menu is unfolded into the connection card.
    std::shared_ptr<const AiConnView> conn;
    // Pointer.
    AiPanelHit hover;
    AiPanelHit pressed;
    AiPeekView peek;
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
    bool Animating() const noexcept { return dock_anim_ || float_anim_ || conv_anim_ || peek_anim_; }
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
    // The composer's text face (13.5), for the LumaText-presented EDIT.
    IDWriteTextFormat* ComposerFormat() const noexcept { return formats_.body; }
    D2D1_RECT_F ModelButtonRect() const noexcept { return model_button_; }
    // 管理连接 card: the text box of the key (0) or address (1) field when
    // the card is settled and that field takes an EDIT this frame.
    bool ConnFieldRect(int field, D2D1_RECT_F& rect) const;

    // The status bar chip; its motion (spinning spark, amber ring) asks for
    // decorative frames like the open panel does.
    void DrawStatusChip(Compositor* compositor, const Theme& theme, const AiStatusChipView& chip,
                        const D2D1_RECT_F& rect, float scale, uint64_t now_ms);
    // The peek bubble over the chip (`chip`: its rect this frame); it fades
    // and rises in and out on its own (ai_panel_peek.cpp).
    void DrawPeek(Compositor* compositor, const AiPanelViewModel& vm, const Theme& theme, const D2D1_RECT_F& chip,
                  float scale, uint64_t now_ms);
    AiPanelHit PeekHitTest(float x, float y) const;
    bool PeekVisible() const noexcept { return peek_value_ > 0.0f; }

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
        IDWriteTextFormat* mono = nullptr;      // 11.5 Cascadia Mono / Consolas
        IDWriteTextFormat* mono125 = nullptr;   // 12.5 (the rename rule's literal text)
        IDWriteTextFormat* num22 = nullptr;     // 22 semibold (the Query card's count)
        IDWriteTextFormat* num20 = nullptr;     // 20 semibold (the disk card's used figure)
        IDWriteTextFormat* num16 = nullptr;     // 16 semibold (a result card's free figure)
        IDWriteTextFormat* num15 = nullptr;     // 15 semibold (the cleanup total)
        IDWriteTextFormat* num14 = nullptr;     // 14 semibold (the disk card's free figure)
        IDWriteTextFormat* small11 = nullptr;   // 11 (sub lines, the small legend)
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
    // ai_panel_conn.cpp. Lays out (draw = false: only measures) the card in
    // `card` (its top-left; the width is the card's) and returns its height.
    float DrawConnCard(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal,
                       const D2D1_RECT_F& card, float scale, uint64_t now_ms, bool draw);

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
    // Peek bubble.
    float peek_value_ = 0.0f;         // 0..1 shown
    bool peek_target_ = false;
    uint64_t peek_t0_ = 0;            // its last turn started
    float peek_from_ = 0.0f;
    bool peek_anim_ = false;
    D2D1_RECT_F peek_rect_{}, peek_close_{}, peek_undo_{}, peek_open_{};
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
    // 管理连接 card: morph between the menu and the card, its hit rects and
    // its own text formats (ai_panel_conn.h).
    struct ConnGeom;
    std::shared_ptr<ConnGeom> conn_;
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
    bool was_empty_ = false;    // the welcome was drawn last frame (it starts at the top)
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
