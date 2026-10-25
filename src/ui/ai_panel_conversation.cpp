// ai_panel_conversation.cpp — the Pulse 助手 conversation: user bubbles,
// thinking, tool steps, plan / confirm / result cards, the streamed answer.
//
// Sizes, margins and motion follow v2.css ("messages" … "result card") of
// plans/ai/pulse_ai_assistant_v2.html; CSS pixels are DIPs. Blocks are laid
// out top to bottom with CSS margin collapsing, so spacing matches the page.
#include "ai_panel_view.h"
#include "ai_panel_draw.h"
#include "ai_panel_conn.h"

#include "ui_motion.h"

#include <algorithm>
#include <cmath>

namespace pulse::ui {
using namespace ai_draw;
namespace {

constexpr float kPi = 3.14159265358979f;

// CSS cubic-bezier(x1, y1, x2, y2) at t.
float CubicBezier(float x1, float y1, float x2, float y2, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    float u = t;
    for (int i = 0; i < 8; ++i) {
        const float x = 3 * (1 - u) * (1 - u) * u * x1 + 3 * (1 - u) * u * u * x2 + u * u * u - t;
        const float dx = 3 * (1 - u) * (1 - u) * x1 + 6 * (1 - u) * u * (x2 - x1) + 3 * u * u * (1 - x2);
        if (std::fabs(dx) < 1e-5f) break;
        u = std::clamp(u - x / dx, 0.0f, 1.0f);
    }
    return 3 * (1 - u) * (1 - u) * u * y1 + 3 * (1 - u) * u * u * y2 + u * u * u;
}

// Negative and positive vertical margins collapse the CSS way.
float Collapse(float a, float b) {
    return (std::max)({a, b, 0.0f}) + (std::min)({a, b, 0.0f});
}

ID2D1Factory* FactoryOf(ID2D1DeviceContext* dc) {
    ID2D1Factory* f = nullptr;
    dc->GetFactory(&f);
    return f;
}

// A rounded rect with its own radius per corner (.user .bub: 16 16 4 16).
void FillCorners(const Ctx& c, const D2D1_RECT_F& r, float tl, float tr, float br, float bl, D2D1_COLOR_F color) {
    ID2D1Factory* f = FactoryOf(c.dc);
    if (!f) return;
    ID2D1PathGeometry* g = nullptr;
    if (SUCCEEDED(f->CreatePathGeometry(&g))) {
        ID2D1GeometrySink* k = nullptr;
        if (SUCCEEDED(g->Open(&k))) {
            const auto arc = [&](float x, float y, float rad) {
                k->AddArc(D2D1::ArcSegment(D2D1::Point2F(x, y), D2D1::SizeF(rad, rad), 0.0f,
                                           D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            };
            k->BeginFigure(D2D1::Point2F(r.left + tl, r.top), D2D1_FIGURE_BEGIN_FILLED);
            k->AddLine(D2D1::Point2F(r.right - tr, r.top));
            if (tr > 0.0f) arc(r.right, r.top + tr, tr);
            k->AddLine(D2D1::Point2F(r.right, r.bottom - br));
            if (br > 0.0f) arc(r.right - br, r.bottom, br);
            k->AddLine(D2D1::Point2F(r.left + bl, r.bottom));
            if (bl > 0.0f) arc(r.left, r.bottom - bl, bl);
            k->AddLine(D2D1::Point2F(r.left, r.top + tl));
            if (tl > 0.0f) arc(r.left + tl, r.top, tl);
            k->EndFigure(D2D1_FIGURE_END_CLOSED);
            k->Close();
            k->Release();
            c.dc->FillGeometry(g, c.B(color));
        }
        g->Release();
    }
    f->Release();
}

void Arc(const Ctx& c, D2D1_POINT_2F mid, float r, float a0, float a1, D2D1_COLOR_F color, float width) {
    ID2D1Factory* f = FactoryOf(c.dc);
    if (!f) return;
    ID2D1PathGeometry* g = nullptr;
    if (SUCCEEDED(f->CreatePathGeometry(&g))) {
        ID2D1GeometrySink* k = nullptr;
        if (SUCCEEDED(g->Open(&k))) {
            k->BeginFigure(D2D1::Point2F(mid.x + r * std::cos(a0), mid.y + r * std::sin(a0)), D2D1_FIGURE_BEGIN_HOLLOW);
            k->AddArc(D2D1::ArcSegment(D2D1::Point2F(mid.x + r * std::cos(a1), mid.y + r * std::sin(a1)),
                                       D2D1::SizeF(r, r), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            k->EndFigure(D2D1_FIGURE_END_OPEN);
            k->Close();
            k->Release();
            c.dc->DrawGeometry(g, c.B(color), width);
        }
        g->Release();
    }
    f->Release();
}

// .spin: a 2 px ring in accent-soft with an accent quarter, .8 s a turn.
void Spinner(const Ctx& c, D2D1_POINT_2F mid, float d, D2D1_COLOR_F track, D2D1_COLOR_F accent, uint64_t now) {
    const float r = d * 0.5f - c.s;
    c.dc->DrawEllipse(D2D1::Ellipse(mid, r, r), c.B(track), 2.0f * c.s);
    const float a = static_cast<float>(now % 800) / 800.0f * 2.0f * kPi - kPi * 0.5f;
    Arc(c, mid, r, a - kPi * 0.25f, a + kPi * 0.25f, accent, 2.0f * c.s);
}

// @keyframes spark, 1.6 s, cubic-bezier(.5,0,.5,1) per half.
AiIconPose SparkPose(uint64_t now) {
    const float t = static_cast<float>(now % 1600) / 1600.0f;
    const float e = CubicBezier(0.5f, 0.0f, 0.5f, 1.0f, t < 0.5f ? t * 2.0f : (t - 0.5f) * 2.0f);
    AiIconPose p;
    if (t < 0.5f) {
        p.rotate_deg = 90.0f * e;
        p.scale = 1.0f - 0.22f * e;
    } else {
        p.rotate_deg = 90.0f + 90.0f * e;
        p.scale = 0.78f + 0.22f * e;
    }
    return p;
}

ID2D1LinearGradientBrush* Gradient(ID2D1DeviceContext* dc, D2D1_POINT_2F a, D2D1_POINT_2F b,
                                   const D2D1_GRADIENT_STOP* stops, UINT32 n) {
    ID2D1GradientStopCollection* col = nullptr;
    if (FAILED(dc->CreateGradientStopCollection(stops, n, &col))) return nullptr;
    ID2D1LinearGradientBrush* brush = nullptr;
    dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), col, &brush);
    col->Release();
    return brush;
}

// One line or paragraph of an answer: "- " bullets, **bold** spans.
struct RichLine {
    std::wstring text;
    std::vector<DWRITE_TEXT_RANGE> bold;
    bool bullet = false;
};

std::vector<RichLine> ParseRich(const std::wstring& src) {
    std::vector<RichLine> out;
    size_t pos = 0;
    while (pos <= src.size()) {
        size_t end = src.find(L'\n', pos);
        if (end == std::wstring::npos) end = src.size();
        std::wstring_view line(src.data() + pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.remove_suffix(1);
        if (line.empty()) {
            if (end == src.size()) break;
            continue;
        }
        RichLine r;
        if (line.size() >= 2 && (line[0] == L'-' || line[0] == L'\x2022') && line[1] == L' ') {
            r.bullet = true;
            line.remove_prefix(2);
        }
        bool bold = false;
        UINT32 start = 0;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == L'*' && i + 1 < line.size() && line[i + 1] == L'*') {
                const UINT32 at = static_cast<UINT32>(r.text.size());
                if (bold && at > start) r.bold.push_back({start, at - start});
                bold = !bold;
                start = at;
                ++i;
                continue;
            }
            r.text.push_back(line[i]);
        }
        // An unclosed ** while streaming still shows bold.
        if (bold && r.text.size() > start) r.bold.push_back({start, static_cast<UINT32>(r.text.size()) - start});
        out.push_back(std::move(r));
        if (end == src.size()) break;
    }
    return out;
}

D2D1_RECT_F Intersect(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    return D2D1::RectF((std::max)(a.left, b.left), (std::max)(a.top, b.top), (std::min)(a.right, b.right),
                       (std::min)(a.bottom, b.bottom));
}

} // namespace

float AiPanelView::TweenTo(uint64_t key, float target, uint32_t ms, uint64_t now_ms) {
    for (auto& [k, t] : tweens_) {
        if (k != key) continue;
        t.used = true;
        const float p = t.ms ? std::clamp(static_cast<float>(now_ms - t.start) / static_cast<float>(t.ms), 0.0f, 1.0f)
                             : 1.0f;
        const float value = t.from + (t.to - t.from) * EaseDesign(p);
        if (target != t.to) {
            t.from = value;
            t.to = target;
            t.start = now_ms;
            t.ms = motion::SystemAnimationsEnabled() ? ms : 0;
            if (t.ms) conv_anim_ = true;
            return value;
        }
        if (p < 1.0f) conv_anim_ = true;
        return value;
    }
    // First sight: start settled.
    Tween t;
    t.from = t.to = target;
    t.start = now_ms;
    t.used = true;
    tweens_.push_back({key, t});
    return target;
}

void AiPanelView::ScrollBy(float dy) {
    // The open model menu's list takes the wheel while it scrolls.
    if (menu_shown_ && conn_ && !conn_->card && conn_->menu_scroll_max > 0.0f) {
        conn_->menu_scroll = std::clamp(conn_->menu_scroll + dy, 0.0f, conn_->menu_scroll_max);
        return;
    }
    const float max_scroll = (std::max)(0.0f, content_h_ - view_h_);
    const float base = scroll_anim_ ? scroll_to_ : scroll_;
    const float to = std::clamp(base + dy, 0.0f, max_scroll);
    scroll_from_ = scroll_;
    scroll_to_ = to;
    scroll_start_ = motion::NowMs();
    scroll_anim_ = motion::SystemAnimationsEnabled();
    if (!scroll_anim_) scroll_ = to;
    follow_ = to >= max_scroll - 1.0f;
}

void AiPanelView::ScrollToEnd() {
    follow_ = true;
    scroll_anim_ = false;
}

void AiPanelView::DrawConversation(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal,
                                   const D2D1_RECT_F& area, float s, uint64_t now_ms) {
    Ctx c{compositor, compositor->Dc(), brush_, s};
    ID2D1DeviceContext* dc = c.dc;
    const bool motion_on = motion::SystemAnimationsEnabled();
    was_empty_ = false;
    for (auto& kv : tweens_) kv.second.used = false;

    // Scroll position for this frame (content height is last frame's; a
    // following view snaps to the end below once this frame is measured).
    view_h_ = area.bottom - area.top;
    if (scroll_anim_) {
        const float p = std::clamp(static_cast<float>(now_ms - scroll_start_) / 160.0f, 0.0f, 1.0f);
        scroll_ = scroll_from_ + (scroll_to_ - scroll_from_) * EaseDesign(p);
        if (p >= 1.0f) scroll_anim_ = false;
        else conv_anim_ = true;
    }
    if (follow_) scroll_ = (std::max)(0.0f, content_h_ - view_h_);

    // .conv padding 16 18 10.
    const float x0 = area.left + 18.0f * s;
    const float x1 = area.right - 18.0f * s;
    const float top = area.top - scroll_;   // content origin on screen

    const auto hot = [&](AiPanelPart part, int i) { return vm.hover.part == part && vm.hover.index == i; };
    const auto down = [&](AiPanelPart part, int i) { return hot(part, i) && vm.pressed.part == part; };
    const auto hit = [&](const D2D1_RECT_F& r, AiPanelPart part, int i) {
        const D2D1_RECT_F v = Intersect(r, area);
        if (v.right > v.left && v.bottom > v.top) conv_hits_.push_back({v, {part, i}});
    };
    const auto visible = [&](float y0, float y1) { return y1 > area.top && y0 < area.bottom; };

    // Text helpers. Plain single line in a box (vertically centred).
    const auto line = [&](const std::wstring& text, IDWriteTextFormat* fmt, float x, float y, float right, float h,
                          D2D1_COLOR_F color) {
        if (right > x) c.Text(text, fmt, D2D1::RectF(x, y, right, y + h), color);
    };
    const auto wrapped_h = [&](const std::wstring& text, IDWriteTextFormat* fmt, float width, float lh) {
        return text.empty() ? 0.0f : c.MeasureWrapped(text, fmt, width, lh);
    };
    const auto wrapped = [&](const std::wstring& text, IDWriteTextFormat* fmt, float x, float y, float width, float lh,
                             D2D1_COLOR_F color) {
        TextOptions o;
        o.wrap = true;
        o.line_height = lh;
        c.Text(text, fmt, D2D1::RectF(x, y, x + width, y + 1.0f), color, o);
    };
    // .shimmer: text2 with an accent band sweeping across every 1.8 s.
    const auto shimmer = [&](const std::wstring& text, IDWriteTextFormat* fmt, float x, float y, float h) {
        const float tw = c.Measure(text, fmt);
        const D2D1_RECT_F r = D2D1::RectF(x, y, x + tw + 2.0f, y + h);
        c.Text(text, fmt, r, pal.text2);
        if (!motion_on || tw <= 0.0f) return;
        conv_decor_ = true;
        const float t = static_cast<float>(now_ms % 1800) / 1800.0f;
        const float gpos = 1.0f - 2.2f * t;        // background-position 100% → -120%
        const float gx = x - 1.2f * tw * gpos;
        const D2D1_COLOR_F a0 = Alpha(pal.accent, 0.0f), a1 = pal.accent;
        const D2D1_GRADIENT_STOP stops[] = {{0.35f, a0}, {0.5f, a1}, {0.65f, a0}};
        ID2D1LinearGradientBrush* mask =
            Gradient(dc, D2D1::Point2F(gx, 0.0f), D2D1::Point2F(gx + 2.2f * tw, 0.0f), stops, 3);
        if (!mask) return;
        dc->PushLayer(D2D1::LayerParameters1(r, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
                                             1.0f, mask, D2D1_LAYER_OPTIONS1_NONE),
                      nullptr);
        c.Text(text, fmt, r, pal.accent);
        dc->PopLayer();
        mask->Release();
    };
    // msg-in: opacity 0 → 1 and translateY(8) → 0, .35 s.
    struct Entrance {
        float opacity = 1.0f;
        float dy = 0.0f;
    };
    const auto entrance = [&](uint64_t born, float dist, uint32_t ms) {
        Entrance e;
        if (!born || !motion_on) return e;
        if (now_ms < born) {
            e.opacity = 0.0f;
            e.dy = dist;
            conv_anim_ = true;
            return e;
        }
        const float t = static_cast<float>(now_ms - born) / static_cast<float>(ms);
        if (t >= 1.0f) return e;
        conv_anim_ = true;
        const float k = CubicBezier(0.3f, 0.7f, 0.2f, 1.0f, t);
        e.opacity = k;
        e.dy = dist * (1.0f - k);
        return e;
    };
    D2D1_MATRIX_3X2_F saved;
    dc->GetTransform(&saved);
    const auto push_entrance = [&](const Entrance& e) {
        if (e.opacity >= 1.0f && e.dy == 0.0f) return false;
        dc->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, e.dy) * saved);
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::IdentityMatrix(), e.opacity, nullptr, D2D1_LAYER_OPTIONS1_NONE),
                      nullptr);
        return true;
    };
    const auto pop_entrance = [&](bool pushed) {
        if (!pushed) return;
        dc->PopLayer();
        dc->SetTransform(saved);
    };
    // .b2 button (h 32, padding 0 14, radius 8); returns its width.
    const auto button = [&](float x_right, float y, const std::wstring& text, bool primary, AiIcon icon, bool with_icon,
                            AiPanelPart part, int index, float max_w = 0.0f) {
        IDWriteTextFormat* fmt = primary ? formats_.body13b : formats_.body13;
        const float tw = c.Measure(text, fmt);
        float bw = 28.0f * s + tw + (with_icon ? 20.0f * s : 0.0f);
        if (max_w > 0.0f && bw > max_w) bw = max_w;   // a narrow panel: the text ends in …
        const D2D1_RECT_F r = D2D1::RectF(x_right - bw, y, x_right, y + 32.0f * s);
        const bool h = hot(part, index), d = down(part, index);
        D2D1_MATRIX_3X2_F before;
        dc->GetTransform(&before);
        if (d && motion_on) {
            const D2D1_POINT_2F mid = D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f);
            dc->SetTransform(D2D1::Matrix3x2F::Scale(0.94f, 0.94f, mid) * before);
        }
        if (primary) {
            c.Fill(r, 8.0f * s, h ? Mix(pal.accent, pal.text, 0.12f) : pal.accent);
        } else {
            c.Fill(r, 8.0f * s, h ? pal.hover : Mix(pal.card, pal.pane, 0.3f));
            c.Stroke(r, 8.0f * s, pal.line);
        }
        const D2D1_COLOR_F ink = primary ? pal.on_accent : pal.text;
        float tx = r.left + 14.0f * s;
        if (with_icon) {
            c.Icon(icon, tx, y + 9.0f * s, 14.0f * s, ink);
            tx += 20.0f * s;
        }
        line(text, fmt, tx, y, r.right - 10.0f * s, 32.0f * s, ink);
        dc->SetTransform(before);
        hit(r, part, index);
        return bw;
    };
    const auto chevron = [&](float x, float cy, float deg, D2D1_COLOR_F color, AiIcon icon) {
        AiIconPose pose;
        pose.rotate_deg = deg;
        c.Icon(icon, x, cy - 6.5f * s, 13.0f * s, color, pose);
    };
    const auto card = [&](const D2D1_RECT_F& r, D2D1_COLOR_F border) {
        c.Fill(r, 12.0f * s, pal.card);
        c.Stroke(r, 12.0f * s, border);
    };
    // .okc: 18 px ok circle with a white check, pops in .35 s
    // (cubic-bezier(.3,1.6,.5,1)).
    const auto ok_check = [&](D2D1_POINT_2F mid, uint64_t done_ms) {
        float k = 1.0f, a = 1.0f;
        if (done_ms && motion_on && now_ms - done_ms < 350) {
            const float t = static_cast<float>(now_ms - done_ms) / 350.0f;
            const float e = CubicBezier(0.3f, 1.6f, 0.5f, 1.0f, t);
            k = 0.3f + 0.7f * e;
            a = std::clamp(e, 0.0f, 1.0f);
            conv_anim_ = true;
        }
        const float r = 9.0f * s * k;
        c.dc->FillEllipse(D2D1::Ellipse(mid, r, r), c.B(Alpha(pal.ok, a)));
        const float is = 12.0f * s * k;
        c.Icon(AiIcon::Check, mid.x - is * 0.5f, mid.y - is * 0.5f, is, Alpha(pal.on_accent, a));
    };

    float y = 16.0f * s;     // content y of the next block's top edge (before margins)
    float pending = 0.0f;    // the previous block's bottom margin
    bool first = true;
    const auto place = [&](float margin_top) {
        y += first ? (std::max)(0.0f, margin_top) : Collapse(pending, margin_top);
        first = false;
    };

    for (size_t bi = 0; bi < vm.blocks.size(); ++bi) {
        const AiBlockView& b = vm.blocks[bi];
        const int index = static_cast<int>(bi);
        const uint64_t key = static_cast<uint64_t>(b.id) * 8u;
        Entrance enter = entrance(b.born_ms, 8.0f * s, 350);
        if (b.kind == AiBlockKind::Acts) {
            // .acts fades in (opacity .3s), no slide.
            enter = entrance(b.born_ms, 0.0f, 300);
        }
        if (b.gone_ms && motion_on) {
            // .err.gone: opacity 0, translateY(-4px) over .25 s (then removed).
            const float t = now_ms <= b.gone_ms ? 0.0f
                                                : std::clamp(static_cast<float>(now_ms - b.gone_ms) / 250.0f, 0.0f, 1.0f);
            enter.opacity *= 1.0f - t;
            enter.dy -= 4.0f * s * t;
            conv_anim_ = true;
        }
        const bool pushed = push_entrance(enter);
        switch (b.kind) {
        case AiBlockKind::User: {
            // A new turn: .turn margin-bottom 12 collapses with the user's 4.
            if (!first) pending = (std::max)(pending, 12.0f * s);
            place(4.0f * s);
            if (!b.context.empty()) {
                // .ctxp: h 24, padding 0 9 0 7, radius 12, folder 13, gap 6.
                const float tw = c.Measure(b.context, formats_.caption);
                const float cw = (7.0f + 13.0f + 6.0f + 9.0f) * s + tw;
                const D2D1_RECT_F r = D2D1::RectF(x1 - cw, top + y, x1, top + y + 24.0f * s);
                if (visible(r.top, r.bottom)) {
                    c.Fill(r, 12.0f * s, pal.card);
                    c.Stroke(r, 12.0f * s, pal.line);
                    c.Icon(AiIcon::FolderFilled, r.left + 7.0f * s, r.top + 5.5f * s, 13.0f * s, pal.folder_icon);
                    line(b.context, formats_.caption, r.left + 26.0f * s, r.top, r.right, 24.0f * s, pal.text2);
                }
                y += 30.0f * s;
            }
            // .bub: max-width 300, padding 10 14, radius 16 16 4 16, 13.5 / 21.
            const float max_text = 300.0f * s - 28.0f * s;
            const float tw = (std::min)(c.Measure(b.text, formats_.body), max_text);
            const float th = (std::max)(21.0f * s, wrapped_h(b.text, formats_.body, max_text, 21.0f * s));
            const float bw = tw + 28.0f * s;
            const D2D1_RECT_F r = D2D1::RectF(x1 - bw, top + y, x1, top + y + th + 20.0f * s);
            if (visible(r.top, r.bottom)) {
                FillCorners(c, r, 16.0f * s, 16.0f * s, 4.0f * s, 16.0f * s, pal.accent_soft);
                wrapped(b.text, formats_.body, r.left + 14.0f * s, r.top + 10.0f * s, max_text + 1.0f, 21.0f * s,
                        pal.text);
            }
            y += th + 20.0f * s;
            pending = 18.0f * s;
            break;
        }
        case AiBlockKind::Thinking: {
            place(0.0f);
            // .think-h: h 28, padding 0 10 0 8, margin-left -8, gap 7.
            const float hx = x0 - 8.0f * s;
            const float hy = top + y;
            const float label_w = c.Measure(b.label, formats_.body13);
            const float meta_w = b.meta.empty() ? 0.0f : c.Measure(b.meta, formats_.caption);
            const float sec_w = b.seconds.empty() ? 0.0f : c.Measure(b.seconds, formats_.caption);
            float hw = (8.0f + 15.0f + 7.0f) * s + label_w + 7.0f * s;
            if (meta_w > 0.0f) hw += meta_w + 7.0f * s;
            if (sec_w > 0.0f) hw += sec_w + 7.0f * s;
            hw += (13.0f + 10.0f) * s;
            const D2D1_RECT_F hr = D2D1::RectF(hx, hy, hx + hw, hy + 28.0f * s);
            const float open_k = TweenTo(key + 1, b.open ? 1.0f : 0.0f, 250, now_ms);
            if (visible(hr.top, hr.bottom + 200.0f * s)) {
                if (hot(AiPanelPart::ThinkToggle, index)) c.Fill(hr, 8.0f * s, pal.hover);
                const float cy = hy + 14.0f * s;
                float x = hx + 8.0f * s;
                AiIconPose pose;
                if (b.running && motion_on) { pose = SparkPose(now_ms); conv_decor_ = true; }
                c.Icon(AiIcon::Spark, x, cy - 7.5f * s, 15.0f * s, pal.accent, pose);
                x += 22.0f * s;
                if (b.running) shimmer(b.label, formats_.body13, x, hy, 28.0f * s);
                else line(b.label, formats_.body13, x, hy, x + label_w + 2.0f, 28.0f * s, pal.text2);
                x += label_w + 7.0f * s;
                if (meta_w > 0.0f) {
                    line(b.meta, formats_.caption, x, hy, x + meta_w + 2.0f, 28.0f * s, pal.text3);
                    x += meta_w + 7.0f * s;
                }
                if (sec_w > 0.0f) {
                    line(b.seconds, formats_.caption, x, hy, x + sec_w + 2.0f, 28.0f * s, pal.text3);
                    x += sec_w + 7.0f * s;
                }
                chevron(x, cy, 90.0f * open_k, pal.text3, AiIcon::ChevronRight);
            }
            if (!b.running) hit(hr, AiPanelPart::ThinkToggle, index);
            y += 28.0f * s;
            // .think-b: margin 2 0 0 7, border-left 2, padding-left 12;
            // text 12.5 / 20, padding 2 0 4. Running shows the last 70 px
            // under a 22 px fade; open shows up to 200 px.
            y += 2.0f * s;
            const float bx = x0 + 7.0f * s;
            const float tx = bx + 14.0f * s;
            const float text_w = x1 - tx;
            const float th = b.text.empty() ? 0.0f : wrapped_h(b.text, formats_.small125, text_w, 20.0f * s) + 6.0f * s;
            const float target = b.running ? (std::min)(70.0f * s, th) : b.open ? (std::min)(200.0f * s, th) : 0.0f;
            const float hv = TweenTo(key + 2, target, 450, now_ms);
            const float alpha = TweenTo(key + 3, (b.running || b.open) ? 1.0f : 0.0f, 200, now_ms);
            if (hv > 0.5f && alpha > 0.0f && visible(top + y, top + y + hv)) {
                const D2D1_RECT_F clip = D2D1::RectF(bx, top + y, x1 + 1.0f, top + y + hv);
                dc->PushAxisAlignedClip(Intersect(clip, area), D2D1_ANTIALIAS_MODE_ALIASED);
                c.Fill(D2D1::RectF(bx, clip.top, bx + 2.0f * s, clip.bottom), 0.0f, Alpha(pal.line, alpha));
                float ty = clip.top + 2.0f * s;
                if (b.running && th > hv) ty = clip.bottom - th + 2.0f * s;
                ID2D1LinearGradientBrush* fade = nullptr;
                if (b.running && th > hv) {
                    const D2D1_GRADIENT_STOP stops[] = {{0.0f, D2D1::ColorF(0, 0, 0, 0)}, {1.0f, D2D1::ColorF(0, 0, 0, 1)}};
                    fade = Gradient(dc, D2D1::Point2F(0.0f, clip.top), D2D1::Point2F(0.0f, clip.top + 22.0f * s), stops, 2);
                }
                if (fade || alpha < 1.0f) {
                    dc->PushLayer(D2D1::LayerParameters1(clip, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                         D2D1::IdentityMatrix(), alpha, fade, D2D1_LAYER_OPTIONS1_NONE),
                                  nullptr);
                }
                wrapped(b.text, formats_.small125, tx, ty, text_w, 20.0f * s, pal.text3);
                if (fade || alpha < 1.0f) dc->PopLayer();
                if (fade) fade->Release();
                dc->PopAxisAlignedClip();
            }
            y += hv;
            pending = 10.0f * s;
            break;
        }
        case AiBlockKind::Text: {
            // .turn: 13.5 / 22; p margin 2 0 10; ul margin 0 0 10, li 2 0,
            // padding-left 18.
            const std::vector<RichLine> lines = ParseRich(b.text);
            if (lines.empty() && !b.streaming) break;
            const float lh = 22.0f * s;
            float caret_x = x0, caret_y = -1.0f;
            for (size_t li = 0; li < lines.size(); ++li) {
                const RichLine& rl = lines[li];
                // p and li both have margin-top 2; li → li collapses to 2,
                // anything next to a paragraph or the list's end to 10.
                place(2.0f * s);
                const float lx = rl.bullet ? x0 + 18.0f * s : x0;
                const float lw = x1 - lx;
                TextOptions o;
                o.wrap = true;
                o.line_height = lh;
                IDWriteTextLayout* layout = c.Layout(rl.text.empty() ? std::wstring(L" ") : rl.text, formats_.body, lw,
                                                     10000.0f, o);
                float h = lh;
                if (layout) {
                    for (const DWRITE_TEXT_RANGE& r : rl.bold) layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, r);
                    DWRITE_TEXT_METRICS m{};
                    layout->GetMetrics(&m);
                    h = (std::max)(lh, m.height);
                    if (visible(top + y, top + y + h)) {
                        if (rl.bullet) {
                            const float r = 2.6f * s;
                            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x0 + 7.0f * s, top + y + lh * 0.5f + 0.5f * s), r, r),
                                            c.B(pal.text));
                        }
                        compositor->DrawTextLayout(D2D1::Point2F(lx, top + y), layout, c.B(pal.text));
                    }
                    if (b.streaming && li + 1 == lines.size()) {
                        DWRITE_HIT_TEST_METRICS hm{};
                        float px = 0.0f, py = 0.0f;
                        const UINT32 n = static_cast<UINT32>(rl.text.size());
                        if (n > 0 && SUCCEEDED(layout->HitTestTextPosition(n - 1, TRUE, &px, &py, &hm))) {
                            caret_x = lx + px;
                            caret_y = top + y + hm.top;
                        } else {
                            caret_x = lx;
                            caret_y = top + y;
                        }
                    }
                    layout->Release();
                }
                y += h;
                const bool next_bullet = li + 1 < lines.size() && lines[li + 1].bullet;
                pending = (rl.bullet && next_bullet) ? 2.0f * s : 10.0f * s;
            }
            if (b.streaming) {
                if (lines.empty()) {
                    place(2.0f * s);
                    caret_x = x0;
                    caret_y = top + y;
                    y += lh;
                    pending = 10.0f * s;
                }
                // .caret: 7 × 15 accent, blinking at 1 s steps(2, start).
                conv_decor_ = true;
                if (!motion_on || now_ms % 1000 < 500) {
                    const float cx = caret_x + 1.0f * s;
                    const float cy = caret_y + (lh - 15.0f * s) * 0.5f + 2.0f * s;
                    c.Fill(D2D1::RectF(cx, cy, cx + 7.0f * s, cy + 15.0f * s), 1.0f * s, pal.accent);
                }
            }
            break;
        }
        case AiBlockKind::Steps: {
            place(0.0f);
            // Heights first: header 38 (+1 border when open), body padding 6,
            // each step padding 8 12 around max(28, text, status).
            std::vector<float> rows;
            float body = 12.0f * s;
            for (const AiStepView& st : b.steps) {
                float mn = 18.0f * s;
                if (!st.sub.empty()) mn += 20.0f * s;
                if (st.progress >= 0.0f && st.state == AiStepState::Running) mn += 10.0f * s;
                const float rh = 16.0f * s + (std::max)({28.0f * s, mn, 23.0f * s});
                rows.push_back(rh);
                body += rh;
            }
            const bool show_body = !b.collapsed || b.open;
            float h = 2.0f * s;
            if (b.collapsed) h += 38.0f * s + (b.open ? 1.0f * s : 0.0f);
            if (show_body) h += body;
            const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + h);
            const float open_k = TweenTo(key + 1, b.open ? 1.0f : 0.0f, 250, now_ms);
            if (visible(r.top, r.bottom)) {
                card(r, pal.line);
                float yy = r.top + 1.0f * s;
                if (b.collapsed) {
                    const D2D1_RECT_F hr = D2D1::RectF(r.left + 1.0f, yy, r.right - 1.0f, yy + 38.0f * s);
                    if (hot(AiPanelPart::StepsToggle, index)) {
                        dc->PushAxisAlignedClip(hr, D2D1_ANTIALIAS_MODE_ALIASED);
                        c.Fill(Inflate(r, -1.0f), 11.0f * s, Alpha(pal.hover, 0.6f));
                        dc->PopAxisAlignedClip();
                    }
                    c.Icon(AiIcon::Check, hr.left + 12.0f * s, yy + 11.5f * s, 15.0f * s, pal.ok);
                    line(b.label, formats_.body13, hr.left + 35.0f * s, yy, hr.right - 33.0f * s, 38.0f * s, pal.text2);
                    chevron(hr.right - 25.0f * s, yy + 19.0f * s, 180.0f * open_k, pal.text3, AiIcon::ChevronDown);
                    hit(hr, AiPanelPart::StepsToggle, index);
                    yy += 38.0f * s;
                    if (b.open) {
                        c.Fill(D2D1::RectF(r.left + 1.0f, yy, r.right - 1.0f, yy + 1.0f * s), 0.0f, pal.line2);
                        yy += 1.0f * s;
                    }
                }
                if (show_body) {
                    yy += 6.0f * s;
                    const float il = r.left + 1.0f * s;
                    for (size_t i = 0; i < b.steps.size(); ++i) {
                        const AiStepView& st = b.steps[i];
                        const float rh = rows[i];
                        const Entrance se = entrance(st.born_ms, 8.0f * s, 350);
                        const bool sp = push_entrance(se);
                        if (i > 0) {
                            // .step + .step::before: left 25, top -8, h 14, w 2.
                            c.Fill(D2D1::RectF(il + 25.0f * s, yy - 8.0f * s, il + 27.0f * s, yy + 6.0f * s), 0.0f,
                                   pal.line);
                        }
                        const bool run = st.state == AiStepState::Running;
                        const float ix = il + 12.0f * s, iy = yy + 8.0f * s;
                        const D2D1_RECT_F tile = D2D1::RectF(ix, iy, ix + 28.0f * s, iy + 28.0f * s);
                        c.Fill(tile, 8.0f * s, run ? pal.accent_soft : pal.hover);
                        c.Icon(st.icon, ix + 6.5f * s, iy + 6.5f * s, 15.0f * s, run ? pal.accent : pal.text2);
                        const float mx = ix + 38.0f * s;
                        const float mr = r.right - 1.0f * s - 12.0f * s - 18.0f * s - 10.0f * s;
                        float tw = c.Measure(st.title, formats_.body13b);
                        tw = (std::min)(tw, mr - mx);
                        line(st.title, formats_.body13b, mx, iy, mx + tw + 1.0f, 18.0f * s, pal.text);
                        if (!st.arg.empty()) {
                            // .arg: margin-left 6, padding 0 6, radius 5, 12 text2.
                            const float ax = mx + tw + 6.0f * s;
                            const float aw = (std::min)(c.Measure(st.arg, formats_.caption) + 12.0f * s, mr - ax);
                            if (aw > 16.0f * s) {
                                const D2D1_RECT_F ar = D2D1::RectF(ax, iy + 1.0f * s, ax + aw, iy + 17.0f * s);
                                c.Fill(ar, 5.0f * s, pal.hover);
                                line(st.arg, formats_.caption, ax + 6.0f * s, ar.top, ar.right - 4.0f * s, 16.0f * s,
                                     pal.text2);
                            }
                        }
                        float sy = iy + 18.0f * s;
                        if (!st.sub.empty()) {
                            sy += 2.0f * s;
                            line(st.sub, formats_.caption, mx, sy, mr, 18.0f * s, run ? pal.accent : pal.text3);
                            sy += 18.0f * s;
                        }
                        if (st.progress >= 0.0f && run) {
                            // .pg: 4 px, radius 2, margin-top 6.
                            const float py = sy + 6.0f * s;
                            const float pw = mr - mx;
                            const float pv = TweenTo(key + 4, std::clamp(st.progress, 0.0f, 1.0f), 250, now_ms);
                            c.Fill(D2D1::RectF(mx, py, mx + pw, py + 4.0f * s), 2.0f * s, pal.line);
                            if (pv > 0.0f)
                                c.Fill(D2D1::RectF(mx, py, mx + pw * pv, py + 4.0f * s), 2.0f * s, pal.accent);
                        }
                        // .stt: 18 × 18, margin-top 5.
                        const D2D1_POINT_2F smid =
                            D2D1::Point2F(r.right - 1.0f * s - 12.0f * s - 9.0f * s, yy + 8.0f * s + 5.0f * s + 9.0f * s);
                        if (run) {
                            Spinner(c, smid, 16.0f * s, pal.accent_soft, pal.accent, now_ms);
                            conv_decor_ = true;
                        } else if (st.state == AiStepState::Done) {
                            ok_check(smid, st.done_ms);
                        } else {
                            c.dc->FillEllipse(D2D1::Ellipse(smid, 9.0f * s, 9.0f * s), c.B(pal.amber_bg));
                            c.Icon(AiIcon::Close, smid.x - 6.0f * s, smid.y - 6.0f * s, 12.0f * s, pal.amber);
                        }
                        pop_entrance(sp);
                        yy += rh;
                    }
                }
            } else if (b.collapsed) {
                hit(D2D1::RectF(r.left, r.top, r.right, r.top + 39.0f * s), AiPanelPart::StepsToggle, index);
            }
            y += h;
            pending = 12.0f * s;
            break;
        }
        case AiBlockKind::Plan: {
            place(0.0f);
            // .card-h 40; .card-b border-top + padding 6; .grp padding 7 12
            // around max(26, 18 + 18).
            const float gh = 50.0f * s;
            float h = 2.0f * s + 40.0f * s;
            if (!b.collapsed) h += 1.0f * s + 12.0f * s + gh * static_cast<float>(b.groups.size());
            const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + h);
            const float col_k = TweenTo(key + 1, b.collapsed ? 1.0f : 0.0f, 250, now_ms);
            const D2D1_RECT_F hr = D2D1::RectF(r.left + 1.0f, r.top + 1.0f, r.right - 1.0f, r.top + 1.0f + 40.0f * s);
            if (visible(r.top, r.bottom)) {
                card(r, pal.line);
                if (hot(AiPanelPart::PlanToggle, index)) {
                    dc->PushAxisAlignedClip(hr, D2D1_ANTIALIAS_MODE_ALIASED);
                    c.Fill(Inflate(r, -1.0f), 11.0f * s, Alpha(pal.hover, 0.6f));
                    dc->PopAxisAlignedClip();
                }
                const float cy = hr.top + 20.0f * s;
                c.Icon(AiIcon::Move, hr.left + 12.0f * s, cy - 8.0f * s, 16.0f * s, pal.accent);
                float x = hr.left + 36.0f * s;
                const float tw = c.Measure(b.label, formats_.body13b);
                line(b.label, formats_.body13b, x, hr.top, x + tw + 1.0f, 40.0f * s, pal.text);
                x += tw + 8.0f * s;
                line(b.meta, formats_.caption, x, hr.top, hr.right - 33.0f * s, 40.0f * s, pal.text3);
                chevron(hr.right - 25.0f * s, cy, -90.0f * col_k, pal.text3, AiIcon::ChevronDown);
                if (!b.collapsed) {
                    float yy = hr.bottom;
                    c.Fill(D2D1::RectF(r.left + 1.0f, yy, r.right - 1.0f, yy + 1.0f * s), 0.0f, pal.line2);
                    yy += 1.0f * s + 6.0f * s;
                    for (const AiGroupView& g : b.groups) {
                        // .grp: opacity 0 + translateY(4) until .in (.3 s).
                        float ga = 0.0f, gdy = 4.0f * s;
                        if (g.in_ms) {
                            const float t = motion_on ? std::clamp(static_cast<float>(now_ms - g.in_ms) / 300.0f, 0.0f, 1.0f)
                                                      : 1.0f;
                            if (t < 1.0f) conv_anim_ = true;
                            const float e = CubicBezier(0.25f, 0.1f, 0.25f, 1.0f, t);
                            ga = e;
                            gdy = 4.0f * s * (1.0f - e);
                        }
                        if (ga > 0.0f) {
                            const bool gp = push_entrance({ga, gdy});
                            const float gx = r.left + 1.0f * s + 12.0f * s;
                            const float gy = yy + 7.0f * s;
                            const int ti = static_cast<int>(g.tint);
                            c.Fill(D2D1::RectF(gx, gy, gx + 26.0f * s, gy + 26.0f * s), 7.0f * s, pal.tint_bg[ti]);
                            c.Icon(g.icon, gx + 5.0f * s, gy + 5.0f * s, 16.0f * s, pal.tint_fg[ti]);
                            const float mx = gx + 36.0f * s;
                            const float mr = r.right - 1.0f * s - 12.0f * s;
                            float nx = mx;
                            const float nw = c.Measure(g.name, formats_.body13b);
                            line(g.name, formats_.body13b, nx, gy, nx + nw + 1.0f, 18.0f * s, pal.text);
                            nx += nw + 6.0f * s;
                            const float cw = c.Measure(g.count, formats_.caption);
                            line(g.count, formats_.caption, nx, gy, nx + cw + 1.0f, 18.0f * s, pal.text3);
                            nx += cw;
                            if (!g.badge.empty()) {
                                // .newp: 10.5 semibold, padding 0 6, radius 8, lh 17.
                                nx += 6.0f * s;
                                const float bw = c.Measure(g.badge, formats_.badge) + 12.0f * s;
                                const D2D1_RECT_F br = D2D1::RectF(nx, gy + 0.5f * s, nx + bw, gy + 17.5f * s);
                                c.Fill(br, 8.0f * s, pal.accent_soft);
                                line(g.badge, formats_.badge, br.left + 6.0f * s, br.top, br.right, 17.0f * s, pal.accent);
                            }
                            line(g.list, formats_.caption, mx, gy + 18.0f * s, mr, 18.0f * s, pal.text2);
                            pop_entrance(gp);
                        }
                        yy += gh;
                    }
                }
            }
            hit(hr, AiPanelPart::PlanToggle, index);
            y += h;
            pending = 12.0f * s;
            break;
        }
        case AiBlockKind::Confirm: {
            place(0.0f);
            const float il = x0 + 1.0f * s;
            const float ir = x1 - 1.0f * s;
            if (b.done) {
                // .confirm.done: padding 9 12, icon 24 accent-soft / ok,
                // title 13 text2 lh 24.
                const float h = 2.0f * s + 42.0f * s;
                const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + h);
                if (visible(r.top, r.bottom)) {
                    card(r, pal.line);
                    const float iy = r.top + 1.0f * s + 9.0f * s;
                    c.Fill(D2D1::RectF(il + 12.0f * s, iy, il + 36.0f * s, iy + 24.0f * s), 8.0f * s, pal.accent_soft);
                    c.Icon(AiIcon::Shield, il + 17.0f * s, iy + 5.0f * s, 14.0f * s, pal.ok);
                    line(b.title, formats_.body13, il + 46.0f * s, iy, ir - 12.0f * s, 24.0f * s, pal.text2);
                }
                y += h;
                pending = 12.0f * s;
                break;
            }
            // .cf-top padding 12 12 4, icon 30; .cf-hint padding 6 12 0 52;
            // .cf-b padding 12, buttons 32.
            const float tx = il + 52.0f * s - 1.0f * s;
            const float text_w = ir - 12.0f * s - tx;
            const float title_h = (std::max)(20.0f * s, wrapped_h(b.title, formats_.body_b, text_w, 20.0f * s));
            const float sub_h = b.sub.empty() ? 0.0f : 2.0f * s + wrapped_h(b.sub, formats_.caption, text_w, 18.0f * s);
            const float top_h = 12.0f * s + (std::max)(30.0f * s, title_h + sub_h) + 4.0f * s;
            const float hint_w = ir - 12.0f * s - (tx + 19.0f * s);
            const float hint_h = b.hint.empty() ? 0.0f : 6.0f * s + wrapped_h(b.hint, formats_.caption, hint_w, 18.0f * s);
            // .lktip: max-height 0 → 40 and opacity while the link is hovered
            // (.3 s / .25 s); padding 0 12 10 52, margin-top -4.
            const float tip_k = b.extra_tip.empty() ? 0.0f
                                                    : TweenTo(key + 2, hot(AiPanelPart::ConfirmExtra, index) ? 1.0f : 0.0f, 300, now_ms);
            const float tip_h = tip_k * (18.0f * s + 10.0f * s - 4.0f * s);
            const float h = 2.0f * s + top_h + hint_h + 56.0f * s + tip_h;
            const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + h);
            if (visible(r.top - 4.0f * s, r.bottom + 4.0f * s)) {
                // box-shadow 0 0 0 3px amber 12%.
                c.Fill(Inflate(r, 3.0f * s), 15.0f * s, Alpha(pal.amber, 0.12f));
                card(r, pal.amber_line);
                const float iy = r.top + 1.0f * s + 12.0f * s;
                c.Fill(D2D1::RectF(il + 12.0f * s, iy, il + 42.0f * s, iy + 30.0f * s), 8.0f * s, pal.amber_bg);
                c.Icon(AiIcon::Shield, il + 18.5f * s, iy + 6.5f * s, 17.0f * s, pal.amber);
                wrapped(b.title, formats_.body_b, tx, iy, text_w, 20.0f * s, pal.text);
                if (!b.sub.empty()) wrapped(b.sub, formats_.caption, tx, iy + title_h + 2.0f * s, text_w, 18.0f * s, pal.text2);
                float yy = r.top + 1.0f * s + top_h;
                if (!b.hint.empty()) {
                    yy += 6.0f * s;
                    c.Icon(b.hint_icon, tx, yy + 2.5f * s, 13.0f * s, pal.text3);
                    wrapped(b.hint, formats_.caption, tx + 19.0f * s, yy, hint_w, 18.0f * s, pal.text3);
                    yy += hint_h - 6.0f * s;
                }
                yy += 12.0f * s;
                float bx = ir - 12.0f * s;
                bx -= button(bx, yy, b.accept, true, AiIcon::Check, false, AiPanelPart::ConfirmAccept, index) + 8.0f * s;
                bx -= button(bx, yy, b.deny, false, AiIcon::Close, false, AiPanelPart::ConfirmDeny, index);
                if (!b.extra.empty()) {
                    // .lk: margin-right auto, no border or fill, accent 600,
                    // padding 0 8, sliders icon 14, gap 6; hover fill. In a
                    // narrow panel the text ends in … (only the icon when tight).
                    const float room = bx - 8.0f * s - (il + 12.0f * s);
                    float lw = 8.0f * s + 14.0f * s + 6.0f * s + c.Measure(b.extra, formats_.body13b) + 8.0f * s;
                    if (lw > room) lw = room >= 8.0f * s + 14.0f * s + 6.0f * s + 40.0f * s + 8.0f * s ? room : 30.0f * s;
                    const D2D1_RECT_F lr = D2D1::RectF(il + 12.0f * s, yy, il + 12.0f * s + lw, yy + 32.0f * s);
                    const bool lh = hot(AiPanelPart::ConfirmExtra, index);
                    D2D1_MATRIX_3X2_F before;
                    dc->GetTransform(&before);
                    if (down(AiPanelPart::ConfirmExtra, index) && motion_on) {
                        const D2D1_POINT_2F mid = D2D1::Point2F((lr.left + lr.right) * 0.5f, (lr.top + lr.bottom) * 0.5f);
                        dc->SetTransform(D2D1::Matrix3x2F::Scale(0.94f, 0.94f, mid) * before);
                    }
                    if (lh) c.Fill(lr, 8.0f * s, pal.hover);
                    c.Icon(AiIcon::Sliders, lr.left + 8.0f * s, yy + 9.0f * s, 14.0f * s, pal.accent);
                    if (lw > 30.0f * s)
                        line(b.extra, formats_.body13b, lr.left + 28.0f * s, yy, lr.right - 8.0f * s, 32.0f * s, pal.accent);
                    dc->SetTransform(before);
                    hit(lr, AiPanelPart::ConfirmExtra, index);
                }
                if (tip_k > 0.0f) {
                    const float ty = yy + 32.0f * s + 12.0f * s - 4.0f * s;
                    const D2D1_RECT_F clip = D2D1::RectF(il, ty, ir, ty + tip_h);
                    dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
                    line(b.extra_tip, formats_.meta, il + 52.0f * s - 1.0f * s, ty, ir - 12.0f * s, 18.0f * s,
                         Alpha(pal.text3, std::clamp(tip_k * 1.2f, 0.0f, 1.0f)));
                    dc->PopAxisAlignedClip();
                }
            }
            y += h;
            pending = 12.0f * s;
            break;
        }
        case AiBlockKind::Result: {
            place(0.0f);
            // .result padding 12; .r-i 30 circle; .r-fs margin 10 0 0 40,
            // gap 6, tags lh 22; .r-b margin 12 0 0 40.
            const float il = x0 + 1.0f * s + 12.0f * s;
            const float ir = x1 - 1.0f * s - 12.0f * s;
            const float tx = il + 40.0f * s;
            const float text_w = ir - tx;
            const float title_h = (std::max)(20.0f * s, wrapped_h(b.title, formats_.body_b, text_w, 20.0f * s));
            const float sub_h = b.sub.empty() ? 0.0f : 2.0f * s + wrapped_h(b.sub, formats_.caption, text_w, 18.0f * s);
            const float top_h = (std::max)(30.0f * s, title_h + sub_h);
            // Tag rows (flex-wrap).
            struct TagPos { float x, y, w; };
            std::vector<TagPos> tags;
            float tags_h = 0.0f;
            if (!b.done && !b.tags.empty()) {
                float tx2 = tx, ty2 = 0.0f;
                for (const AiTagView& t : b.tags) {
                    const float tw = (6.0f + 13.0f + 5.0f + 8.0f) * s + c.Measure(t.text, formats_.caption_b);
                    if (tx2 > tx && tx2 + tw > ir) { tx2 = tx; ty2 += 28.0f * s; }
                    tags.push_back({tx2, ty2, tw});
                    tx2 += tw + 6.0f * s;
                }
                tags_h = 10.0f * s + ty2 + 22.0f * s;
            }
            // .r-ln: margin 10 0 0 40, gap 4, 12.5 text2 lh 18, icon 13 (ok / amber).
            const float lines_h = b.done || b.lines.empty()
                                      ? 0.0f
                                      : 10.0f * s + 18.0f * s * static_cast<float>(b.lines.size()) +
                                            4.0f * s * static_cast<float>(b.lines.size() - 1);
            const bool buttons = !b.done && (!b.deny.empty() || !b.accept.empty());
            const float h = 2.0f * s + 24.0f * s + top_h + lines_h + tags_h + (buttons ? 44.0f * s : 0.0f);
            const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + h);
            if (visible(r.top, r.bottom)) {
                card(r, pal.line);
                const float iy = r.top + 1.0f * s + 12.0f * s;
                const D2D1_POINT_2F mid = D2D1::Point2F(il + 15.0f * s, iy + 15.0f * s);
                c.dc->FillEllipse(D2D1::Ellipse(mid, 15.0f * s, 15.0f * s), c.B(b.done ? pal.hover : pal.accent_soft));
                c.Icon(b.done ? AiIcon::Undo : AiIcon::Check, mid.x - 8.0f * s, mid.y - 8.0f * s, 16.0f * s,
                       b.done ? pal.text2 : pal.ok);
                wrapped(b.title, formats_.body_b, tx, iy, text_w, 20.0f * s, pal.text);
                if (!b.sub.empty()) wrapped(b.sub, formats_.caption, tx, iy + title_h + 2.0f * s, text_w, 18.0f * s, pal.text2);
                float yy = iy + top_h;
                if (lines_h > 0.0f) {
                    yy += 10.0f * s;
                    for (size_t i = 0; i < b.lines.size(); ++i) {
                        const AiResultLine& ln = b.lines[i];
                        c.Icon(ln.warn ? AiIcon::Warn : AiIcon::Check, tx, yy + 2.5f * s, 13.0f * s, ln.warn ? pal.amber : pal.ok);
                        float lx = tx + 20.0f * s;
                        const float w1 = c.Measure(ln.before, formats_.small125);
                        line(ln.before, formats_.small125, lx, yy, ir, 18.0f * s, pal.text2);
                        lx += w1;
                        const float w2 = c.Measure(ln.bold, formats_.small125b);
                        line(ln.bold, formats_.small125b, lx, yy, ir, 18.0f * s, pal.text);
                        lx += w2;
                        line(ln.after, formats_.small125, lx, yy, ir, 18.0f * s, pal.text2);
                        yy += 18.0f * s + (i + 1 < b.lines.size() ? 4.0f * s : 0.0f);
                    }
                }
                if (!tags.empty()) {
                    yy += 10.0f * s;
                    for (size_t i = 0; i < tags.size(); ++i) {
                        const AiTagView& t = b.tags[i];
                        const int ti = static_cast<int>(t.tint);
                        const D2D1_RECT_F tr = D2D1::RectF(tags[i].x, yy + tags[i].y, tags[i].x + tags[i].w,
                                                           yy + tags[i].y + 22.0f * s);
                        c.Fill(tr, 7.0f * s, pal.tint_bg[ti]);
                        c.Icon(AiIcon::Folder, tr.left + 6.0f * s, tr.top + 4.5f * s, 13.0f * s, pal.tint_fg[ti]);
                        line(t.text, formats_.caption_b, tr.left + 24.0f * s, tr.top, tr.right, 22.0f * s, pal.tint_fg[ti]);
                    }
                    yy += tags_h - 10.0f * s;
                }
                if (buttons) {
                    yy += 12.0f * s;
                    // Buttons flow left to right from the text column.
                    const float uw = 28.0f * s + 20.0f * s + c.Measure(b.deny, formats_.body13);
                    // Too tight for both from the text column: start them
                    // further left (down to the card's padding) before
                    // shortening the second one.
                    float bx0 = tx;
                    if (!b.accept.empty()) {
                        const float need = uw + 8.0f * s + 28.0f * s + 20.0f * s + c.Measure(b.accept, formats_.body13);
                        if (need > text_w) bx0 = (std::max)(il + 12.0f * s, tx + text_w - need);
                    }
                    float bx = bx0 + uw;
                    button(bx, yy, b.deny, false, AiIcon::Undo, true, AiPanelPart::ResultUndo, index);
                    if (!b.accept.empty()) {
                        const float left = bx + 8.0f * s;
                        const float room = tx + text_w - left;
                        const float aw = (std::min)(28.0f * s + 20.0f * s + c.Measure(b.accept, formats_.body13), room);
                        button(left + aw, yy, b.accept, false, b.accept_icon, true, AiPanelPart::ResultShow, index, aw);
                    }
                }
            }
            y += h;
            pending = 12.0f * s;
            break;
        }
        case AiBlockKind::Acts: {
            // .acts: margin -4 0 8 -6, gap 2, buttons 28 (icon 15), meta 11.5.
            place(-4.0f * s);
            const float ay = top + y;
            if (visible(ay, ay + 28.0f * s)) {
                float x = x0 - 6.0f * s;
                const struct { AiPanelPart part; AiIcon icon; } items[] = {
                    {AiPanelPart::ActCopy, AiIcon::Copy},
                    {AiPanelPart::ActRetry, AiIcon::Retry},
                    {AiPanelPart::ActThumb, AiIcon::Thumb},
                };
                for (const auto& it : items) {
                    if (it.part == AiPanelPart::ActThumb && b.done) continue;   // failed turn
                    const D2D1_RECT_F br = D2D1::RectF(x, ay, x + 28.0f * s, ay + 28.0f * s);
                    const bool h = hot(it.part, index);
                    if (h) c.Fill(br, 8.0f * s, down(it.part, index) ? Mix(pal.hover, pal.text, 0.06f) : pal.hover);
                    c.Icon(it.icon, x + 6.5f * s, ay + 6.5f * s, 15.0f * s, h ? pal.text : pal.text2);
                    hit(br, it.part, index);
                    x += 30.0f * s;
                }
                line(b.meta, formats_.meta, x - 2.0f * s + 8.0f * s, ay, x1, 28.0f * s, pal.text3);
            }
            y += 28.0f * s;
            pending = 8.0f * s;
            break;
        }
        case AiBlockKind::Working: {
            // .working: h 28, gap 8, spark 16 spinning, 12.5 shimmer; margin 2 0 6.
            place(2.0f * s);
            const float wy = top + y;
            if (visible(wy, wy + 28.0f * s)) {
                AiIconPose pose;
                if (motion_on) { pose = SparkPose(now_ms); conv_decor_ = true; }
                c.Icon(AiIcon::Spark, x0, wy + 6.0f * s, 16.0f * s, pal.accent, pose);
                shimmer(b.text, formats_.small125, x0 + 24.0f * s, wy, 28.0f * s);
            }
            y += 28.0f * s;
            pending = 6.0f * s;
            break;
        }
        case AiBlockKind::Error: {
            // .err: border 1, radius 12, padding 12, margin 0 0 12. .e-top gap 10:
            // .e-i 30 tile (radius 8, icon 17); .e-t 13.5/600 lh 20; .e-s 12 lh 18
            // margin-top 2. .e-d / .e-h / .e-b margin-left 40.
            place(0.0f);
            if (b.done) {
                // .err.done (a paused task decided): padding 9 12, .e-i 24 on
                // hover (icon 14 text2), .e-t 13 regular text2 lh 24.
                const float h = 2.0f * s + 42.0f * s;
                const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + h);
                if (visible(r.top, r.bottom)) {
                    card(r, pal.line);
                    const float il = x0 + 1.0f * s + 12.0f * s;
                    const float iy = r.top + 1.0f * s + 9.0f * s;
                    c.Fill(D2D1::RectF(il, iy, il + 24.0f * s, iy + 24.0f * s), 8.0f * s, pal.hover);
                    c.Icon(b.icon, il + 5.0f * s, iy + 5.0f * s, 14.0f * s, pal.text2);
                    line(b.title, formats_.body13, il + 34.0f * s, iy, x1 - 13.0f * s, 24.0f * s, pal.text2);
                }
                y += h;
                pending = 12.0f * s;
                break;
            }
            const bool live = !b.gone_ms;
            const float il = x0 + 1.0f * s + 12.0f * s;
            const float ir = x1 - 1.0f * s - 12.0f * s;
            const float tx = il + 40.0f * s;
            const float text_w = (std::max)(1.0f, ir - tx);
            const float title_h = (std::max)(20.0f * s, wrapped_h(b.title, formats_.body_b, text_w, 20.0f * s));
            const float sub_h = b.sub.empty() ? 0.0f : 2.0f * s + wrapped_h(b.sub, formats_.caption, text_w, 18.0f * s);
            const float top_h = (std::max)(30.0f * s, title_h + sub_h);
            // .e-d: padding 7 10, border 1 line2, radius 8, mono 11.5/17, at most 3 lines.
            const float dw = text_w - 22.0f * s;
            const float detail_h =
                b.detail.empty() ? 0.0f
                                 : (std::min)(51.0f * s, (std::max)(17.0f * s, wrapped_h(b.detail, formats_.mono, dw, 17.0f * s)));
            const float detail_box = b.detail.empty() ? 0.0f : 8.0f * s + detail_h + 16.0f * s;
            // .e-h: warn 13, gap 6, 12 text3.
            const float hint_w = text_w - 19.0f * s;
            const float hint_h = b.hint.empty() ? 0.0f : (std::max)(18.0f * s, wrapped_h(b.hint, formats_.caption, hint_w, 18.0f * s));
            const float hint_box = b.hint.empty() ? 0.0f : 8.0f * s + hint_h;
            // .e-f: the file a paused task is stuck on; margin 8 0 0 40,
            // padding 6 10, radius 8, chrome, border line2, 12.
            const float file_box = b.file.empty() ? 0.0f : 8.0f * s + 30.0f * s;
            // .e-b: gap 8; buttons wrap when the panel is narrow.
            struct BtnPos { float x, y, w; };
            std::vector<BtnPos> btns;
            float bx = tx, by = 0.0f;
            for (const AiErrButton& eb : b.actions) {
                IDWriteTextFormat* fmt = eb.primary ? formats_.body13b : formats_.body13;
                const float bw = 28.0f * s + 20.0f * s + c.Measure(eb.text, fmt);
                if (bx > tx && bx + bw > ir) { bx = tx; by += 40.0f * s; }
                btns.push_back({bx, by, bw});
                bx += bw + 8.0f * s;
            }
            const float btn_h = btns.empty() ? 0.0f : 12.0f * s + by + 32.0f * s;
            const float h = 2.0f * s + 24.0f * s + top_h + detail_box + hint_box + file_box + btn_h;
            const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + h);
            if (visible(r.top - 4.0f * s, r.bottom + 4.0f * s)) {
                const D2D1_COLOR_F ink = b.tone == AiErrTone::Err ? pal.bad : b.tone == AiErrTone::Warn ? pal.amber : pal.accent;
                const D2D1_COLOR_F tile = b.tone == AiErrTone::Err    ? pal.bad_bg
                                          : b.tone == AiErrTone::Warn ? pal.amber_bg
                                                                      : pal.accent_soft;
                // Only the red tone takes a tinted border; a paused task (a
                // file row) asks like a confirmation: amber line + 3 px ring.
                const bool pause = !b.file.empty() && live;
                if (pause) c.Fill(Inflate(r, 3.0f * s), 15.0f * s, Alpha(pal.amber, 0.12f));
                card(r, b.tone == AiErrTone::Err ? pal.bad_line : pause ? pal.amber_line : pal.line);
                const float iy = r.top + 1.0f * s + 12.0f * s;
                const D2D1_RECT_F tr = D2D1::RectF(il, iy, il + 30.0f * s, iy + 30.0f * s);
                c.Fill(tr, 8.0f * s, tile);
                c.Icon(b.icon, il + 6.5f * s, iy + 6.5f * s, 17.0f * s, ink);
                wrapped(b.title, formats_.body_b, tx, iy, text_w, 20.0f * s, pal.text);
                if (!b.sub.empty()) wrapped(b.sub, formats_.caption, tx, iy + title_h + 2.0f * s, text_w, 18.0f * s, pal.text2);
                float yy = iy + top_h;
                if (!b.detail.empty()) {
                    yy += 8.0f * s;
                    const D2D1_RECT_F dr = D2D1::RectF(tx, yy, ir, yy + detail_h + 16.0f * s);
                    c.Fill(dr, 8.0f * s, pal.pane);
                    c.Stroke(dr, 8.0f * s, pal.line2);
                    dc->PushAxisAlignedClip(D2D1::RectF(dr.left, dr.top + 7.0f * s, dr.right, dr.top + 7.0f * s + detail_h + 1.0f),
                                            D2D1_ANTIALIAS_MODE_ALIASED);
                    wrapped(b.detail, formats_.mono, dr.left + 11.0f * s, dr.top + 8.0f * s, dw, 17.0f * s, pal.text2);
                    dc->PopAxisAlignedClip();
                    yy = dr.bottom;
                }
                if (!b.hint.empty()) {
                    yy += 8.0f * s;
                    c.Icon(AiIcon::Warn, tx, yy + 2.5f * s, 13.0f * s, pal.text3);
                    wrapped(b.hint, formats_.caption, tx + 19.0f * s, yy, hint_w, 18.0f * s, pal.text3);
                    yy += hint_h;
                }
                if (!b.file.empty()) {
                    yy += 8.0f * s;
                    const D2D1_RECT_F fr = D2D1::RectF(tx, yy, ir, yy + 30.0f * s);
                    c.Fill(fr, 8.0f * s, Mix(pal.pane, pal.hover, 0.45f));
                    c.Stroke(fr, 8.0f * s, pal.line2);
                    float note_l = fr.right - 10.0f * s;
                    if (!b.file_note.empty()) {
                        // The name comes first: the note gets what is left,
                        // but never less than 40% of the row.
                        const float nw = c.Measure(b.file_note, formats_.caption) + 16.0f * s;
                        const float name_w = c.Measure(b.file, formats_.caption_b);
                        const float row_l = fr.left + 34.0f * s, row_r = fr.right - 10.0f * s;
                        const float room = row_r - row_l - 12.0f * s;
                        const float note_w = (std::min)(nw, (std::max)(room - name_w, room * 0.4f));
                        note_l = row_r - note_w;
                        c.Icon(AiIcon::Lock, note_l, yy + 9.0f * s, 12.0f * s, pal.text3);
                        line(b.file_note, formats_.caption, note_l + 16.0f * s, yy, fr.right - 10.0f * s, 30.0f * s, pal.text3);
                    }
                    c.Icon(AiIcon::Document, fr.left + 10.0f * s, yy + 7.0f * s, 16.0f * s, pal.text2);
                    line(b.file, formats_.caption_b, fr.left + 34.0f * s, yy, note_l - 8.0f * s, 30.0f * s, pal.text);
                    yy = fr.bottom;
                }
                if (!btns.empty()) {
                    yy += 12.0f * s;
                    for (size_t i = 0; i < btns.size(); ++i) {
                        const AiErrButton& eb = b.actions[i];
                        const int hi = live ? index * 4 + static_cast<int>(i) : -1;
                        button(btns[i].x + btns[i].w, yy + btns[i].y, eb.text, eb.primary, eb.icon, true,
                               AiPanelPart::ErrAction, hi);
                    }
                    // .e-m: the model, pushed right; only with fewer than 3 buttons.
                    if (b.actions.size() < 3 && b.tone != AiErrTone::Accent && !b.meta.empty() && by == 0.0f) {
                        const float last = btns.back().x + btns.back().w + 12.0f * s;
                        const float mw = c.Measure(b.meta, formats_.meta);
                        if (last + mw <= ir)
                            line(b.meta, formats_.meta, ir - mw, yy, ir + 2.0f, 32.0f * s, pal.text3);
                    }
                }
            }
            y += h;
            pending = 12.0f * s;
            break;
        }
        case AiBlockKind::Cut: {
            // .cut: h 34, padding 0 6 0 10, margin -2 0 12, radius 9, amber-bg,
            // amber 12.5; warn 15 gap 8; .t 600; .lnk h 26 padding 0 10 radius 7.
            place(-2.0f * s);
            const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + 34.0f * s);
            if (visible(r.top, r.bottom)) {
                c.Fill(r, 9.0f * s, pal.amber_bg);
                float x = r.left + 10.0f * s;
                c.Icon(AiIcon::Warn, x, r.top + 9.5f * s, 15.0f * s, pal.amber);
                x += 15.0f * s + 8.0f * s;
                // The link at the right first, so the words know their room.
                const float lw = b.accept.empty() ? 0.0f : 20.0f * s + 13.0f * s + 5.0f * s + c.Measure(b.accept, formats_.small125b);
                const D2D1_RECT_F lr = D2D1::RectF(r.right - 6.0f * s - lw, r.top + 4.0f * s, r.right - 6.0f * s, r.top + 30.0f * s);
                const float room = (lr.left - 8.0f * s);
                const float tw = c.Measure(b.title, formats_.small125b);
                line(b.title, formats_.small125b, x, r.top, room, 34.0f * s, pal.amber);
                x += tw + 8.0f * s;
                if (!b.sub.empty() && x + c.Measure(b.sub, formats_.small125) <= room)
                    line(b.sub, formats_.small125, x, r.top, room, 34.0f * s, pal.amber);
                if (lw > 0.0f) {
                    const int hi = b.gone_ms ? -1 : index;
                    const bool h = hot(AiPanelPart::CutRetry, hi), d = down(AiPanelPart::CutRetry, hi);
                    D2D1_MATRIX_3X2_F before;
                    dc->GetTransform(&before);
                    if (d && motion_on) {
                        const D2D1_POINT_2F mid = D2D1::Point2F((lr.left + lr.right) * 0.5f, (lr.top + lr.bottom) * 0.5f);
                        dc->SetTransform(D2D1::Matrix3x2F::Scale(0.94f, 0.94f, mid) * before);
                    }
                    if (h) c.Fill(lr, 7.0f * s, Alpha(pal.amber, 0.14f));
                    c.Icon(AiIcon::Retry, lr.left + 10.0f * s, lr.top + 6.5f * s, 13.0f * s, pal.amber);
                    line(b.accept, formats_.small125b, lr.left + 28.0f * s, lr.top, lr.right, 26.0f * s, pal.amber);
                    dc->SetTransform(before);
                    if (hi >= 0) hit(lr, AiPanelPart::CutRetry, hi);
                }
            }
            y += 34.0f * s;
            pending = 12.0f * s;
            break;
        }
        case AiBlockKind::Query: {
            // .qcard: padding 10 12 12, radius 12; .q-h 24; .q-chips margin
            // 9 0 10, gap 6; .q-res padding-top 10 over a line2 rule; .q-loc
            // margin-top 8, gap 6.
            place(0.0f);
            const float il = x0 + 1.0f * s + 12.0f * s;
            const float ir = x1 - 1.0f * s - 12.0f * s;
            // Chips (flex-wrap). A removed chip shrinks away (.qc.out: opacity
            // .2 s, scale .25 s, its width .35 s after .1 s).
            struct ChipPos { float x, y, w, full, alpha, scale; };
            std::vector<ChipPos> chips;
            float cx = il, cy = 0.0f;
            const auto chip_w = [&](const AiQueryChipView& q) {
                return 2.0f * s + 9.0f * s + c.Measure(q.key, formats_.caption_b) + 5.0f * s +
                       c.Measure(q.value, formats_.caption) + (q.fixed ? 9.0f * s : 5.0f * s + 18.0f * s + 4.0f * s);
            };
            for (const AiQueryChipView& q : b.chips) {
                const float full = chip_w(q);
                float w = full, alpha = 1.0f, scale = 1.0f;
                if (q.out_ms) {
                    const float t = motion_on ? static_cast<float>(now_ms - q.out_ms) : 1000.0f;
                    alpha = 1.0f - std::clamp(t / 200.0f, 0.0f, 1.0f);
                    scale = 1.0f - 0.3f * std::clamp(t / 250.0f, 0.0f, 1.0f);
                    const float k = EaseDesign(std::clamp((t - 100.0f) / 350.0f, 0.0f, 1.0f));
                    w = (full + 6.0f * s) * (1.0f - k) - 6.0f * s;
                    if (t < 450.0f) conv_anim_ = true;
                    if (w <= -6.0f * s + 0.5f) { chips.push_back({cx, cy, -6.0f * s, full, 0.0f, scale}); continue; }
                } else if (q.in_ms && motion_on) {
                    if (now_ms < q.in_ms) {
                        alpha = 0.0f;
                        scale = 0.9f;
                        conv_anim_ = true;
                    } else if (now_ms - q.in_ms < 250) {
                        const float t = static_cast<float>(now_ms - q.in_ms) / 250.0f;
                        alpha = t;
                        scale = 0.9f + 0.1f * CubicBezier(0.3f, 1.5f, 0.5f, 1.0f, t);
                        conv_anim_ = true;
                    }
                }
                if (cx > il && cx + w > ir) { cx = il; cy += 32.0f * s; }
                chips.push_back({cx, cy, w, full, alpha, scale});
                cx += w + 6.0f * s;
            }
            const float add_w = b.add.empty() ? 0.0f
                                              : 2.0f * s + 10.0f * s + 12.0f * s + 4.0f * s +
                                                    c.Measure(b.add, formats_.caption) + 10.0f * s;
            float add_x = cx, add_y = cy;
            if (add_w > 0.0f && add_x > il && add_x + add_w > ir) { add_x = il; add_y += 32.0f * s; }
            const float chips_h = (b.chips.empty() && add_w <= 0.0f) ? 0.0f
                                                                      : (add_w > 0.0f ? add_y : cy) + 26.0f * s;
            // Locations (flex-wrap).
            struct LocPos { float x, y, w; };
            std::vector<LocPos> locs;
            float lx = il, ly = 0.0f;
            for (const AiTagView& t : b.tags) {
                const float w = (6.0f + 16.0f + 6.0f) * s + c.Measure(t.text, formats_.caption) +
                                (t.bold.empty() ? 0.0f : 6.0f * s + c.Measure(t.bold, formats_.caption_b)) + 8.0f * s;
                if (lx > il && lx + w > ir) { lx = il; ly += 30.0f * s; }
                locs.push_back({lx, ly, (std::min)(w, ir - il)});
                lx += w + 6.0f * s;
            }
            const float locs_h = locs.empty() ? 0.0f : 8.0f * s + ly + 24.0f * s;
            // Result row: the re-search hint sits right-aligned after the size;
            // a narrow panel moves it to its own line under the size text.
            const float res_nw = c.Measure(b.count, formats_.num22);
            const float res_ux = il + (std::max)(res_nw, 14.0f * s) + 6.0f * s;
            const float res_uw = c.Measure(b.label, formats_.body13);
            const bool has_hint = b.re_ms && !b.hint.empty();
            const float hint_w = has_hint ? c.Measure(b.hint, formats_.heading) : 0.0f;
            const bool hint_wrap = has_hint && res_ux + res_uw + 12.0f * s + 16.0f * s + hint_w > ir;
            const float res_h = 26.0f * s + (hint_wrap ? 20.0f * s : 0.0f);
            const float h = 2.0f * s + 10.0f * s + 24.0f * s + 9.0f * s + chips_h + 10.0f * s +
                            1.0f * s + 10.0f * s + res_h + locs_h + 12.0f * s;
            const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + h);
            if (visible(r.top, r.bottom)) {
                card(r, pal.line);
                // Header: search icon, 搜索条件, and the index time on the right.
                const float hy = r.top + 1.0f * s + 10.0f * s;
                c.Icon(AiIcon::Search, il, hy + 4.5f * s, 15.0f * s, pal.accent);
                const float tw = c.Measure(b.title, formats_.body13b);
                line(b.title, formats_.body13b, il + 23.0f * s, hy, il + 23.0f * s + tw + 1.0f, 24.0f * s, pal.text);
                if (!b.meta.empty()) {
                    const float mw = c.Measure(b.meta, formats_.meta);
                    const float mx = (std::max)(il + 23.0f * s + tw + 12.0f * s, ir - mw);
                    c.Icon(AiIcon::Bolt, mx - 17.0f * s, hy + 6.0f * s, 12.0f * s, pal.accent);
                    line(b.meta, formats_.meta, mx, hy, ir + 1.0f, 24.0f * s, pal.text3);
                }
                // Chips.
                const float chy = hy + 24.0f * s + 9.0f * s;
                for (size_t i = 0; i < b.chips.size(); ++i) {
                    const ChipPos& p = chips[i];
                    if (p.alpha <= 0.0f || p.w <= 0.0f) continue;
                    const AiQueryChipView& q = b.chips[i];
                    const D2D1_RECT_F cr = D2D1::RectF(p.x, chy + p.y, p.x + p.full, chy + p.y + 26.0f * s);
                    const D2D1_POINT_2F mid = D2D1::Point2F((cr.left + cr.right) * 0.5f, (cr.top + cr.bottom) * 0.5f);
                    D2D1_MATRIX_3X2_F before;
                    dc->GetTransform(&before);
                    const bool clip = p.w < p.full;
                    if (clip) dc->PushAxisAlignedClip(D2D1::RectF(cr.left, cr.top - 2.0f * s, cr.left + p.w, cr.bottom + 2.0f * s),
                                                      D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    if (p.scale != 1.0f) dc->SetTransform(D2D1::Matrix3x2F::Scale(p.scale, p.scale, mid) * before);
                    const bool layer = p.alpha < 1.0f;
                    if (layer)
                        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                             D2D1::IdentityMatrix(), p.alpha, nullptr,
                                                             D2D1_LAYER_OPTIONS1_NONE),
                                      nullptr);
                    c.Fill(cr, 13.0f * s, pal.accent_tint);
                    c.Stroke(cr, 13.0f * s, pal.accent_soft);
                    float tx = cr.left + 1.0f * s + 9.0f * s;
                    const float kw = c.Measure(q.key, formats_.caption_b);
                    line(q.key, formats_.caption_b, tx, cr.top, tx + kw + 1.0f, 26.0f * s, pal.accent);
                    tx += kw + 5.0f * s;
                    const float vw = c.Measure(q.value, formats_.caption);
                    line(q.value, formats_.caption, tx, cr.top, tx + vw + 1.0f, 26.0f * s, pal.text);
                    tx += vw + 5.0f * s;
                    const int hi = index * 16 + static_cast<int>(i);
                    const D2D1_RECT_F xr = D2D1::RectF(tx, cr.top + 4.0f * s, tx + 18.0f * s, cr.top + 22.0f * s);
                    const bool xh = !q.out_ms && !q.fixed && hot(AiPanelPart::QueryChipRemove, hi);
                    if (xh) c.Fill(xr, 9.0f * s, Alpha(pal.accent, 0.16f));
                    if (!q.fixed)
                        c.Icon(AiIcon::Close, xr.left + 4.0f * s, xr.top + 4.0f * s, 10.0f * s, xh ? pal.text : pal.text3);
                    if (layer) dc->PopLayer();
                    dc->SetTransform(before);
                    if (clip) dc->PopAxisAlignedClip();
                    if (!q.out_ms && !q.fixed) hit(Inflate(xr, 3.0f * s), AiPanelPart::QueryChipRemove, hi);
                }
                if (add_w > 0.0f) {
                    // .qc.add: transparent, dashed line border, text3.
                    const D2D1_RECT_F ar = D2D1::RectF(add_x, chy + add_y, add_x + add_w, chy + add_y + 26.0f * s);
                    const bool ah = hot(AiPanelPart::QueryAdd, index);
                    if (ah) c.Fill(ar, 13.0f * s, pal.accent_tint);
                    ID2D1StrokeStyle* dash = nullptr;
                    if (ID2D1Factory* f = FactoryOf(dc)) {
                        const float dashes[] = {3.0f, 2.0f};
                        f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                                          D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10.0f,
                                                                          D2D1_DASH_STYLE_CUSTOM, 0.0f),
                                             dashes, 2, &dash);
                        f->Release();
                    }
                    const float hw = 0.5f * s;
                    const D2D1_RECT_F sr = D2D1::RectF(ar.left + hw, ar.top + hw, ar.right - hw, ar.bottom - hw);
                    dc->DrawRoundedRectangle(D2D1::RoundedRect(sr, 13.0f * s - hw, 13.0f * s - hw),
                                             c.B(ah ? pal.accent : pal.line), 1.0f * s, dash);
                    if (dash) dash->Release();
                    const D2D1_COLOR_F ink = ah ? pal.text : pal.text3;
                    c.Icon(AiIcon::Plus, ar.left + 11.0f * s, ar.top + 7.0f * s, 12.0f * s, ink);
                    line(b.add, formats_.caption, ar.left + 27.0f * s, ar.top, ar.right, 26.0f * s, ink);
                    hit(ar, AiPanelPart::QueryAdd, index);
                }
                // Result line: the count (bumps when it changes), what and how
                // big, and 本机重新搜索… once a chip changed it.
                float yy = chy + chips_h + 10.0f * s;
                c.Fill(D2D1::RectF(il, yy, ir, yy + 1.0f * s), 0.0f, pal.line2);
                yy += 1.0f * s + 10.0f * s;
                const float nw = res_nw;
                float bump = 1.0f;
                if (b.bump_ms && motion_on && now_ms >= b.bump_ms && now_ms - b.bump_ms < 450) {
                    const float t = static_cast<float>(now_ms - b.bump_ms) / 450.0f;
                    bump = t < 0.4f ? 1.0f + 0.28f * EaseDesign(t / 0.4f) : 1.28f - 0.28f * EaseDesign((t - 0.4f) / 0.6f);
                    conv_anim_ = true;
                }
                {
                    D2D1_MATRIX_3X2_F before;
                    dc->GetTransform(&before);
                    const D2D1_POINT_2F mid = D2D1::Point2F(il + nw * 0.5f, yy + 13.0f * s);
                    if (bump != 1.0f) dc->SetTransform(D2D1::Matrix3x2F::Scale(bump, bump, mid) * before);
                    line(b.count, formats_.num22, il, yy, il + nw + 2.0f, 26.0f * s, pal.text);
                    dc->SetTransform(before);
                }
                const float ux = res_ux;
                const float uw = res_uw;
                line(b.label, formats_.body13, ux, yy + 6.0f * s, ux + uw + 1.0f, 18.0f * s, pal.text2);
                if (has_hint) {
                    float ra = 1.0f, rdy = 0.0f;
                    if (motion_on && now_ms - b.re_ms < 300) {
                        const float t = now_ms < b.re_ms ? 0.0f : static_cast<float>(now_ms - b.re_ms) / 300.0f;
                        ra = EaseDesign(t);
                        rdy = 3.0f * s * (1.0f - ra);
                        conv_anim_ = true;
                    }
                    const float hx = hint_wrap ? ux + 16.0f * s : ir - hint_w;
                    const float hy2 = hint_wrap ? yy + 26.0f * s : yy;
                    const D2D1_COLOR_F ink = Alpha(pal.accent, ra);
                    c.Icon(AiIcon::Bolt, hx - 16.0f * s, hy2 + 7.0f * s + rdy, 12.0f * s, ink);
                    line(b.hint, formats_.heading, hx, hy2 + 4.0f * s + rdy, ir + 1.0f, 18.0f * s, ink);
                }
                yy += res_h;
                // Where the results are: folder pills with a count.
                if (!locs.empty()) {
                    yy += 8.0f * s;
                    const D2D1_COLOR_F chrome = Mix(pal.pane, pal.hover, 0.45f);
                    for (size_t i = 0; i < locs.size(); ++i) {
                        const AiTagView& t = b.tags[i];
                        const D2D1_RECT_F lr = D2D1::RectF(locs[i].x, yy + locs[i].y, locs[i].x + locs[i].w,
                                                           yy + locs[i].y + 24.0f * s);
                        c.Fill(lr, 7.0f * s, chrome);
                        c.Icon(AiIcon::FolderFilled, lr.left + 6.0f * s, lr.top + 4.0f * s, 16.0f * s, pal.folder_icon);
                        const float bw = t.bold.empty() ? 0.0f : c.Measure(t.bold, formats_.caption_b);
                        const float nx = lr.left + 28.0f * s;
                        const float nr = lr.right - 8.0f * s - (bw > 0.0f ? bw + 6.0f * s : 0.0f);
                        line(t.text, formats_.caption, nx, lr.top, nr, 24.0f * s, pal.text2);
                        if (bw > 0.0f) {
                            const float tw2 = (std::min)(c.Measure(t.text, formats_.caption), nr - nx);
                            const float bx = nx + tw2 + 6.0f * s;
                            line(t.bold, formats_.caption_b, bx, lr.top, bx + bw + 1.0f, 24.0f * s, pal.text);
                        }
                    }
                }
            }
            y += h;
            pending = 10.0f * s;
            break;
        }
        case AiBlockKind::Follow: {
            // .fu: flex-wrap gap 6, margin 0 0 12; .fl 11.5 text3 on its own
            // row (margin 0 0 1 2); chips h 30, padding 0 12 0 10, radius 15.
            place(0.0f);
            const float label_h = b.label.empty() ? 0.0f : 16.0f * s + 1.0f * s + 6.0f * s;
            struct FPos { float x, y, w; };
            std::vector<FPos> pos;
            float fx = x0, fy = 0.0f;
            for (const AiFollowView& f : b.follow) {
                const float w = 2.0f * s + 10.0f * s + 14.0f * s + 6.0f * s + c.Measure(f.text, formats_.small125) + 12.0f * s;
                if (fx > x0 && fx + w > x1) { fx = x0; fy += 36.0f * s; }
                pos.push_back({fx, fy, (std::min)(w, x1 - x0)});
                fx += w + 6.0f * s;
            }
            const float h = label_h + (pos.empty() ? 0.0f : fy + 30.0f * s);
            const float fy0 = top + y;
            if (visible(fy0, fy0 + h)) {
                if (!b.label.empty()) line(b.label, formats_.meta, x0 + 2.0f * s, fy0, x1, 16.0f * s, pal.text3);
                for (size_t i = 0; i < pos.size(); ++i) {
                    const AiFollowView& f = b.follow[i];
                    const int fi = index * 8 + static_cast<int>(i);
                    const D2D1_RECT_F fr = D2D1::RectF(pos[i].x, fy0 + label_h + pos[i].y, pos[i].x + pos[i].w,
                                                       fy0 + label_h + pos[i].y + 30.0f * s);
                    const bool fh = hot(AiPanelPart::FollowItem, fi), fd = down(AiPanelPart::FollowItem, fi);
                    D2D1_MATRIX_3X2_F before;
                    dc->GetTransform(&before);
                    if (fd && motion_on) {
                        const D2D1_POINT_2F mid = D2D1::Point2F((fr.left + fr.right) * 0.5f, (fr.top + fr.bottom) * 0.5f);
                        dc->SetTransform(D2D1::Matrix3x2F::Scale(0.95f, 0.95f, mid) * before);
                    }
                    c.Fill(fr, 15.0f * s, fh ? pal.accent_tint : pal.card);
                    c.Stroke(fr, 15.0f * s, fh ? pal.accent : pal.line);
                    c.Icon(f.icon, fr.left + 11.0f * s, fr.top + 8.0f * s, 14.0f * s, pal.accent);
                    line(f.text, formats_.small125, fr.left + 31.0f * s, fr.top, fr.right - 11.0f * s, 30.0f * s, pal.text);
                    dc->SetTransform(before);
                    hit(fr, AiPanelPart::FollowItem, fi);
                }
            }
            y += h;
            pending = 12.0f * s;
            break;
        }
        case AiBlockKind::Rename: {
            // .card: .card-h 40 (icon 16 accent, title 13 semibold, .mu 12
            // text3, chevron); .card-b border-top line2, padding 6 0, holding
            // .rule (margin 6 12 6, padding 8 10, radius 9, chrome), the .rnr
            // rows (padding 5 12, lh 18; old .78fr | 14 | new 1.3fr, gap 8),
            // .rn-more (padding 3 12 7), .checks (border-top, padding 8 12 6,
            // gap 3) and .foot (padding 2 12 10).
            place(0.0f);
            const float il = x0 + 1.0f * s, ir = x1 - 1.0f * s;
            const float bl = il + 12.0f * s, br = ir - 12.0f * s;
            const D2D1_COLOR_F chrome = Mix(pal.pane, pal.hover, 0.45f);
            // The rule's pieces (flex-wrap, gap 4, 22 high): 规则, chips and
            // literal text, then 扩展名不变 pushed to the right.
            struct Piece { float x, y, w; int kind; size_t i; };   // 0 label, 1 chip, 2 literal, 3 note, 4 word
            std::vector<Piece> pieces;
            const float rx0 = bl + 10.0f * s, rx1 = br - 10.0f * s;
            float px = rx0, py = 0.0f;
            const auto put = [&](float w, int kind, size_t i, float gap) {
                if (px > rx0 && px + w > rx1) { px = rx0; py += 26.0f * s; }
                pieces.push_back({px, py, (std::min)(w, rx1 - rx0), kind, i});
                px += w + gap;
            };
            if (!b.rule_label.empty()) put(c.Measure(b.rule_label, formats_.meta), 0, 0, 10.0f * s);
            for (size_t i = 0; i < b.rule.size(); ++i) {
                const AiRuleToken& t = b.rule[i];
                if (t.token) put(c.Measure(t.text, formats_.caption_b) + 14.0f * s, 1, i, 4.0f * s);
                else if (t.word) put(c.Measure(t.text, formats_.meta), 4, i, 4.0f * s);
                else put(c.Measure(t.text, formats_.mono125), 2, i, 4.0f * s);
            }
            if (!b.hint.empty()) {
                const float w = c.Measure(b.hint, formats_.meta);
                if (px + w > rx1 + 0.5f) { px = rx0; py += 26.0f * s; }
                pieces.push_back({rx1 - w, py, w, 3, 0});
            }
            const float rule_h = 8.0f * s + py + 22.0f * s + 8.0f * s;
            std::vector<size_t> shown;
            for (size_t i = 0; i < b.rows.size(); ++i)
                if (b.open || b.rows[i].sample) shown.push_back(i);
            const std::wstring& toggle = b.open ? b.less : b.more;
            const float more_h = toggle.empty() ? 0.0f : 28.0f * s;
            const float checks_h =
                b.checks.empty() ? 0.0f
                                 : 1.0f * s + 8.0f * s + 18.0f * s * static_cast<float>(b.checks.size()) +
                                       3.0f * s * static_cast<float>(b.checks.size() - 1) + 6.0f * s;
            const float foot_h = b.sub.empty() ? 0.0f : 30.0f * s;
            float h = 2.0f * s + 40.0f * s;
            if (!b.collapsed)
                h += 1.0f * s + 6.0f * s + 6.0f * s + rule_h + 6.0f * s + 28.0f * s * static_cast<float>(shown.size()) + more_h + checks_h + foot_h +
                     6.0f * s;
            const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + h);
            const float col_k = TweenTo(key + 1, b.collapsed ? 1.0f : 0.0f, 250, now_ms);
            const D2D1_RECT_F hr = D2D1::RectF(il, r.top + 1.0f, ir, r.top + 1.0f + 40.0f * s);
            if (visible(r.top, r.bottom)) {
                card(r, pal.line);
                if (hot(AiPanelPart::RenameToggle, index)) {
                    dc->PushAxisAlignedClip(hr, D2D1_ANTIALIAS_MODE_ALIASED);
                    c.Fill(Inflate(r, -1.0f), 11.0f * s, Alpha(pal.hover, 0.6f));
                    dc->PopAxisAlignedClip();
                }
                const float cy = hr.top + 20.0f * s;
                c.Icon(AiIcon::Rename, hr.left + 12.0f * s, cy - 8.0f * s, 16.0f * s, pal.accent);
                float hx = hr.left + 36.0f * s;
                const float tw = c.Measure(b.label, formats_.body13b);
                line(b.label, formats_.body13b, hx, hr.top, hx + tw + 1.0f, 40.0f * s, pal.text);
                hx += tw + 8.0f * s;
                line(b.meta, formats_.caption, hx, hr.top, hr.right - 33.0f * s, 40.0f * s, pal.text3);
                chevron(hr.right - 25.0f * s, cy, -90.0f * col_k, pal.text3, AiIcon::ChevronDown);
                if (!b.collapsed) {
                    float yy = hr.bottom;
                    c.Fill(D2D1::RectF(il, yy, ir, yy + 1.0f * s), 0.0f, pal.line2);
                    yy += 1.0f * s + 6.0f * s + 6.0f * s;
                    // .rule
                    const D2D1_RECT_F rb = D2D1::RectF(bl, yy, br, yy + rule_h);
                    c.Fill(rb, 9.0f * s, chrome);
                    for (const Piece& p : pieces) {
                        const float pyy = rb.top + 8.0f * s + p.y;
                        if (p.kind == 0) {
                            line(b.rule_label, formats_.meta, p.x, pyy, p.x + p.w + 1.0f, 22.0f * s, pal.text3);
                        } else if (p.kind == 3) {
                            line(b.hint, formats_.meta, p.x, pyy, p.x + p.w + 1.0f, 22.0f * s, pal.text3);
                        } else if (p.kind == 4) {
                            line(b.rule[p.i].text, formats_.meta, p.x, pyy, p.x + p.w + 1.0f, 22.0f * s, pal.text3);
                        } else if (p.kind == 1) {
                            const D2D1_RECT_F tr = D2D1::RectF(p.x, pyy, p.x + p.w, pyy + 22.0f * s);
                            c.Fill(tr, 6.0f * s, pal.accent_soft);
                            line(b.rule[p.i].text, formats_.caption_b, tr.left + 7.0f * s, tr.top, tr.right - 6.0f * s, 22.0f * s,
                                 pal.accent);
                        } else {
                            line(b.rule[p.i].text, formats_.mono125, p.x, pyy, p.x + p.w + 1.0f, 22.0f * s, pal.text);
                        }
                    }
                    yy = rb.bottom + 6.0f * s;
                    // .rnr rows
                    const float fr = (br - bl - 14.0f * s - 16.0f * s) / 2.08f;
                    const float old_r = bl + 0.78f * fr;
                    const float arrow_x = old_r + 8.0f * s;
                    const float nx0 = arrow_x + 14.0f * s + 8.0f * s;
                    for (size_t k : shown) {
                        const AiRenameRow& row = b.rows[k];
                        // .rnr: opacity 0 + translateY(4) until .in (.3 s).
                        float ra = 0.0f, rdy = 4.0f * s;
                        if (row.in_ms) {
                            const float t = motion_on ? std::clamp(static_cast<float>(now_ms - row.in_ms) / 300.0f, 0.0f, 1.0f) : 1.0f;
                            if (t < 1.0f) conv_anim_ = true;
                            const float e = CubicBezier(0.25f, 0.1f, 0.25f, 1.0f, t);
                            ra = e;
                            rdy = 4.0f * s * (1.0f - e);
                        }
                        if (ra > 0.0f && visible(yy, yy + 28.0f * s)) {
                            const bool rp = push_entrance({ra, rdy});
                            const float ty = yy + 5.0f * s;
                            // Old name: text3, struck through (55 % text3).
                            const float ow = (std::min)(c.Measure(row.old_name, formats_.small125), old_r - bl);
                            line(row.old_name, formats_.small125, bl, ty, old_r, 18.0f * s, pal.text3);
                            const float sy = std::round(ty + 9.5f * s);
                            c.Fill(D2D1::RectF(bl, sy, bl + ow, sy + (std::max)(1.0f, std::round(1.0f * s))), 0.0f,
                                   Alpha(pal.text3, 0.55f));
                            c.Icon(AiIcon::Arrow, arrow_x, ty + 3.0f * s, 12.0f * s, pal.text3);
                            // New name: semibold, token parts in accent, the
                            // extension regular text3, then the amber dot.
                            float nx = nx0;
                            const float nr = br - (row.warn ? 12.0f * s : 0.0f);
                            for (const AiRuleToken& part : row.parts) {
                                if (nx >= nr) break;
                                const float pw = c.Measure(part.text, formats_.small125b);
                                line(part.text, formats_.small125b, nx, ty, nr, 18.0f * s, part.token ? pal.accent : pal.text);
                                nx += pw;
                            }
                            if (nx < nr && !row.ext.empty()) {
                                const float ew = c.Measure(row.ext, formats_.small125);
                                line(row.ext, formats_.small125, nx, ty, nr, 18.0f * s, pal.text3);
                                nx += ew;
                            }
                            if (row.warn) {
                                const float dx = (std::min)(nx, nr) + 6.0f * s + 3.0f * s;
                                c.dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dx, ty + 9.5f * s), 3.0f * s, 3.0f * s),
                                                  c.B(pal.amber));
                            }
                            pop_entrance(rp);
                        }
                        yy += 28.0f * s;
                    }
                    // .rn-more
                    if (!toggle.empty()) {
                        const float mw = 16.0f * s + c.Measure(toggle, formats_.caption_b);
                        const D2D1_RECT_F mr = D2D1::RectF(bl, yy + 3.0f * s, bl + mw, yy + 21.0f * s);
                        const bool mh = hot(AiPanelPart::RenameMore, index);
                        AiIconPose pose;
                        pose.rotate_deg = b.open ? 180.0f : 0.0f;
                        const D2D1_COLOR_F ink = mh ? Mix(pal.accent, pal.text, 0.25f) : pal.accent;
                        c.Icon(AiIcon::ChevronDown, mr.left, mr.top + 3.0f * s, 12.0f * s, ink, pose);
                        line(toggle, formats_.caption_b, mr.left + 16.0f * s, mr.top, mr.right + 1.0f, 18.0f * s, ink);
                        hit(Inflate(mr, 4.0f * s), AiPanelPart::RenameMore, index);
                        yy += 28.0f * s;
                    }
                    // .checks
                    if (!b.checks.empty()) {
                        c.Fill(D2D1::RectF(il, yy, ir, yy + 1.0f * s), 0.0f, pal.line2);
                        yy += 1.0f * s + 8.0f * s;
                        for (size_t i = 0; i < b.checks.size(); ++i) {
                            const AiCheckView& ck = b.checks[i];
                            c.Icon(ck.warn ? AiIcon::Warn : AiIcon::Check, bl, yy + 2.5f * s, 13.0f * s, ck.warn ? pal.amber : pal.ok);
                            line(ck.text, formats_.caption, bl + 20.0f * s, yy, br, 18.0f * s, pal.text2);
                            yy += 18.0f * s + (i + 1 < b.checks.size() ? 3.0f * s : 0.0f);
                        }
                        yy += 6.0f * s;
                    }
                    // .foot
                    if (!b.sub.empty()) {
                        yy += 2.0f * s;
                        c.Icon(AiIcon::Bolt, bl, yy + 2.5f * s, 13.0f * s, pal.accent);
                        line(b.sub, formats_.meta, bl + 19.0f * s, yy, br, 18.0f * s, pal.text3);
                    }
                }
            }
            hit(hr, AiPanelPart::RenameToggle, index);
            y += h;
            pending = 12.0f * s;
            break;
        }
        case AiBlockKind::Task: {
            // .card .plan: .card-h 40 (tasks icon), .card-b border-top line2.
            // .ps padding 9 12, gap 10: .pn 24 circle (600 12), .pt 13/600
            // lh 18 with .arg (12, hover, r 5, 0 6) and .tg (10.5/600 lh 17,
            // r 8, 0 6), .pd 12 text2 lh 18 margin-top 2, .pg 4 bar margin-top
            // 6 (running / paused). .ps+.ps::before: 2 px line at x 23, -9..9.
            // .p-sum: border-top line2, padding 9 12 10, 12 text2, items
            // (icon 13 accent, gap 5) wrap with gap 4 12; .sf (ok) on its own line.
            place(0.0f);
            const float il = x0 + 1.0f * s, ir = x1 - 1.0f * s;
            const float pl = il + 12.0f * s, pr = ir - 12.0f * s;
            const float mx = pl + 24.0f * s + 10.0f * s;   // .pm
            const float mw = (std::max)(1.0f, pr - mx);
            struct StepBox { float h, desc_h; bool bar; };
            std::vector<StepBox> boxes;
            float steps_h = 0.0f;
            for (const AiTaskStepView& st : b.task) {
                StepBox sb{};
                sb.desc_h = st.desc.empty() ? 0.0f : 2.0f * s + wrapped_h(st.desc, formats_.caption, mw, 18.0f * s);
                sb.bar = st.state == AiTaskState::Running || st.state == AiTaskState::Paused;
                sb.h = 9.0f * s + (std::max)(24.0f * s, 18.0f * s + sb.desc_h + (sb.bar ? 10.0f * s : 0.0f)) + 9.0f * s;
                boxes.push_back(sb);
                steps_h += sb.h;
            }
            // Summary items, flex-wrap.
            struct SumPos { float x, y, w; };
            std::vector<SumPos> sums;
            float sum_h = 0.0f;
            if (!b.sums.empty() || !b.safe.empty()) {
                float sx = pl, sy = 0.0f;
                for (const AiTaskSumView& it : b.sums) {
                    const float w = 13.0f * s + 5.0f * s + c.Measure(it.before, formats_.caption) +
                                    c.Measure(it.bold, formats_.caption_b) + c.Measure(it.after, formats_.caption);
                    if (sx > pl && sx + w > pr) { sx = pl; sy += 22.0f * s; }
                    sums.push_back({sx, sy, (std::min)(w, pr - pl)});
                    sx += w + 12.0f * s;
                }
                float lines_h = sums.empty() ? 0.0f : sy + 18.0f * s;
                if (!b.safe.empty()) lines_h += (sums.empty() ? 0.0f : 4.0f * s) + 18.0f * s;
                sum_h = 2.0f * s + 1.0f * s + 9.0f * s + lines_h + 10.0f * s;
            }
            float h = 2.0f * s + 40.0f * s;
            if (!b.collapsed) h += 1.0f * s + 6.0f * s + steps_h + sum_h;
            const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + h);
            const float col_k = TweenTo(key + 1, b.collapsed ? 1.0f : 0.0f, 250, now_ms);
            const D2D1_RECT_F hr = D2D1::RectF(il, r.top + 1.0f, ir, r.top + 1.0f + 40.0f * s);
            if (visible(r.top, r.bottom)) {
                card(r, pal.line);
                if (hot(AiPanelPart::TaskToggle, index)) {
                    dc->PushAxisAlignedClip(hr, D2D1_ANTIALIAS_MODE_ALIASED);
                    c.Fill(Inflate(r, -1.0f), 11.0f * s, Alpha(pal.hover, 0.6f));
                    dc->PopAxisAlignedClip();
                }
                const float cy = hr.top + 20.0f * s;
                c.Icon(AiIcon::Tasks, hr.left + 12.0f * s, cy - 8.0f * s, 16.0f * s, pal.accent);
                float hx = hr.left + 36.0f * s;
                const float tw = c.Measure(b.label, formats_.body13b);
                line(b.label, formats_.body13b, hx, hr.top, hx + tw + 1.0f, 40.0f * s, pal.text);
                hx += tw + 8.0f * s;
                line(b.meta, formats_.caption, hx, hr.top, hr.right - 33.0f * s, 40.0f * s, pal.text3);
                chevron(hr.right - 25.0f * s, cy, -90.0f * col_k, pal.text3, AiIcon::ChevronDown);
                if (!b.collapsed) {
                    float yy = hr.bottom;
                    c.Fill(D2D1::RectF(il, yy, ir, yy + 1.0f * s), 0.0f, pal.line2);
                    yy += 1.0f * s + 6.0f * s;
                    const D2D1_COLOR_F on_state = pal.on_accent;
                    for (size_t i = 0; i < b.task.size(); ++i) {
                        const AiTaskStepView& st = b.task[i];
                        const StepBox& sb = boxes[i];
                        const D2D1_RECT_F rowr = D2D1::RectF(il, yy, ir, yy + sb.h);
                        // State colours fade over .3 s (background / circle).
                        const float k = st.state_ms && motion_on
                                            ? std::clamp(static_cast<float>(now_ms - st.state_ms) / 300.0f, 0.0f, 1.0f)
                                            : 1.0f;
                        if (k < 1.0f) conv_anim_ = true;
                        if (st.state == AiTaskState::Running) c.Fill(rowr, 0.0f, Alpha(pal.accent_tint, k));
                        else if (st.state == AiTaskState::Paused) c.Fill(rowr, 0.0f, Alpha(pal.amber_bg, k));
                        if (i > 0)
                            c.Fill(D2D1::RectF(pl + 11.0f * s, yy - 9.0f * s, pl + 13.0f * s, yy + 9.0f * s), 0.0f, pal.line);
                        const float py = yy + 9.0f * s;
                        const D2D1_POINT_2F mid = D2D1::Point2F(pl + 12.0f * s, py + 12.0f * s);
                        D2D1_COLOR_F circle = pal.hover, ink = pal.text2;
                        if (st.state == AiTaskState::Done) { circle = pal.ok; ink = on_state; }
                        else if (st.state == AiTaskState::Running) { circle = pal.accent; ink = on_state; }
                        else if (st.state == AiTaskState::Paused) { circle = pal.amber; ink = on_state; }
                        if (k < 1.0f) {
                            circle = Mix(pal.hover, circle, k);
                            ink = Mix(pal.text2, ink, k);
                        }
                        c.dc->FillEllipse(D2D1::Ellipse(mid, 12.0f * s, 12.0f * s), c.B(circle));
                        if (st.state == AiTaskState::Done) {
                            c.Icon(AiIcon::Check, mid.x - 6.5f * s, mid.y - 6.5f * s, 13.0f * s, ink);
                        } else {
                            TextOptions o;
                            o.align = DWRITE_TEXT_ALIGNMENT_CENTER;
                            o.ellipsis = false;
                            const std::wstring n = st.state == AiTaskState::Paused ? std::wstring(L"!") : std::to_wstring(i + 1);
                            c.Text(n, formats_.caption_b, D2D1::RectF(mid.x - 12.0f * s, mid.y - 9.0f * s, mid.x + 12.0f * s, mid.y + 9.0f * s), ink, o);
                        }
                        // .pt: title, then the arg pill and the tag on the same line.
                        float tx2 = mx;
                        const float ttw = (std::min)(c.Measure(st.title, formats_.body13b), mw);
                        line(st.title, formats_.body13b, tx2, py, mx + mw, 18.0f * s, pal.text);
                        tx2 += ttw + 6.0f * s;
                        if (!st.arg.empty() && tx2 + 30.0f * s < pr) {
                            const float aw = (std::min)(c.Measure(st.arg, formats_.caption) + 12.0f * s, pr - tx2);
                            const D2D1_RECT_F ar = D2D1::RectF(tx2, py, tx2 + aw, py + 18.0f * s);
                            c.Fill(ar, 5.0f * s, pal.hover);
                            line(st.arg, formats_.caption, ar.left + 6.0f * s, py, ar.right - 4.0f * s, 18.0f * s, pal.text2);
                            tx2 += aw + 6.0f * s;
                        }
                        if (!st.tag.empty() && tx2 + 30.0f * s < pr) {
                            const float gw = (std::min)(c.Measure(st.tag, formats_.badge) + 12.0f * s, pr - tx2);
                            const D2D1_RECT_F gr = D2D1::RectF(tx2, py + 0.5f * s, tx2 + gw, py + 17.5f * s);
                            const bool ok_tag = !st.tag_warn;
                            c.Fill(gr, 8.0f * s, ok_tag ? pal.accent_soft : pal.amber_bg);
                            line(st.tag, formats_.badge, gr.left + 6.0f * s, gr.top, gr.right - 4.0f * s, 17.0f * s,
                                 ok_tag ? pal.ok : pal.amber);
                        }
                        float dy = py + 18.0f * s;
                        if (!st.desc.empty()) {
                            wrapped(st.desc, formats_.caption, mx, dy + 2.0f * s, mw, 18.0f * s, pal.text2);
                            dy += sb.desc_h;
                        }
                        if (sb.bar) {
                            const D2D1_RECT_F br2 = D2D1::RectF(mx, dy + 6.0f * s, mx + mw, dy + 10.0f * s);
                            c.Fill(br2, 2.0f * s, pal.line);
                            const float p = std::clamp(TweenTo(key + 8 + static_cast<int>(i), st.progress, 250, now_ms), 0.0f, 1.0f);
                            if (p > 0.0f)
                                c.Fill(D2D1::RectF(br2.left, br2.top, br2.left + (br2.right - br2.left) * p, br2.bottom), 2.0f * s,
                                       st.state == AiTaskState::Paused ? pal.amber : pal.accent);
                        }
                        yy += sb.h;
                    }
                    if (sum_h > 0.0f) {
                        yy += 2.0f * s;
                        c.Fill(D2D1::RectF(il, yy, ir, yy + 1.0f * s), 0.0f, pal.line2);
                        yy += 1.0f * s + 9.0f * s;
                        for (size_t i = 0; i < sums.size(); ++i) {
                            const AiTaskSumView& it = b.sums[i];
                            const float sy = yy + sums[i].y;
                            const float right = sums[i].x + sums[i].w;
                            c.Icon(it.icon, sums[i].x, sy + 2.5f * s, 13.0f * s, pal.accent);
                            float sx = sums[i].x + 18.0f * s;
                            const float w1 = c.Measure(it.before, formats_.caption);
                            line(it.before, formats_.caption, sx, sy, right, 18.0f * s, pal.text2);
                            sx += w1;
                            const float w2 = c.Measure(it.bold, formats_.caption_b);
                            line(it.bold, formats_.caption_b, sx, sy, right, 18.0f * s, pal.text);
                            sx += w2;
                            line(it.after, formats_.caption, sx, sy, right + 1.0f, 18.0f * s, pal.text2);
                        }
                        if (!b.safe.empty()) {
                            const float sy = yy + (sums.empty() ? 0.0f : sums.back().y + 18.0f * s + 4.0f * s);
                            c.Icon(AiIcon::Shield, pl, sy + 2.5f * s, 13.0f * s, pal.ok);
                            line(b.safe, formats_.caption, pl + 17.0f * s, sy, pr, 18.0f * s, pal.ok);
                        }
                    }
                }
            }
            hit(hr, AiPanelPart::TaskToggle, index);
            y += h;
            pending = 12.0f * s;
            break;
        }
        case AiBlockKind::Note: {
            // .note: centred 12 text3 between 1 px rules, gap 8; margin 4 0 14.
            place(4.0f * s);
            const float ny = top + y;
            const float tw = c.Measure(b.text, formats_.caption);
            const float cx = (x0 + x1) * 0.5f;
            if (visible(ny, ny + 18.0f * s)) {
                const float l = cx - tw * 0.5f - 8.0f * s, rr = cx + tw * 0.5f + 8.0f * s;
                c.Fill(D2D1::RectF(x0, ny + 9.0f * s, l, ny + 9.0f * s + 1.0f), 0.0f, pal.line);
                c.Fill(D2D1::RectF(rr, ny + 9.0f * s, x1, ny + 9.0f * s + 1.0f), 0.0f, pal.line);
                line(b.text, formats_.caption, cx - tw * 0.5f, ny, cx + tw * 0.5f + 2.0f, 18.0f * s, pal.text3);
            }
            y += 18.0f * s;
            pending = 14.0f * s;
            break;
        }
        }
        pop_entrance(pushed);
    }
    y += pending + 10.0f * s;
    content_h_ = y;

    // Snap a following view to the end now that the height is known (no
    // one-frame lag while text streams in).
    const float max_scroll = (std::max)(0.0f, content_h_ - view_h_);
    if (follow_ && std::fabs(scroll_ - max_scroll) > 0.5f) {
        scroll_ = max_scroll;
        conv_anim_ = true;   // redraw at the new offset
    }
    if (scroll_ > max_scroll) scroll_ = max_scroll;

    // Overlay scrollbar (Chromium's thin bar): only when it overflows.
    if (max_scroll > 0.5f) {
        const float track = view_h_ - 8.0f * s;
        const float thumb = (std::max)(28.0f * s, track * view_h_ / content_h_);
        const float ty = area.top + 4.0f * s + (track - thumb) * (scroll_ / max_scroll);
        const D2D1_RECT_F tr = D2D1::RectF(area.right - 7.0f * s, ty, area.right - 3.0f * s, ty + thumb);
        c.Fill(tr, 2.0f * s, Alpha(pal.text3, 0.35f));
    }

    // Drop tweens of blocks that are gone.
    tweens_.erase(std::remove_if(tweens_.begin(), tweens_.end(), [](const auto& kv) { return !kv.second.used; }),
                  tweens_.end());
}

} // namespace pulse::ui
