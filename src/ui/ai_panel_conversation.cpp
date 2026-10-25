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
                            AiPanelPart part, int index) {
        IDWriteTextFormat* fmt = primary ? formats_.body13b : formats_.body13;
        const float tw = c.Measure(text, fmt);
        const float bw = 28.0f * s + tw + (with_icon ? 20.0f * s : 0.0f);
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
        line(text, fmt, tx, y, r.right, 32.0f * s, ink);
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
            const float hint_h = b.hint.empty() ? 0.0f : 6.0f * s + 18.0f * s;
            const float h = 2.0f * s + top_h + hint_h + 56.0f * s;
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
                    c.Icon(AiIcon::Eye, tx, yy + 2.5f * s, 13.0f * s, pal.text3);
                    line(b.hint, formats_.caption, tx + 19.0f * s, yy, ir - 12.0f * s, 18.0f * s, pal.text3);
                    yy += 18.0f * s;
                }
                yy += 12.0f * s;
                float bx = ir - 12.0f * s;
                bx -= button(bx, yy, b.accept, true, AiIcon::Check, false, AiPanelPart::ConfirmAccept, index) + 8.0f * s;
                button(bx, yy, b.deny, false, AiIcon::Close, false, AiPanelPart::ConfirmDeny, index);
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
            const bool buttons = !b.done && (!b.deny.empty() || !b.accept.empty());
            const float h = 2.0f * s + 24.0f * s + top_h + tags_h + (buttons ? 44.0f * s : 0.0f);
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
                    float bx = tx + uw;
                    button(bx, yy, b.deny, false, AiIcon::Undo, true, AiPanelPart::ResultUndo, index);
                    if (!b.accept.empty()) {
                        bx += 8.0f * s + 28.0f * s + 20.0f * s + c.Measure(b.accept, formats_.body13);
                        button(bx, yy, b.accept, false, AiIcon::Open, true, AiPanelPart::ResultShow, index);
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
            const float h = 2.0f * s + 24.0f * s + top_h + detail_box + hint_box + btn_h;
            const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + h);
            if (visible(r.top, r.bottom)) {
                const D2D1_COLOR_F ink = b.tone == AiErrTone::Err ? pal.bad : b.tone == AiErrTone::Warn ? pal.amber : pal.accent;
                const D2D1_COLOR_F tile = b.tone == AiErrTone::Err    ? pal.bad_bg
                                          : b.tone == AiErrTone::Warn ? pal.amber_bg
                                                                      : pal.accent_soft;
                // Only the red tone takes a tinted border.
                card(r, b.tone == AiErrTone::Err ? pal.bad_line : pal.line);
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
