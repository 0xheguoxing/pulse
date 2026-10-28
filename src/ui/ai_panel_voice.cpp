// ai_panel_voice.cpp - 语音输入 (plans/ai/Pulse_AI助手_第三批_操控Pulse与语音.html,
// flow V): the mic button (.vsel), the composer while it listens (.rec,
// .asr), the 语音设置 menu (.vmenu) and the 语音识别连接 card (.vc). Sizes
// are the design's CSS pixels used as DIPs.
#include "ai_panel_voice.h"
#include "ai_panel_draw.h"

#include "ui_compositor.h"
#include "ui_motion.h"

#include <algorithm>
#include <cmath>

namespace pulse::ui {
using namespace ai_draw;
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr uint64_t kMenuMs = 220;     // the model menu's pop
constexpr uint64_t kCardMs = 350;     // .vc: transform .35 s, opacity .28 s
constexpr uint64_t kShakeMs = 450;    // .mic.shake
constexpr uint64_t kShakeRedMs = 720; // the icon stays red for 60% of 1.2 s
constexpr uint64_t kRingMs = 1400;    // micring

void Spin(const Ctx& c, D2D1_POINT_2F mid, float d, D2D1_COLOR_F track, D2D1_COLOR_F accent, uint64_t now) {
    const float r = d * 0.5f - c.s;
    c.dc->DrawEllipse(D2D1::Ellipse(mid, r, r), c.B(track), 1.6f * c.s);
    const float a = static_cast<float>(now % 900) / 900.0f * 2.0f * kPi - kPi * 0.5f;
    ID2D1Factory* f = nullptr;
    c.dc->GetFactory(&f);
    if (!f) return;
    ID2D1PathGeometry* g = nullptr;
    if (SUCCEEDED(f->CreatePathGeometry(&g))) {
        ID2D1GeometrySink* k = nullptr;
        if (SUCCEEDED(g->Open(&k))) {
            const float a0 = a - kPi * 0.25f, a1 = a + kPi * 0.25f;
            k->BeginFigure(D2D1::Point2F(mid.x + r * std::cos(a0), mid.y + r * std::sin(a0)), D2D1_FIGURE_BEGIN_HOLLOW);
            k->AddArc(D2D1::ArcSegment(D2D1::Point2F(mid.x + r * std::cos(a1), mid.y + r * std::sin(a1)),
                                       D2D1::SizeF(r, r), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            k->EndFigure(D2D1_FIGURE_END_OPEN);
            k->Close();
            k->Release();
            c.dc->DrawGeometry(g, c.B(accent), 1.6f * c.s);
        }
        g->Release();
    }
    f->Release();
}

// .shimmer: text2 with an accent band sweeping across every 1.8 s.
void Shimmer(const Ctx& c, const std::wstring& text, IDWriteTextFormat* fmt, float x, float y, float h,
             const AiPanelView::Palette& pal, uint64_t now, bool moving) {
    const float tw = c.Measure(text, fmt);
    const D2D1_RECT_F r = D2D1::RectF(x, y, x + tw + 2.0f, y + h);
    c.Text(text, fmt, r, pal.text2);
    if (!moving || tw <= 0.0f) return;
    const float t = static_cast<float>(now % 1800) / 1800.0f;
    const float gx = x - 1.2f * tw * (1.0f - 2.2f * t);
    const D2D1_GRADIENT_STOP stops[] = {{0.35f, Alpha(pal.accent, 0.0f)}, {0.5f, pal.accent}, {0.65f, Alpha(pal.accent, 0.0f)}};
    ID2D1GradientStopCollection* col = nullptr;
    if (FAILED(c.dc->CreateGradientStopCollection(stops, 3, &col))) return;
    ID2D1LinearGradientBrush* mask = nullptr;
    if (SUCCEEDED(c.dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(gx, 0.0f), D2D1::Point2F(gx + 2.2f * tw, 0.0f)), col, &mask))) {
        c.dc->PushLayer(D2D1::LayerParameters1(r, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1.0f,
                                               mask, D2D1_LAYER_OPTIONS1_NONE),
                        nullptr);
        c.Text(text, fmt, r, pal.accent);
        c.dc->PopLayer();
        mask->Release();
    }
    col->Release();
}

// The fill of `group` showing only inside `part` (a half-rounded hover).
void FillPart(const Ctx& c, const D2D1_RECT_F& group, const D2D1_RECT_F& part, float radius, D2D1_COLOR_F color) {
    c.dc->PushAxisAlignedClip(part, D2D1_ANTIALIAS_MODE_ALIASED);
    c.Fill(group, radius, color);
    c.dc->PopAxisAlignedClip();
}

// .wave i: 3 px bars, gap 2, radius 2, centred on `mid`.
float Bars(const Ctx& c, const float* levels, size_t n, float x, float mid, float max_h, D2D1_COLOR_F color) {
    const float s = c.s;
    for (size_t i = 0; i < n; ++i) {
        const float h = (3.0f + std::clamp(levels[i], 0.0f, 1.0f) * (max_h - 3.0f)) * s;
        const D2D1_RECT_F r = D2D1::RectF(x, mid - h * 0.5f, x + 3.0f * s, mid + h * 0.5f);
        c.Fill(r, 1.5f * s, color);
        x += 5.0f * s;
    }
    return static_cast<float>(n) * 5.0f * s - 2.0f * s;
}

} // namespace

int AiPanelView::InputLines(const AiPanelViewModel& vm) const {
    if (!vm.voice.listening) return vm.composer_lines;
    if (vm.composer_empty) return 2;
    return vm.composer_lines + (voice_ && voice_->extra_line ? 1 : 0);
}

bool AiPanelView::VoiceFieldRect(int field, D2D1_RECT_F& rect) const {
    if (!voice_ || !drawn_ || !open_) return false;
    if (field == 0 && voice_->url_place) { rect = voice_->url_text; return true; }
    if (field == 1 && voice_->key_place) { rect = voice_->key_text; return true; }
    return false;
}

bool AiPanelView::VoiceAnchor(AiPanelPart part, D2D1_RECT_F& rect) const {
    if (!voice_) return false;
    rect = part == AiPanelPart::VoiceModel ? voice_->model_box : part == AiPanelPart::VoiceMicPick ? voice_->mic_box : D2D1_RECT_F{};
    return rect.right > rect.left;
}

AiPanelHit AiPanelView::VoiceHitTest(float x, float y) const {
    if (!voice_) return {};
    const VoiceGeom& g = *voice_;
    if (In(g.card_rect, x, y)) {
        for (auto it = g.card_hits.rbegin(); it != g.card_hits.rend(); ++it)
            if (In(it->rect, x, y)) return it->hit;
        return {AiPanelPart::VoiceCard, -1};
    }
    if (In(g.menu_rect, x, y)) {
        for (auto it = g.menu_hits.rbegin(); it != g.menu_hits.rend(); ++it)
            if (In(it->rect, x, y)) return it->hit;
        return {AiPanelPart::VoiceMenu, -1};
    }
    return {};
}

void AiPanelView::DrawMic(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal, float s, uint64_t now_ms) {
    if (!vm.voice.available) return;
    Ctx c{compositor, compositor->Dc(), brush_, s};
    const AiVoiceView& v = vm.voice;
    const bool moving = animate_ && motion::SystemAnimationsEnabled();
    // .mic.shake: ±3 px at 20/40/60/80%, the icon red for the first 60% of 1.2 s.
    float dx = 0.0f;
    bool red = false;
    if (v.shake_at && now_ms >= v.shake_at) {
        const uint64_t t = now_ms - v.shake_at;
        if (moving && t < kShakeMs) {
            const float p = static_cast<float>(t) / static_cast<float>(kShakeMs);
            constexpr float keys[] = {0.0f, -3.0f, 3.0f, -3.0f, 3.0f, 0.0f};
            const float at = p * 5.0f;
            const int i = (std::min)(4, static_cast<int>(at));
            dx = (keys[i] + (keys[i + 1] - keys[i]) * (at - static_cast<float>(i))) * s;
        }
        if (t < kShakeRedMs) {
            red = true;
            decor_ = true;
        }
    }
    const D2D1_RECT_F mic = D2D1::RectF(mic_button_.left + dx, mic_button_.top, mic_button_.right + dx, mic_button_.bottom);
    const D2D1_RECT_F group = D2D1::RectF(mic.left, mic.top, mic_menu_.right + dx, mic.bottom);
    const float icon_x = (mic.left + mic.right) * 0.5f - 8.0f * s;
    const float icon_y = (mic.top + mic.bottom) * 0.5f - 8.0f * s;
    if (v.listening) {
        // .composer.listen .mic: accent, white icon, radius 8; ::after a 2 px
        // ring at inset -3 that grows (.92 → 1.3) and fades (.55 → 0) in 1.4 s.
        if (moving) {
            const float t = static_cast<float>(now_ms % kRingMs) / static_cast<float>(kRingMs);
            const float k = 0.92f + 0.38f * t;
            const float a = 0.55f * (1.0f - t);
            const D2D1_POINT_2F mid = D2D1::Point2F((mic.left + mic.right) * 0.5f, (mic.top + mic.bottom) * 0.5f);
            const float hw = ((mic.right - mic.left) * 0.5f + 3.0f * s) * k;
            const float hh = ((mic.bottom - mic.top) * 0.5f + 3.0f * s) * k;
            const D2D1_RECT_F ring = D2D1::RectF(mid.x - hw, mid.y - hh, mid.x + hw, mid.y + hh);
            c.Stroke(ring, 10.0f * s * k, Alpha(pal.accent, a), 2.0f * s);
            decor_ = true;
        }
        c.Fill(mic, 8.0f * s, pal.accent);
        c.Icon(AiIcon::Mic, icon_x, icon_y, 16.0f * s, pal.on_accent);
        return;   // .composer.listen .vch: opacity 0
    }
    const bool hot = vm.hover.part == AiPanelPart::Mic;
    if (hot) FillPart(c, group, mic, 8.0f * s, pal.hover);
    c.Icon(AiIcon::Mic, icon_x, icon_y, 16.0f * s, red ? pal.bad : hot ? pal.text : pal.text2);
    const D2D1_RECT_F chev = D2D1::RectF(mic_menu_.left + dx, mic_menu_.top, mic_menu_.right + dx, mic_menu_.bottom);
    const bool chev_hot = vm.hover.part == AiPanelPart::MicMenu || v.menu_open;
    if (chev_hot) FillPart(c, group, chev, 8.0f * s, pal.hover);
    c.Icon(AiIcon::ChevronDown, (chev.left + chev.right) * 0.5f - 5.5f * s, (chev.top + chev.bottom) * 0.5f - 5.5f * s,
           11.0f * s, v.menu_open ? pal.text : pal.text3);
}

void AiPanelView::DrawVoiceInput(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal, float line,
                                 float s, uint64_t now_ms) {
    if (!voice_) voice_ = std::make_shared<VoiceGeom>();
    VoiceGeom& g = *voice_;
    Ctx c{compositor, compositor->Dc(), brush_, s};
    const AiVoiceView& v = vm.voice;
    const bool moving = animate_ && motion::SystemAnimationsEnabled();
    const D2D1_RECT_F r = input_rect_;
    // The bars ease toward the levels (.cp-in .rec .wave i: transition height .12s).
    {
        const float dt = g.shown_at ? static_cast<float>(now_ms - g.shown_at) : 1000.0f;
        const float k = moving ? std::clamp(dt / 120.0f, 0.0f, 1.0f) : 1.0f;
        for (size_t i = 0; i < g.shown.size(); ++i) g.shown[i] += (v.levels[i] - g.shown[i]) * k;
        g.shown_at = now_ms;
        decor_ = true;   // the waveform and the timer move while listening
    }
    if (vm.composer_empty) {
        g.extra_line = false;
        if (v.recognizing) {
            Shimmer(c, v.recognizing_text, formats_.small125, r.left, r.top, line, pal, now_ms, moving);
            return;
        }
        // .rec: 14 bars (height 18), gap 10, the time (12 text2), the service
        // pushed right (11.5 text3); .rec2 under it (11.5 text3, margin-top 2).
        const float mid = r.top + line * 0.5f;
        float x = r.left + Bars(c, g.shown.data(), g.shown.size(), r.left, mid, 18.0f, pal.accent) + 10.0f * s;
        const float tw = c.Measure(v.time, formats_.caption);
        c.Text(v.time, formats_.caption, D2D1::RectF(x, r.top, x + tw + 1.0f, r.top + line), pal.text2);
        x += tw + 10.0f * s;
        const float sw = c.Measure(v.service, formats_.meta);
        if (r.right - sw >= x)
            c.Text(v.service, formats_.meta, D2D1::RectF(r.right - sw, r.top, r.right + 1.0f, r.top + line), pal.text3);
        c.Text(v.hint, formats_.meta, D2D1::RectF(r.left, r.top + line, r.right, r.top + line * 2.0f), pal.text3);
        return;
    }
    // The words so far, then 识别中… or a small waveform where they end.
    TextOptions o;
    o.wrap = true;
    o.line_height = line;
    const float width = r.right - r.left;
    IDWriteTextLayout* layout = c.Layout(vm.composer_text, formats_.body, width, 10000.0f, o);
    float end_x = 0.0f, end_y = 0.0f;
    if (layout) {
        c.dc->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
        compositor->DrawTextLayout(D2D1::Point2F(r.left, r.top), layout, c.B(pal.text));
        c.dc->PopAxisAlignedClip();
        DWRITE_HIT_TEST_METRICS m{};
        FLOAT px = 0.0f, py = 0.0f;
        const UINT32 last = static_cast<UINT32>(vm.composer_text.size()) - 1;
        if (SUCCEEDED(layout->HitTestTextPosition(last, TRUE, &px, &py, &m))) {
            end_x = px;
            end_y = std::floor((m.top + m.height * 0.5f) / line) * line;   // the box's middle is inside its line
        }
        layout->Release();
    }
    const float ind_w = v.recognizing ? c.Measure(v.recognizing_text, formats_.small125) + 4.0f * s : 6.0f * s + 8.0f * 5.0f * s;
    const bool wrap = end_x + ind_w > width;
    if (wrap != g.extra_line) {
        g.extra_line = wrap;
        conv_anim_ = true;   // the input grows or shrinks on the next frame
    }
    const float ix = r.left + (wrap ? 0.0f : end_x + (v.recognizing ? 4.0f : 6.0f) * s);
    const float iy = r.top + end_y + (wrap ? line : 0.0f);
    if (iy + line > r.bottom + 0.5f) return;   // until the input has grown
    if (v.recognizing) {
        Shimmer(c, v.recognizing_text, formats_.small125, ix, iy, line, pal, now_ms, moving);
    } else {
        Bars(c, g.shown.data() + (g.shown.size() - 8), 8, ix, iy + line * 0.5f, 18.0f, pal.accent);
    }
}

void AiPanelView::DrawVoiceMenu(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal, float s,
                                uint64_t now_ms) {
    if (!voice_) voice_ = std::make_shared<VoiceGeom>();
    VoiceGeom& g = *voice_;
    g.menu_hits.clear();
    g.menu_rect = {};
    g.mic_box = {};
    const bool want = vm.voice.menu != nullptr;
    if (want) g.menu_view = vm.voice.menu;
    if (want != g.menu_shown) {
        g.menu_shown = want;
        g.menu_at = now_ms;
    }
    const float t = animate_ ? std::clamp(static_cast<float>(now_ms - g.menu_at) / static_cast<float>(kMenuMs), 0.0f, 1.0f) : 1.0f;
    const float p = g.menu_shown ? EaseDesign(t) : 1.0f - EaseDesign(t);
    if (p <= 0.0f || !g.menu_view || !vm.voice.available) {
        if (!g.menu_shown) g.menu_view.reset();
        return;
    }
    if (t < 1.0f) decor_ = true;
    const AiVoiceMenuView& v = *g.menu_view;
    Ctx c{compositor, compositor->Dc(), brush_, s};
    // .vmenu: width 312, right -40 and bottom 36 from the .vsel, padding 6.
    const float w = 312.0f * s;
    const float pad = 6.0f * s;
    const float head_h = 25.0f * s;
    const float conn_h = 51.0f * s;     // .vrow: padding 8 10, two lines (18 + 17)
    const float sep_h = 13.0f * s;
    const float mic_h = 40.0f * s;      // .vrow with the .vsel2 box (24)
    const float seg_h = 34.0f * s;      // .seg3: margin-top 2, padding 3, items 26
    const float note_tx = 10.0f * s + 13.0f * s + 7.0f * s;
    const float note_w = w - pad * 2 - note_tx - 10.0f * s;
    const float note_h = c.MeasureWrapped(v.note, formats_.caption, note_w, 17.0f * s);
    const float h = pad * 2 + head_h + conn_h + sep_h + mic_h + head_h + seg_h + 6.0f * s + 1.0f * s + 8.0f * s + note_h + 4.0f * s;
    float right = (std::min)(mic_menu_.right + 40.0f * s, panel_rect_.right - 8.0f * s);
    float left = right - w;
    if (left < panel_rect_.left + 8.0f * s) {
        left = panel_rect_.left + 8.0f * s;
        right = left + w;
    }
    const float bottom = mic_button_.bottom - 36.0f * s;
    const D2D1_RECT_F box = D2D1::RectF(std::round(left), std::round(bottom - h), std::round(right), std::round(bottom));
    if (want) g.menu_rect = box;
    const D2D1_POINT_2F origin = D2D1::Point2F(box.left + (box.right - box.left) * 0.8f, box.bottom);
    const float k = 0.97f + 0.03f * p;
    D2D1_MATRIX_3X2_F saved;
    c.dc->GetTransform(&saved);
    c.dc->SetTransform(D2D1::Matrix3x2F::Scale(k, k, origin) * D2D1::Matrix3x2F::Translation(0.0f, (1.0f - p) * 6.0f * s) * saved);
    c.dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                           D2D1::IdentityMatrix(), p, nullptr, D2D1_LAYER_OPTIONS1_NONE),
                    nullptr);
    const float radius = 14.0f * s;
    c.Shadow(box, radius, 40.0f * s, 16.0f * s, pal.shadow);
    c.Fill(box, radius, pal.card);
    c.Stroke(box, radius, pal.line);
    auto hit = [&](const D2D1_RECT_F& r, AiPanelPart part, int index = -1) {
        if (want) g.menu_hits.push_back({r, {part, index}});
    };
    auto hot = [&](AiPanelPart part, int index = -1) {
        return vm.hover.part == part && (index < 0 || vm.hover.index == index);
    };
    const float x0 = box.left + pad, x1 = box.right - pad;
    float y = box.top + pad;
    // .mh 语音识别
    c.Text(v.heading, formats_.heading, D2D1::RectF(x0 + 10.0f * s, y + 6.0f * s, x1, y + 21.0f * s), pal.text3);
    y += head_h;
    // .vrow: the connection; spark 16 text2, gap 10; .mt 13/600; .md 12 text3
    // with a 7 px dot; 管理连接 12/600 accent pushed right.
    {
        const float ix = x0 + 10.0f * s;
        c.Icon(AiIcon::Spark, ix, y + (conn_h - 16.0f * s) * 0.5f, 16.0f * s, pal.text2);
        const float tx = ix + 16.0f * s + 10.0f * s;
        const float mw = c.Measure(v.manage, formats_.caption_b);
        const D2D1_RECT_F link = D2D1::RectF(x1 - 10.0f * s - mw - 4.0f * s, y + 14.0f * s, x1 - 6.0f * s, y + conn_h - 14.0f * s);
        const bool lh = hot(AiPanelPart::VoiceManage);
        c.Text(v.manage, formats_.caption_b, D2D1::RectF(link.left + 2.0f * s, link.top, link.right, link.bottom),
               lh ? Mix(pal.accent, pal.text, 0.2f) : pal.accent);
        hit(link, AiPanelPart::VoiceManage);
        const float tr = link.left - 8.0f * s;
        c.Text(v.service, formats_.body13b, D2D1::RectF(tx, y + 8.0f * s, tr, y + 26.0f * s), pal.text);
        const float dy = y + 26.0f * s + 8.5f * s;
        c.dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(tx + 3.5f * s, dy), 3.5f * s, 3.5f * s),
                          c.B(v.connected ? pal.ok : pal.line));
        c.Text(v.service_sub, formats_.caption, D2D1::RectF(tx + 12.0f * s, y + 26.0f * s, tr, y + 43.0f * s), pal.text3);
        y += conn_h;
    }
    c.Fill(D2D1::RectF(x0 + 4.0f * s, y + 6.0f * s, x1 - 4.0f * s, y + 7.0f * s), 0.0f, pal.line2);
    y += sep_h;
    // .vrow: the microphone; .vsel2 12, border line, radius 7, padding 3 8, chevron 11.
    {
        const float ix = x0 + 10.0f * s;
        c.Icon(AiIcon::Mic, ix, y + 12.0f * s, 16.0f * s, pal.text2);
        const float tx = ix + 16.0f * s + 10.0f * s;
        const float lw = c.Measure(v.mic_label, formats_.body13b);
        c.Text(v.mic_label, formats_.body13b, D2D1::RectF(tx, y + 8.0f * s, tx + lw + 1.0f, y + 32.0f * s), pal.text);
        const float room = x1 - 10.0f * s - (tx + lw + 12.0f * s);
        const float bw = (std::min)(c.Measure(v.mic_name, formats_.caption) + (8.0f + 4.0f + 11.0f + 8.0f) * s, room);
        const D2D1_RECT_F sel = D2D1::RectF(x1 - 10.0f * s - bw, y + 8.0f * s, x1 - 10.0f * s, y + 32.0f * s);
        const bool sh = hot(AiPanelPart::VoiceMicPick);
        if (sh) c.Fill(sel, 7.0f * s, pal.hover);
        c.Stroke(sel, 7.0f * s, pal.line);
        c.Text(v.mic_name, formats_.caption, D2D1::RectF(sel.left + 8.0f * s, sel.top, sel.right - 23.0f * s, sel.bottom),
               v.mic_missing ? pal.text3 : pal.text);
        c.Icon(AiIcon::ChevronDown, sel.right - 19.0f * s, (sel.top + sel.bottom) * 0.5f - 5.5f * s, 11.0f * s, pal.text3);
        g.mic_box = sel;
        hit(sel, AiPanelPart::VoiceMicPick);
        y += mic_h;
    }
    // .mh 停顿多久算说完 and .seg3 (1 秒 · 2 秒 · 手动).
    c.Text(v.pause_heading, formats_.heading, D2D1::RectF(x0 + 10.0f * s, y + 6.0f * s, x1, y + 21.0f * s), pal.text3);
    y += head_h;
    {
        const D2D1_RECT_F seg = D2D1::RectF(x0 + 8.0f * s, y + 2.0f * s, x1 - 8.0f * s, y + 2.0f * s + 32.0f * s);
        c.Fill(seg, 9.0f * s, pal.hover);
        const float iw = (seg.right - seg.left - 6.0f * s - 4.0f * s) / 3.0f;
        for (int i = 0; i < 3; ++i) {
            const float ix = seg.left + 3.0f * s + static_cast<float>(i) * (iw + 2.0f * s);
            const D2D1_RECT_F r = D2D1::RectF(ix, seg.top + 3.0f * s, ix + iw, seg.top + 29.0f * s);
            const bool sel = v.pause == i;
            if (sel) {
                c.Shadow(r, 7.0f * s, 3.0f * s, 1.0f * s, Hex(0x000000, 0.12f));
                c.Fill(r, 7.0f * s, pal.card);
            }
            TextOptions o;
            o.align = DWRITE_TEXT_ALIGNMENT_CENTER;
            c.Text(v.pauses[static_cast<size_t>(i)], sel ? formats_.caption_b : formats_.caption, r,
                   sel || hot(AiPanelPart::VoicePause, i) ? pal.text : pal.text2, o);
            hit(r, AiPanelPart::VoicePause, i);
        }
        y += seg_h;
    }
    // .vnote: margin-top 6, border-top line2, padding 8 10 4; shield 13, gap 7, 12/17 text3.
    y += 6.0f * s;
    c.Fill(D2D1::RectF(x0, y, x1, y + 1.0f * s), 0.0f, pal.line2);
    y += 1.0f * s + 8.0f * s;
    c.Icon(AiIcon::Shield, x0 + 10.0f * s, y + 2.0f * s, 13.0f * s, pal.text3);
    {
        TextOptions o;
        o.wrap = true;
        o.line_height = 17.0f * s;
        c.Text(v.note, formats_.caption, D2D1::RectF(x0 + note_tx, y, x0 + note_tx + note_w, y + note_h + 2.0f * s), pal.text3, o);
    }
    c.dc->PopLayer();
    c.dc->SetTransform(saved);
}

void AiPanelView::DrawVoiceCard(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal, float s,
                                uint64_t now_ms) {
    if (!voice_) voice_ = std::make_shared<VoiceGeom>();
    VoiceGeom& g = *voice_;
    g.card_hits.clear();
    g.card_rect = {};
    g.model_box = {};
    g.url_place = g.key_place = false;
    const bool want = vm.voice.card != nullptr;
    if (want) g.card_view = vm.voice.card;
    if (want != g.card_shown) {
        g.card_shown = want;
        g.card_at = now_ms;
    }
    const float elapsed = static_cast<float>(now_ms - g.card_at);
    const float t = animate_ ? std::clamp(elapsed / static_cast<float>(kCardMs), 0.0f, 1.0f) : 1.0f;
    const float to = animate_ ? std::clamp(elapsed / 280.0f, 0.0f, 1.0f) : 1.0f;
    const float p = g.card_shown ? EaseDesign(t) : 1.0f - EaseDesign(t);
    const float op = g.card_shown ? to : 1.0f - to;
    if (op <= 0.0f || !g.card_view) {
        if (!g.card_shown) g.card_view.reset();
        return;
    }
    if (t < 1.0f) decor_ = true;
    const AiVoiceCardView& v = *g.card_view;
    Ctx c{compositor, compositor->Dc(), brush_, s};
    auto hit = [&](const D2D1_RECT_F& r, AiPanelPart part, int index = -1) {
        if (want) g.card_hits.push_back({r, {part, index}});
    };
    auto hot = [&](AiPanelPart part, int index = -1) {
        return vm.hover.part == part && (index < 0 || vm.hover.index == index);
    };
    // .vc: left / right / bottom 12 in the panel, padding 14 14 12, radius 14.
    const float left = panel_rect_.left + 12.0f * s, right = panel_rect_.right - 12.0f * s;
    const float x0 = left + 15.0f * s, x1 = right - 15.0f * s;   // 1 px border + 14
    const float text_w = x1 - x0;
    const float sub_h = c.MeasureWrapped(v.sub, formats_.caption, text_w, 18.0f * s);
    const float pr_tx = 13.0f * s + 7.0f * s;
    const float pr_h = c.MeasureWrapped(v.privacy, formats_.meta, text_w - pr_tx, 17.0f * s);
    const bool st_wrap = v.test == AiVoiceTest::Bad;
    const float st_h = (std::max)(18.0f * s, st_wrap ? c.MeasureWrapped(v.test_text, formats_.caption, text_w - 20.0f * s, 18.0f * s)
                                                    : 18.0f * s);
    const float label_h = 15.0f * s + 4.0f * s;   // .vc-l 11.5 + margin-bottom 4
    const float field_h = 9.0f * s + label_h;     // .vc-f margin-top 9
    const float h = 1.0f * s + 14.0f * s + 20.0f * s + 4.0f * s + sub_h + 10.0f * s +   // head, sub
                    (field_h + 30.0f * s) + 3.0f * (field_h + 32.0f * s) +                // 服务 地址 key 模型
                    10.0f * s + st_h +                                                    // .vc-st
                    8.0f * s + 1.0f * s + 8.0f * s + pr_h +                               // .vc-pr
                    10.0f * s + 32.0f * s + 12.0f * s + 1.0f * s;                         // .vc-b
    const float bottom = panel_rect_.bottom - 12.0f * s;
    const float top0 = (std::max)(bottom - h, panel_rect_.top + 8.0f * s);
    const D2D1_RECT_F box = D2D1::RectF(std::round(left), std::round(top0), std::round(right), std::round(top0 + h));
    if (want) g.card_rect = box;
    // opacity .28 s; translateY(10) scale(.98) → none, .35 s.
    const float k = 0.98f + 0.02f * p;
    const D2D1_POINT_2F origin = D2D1::Point2F((box.left + box.right) * 0.5f, (box.top + box.bottom) * 0.5f);
    D2D1_MATRIX_3X2_F saved;
    c.dc->GetTransform(&saved);
    c.dc->SetTransform(D2D1::Matrix3x2F::Scale(k, k, origin) * D2D1::Matrix3x2F::Translation(0.0f, (1.0f - p) * 10.0f * s) * saved);
    c.dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                           D2D1::IdentityMatrix(), op, nullptr, D2D1_LAYER_OPTIONS1_NONE),
                    nullptr);
    const float radius = 14.0f * s;
    c.Shadow(box, radius, 48.0f * s, 20.0f * s, pal.shadow);
    c.Shadow(box, radius, 8.0f * s, 3.0f * s, Alpha(pal.shadow, 0.4f));
    c.Fill(box, radius, pal.card);
    c.Stroke(box, radius, pal.line);
    float y = box.top + 1.0f * s + 14.0f * s;
    // .vc-h: mic 16 accent, gap 8, 14/600; × 15 text3 pushed right.
    {
        c.Icon(AiIcon::Mic, x0, y + 2.0f * s, 16.0f * s, pal.accent);
        const D2D1_RECT_F x = D2D1::RectF(x1 - 23.0f * s, y - 4.0f * s, x1 + 4.0f * s, y + 24.0f * s);
        const bool xh = hot(AiPanelPart::VoiceClose);
        if (xh) c.Fill(x, 7.0f * s, pal.hover);
        c.Icon(AiIcon::Close, x1 - 15.0f * s, y + 2.5f * s, 15.0f * s, xh ? pal.text : pal.text3);
        hit(x, AiPanelPart::VoiceClose);
        c.Text(v.title, formats_.num14, D2D1::RectF(x0 + 24.0f * s, y, x.left - 6.0f * s, y + 20.0f * s), pal.text);
        y += 20.0f * s;
    }
    // .vc-s: 12 text2, line 18, margin 4 0 10.
    {
        y += 4.0f * s;
        TextOptions o;
        o.wrap = true;
        o.line_height = 18.0f * s;
        c.Text(v.sub, formats_.caption, D2D1::RectF(x0, y, x1, y + sub_h + 2.0f * s), pal.text2, o);
        y += sub_h + 10.0f * s;
    }
    // .vc-l: 11.5/600 text2; .mu pushed right: 400 text3 with a 12 px icon, gap 4.
    auto label = [&](const std::wstring& text, const std::wstring& note, bool lock) {
        y += 9.0f * s;
        c.Text(text, formats_.heading, D2D1::RectF(x0, y, x1, y + 15.0f * s), pal.text2);
        if (!note.empty()) {
            const float nw = c.Measure(note, formats_.meta);
            const float nx = x1 - nw;
            c.Text(note, formats_.meta, D2D1::RectF(nx, y, x1 + 1.0f, y + 15.0f * s), pal.text3);
            if (lock) c.Icon(AiIcon::Lock, nx - 16.0f * s, y + 1.5f * s, 12.0f * s, pal.text3);
        }
        y += label_h;
    };
    // .vc-i: 32 high, border line, radius 8, bg; focus: accent + a 3 px ring at 14%.
    auto field = [&](bool focus, AiPanelPart part) {
        const D2D1_RECT_F r = D2D1::RectF(x0, y, x1, y + 32.0f * s);
        if (focus) c.Fill(Inflate(r, 3.0f * s), 11.0f * s, Alpha(pal.accent, 0.14f));
        c.Fill(r, 8.0f * s, pal.bg);
        c.Stroke(r, 8.0f * s, focus ? pal.accent : hot(part) ? Mix(pal.line, pal.text3, 0.5f) : pal.line);
        hit(r, part);
        return r;
    };
    // 服务: .vc-pv, three 30 px items, gap 6, radius 8.
    label(v.service_label, {}, false);
    {
        const float iw = (text_w - 12.0f * s) / 3.0f;
        for (int i = 0; i < 3; ++i) {
            const float ix = x0 + static_cast<float>(i) * (iw + 6.0f * s);
            const D2D1_RECT_F r = D2D1::RectF(ix, y, ix + iw, y + 30.0f * s);
            const bool on = v.service == i;
            const bool h2 = hot(AiPanelPart::VoiceService, i);
            c.Fill(r, 8.0f * s, on ? pal.accent_tint : h2 ? pal.hover : pal.bg);
            c.Stroke(r, 8.0f * s, on ? pal.accent : pal.line);
            TextOptions o;
            o.align = DWRITE_TEXT_ALIGNMENT_CENTER;
            c.Text(v.services[static_cast<size_t>(i)], on ? formats_.small125b : formats_.small125, r,
                   on ? pal.accent : h2 ? pal.text : pal.text2, o);
            hit(r, AiPanelPart::VoiceService, i);
        }
        y += 30.0f * s;
    }
    const bool settled = want && t >= 1.0f;
    // 地址
    label(v.url_label, {}, false);
    {
        const D2D1_RECT_F r = field(v.url_focused, AiPanelPart::VoiceUrlField);
        const D2D1_RECT_F text = D2D1::RectF(r.left + 10.0f * s, r.top + 1.0f * s, r.right - 10.0f * s, r.bottom - 1.0f * s);
        if (!v.url_edit || !settled) {
            if (v.url_text.empty()) c.Text(v.url_placeholder, formats_.mono125, text, pal.text3);
            else c.Text(v.url_text, formats_.mono125, text, pal.text);
        }
        g.url_text = text;
        g.url_place = settled && v.url_edit;
        y += 32.0f * s;
    }
    // API key (只存在本机)
    label(v.key_label, v.key_note, true);
    {
        const D2D1_RECT_F r = field(v.key_focused, AiPanelPart::VoiceKeyField);
        const D2D1_RECT_F text = D2D1::RectF(r.left + 10.0f * s, r.top + 1.0f * s, r.right - 10.0f * s, r.bottom - 1.0f * s);
        if (!v.key_edit || !settled) {
            if (v.key_text.empty()) c.Text(v.key_placeholder, formats_.mono125, text, pal.text3);
            else c.Text(v.key_text, formats_.mono125, text, pal.text);
        }
        g.key_text = text;
        g.key_place = settled && v.key_edit;
        y += 32.0f * s;
    }
    // 模型 (已从服务读取): .vc-i.sel in the UI face, a chevron 12 text3.
    label(v.model_label, v.model_note, false);
    {
        const D2D1_RECT_F r = D2D1::RectF(x0, y, x1, y + 32.0f * s);
        const bool mh = v.model_ready && hot(AiPanelPart::VoiceModel);
        c.Fill(r, 8.0f * s, mh ? Mix(pal.bg, pal.hover, 0.6f) : pal.bg);
        c.Stroke(r, 8.0f * s, pal.line);
        const D2D1_RECT_F text = D2D1::RectF(r.left + 10.0f * s, r.top, r.right - 28.0f * s, r.bottom);
        if (v.model.empty()) c.Text(v.model_placeholder, formats_.small125, text, pal.text3);
        else c.Text(v.model, formats_.small125, text, pal.text);
        c.Icon(AiIcon::ChevronDown, r.right - 22.0f * s, (r.top + r.bottom) * 0.5f - 6.0f * s, 12.0f * s, pal.text3);
        g.model_box = r;
        if (v.model_ready) hit(r, AiPanelPart::VoiceModel);
        y += 32.0f * s;
    }
    // .vc-st: margin-top 10, min-height 18, 12 text3; ok in ok; a 12 px spinner / 13 px icons, gap 7.
    {
        y += 10.0f * s;
        if (v.test != AiVoiceTest::None) {
            const float tx = x0 + 20.0f * s;
            D2D1_COLOR_F col = pal.text3;
            if (v.test == AiVoiceTest::Running) {
                Spin(c, D2D1::Point2F(x0 + 6.5f * s, y + 9.0f * s), 12.0f * s, pal.line, pal.accent, now_ms);
                decor_ = true;
            } else if (v.test == AiVoiceTest::Ok) {
                col = pal.ok;
                c.Icon(AiIcon::Check, x0, y + 2.5f * s, 13.0f * s, pal.ok);
            } else {
                col = pal.text2;
                c.Icon(AiIcon::AlertCircle, x0, y + 2.5f * s, 13.0f * s, pal.bad);
            }
            TextOptions o;
            o.wrap = st_wrap;
            o.line_height = 18.0f * s;
            c.Text(v.test_text, formats_.caption, D2D1::RectF(tx, y, x1, y + (st_wrap ? st_h + 2.0f * s : 18.0f * s)), col, o);
        }
        y += st_h;
    }
    // .vc-pr: margin-top 8, padding-top 8, border-top line2; shield 13, gap 7, 11.5/17 text3.
    {
        y += 8.0f * s;
        c.Fill(D2D1::RectF(x0, y, x1, y + 1.0f * s), 0.0f, pal.line2);
        y += 1.0f * s + 8.0f * s;
        c.Icon(AiIcon::Shield, x0, y + 2.0f * s, 13.0f * s, pal.text3);
        TextOptions o;
        o.wrap = true;
        o.line_height = 17.0f * s;
        c.Text(v.privacy, formats_.meta, D2D1::RectF(x0 + pr_tx, y, x1, y + pr_h + 2.0f * s), pal.text3, o);
        y += pr_h;
    }
    // .vc-b: margin-top 10, right-aligned, gap 8; .b2 32 high, padding 0 14, radius 8.
    {
        y += 10.0f * s;
        TextOptions o;
        o.align = DWRITE_TEXT_ALIGNMENT_CENTER;
        D2D1_MATRIX_3X2_F before;
        c.dc->GetTransform(&before);
        auto press = [&](const D2D1_RECT_F& r, AiPanelPart part) {
            if (!animate_ || vm.pressed.part != part || !hot(part)) return;
            const D2D1_POINT_2F mid = D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f);
            c.dc->SetTransform(D2D1::Matrix3x2F::Scale(0.94f, 0.94f, mid) * before);
        };
        const float bw1 = c.Measure(v.save, formats_.body13b) + 30.0f * s;
        const D2D1_RECT_F b1 = D2D1::RectF(x1 - bw1, y, x1, y + 32.0f * s);
        const float bw2 = c.Measure(v.cancel, formats_.body13) + 30.0f * s;
        const D2D1_RECT_F b2 = D2D1::RectF(b1.left - 8.0f * s - bw2, y, b1.left - 8.0f * s, y + 32.0f * s);
        press(b2, AiPanelPart::VoiceCancel);
        c.Fill(b2, 8.0f * s, hot(AiPanelPart::VoiceCancel) ? pal.hover : pal.bg);
        c.Stroke(b2, 8.0f * s, pal.line);
        c.Text(v.cancel, formats_.body13, b2, pal.text, o);
        c.dc->SetTransform(before);
        hit(b2, AiPanelPart::VoiceCancel);
        if (v.can_save) press(b1, AiPanelPart::VoiceSave);
        const bool h1 = v.can_save && hot(AiPanelPart::VoiceSave);
        const D2D1_COLOR_F f1 = !v.can_save ? pal.line : h1 ? Mix(pal.accent, pal.text, 0.12f) : pal.accent;
        c.Fill(b1, 8.0f * s, f1);
        c.Text(v.save, formats_.body13b, b1, v.can_save ? pal.on_accent : pal.text3, o);
        c.dc->SetTransform(before);
        if (v.can_save) hit(b1, AiPanelPart::VoiceSave);
        if (!v.remove.empty()) {
            const float rw = c.Measure(v.remove, formats_.small125) + 8.0f * s;
            const D2D1_RECT_F r = D2D1::RectF(x0 - 4.0f * s, y, x0 - 4.0f * s + rw, y + 32.0f * s);
            c.Text(v.remove, formats_.small125, r, hot(AiPanelPart::VoiceRemove) ? Mix(pal.bad, pal.text, 0.25f) : pal.bad, o);
            hit(r, AiPanelPart::VoiceRemove);
        }
    }
    c.dc->PopLayer();
    c.dc->SetTransform(saved);
}

} // namespace pulse::ui
