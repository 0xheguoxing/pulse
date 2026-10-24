// ai_panel_icons.cpp — SVG path data -> D2D geometry for the assistant panel.
#include "ai_panel_icons.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iterator>

namespace pulse::ui {
namespace {

struct IconPart {
    const char* d;
    uint8_t mode;      // 0 stroke, 1 fill with the icon color, 2 fill with `rgb`
    uint32_t rgb;
    float stroke;      // view-box units
    float opacity;
};

struct IconDef {
    const IconPart* parts;
    size_t count;
};

constexpr IconPart k_spark[] = {
    {"M11 3.2c.7 4.9 3.1 7.3 8 8-4.9.7-7.3 3.1-8 8-.7-4.9-3.1-7.3-8-8 4.9-.7 7.3-3.1 8-8z", 1, 0x000000u, 1.6f, 1.0f},
    {"M19 2.6c.25 1.7 1.05 2.5 2.75 2.75-1.7.25-2.5 1.05-2.75 2.75-.25-1.7-1.05-2.5-2.75-2.75 1.7-.25 2.5-1.05 2.75-2.75z", 1, 0x000000u, 1.6f, 0.75f},
};
constexpr IconPart k_folder[] = {
    {"M3 7.5A1.5 1.5 0 0 1 4.5 6h4l2 2h9A1.5 1.5 0 0 1 21 9.5v8a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 17.5z", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_folderf[] = {
    {"M2.5 6.5A1.5 1.5 0 0 1 4 5h4.6l2 2H20a1.5 1.5 0 0 1 1.5 1.5V18a1.5 1.5 0 0 1-1.5 1.5H4A1.5 1.5 0 0 1 2.5 18z", 2, 0xE9B949u, 1.6f, 1.0f},
    {"M2.5 9.5h19V18a1.5 1.5 0 0 1-1.5 1.5H4A1.5 1.5 0 0 1 2.5 18z", 2, 0xF4CD63u, 1.6f, 1.0f},
};
constexpr IconPart k_file[] = {
    {"M7 3h7l5 5v12a1 1 0 0 1-1 1H7a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1z", 0, 0x000000u, 1.6f, 1.0f},
    {"M14 3v5h5", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_x[] = {
    {"M6 6l12 12M18 6L6 18", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_plus[] = {
    {"M12 5v14M5 12h14", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_dock[] = {
    {"M5 4H19A2 2 0 0 1 21 6V18A2 2 0 0 1 19 20H5A2 2 0 0 1 3 18V6A2 2 0 0 1 5 4Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M9 4v16", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_float[] = {
    {"M5 4H19A2 2 0 0 1 21 6V18A2 2 0 0 1 19 20H5A2 2 0 0 1 3 18V6A2 2 0 0 1 5 4Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M7 10H12A1 1 0 0 1 13 11V16A1 1 0 0 1 12 17H7A1 1 0 0 1 6 16V11A1 1 0 0 1 7 10Z", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_chevd[] = {
    {"M6 9l6 6 6-6", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_chevr[] = {
    {"M9 6l6 6-6 6", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_send[] = {
    {"M12 19V5M6 11l6-6 6 6", 0, 0x000000u, 2.2f, 1.0f},
};
constexpr IconPart k_stop[] = {
    {"M9 7H15A2 2 0 0 1 17 9V15A2 2 0 0 1 15 17H9A2 2 0 0 1 7 15V9A2 2 0 0 1 9 7Z", 1, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_retry[] = {
    {"M20 11a8 8 0 1 0-2.3 5.7", 0, 0x000000u, 1.6f, 1.0f},
    {"M20 4v7h-7", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_check[] = {
    {"M5 12.5l4.2 4L19 7", 0, 0x000000u, 2.6f, 1.0f},
};
constexpr IconPart k_undo[] = {
    {"M9 14L4 9l5-5", 0, 0x000000u, 1.6f, 1.0f},
    {"M4 9h10.5a5.5 5.5 0 0 1 0 11H11", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_open[] = {
    {"M14 4h6v6M20 4l-9 9", 0, 0x000000u, 1.6f, 1.0f},
    {"M18 14v5a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1h5", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_clock[] = {
    {"M3.5 12A8.5 8.5 0 1 0 20.5 12A8.5 8.5 0 1 0 3.5 12Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M12 7.5V12l3 2", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_at[] = {
    {"M8.4 12A3.6 3.6 0 1 0 15.6 12A3.6 3.6 0 1 0 8.4 12Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M15.6 12v1.4a2.6 2.6 0 0 0 5.2 0V12a8.8 8.8 0 1 0-3.4 6.9", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_shield[] = {
    {"M12 3l7 3v5.2c0 4.4-3 8-7 9.8-4-1.8-7-5.4-7-9.8V6z", 0, 0x000000u, 1.6f, 1.0f},
    {"M9 12l2.2 2.2L15.5 10", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_thumb[] = {
    {"M7 10v10H4V10zM7 10l4-7a2 2 0 0 1 2 2.2V9h5.5a2 2 0 0 1 2 2.3l-1.2 7A2 2 0 0 1 17.3 20H7", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_copy[] = {
    {"M10 8H18A2 2 0 0 1 20 10V18A2 2 0 0 1 18 20H10A2 2 0 0 1 8 18V10A2 2 0 0 1 10 8Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M16 8V5a1 1 0 0 0-1-1H5a1 1 0 0 0-1 1v10a1 1 0 0 0 1 1h3", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_eye[] = {
    {"M2.5 12s3.5-6.5 9.5-6.5S21.5 12 21.5 12s-3.5 6.5-9.5 6.5S2.5 12 2.5 12z", 0, 0x000000u, 1.6f, 1.0f},
    {"M9.4 12A2.6 2.6 0 1 0 14.6 12A2.6 2.6 0 1 0 9.4 12Z", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_win[] = {
    {"M5 5H19A2 2 0 0 1 21 7V17A2 2 0 0 1 19 19H5A2 2 0 0 1 3 17V7A2 2 0 0 1 5 5Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M3 9h18M7 7h.01M9.5 7h.01", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_rows[] = {
    {"M8 6h12M8 12h12M8 18h12", 0, 0x000000u, 1.6f, 1.0f},
    {"M3.5 6h.01M3.5 12h.01M3.5 18h.01", 0, 0x000000u, 2.6f, 1.0f},
};
constexpr IconPart k_move[] = {
    {"M3 7.5A1.5 1.5 0 0 1 4.5 6h4l2 2h9A1.5 1.5 0 0 1 21 9.5v8a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 17.5z", 0, 0x000000u, 1.6f, 1.0f},
    {"M9 13.5h6M13 11l2.5 2.5L13 16", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_image[] = {
    {"M5.5 4.5H18.5A2 2 0 0 1 20.5 6.5V17.5A2 2 0 0 1 18.5 19.5H5.5A2 2 0 0 1 3.5 17.5V6.5A2 2 0 0 1 5.5 4.5Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M7.2 10A1.8 1.8 0 1 0 10.8 10A1.8 1.8 0 1 0 7.2 10Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M4 18l5.5-5.5 4 4 2.5-2.5 4 4", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_video[] = {
    {"M5 6H14A2 2 0 0 1 16 8V16A2 2 0 0 1 14 18H5A2 2 0 0 1 3 16V8A2 2 0 0 1 5 6Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M16 10l5-3v10l-5-3", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_doc[] = {
    {"M7 3h7l5 5v12a1 1 0 0 1-1 1H7a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1z", 0, 0x000000u, 1.6f, 1.0f},
    {"M14 3v5h5M9 13h6M9 17h6", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_down[] = {
    {"M12 4v11M7 10.5l5 5 5-5M5 19.5h14", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_size[] = {
    {"M4 20V10M10 20V4M16 20v-7M22 20H2", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_dup[] = {
    {"M10 8H18A2 2 0 0 1 20 10V18A2 2 0 0 1 18 20H10A2 2 0 0 1 8 18V10A2 2 0 0 1 10 8Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M16 8V5a1 1 0 0 0-1-1H5a1 1 0 0 0-1 1v10a1 1 0 0 0 1 1h3", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_cam[] = {
    {"M4 8h3l1.5-2h7L17 8h3a1 1 0 0 1 1 1v9a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V9a1 1 0 0 1 1-1z", 0, 0x000000u, 1.6f, 1.0f},
    {"M8.5 13A3.5 3.5 0 1 0 15.5 13A3.5 3.5 0 1 0 8.5 13Z", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_arr[] = {
    {"M5 12h12M13 7l5 5-5 5", 0, 0x000000u, 2.0f, 1.0f},
};
constexpr IconPart k_tag[] = {
    {"M3.5 12.5V4.5a1 1 0 0 1 1-1h8l8 8-9 9z", 0, 0x000000u, 1.6f, 1.0f},
    {"M6.5 8A1.5 1.5 0 1 0 9.5 8A1.5 1.5 0 1 0 6.5 8Z", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_folderplus[] = {
    {"M3 7.5A1.5 1.5 0 0 1 4.5 6h4l2 2h9A1.5 1.5 0 0 1 21 9.5v8a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 17.5z", 0, 0x000000u, 1.6f, 1.0f},
    {"M12 11v5M9.5 13.5h5", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_search[] = {
    {"M4.5 11A6.5 6.5 0 1 0 17.5 11A6.5 6.5 0 1 0 4.5 11Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M16 16l4.5 4.5", 0, 0x000000u, 1.6f, 1.0f},
};
// 管理连接 (plans/ai/Pulse_AI助手_管理连接.html, its 20 px boxes scaled to 24).
constexpr IconPart k_chevl[] = {
    {"M15 5.4L8.4 12l6.6 6.6", 0, 0x000000u, 1.9f, 1.0f},
};
constexpr IconPart k_okc[] = {
    {"M3 12A9 9 0 1 0 21 12A9 9 0 1 0 3 12Z", 0, 0x000000u, 1.8f, 1.0f},
    {"M8.16 12.24l2.64 2.64 5.16-5.52", 0, 0x000000u, 2.16f, 1.0f},
};
constexpr IconPart k_alert[] = {
    {"M3 12A9 9 0 1 0 21 12A9 9 0 1 0 3 12Z", 0, 0x000000u, 1.8f, 1.0f},
    {"M12 7.44v5.52M12 16.32v.12", 0, 0x000000u, 1.92f, 1.0f},
};
constexpr IconPart k_eyeoff[] = {
    {"M2.5 12s3.5-6.5 9.5-6.5S21.5 12 21.5 12s-3.5 6.5-9.5 6.5S2.5 12 2.5 12z", 0, 0x000000u, 1.6f, 1.0f},
    {"M9.4 12A2.6 2.6 0 1 0 14.6 12A2.6 2.6 0 1 0 9.4 12Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M4.5 4.5l15 15", 0, 0x000000u, 1.6f, 1.0f},
};

constexpr IconDef kIcons[] = {
    {k_spark, std::size(k_spark)},
    {k_folder, std::size(k_folder)},
    {k_folderf, std::size(k_folderf)},
    {k_file, std::size(k_file)},
    {k_x, std::size(k_x)},
    {k_plus, std::size(k_plus)},
    {k_dock, std::size(k_dock)},
    {k_float, std::size(k_float)},
    {k_chevd, std::size(k_chevd)},
    {k_chevr, std::size(k_chevr)},
    {k_send, std::size(k_send)},
    {k_stop, std::size(k_stop)},
    {k_retry, std::size(k_retry)},
    {k_check, std::size(k_check)},
    {k_undo, std::size(k_undo)},
    {k_open, std::size(k_open)},
    {k_clock, std::size(k_clock)},
    {k_at, std::size(k_at)},
    {k_shield, std::size(k_shield)},
    {k_thumb, std::size(k_thumb)},
    {k_copy, std::size(k_copy)},
    {k_eye, std::size(k_eye)},
    {k_win, std::size(k_win)},
    {k_rows, std::size(k_rows)},
    {k_move, std::size(k_move)},
    {k_image, std::size(k_image)},
    {k_video, std::size(k_video)},
    {k_doc, std::size(k_doc)},
    {k_down, std::size(k_down)},
    {k_size, std::size(k_size)},
    {k_dup, std::size(k_dup)},
    {k_cam, std::size(k_cam)},
    {k_arr, std::size(k_arr)},
    {k_tag, std::size(k_tag)},
    {k_folderplus, std::size(k_folderplus)},
    {k_search, std::size(k_search)},
    {k_chevl, std::size(k_chevl)},
    {k_okc, std::size(k_okc)},
    {k_alert, std::size(k_alert)},
    {k_eyeoff, std::size(k_eyeoff)},
};

static_assert(std::size(kIcons) == static_cast<size_t>(AiIcon::Count), "one table entry per AiIcon");

// Minimal SVG path parser: M L H V C S Q Z and A, absolute and relative,
// implicit command repetition and packed numbers ("-4.9.7").
class PathParser {
public:
    PathParser(const char* d, ID2D1GeometrySink* sink) : s_(d), sink_(sink) {}

    void Run() {
        char cmd = 0;
        for (;;) {
            SkipSeparators();
            if (!*s_) break;
            if (IsCommand(*s_)) cmd = *s_++;
            else if (cmd == 'M') cmd = 'L';   // implicit lineto after moveto
            else if (cmd == 'm') cmd = 'l';
            if (!cmd) break;
            const bool rel = cmd >= 'a' && cmd <= 'z';
            switch (cmd) {
            case 'M': case 'm': {
                float x, y;
                if (!Num(x) || !Num(y)) return;
                if (rel) { x += cx_; y += cy_; }
                Begin(x, y);
                break;
            }
            case 'L': case 'l': {
                float x, y;
                if (!Num(x) || !Num(y)) return;
                if (rel) { x += cx_; y += cy_; }
                Line(x, y);
                break;
            }
            case 'H': case 'h': {
                float x;
                if (!Num(x)) return;
                Line(rel ? cx_ + x : x, cy_);
                break;
            }
            case 'V': case 'v': {
                float y;
                if (!Num(y)) return;
                Line(cx_, rel ? cy_ + y : y);
                break;
            }
            case 'C': case 'c': {
                float x1, y1, x2, y2, x, y;
                if (!Num(x1) || !Num(y1) || !Num(x2) || !Num(y2) || !Num(x) || !Num(y)) return;
                if (rel) { x1 += cx_; y1 += cy_; x2 += cx_; y2 += cy_; x += cx_; y += cy_; }
                Cubic(x1, y1, x2, y2, x, y);
                break;
            }
            case 'S': case 's': {
                float x2, y2, x, y;
                if (!Num(x2) || !Num(y2) || !Num(x) || !Num(y)) return;
                if (rel) { x2 += cx_; y2 += cy_; x += cx_; y += cy_; }
                const bool smooth = last_ == 'C' || last_ == 'S';
                const float x1 = smooth ? 2.0f * cx_ - c2x_ : cx_;
                const float y1 = smooth ? 2.0f * cy_ - c2y_ : cy_;
                Cubic(x1, y1, x2, y2, x, y);
                break;
            }
            case 'Q': case 'q': {
                float x1, y1, x, y;
                if (!Num(x1) || !Num(y1) || !Num(x) || !Num(y)) return;
                if (rel) { x1 += cx_; y1 += cy_; x += cx_; y += cy_; }
                EnsureFigure();
                sink_->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(x1, y1), D2D1::Point2F(x, y)));
                cx_ = x; cy_ = y;
                break;
            }
            case 'A': case 'a': {
                float rx, ry, rot, large, sweep, x, y;
                if (!Num(rx) || !Num(ry) || !Num(rot) || !Flag(large) || !Flag(sweep) || !Num(x) || !Num(y)) return;
                if (rel) { x += cx_; y += cy_; }
                EnsureFigure();
                D2D1_ARC_SEGMENT arc{};
                arc.point = D2D1::Point2F(x, y);
                arc.size = D2D1::SizeF(std::fabs(rx), std::fabs(ry));
                arc.rotationAngle = rot;
                arc.sweepDirection = sweep != 0.0f ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE;
                arc.arcSize = large != 0.0f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;
                sink_->AddArc(arc);
                cx_ = x; cy_ = y;
                break;
            }
            case 'Z': case 'z':
                if (open_) {
                    sink_->EndFigure(D2D1_FIGURE_END_CLOSED);
                    open_ = false;
                }
                cx_ = sx_; cy_ = sy_;
                break;
            default:
                return;
            }
            last_ = static_cast<char>(cmd & ~0x20);
        }
        if (open_) sink_->EndFigure(D2D1_FIGURE_END_OPEN);
    }

private:
    static bool IsCommand(char c) {
        switch (c) {
        case 'M': case 'm': case 'L': case 'l': case 'H': case 'h': case 'V': case 'v':
        case 'C': case 'c': case 'S': case 's': case 'Q': case 'q': case 'A': case 'a':
        case 'Z': case 'z':
            return true;
        default:
            return false;
        }
    }
    void SkipSeparators() {
        while (*s_ == ' ' || *s_ == ',' || *s_ == '\t' || *s_ == '\n' || *s_ == '\r') ++s_;
    }
    bool Num(float& out) {
        SkipSeparators();
        const char* start = s_;
        if (*s_ == '-' || *s_ == '+') ++s_;
        bool dot = false, digits = false;
        while ((*s_ >= '0' && *s_ <= '9') || (*s_ == '.' && !dot)) {
            if (*s_ == '.') dot = true; else digits = true;
            ++s_;
        }
        if (*s_ == 'e' || *s_ == 'E') {
            ++s_;
            if (*s_ == '-' || *s_ == '+') ++s_;
            while (*s_ >= '0' && *s_ <= '9') ++s_;
        }
        if (!digits) { s_ = start; return false; }
        out = std::strtof(start, nullptr);
        return true;
    }
    bool Flag(float& out) {   // arc flags may be packed without separators: "0 01"
        SkipSeparators();
        if (*s_ != '0' && *s_ != '1') return false;
        out = *s_ == '1' ? 1.0f : 0.0f;
        ++s_;
        return true;
    }
    void Begin(float x, float y) {
        if (open_) sink_->EndFigure(D2D1_FIGURE_END_OPEN);
        sink_->BeginFigure(D2D1::Point2F(x, y), D2D1_FIGURE_BEGIN_FILLED);
        open_ = true;
        cx_ = sx_ = x; cy_ = sy_ = y;
    }
    void EnsureFigure() { if (!open_) Begin(cx_, cy_); }
    void Line(float x, float y) {
        EnsureFigure();
        sink_->AddLine(D2D1::Point2F(x, y));
        cx_ = x; cy_ = y;
    }
    void Cubic(float x1, float y1, float x2, float y2, float x, float y) {
        EnsureFigure();
        sink_->AddBezier(D2D1::BezierSegment(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), D2D1::Point2F(x, y)));
        c2x_ = x2; c2y_ = y2;
        cx_ = x; cy_ = y;
    }

    const char* s_;
    ID2D1GeometrySink* sink_;
    bool open_ = false;
    char last_ = 0;
    float cx_ = 0, cy_ = 0, sx_ = 0, sy_ = 0, c2x_ = 0, c2y_ = 0;
};

constexpr size_t kMaxParts = 8;

struct Cache {
    ID2D1Factory* factory = nullptr;
    std::array<std::array<ID2D1PathGeometry*, kMaxParts>, static_cast<size_t>(AiIcon::Count)> geometry{};
    ID2D1StrokeStyle* stroke = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    ID2D1DeviceContext* brush_dc = nullptr;

    void Release() {
        for (auto& icon : geometry)
            for (auto*& g : icon) if (g) { g->Release(); g = nullptr; }
        if (stroke) { stroke->Release(); stroke = nullptr; }
        if (brush) { brush->Release(); brush = nullptr; }
        brush_dc = nullptr;
        if (factory) { factory->Release(); factory = nullptr; }
    }
    ~Cache() { Release(); }
};

Cache& TheCache() {
    static Cache cache;
    return cache;
}

ID2D1PathGeometry* Geometry(Cache& c, AiIcon icon, size_t part) {
    ID2D1PathGeometry*& slot = c.geometry[static_cast<size_t>(icon)][part];
    if (slot) return slot;
    ID2D1PathGeometry* path = nullptr;
    if (FAILED(c.factory->CreatePathGeometry(&path))) return nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(path->Open(&sink))) { path->Release(); return nullptr; }
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    PathParser(kIcons[static_cast<size_t>(icon)].parts[part].d, sink).Run();
    const HRESULT hr = sink->Close();
    sink->Release();
    if (FAILED(hr)) { path->Release(); return nullptr; }
    slot = path;
    return slot;
}

D2D1_COLOR_F Rgb(uint32_t rgb, float alpha) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha);
}

} // namespace

void ReleaseAiIcons() { TheCache().Release(); }

void DrawAiIcon(ID2D1DeviceContext* dc, AiIcon icon, const D2D1_RECT_F& box, D2D1_COLOR_F color,
                const AiIconPose& pose) {
    if (!dc || icon >= AiIcon::Count || pose.opacity <= 0.0f) return;
    Cache& c = TheCache();
    ID2D1Factory* factory = nullptr;
    dc->GetFactory(&factory);
    if (!factory) return;
    if (factory != c.factory) {
        c.Release();
        c.factory = factory;   // keeps the reference GetFactory added
    } else {
        factory->Release();
    }
    if (!c.stroke) {
        const D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        c.factory->CreateStrokeStyle(props, nullptr, 0, &c.stroke);
    }
    if (c.brush_dc != dc) {
        if (c.brush) { c.brush->Release(); c.brush = nullptr; }
        if (FAILED(dc->CreateSolidColorBrush(color, &c.brush))) return;
        c.brush_dc = dc;
    }
    const float size = (std::min)(box.right - box.left, box.bottom - box.top);
    if (size <= 0.0f) return;
    const float k = size / 24.0f;
    D2D1_MATRIX_3X2_F saved;
    dc->GetTransform(&saved);
    const D2D1_POINT_2F centre = D2D1::Point2F(box.left + size * 0.5f, box.top + size * 0.5f);
    D2D1_MATRIX_3X2_F m = D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(box.left, box.top);
    if (pose.scale != 1.0f) m = m * D2D1::Matrix3x2F::Scale(pose.scale, pose.scale, centre);
    if (pose.rotate_deg != 0.0f) m = m * D2D1::Matrix3x2F::Rotation(pose.rotate_deg, centre);
    dc->SetTransform(m * saved);
    const IconDef& def = kIcons[static_cast<size_t>(icon)];
    for (size_t i = 0; i < def.count && i < kMaxParts; ++i) {
        const IconPart& part = def.parts[i];
        ID2D1PathGeometry* g = Geometry(c, icon, i);
        if (!g) continue;
        const float alpha = color.a * pose.opacity * part.opacity;
        c.brush->SetColor(part.mode == 2 ? Rgb(part.rgb, pose.opacity * part.opacity)
                                         : D2D1::ColorF(color.r, color.g, color.b, alpha));
        if (part.mode == 0) dc->DrawGeometry(g, c.brush, part.stroke, c.stroke);
        else dc->FillGeometry(g, c.brush);
    }
    dc->SetTransform(saved);
}

} // namespace pulse::ui
