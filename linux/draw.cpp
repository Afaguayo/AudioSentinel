#include "draw.h"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>

namespace
{

struct Rgb
{
    double r, g, b;
};

constexpr Rgb Hex(unsigned v)
{
    return { ((v >> 16) & 0xFF) / 255.0, ((v >> 8) & 0xFF) / 255.0, (v & 0xFF) / 255.0 };
}

// Same palette as the Windows dashboard.
constexpr Rgb BG = Hex(0x12141A);
constexpr Rgb CARD = Hex(0x1D212A);
constexpr Rgb TRACK = Hex(0x2D323E);
constexpr Rgb TEXT = Hex(0xECEFF4);
constexpr Rgb MUTED = Hex(0x8C93A0);
constexpr Rgb DIM = Hex(0x606673);
constexpr Rgb GREEN = Hex(0x3DDC84);
constexpr Rgb AMBER = Hex(0xFFB020);
constexpr Rgb RED = Hex(0xFF5A5F);
constexpr Rgb GREY = Hex(0x8A8F98);
constexpr Rgb SPOTIFY = Hex(0x1ED760);

Rgb ZoneColor(as::Zone z)
{
    switch (z)
    {
    case as::Zone::Safe: return GREEN;
    case as::Zone::Loud: return AMBER;
    case as::Zone::Harmful: return RED;
    default: return GREY;
    }
}

Rgb AllowanceColor(double fraction)
{
    return fraction < 0.5 ? GREEN : fraction < 0.8 ? AMBER : RED;
}

void SetColor(cairo_t* cr, Rgb c, double alpha = 1.0)
{
    cairo_set_source_rgba(cr, c.r, c.g, c.b, alpha);
}

void RoundRect(cairo_t* cr, double x, double y, double w, double h, double r)
{
    r = std::min({ r, w / 2, h / 2 });
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

void FillRoundRect(cairo_t* cr, Rgb c, double alpha, double x, double y, double w, double h, double r)
{
    RoundRect(cr, x, y, w, h, r);
    SetColor(cr, c, alpha);
    cairo_fill(cr);
}

class Text
{
public:
    Text(cairo_t* cr, double px, bool bold) : cr_(cr)
    {
        layout_ = pango_cairo_create_layout(cr);
        PangoFontDescription* font = pango_font_description_from_string("Sans");
        pango_font_description_set_weight(font, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
        pango_font_description_set_absolute_size(font, px * PANGO_SCALE);
        pango_layout_set_font_description(layout_, font);
        pango_font_description_free(font);
    }
    ~Text() { g_object_unref(layout_); }

    double Width(const std::string& s)
    {
        pango_layout_set_text(layout_, s.c_str(), -1);
        int w = 0, h = 0;
        pango_layout_get_pixel_size(layout_, &w, &h);
        return w;
    }

    void Draw(const std::string& s, Rgb c, double x, double y, double alpha = 1.0)
    {
        pango_layout_set_text(layout_, s.c_str(), -1);
        SetColor(cr_, c, alpha);
        cairo_move_to(cr_, x, y);
        pango_cairo_show_layout(cr_, layout_);
    }

    void DrawRight(const std::string& s, Rgb c, double right, double y, double alpha = 1.0)
    {
        Draw(s, c, right - Width(s), y, alpha);
    }

    // Draws s, shortened with an ellipsis to fit maxWidth.
    void DrawClipped(const std::string& s, Rgb c, double x, double y, double maxWidth)
    {
        pango_layout_set_text(layout_, s.c_str(), -1);
        pango_layout_set_width(layout_, (int)(maxWidth * PANGO_SCALE));
        pango_layout_set_ellipsize(layout_, PANGO_ELLIPSIZE_END);
        SetColor(cr_, c);
        cairo_move_to(cr_, x, y);
        pango_cairo_show_layout(cr_, layout_);
        pango_layout_set_width(layout_, -1);
        pango_layout_set_ellipsize(layout_, PANGO_ELLIPSIZE_NONE);
    }

private:
    cairo_t* cr_;
    PangoLayout* layout_;
};

} // namespace

void DrawDashboard(cairo_t* cr, double W, double H, const as::DashboardModel& m)
{
    as::Zone zone = as::ModelZone(m);
    Rgb zoneColor = ZoneColor(zone);
    const double margin = 20;

    SetColor(cr, BG);
    cairo_paint(cr);

    Text big(cr, 52, true), unit(cr, 20, false), label(cr, 12, false), labelBold(cr, 12, true),
        body(cr, 13, false), value(cr, 15, true), tiny(cr, 11, false);

    // Header: current level and zone pill.
    labelBold.Draw("CURRENT LEVEL", MUTED, margin, 16);
    bool noNumber = zone == as::Zone::Silent || zone == as::Zone::Paused;
    std::string number = noNumber ? "--" : std::to_string((int)std::lround(m.db));
    big.Draw(number, TEXT, margin - 2, 28);
    unit.Draw("dB", MUTED, margin + big.Width(number) + 4, 58);

    std::string pill = as::ZoneLabel(zone);
    double pillW = labelBold.Width(pill) + 22;
    FillRoundRect(cr, zoneColor, 0.18, W - margin - pillW, 44, pillW, 26, 13);
    labelBold.Draw(pill, zoneColor, W - margin - pillW + 11, 49);

    body.Draw(as::StatusLine(m), TEXT, margin, 96);

    // Spotify row.
    std::string spotify = as::SpotifyLine(m);
    if (!spotify.empty())
    {
        cairo_arc(cr, margin + 6, 128, 6, 0, 2 * M_PI);
        SetColor(cr, m.spotify.playing ? SPOTIFY : GREY);
        cairo_fill(cr);
        body.DrawClipped(spotify, m.spotify.playing ? TEXT : MUTED, margin + 18, 119, W - 2 * margin - 18);
    }

    // Allowance card.
    double cardY = 150, cardH = 82;
    FillRoundRect(cr, CARD, 1.0, margin, cardY, W - 2 * margin, cardH, 10);
    double inner = margin + 14, innerRight = W - margin - 14;
    Rgb allowance = AllowanceColor(m.stats.exposure);
    label.Draw("Today's allowance used", MUTED, inner, cardY + 12);
    value.DrawRight(std::to_string((int)(m.stats.exposure * 100.0)) + "%", allowance, innerRight, cardY + 9);
    double barW = innerRight - inner;
    FillRoundRect(cr, TRACK, 1.0, inner, cardY + 36, barW, 10, 5);
    double filled = std::min(m.stats.exposure, 1.0) * barW;
    if (filled > 0.5)
        FillRoundRect(cr, allowance, 1.0, inner, cardY + 36, std::max(filled, 10.0), 10, 5);
    label.Draw(as::StatsLine(m), MUTED, inner, cardY + 56);

    // History graph card.
    double graphY = 244, graphH = H - graphY - 40;
    FillRoundRect(cr, CARD, 1.0, margin, graphY, W - 2 * margin, graphH, 10);
    label.Draw("Last 2 minutes", MUTED, inner, graphY + 10);

    double px = inner, py = graphY + 34, pw = innerRight - inner, ph = graphH - 46;
    const double minDb = 30, maxDb = 110;
    auto yFor = [&](double v) {
        double t = (std::clamp(v, minDb, maxDb) - minDb) / (maxDb - minDb);
        return py + ph - t * ph;
    };

    cairo_set_line_width(cr, 1);
    SetColor(cr, TRACK);
    cairo_move_to(cr, px, py + ph);
    cairo_line_to(cr, px + pw, py + ph);
    cairo_stroke(cr);

    double refY = yFor(as::REF_DB);
    double dash[] = { 4, 3 };
    cairo_set_dash(cr, dash, 2, 0);
    SetColor(cr, RED, 0.6);
    cairo_move_to(cr, px, refY);
    cairo_line_to(cr, px + pw, refY);
    cairo_stroke(cr);
    cairo_set_dash(cr, nullptr, 0, 0);
    tiny.DrawRight("85 dB", RED, px + pw, refY - 16, 0.8);

    if (m.history.size() >= 2)
    {
        Rgb line = noNumber ? GREEN : zoneColor;
        double step = pw / (double)(as::HISTORY_LEN - 1);
        double x0 = px + pw - step * (double)(m.history.size() - 1);

        cairo_move_to(cr, x0, yFor(m.history[0]));
        for (size_t i = 1; i < m.history.size(); i++)
            cairo_line_to(cr, x0 + step * i, yFor(m.history[i]));
        cairo_path_t* path = cairo_copy_path(cr);
        cairo_line_to(cr, px + pw, py + ph);
        cairo_line_to(cr, x0, py + ph);
        cairo_close_path(cr);
        SetColor(cr, line, 0.16);
        cairo_fill(cr);

        cairo_append_path(cr, path);
        cairo_path_destroy(path);
        cairo_set_line_width(cr, 2);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        SetColor(cr, line);
        cairo_stroke(cr);
    }

    tiny.Draw("Right-click for options  \xC2\xB7  \xE2\x86\x91/\xE2\x86\x93 change opacity  \xC2\xB7  Esc hides",
              DIM, margin, H - 28);
}

void DrawTrayIcon(cairo_t* cr, double s, as::Zone zone, double fraction)
{
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    double stroke = s * 0.17, inset = stroke / 2 + s * 0.03, radius = s / 2 - inset;
    cairo_set_line_width(cr, stroke);
    cairo_set_source_rgba(cr, 120 / 255.0, 126 / 255.0, 138 / 255.0, 0.67);
    cairo_arc(cr, s / 2, s / 2, radius, 0, 2 * M_PI);
    cairo_stroke(cr);

    double sweep = std::min(fraction, 1.0) * 2 * M_PI;
    if (sweep > 0.02)
    {
        SetColor(cr, AllowanceColor(fraction));
        cairo_arc(cr, s / 2, s / 2, radius, -M_PI / 2, -M_PI / 2 + sweep);
        cairo_stroke(cr);
    }

    double dot = s - 2 * (inset + stroke) - s * 0.02;
    SetColor(cr, ZoneColor(zone));
    cairo_arc(cr, s / 2, s / 2, dot / 2, 0, 2 * M_PI);
    cairo_fill(cr);
}

bool RenderDashboardPng(const std::string& path, const as::DashboardModel& m, double scale)
{
    cairo_surface_t* surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, (int)(DASHBOARD_WIDTH * scale), (int)(DASHBOARD_HEIGHT * scale));
    cairo_t* cr = cairo_create(surface);
    cairo_scale(cr, scale, scale);
    DrawDashboard(cr, DASHBOARD_WIDTH, DASHBOARD_HEIGHT, m);
    cairo_destroy(cr);
    bool ok = cairo_surface_write_to_png(surface, path.c_str()) == CAIRO_STATUS_SUCCESS;
    cairo_surface_destroy(surface);
    return ok;
}

bool RenderTrayIconPng(const std::string& path, int size, as::Zone zone, double fraction)
{
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t* cr = cairo_create(surface);
    DrawTrayIcon(cr, size, zone, fraction);
    cairo_destroy(cr);
    bool ok = cairo_surface_write_to_png(surface, path.c_str()) == CAIRO_STATUS_SUCCESS;
    cairo_surface_destroy(surface);
    return ok;
}
