#include "HSLColorPicker.hpp"

#include <wx/wx.h>
#include <wx/dcgraph.h>
#include <cmath>
#include <algorithm>

#include "Slic3r/GUI/I18N.hpp"
#include "Slic3r/GUI/GUI.hpp"
#include "Slic3r/GUI/GUI_App.hpp"
#include "Label.hpp"

namespace Slic3r::GUI {

// ─── Construction ────────────────────────────────────────────────────────────

HSLColorPicker::HSLColorPicker(
    wxWindow*                            parent,
    const wxColour&                      initial_color,
    std::function<void(const wxColour&, bool)> on_color_changed)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    , m_on_color_changed(std::move(on_color_changed))
{
    SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));

    // Decompose the initial color
    rgb_to_hsl(initial_color, m_hue, m_saturation, m_lightness);

    // --- Main vertical sizer ---
    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);

    // --- Wheel panel (custom-painted) ---
    m_wheel_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_wheel_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_wheel_panel->SetMinSize(wxSize(FromDIP(220), FromDIP(220)));

    m_wheel_panel->Bind(wxEVT_PAINT,     &HSLColorPicker::on_wheel_paint, this);
    m_wheel_panel->Bind(wxEVT_LEFT_DOWN, &HSLColorPicker::on_left_down,   this);
    m_wheel_panel->Bind(wxEVT_LEFT_UP,   &HSLColorPicker::on_left_up,     this);
    m_wheel_panel->Bind(wxEVT_MOTION,    &HSLColorPicker::on_motion,      this);
    m_wheel_panel->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
        m_dragging    = false;
        m_drag_target = DragTarget::None;
    });

    main_sizer->Add(m_wheel_panel, 1, wxEXPAND);
    main_sizer->AddSpacer(FromDIP(8));

    // --- Bottom row: hex input + color preview ---
    wxBoxSizer* bottom_sizer = new wxBoxSizer(wxHORIZONTAL);

    // Hex input
    wxStaticText* hash_label = new wxStaticText(this, wxID_ANY, "#");
    hash_label->SetFont(::Label::Body_14);

    m_hex_input = new wxTextCtrl(this, wxID_ANY, colour_to_hex(initial_color).Mid(1),
                                 wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    m_hex_input->SetFont(::Label::Body_14);
    m_hex_input->SetMaxLength(6);
    m_hex_input->SetMinSize(wxSize(FromDIP(70), -1));

    m_hex_input->Bind(wxEVT_TEXT_ENTER, &HSLColorPicker::on_hex_input_changed, this);
    m_hex_input->Bind(wxEVT_KILL_FOCUS, &HSLColorPicker::on_hex_input_focus_lost, this);

    // Color preview swatch
    m_color_preview = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    m_color_preview->SetMinSize(wxSize(FromDIP(70), FromDIP(26)));
    sync_preview_color();

    bottom_sizer->Add(hash_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(2));
    bottom_sizer->Add(m_hex_input, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    bottom_sizer->Add(m_color_preview, 1, wxALIGN_CENTER_VERTICAL);

    main_sizer->Add(bottom_sizer, 0, wxEXPAND);

    SetSizer(main_sizer);
}

// ─── Public API ──────────────────────────────────────────────────────────────

wxColour HSLColorPicker::GetColor() const
{
    return hsl_to_rgb(m_hue, m_saturation, m_lightness);
}

void HSLColorPicker::SetColor(const wxColour& color)
{
    rgb_to_hsl(color, m_hue, m_saturation, m_lightness);
    update_from_hsl();
}

// ─── HSL ↔ RGB conversion ───────────────────────────────────────────────────

static double hue_to_channel(double p, double q, double t)
{
    if (t < 0.0) t += 1.0;
    if (t > 1.0) t -= 1.0;
    if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
    if (t < 1.0 / 2.0) return q;
    if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
    return p;
}

wxColour HSLColorPicker::hsl_to_rgb(double h, double s, double l)
{
    // h: 0..360, s: 0..1, l: 0..1
    double r, g, b;

    if (s <= 0.0) {
        r = g = b = l;
    } else {
        double hh = h / 360.0;
        double q  = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
        double p  = 2.0 * l - q;

        r = hue_to_channel(p, q, hh + 1.0 / 3.0);
        g = hue_to_channel(p, q, hh);
        b = hue_to_channel(p, q, hh - 1.0 / 3.0);
    }

    return wxColour(
        static_cast<unsigned char>(std::clamp(std::round(r * 255.0), 0.0, 255.0)),
        static_cast<unsigned char>(std::clamp(std::round(g * 255.0), 0.0, 255.0)),
        static_cast<unsigned char>(std::clamp(std::round(b * 255.0), 0.0, 255.0)));
}

void HSLColorPicker::rgb_to_hsl(const wxColour& c, double& h, double& s, double& l)
{
    double r = c.Red()   / 255.0;
    double g = c.Green() / 255.0;
    double b = c.Blue()  / 255.0;

    double max_c = std::max({r, g, b});
    double min_c = std::min({r, g, b});
    double delta = max_c - min_c;

    l = (max_c + min_c) / 2.0;

    if (delta < 1e-9) {
        h = 0.0;
        s = 0.0;
    } else {
        s = l > 0.5 ? delta / (2.0 - max_c - min_c) : delta / (max_c + min_c);

        if (max_c == r)
            h = (g - b) / delta + (g < b ? 6.0 : 0.0);
        else if (max_c == g)
            h = (b - r) / delta + 2.0;
        else
            h = (r - g) / delta + 4.0;

        h *= 60.0;
    }
}

wxColour HSLColorPicker::hex_to_colour(const wxString& hex)
{
    wxString clean = hex;
    if (clean.StartsWith("#"))
        clean = clean.Mid(1);

    if (clean.length() != 6)
        return *wxBLACK;

    unsigned long val = 0;
    if (!clean.ToULong(&val, 16))
        return *wxBLACK;

    return wxColour(
        static_cast<unsigned char>((val >> 16) & 0xFF),
        static_cast<unsigned char>((val >>  8) & 0xFF),
        static_cast<unsigned char>( val        & 0xFF));
}

wxString HSLColorPicker::colour_to_hex(const wxColour& c)
{
    return wxString::Format("#%02X%02X%02X", c.Red(), c.Green(), c.Blue());
}

wxColour HSLColorPicker::get_contrast_border_color(const wxColour& bg_color) const
{
    double luma = (0.299 * bg_color.Red() + 0.587 * bg_color.Green() + 0.114 * bg_color.Blue()) / 255.0;
    if (luma <= 0.75) {
        return *wxWHITE;
    } else if (luma >= 0.9) {
        return wxColour(0x4F, 0x4F, 0x4F);
    } else {
        double t = (luma - 0.75) / 0.15;
        unsigned char r = static_cast<unsigned char>(255 - t * (255 - 0x4F));
        unsigned char g = static_cast<unsigned char>(255 - t * (255 - 0x4F));
        unsigned char b = static_cast<unsigned char>(255 - t * (255 - 0x4F));
        return wxColour(r, g, b);
    }
}

// ─── Geometry ────────────────────────────────────────────────────────────────

HSLColorPicker::WheelGeometry HSLColorPicker::get_wheel_geometry() const
{
    wxSize size = m_wheel_panel->GetClientSize();
    WheelGeometry g;
    g.cx           = size.x / 2.0;
    g.cy           = size.y / 2.0;
    double margin  = m_wheel_panel->FromDIP(8);
    g.outer_radius = std::min(g.cx, g.cy) - margin;
    g.inner_radius = g.outer_radius - get_ring_width();
    return g;
}

// ─── Painting ────────────────────────────────────────────────────────────────

void HSLColorPicker::on_wheel_paint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(m_wheel_panel);
    wxColour bg = GetBackgroundColour();
    dc.SetBackground(wxBrush(bg));
    dc.Clear();

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;

    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

    wxSize size = m_wheel_panel->GetClientSize();
    if (size.x <= 0 || size.y <= 0) return;

    WheelGeometry geom = get_wheel_geometry();
    if (geom.outer_radius <= 0 || geom.inner_radius <= 0) return;

    paint_hue_ring(*gc, geom, size);
    paint_square(*gc, geom, size);
    paint_hue_handle(*gc, geom);
    paint_square_handle(*gc, geom);
}

void HSLColorPicker::paint_hue_ring(wxGraphicsContext& gc, const WheelGeometry& geom, const wxSize& size)
{
    // Regenerate cached hue ring bitmap if size changed
    if (!m_cached_hue_ring.IsOk() || m_cached_hue_ring_size != size) {
        wxImage image(size.x, size.y, false);
        wxColour bg = GetBackgroundColour();

        // Fill with background color
        unsigned char* data = image.GetData();
        for (int i = 0; i < size.x * size.y; ++i) {
            data[i * 3 + 0] = bg.Red();
            data[i * 3 + 1] = bg.Green();
            data[i * 3 + 2] = bg.Blue();
        }

        // Also need alpha for transparent interior
        image.InitAlpha();
        unsigned char* alpha = image.GetAlpha();
        for (int i = 0; i < size.x * size.y; ++i) {
            alpha[i] = 255;
        }

        double cx = geom.cx;
        double cy = geom.cy;
        double r_outer = geom.outer_radius;
        double r_inner = geom.inner_radius;

        int min_x = std::max(0, (int)std::floor(cx - r_outer));
        int max_x = std::min(size.x - 1, (int)std::ceil(cx + r_outer));
        int min_y = std::max(0, (int)std::floor(cy - r_outer));
        int max_y = std::min(size.y - 1, (int)std::ceil(cy + r_outer));

        for (int y = min_y; y <= max_y; ++y) {
            for (int x = min_x; x <= max_x; ++x) {
                double dx   = x + 0.5 - cx;
                double dy   = y + 0.5 - cy;
                double dist = std::sqrt(dx * dx + dy * dy);

                if (dist >= r_inner && dist <= r_outer) {
                    double angle = std::atan2(dy, dx) + M_PI / 2.0; // 0° at top
                    if (angle < 0.0) angle += 2.0 * M_PI;
                    double hue_deg = angle * 180.0 / M_PI;

                    wxColour c = hsl_to_rgb(hue_deg, 1.0, 0.5);
                    data[(y * size.x + x) * 3 + 0] = c.Red();
                    data[(y * size.x + x) * 3 + 1] = c.Green();
                    data[(y * size.x + x) * 3 + 2] = c.Blue();
                }
            }
        }

        m_cached_hue_ring  = wxBitmap(image);
        m_cached_hue_ring_size = size;
    }

    gc.DrawBitmap(m_cached_hue_ring, 0, 0, size.x, size.y);

    // Draw ring borders (two concentric circles)
    double bw = get_border_width();
    wxColour border_col = get_border_color();
    gc.SetPen(wxPen(border_col, bw));
    gc.SetBrush(*wxTRANSPARENT_BRUSH);

    wxGraphicsPath outer_path = gc.CreatePath();
    outer_path.AddCircle(geom.cx, geom.cy, geom.outer_radius);
    gc.StrokePath(outer_path);

    wxGraphicsPath inner_path = gc.CreatePath();
    inner_path.AddCircle(geom.cx, geom.cy, geom.inner_radius);
    gc.StrokePath(inner_path);
}

void HSLColorPicker::paint_square(wxGraphicsContext& gc, const WheelGeometry& geom, const wxSize& size)
{
    double half_side = geom.inner_radius / std::sqrt(2.0) - m_wheel_panel->FromDIP(2);
    double left = geom.cx - half_side;
    double top = geom.cy - half_side;
    double side_len = half_side * 2.0;

    // Regenerate cached square bitmap when hue, size, or cached square changes
    if (!m_cached_square.IsOk() || m_cached_hue_for_square != m_hue || m_cached_square_size != size) {
        wxColour bg = GetBackgroundColour();
        wxImage image(size.x, size.y, false);
        image.InitAlpha();
        unsigned char* data  = image.GetData();
        unsigned char* alpha = image.GetAlpha();

        // Fill background
        for (int i = 0; i < size.x * size.y; ++i) {
            data[i * 3 + 0] = bg.Red();
            data[i * 3 + 1] = bg.Green();
            data[i * 3 + 2] = bg.Blue();
            alpha[i] = 0;
        }

        int min_x = std::max(0, (int)std::floor(left));
        int max_x = std::min(size.x - 1, (int)std::ceil(left + side_len));
        int min_y = std::max(0, (int)std::floor(top));
        int max_y = std::min(size.y - 1, (int)std::ceil(top + side_len));

        for (int y = min_y; y <= max_y; ++y) {
            for (int x = min_x; x <= max_x; ++x) {
                double px = x + 0.5;
                double py = y + 0.5;

                double S = (px - left) / side_len;
                double L = 1.0 - (py - top) / side_len;

                S = std::clamp(S, 0.0, 1.0);
                L = std::clamp(L, 0.0, 1.0);

                wxColour blended = hsl_to_rgb(m_hue, S, L);

                int idx = y * size.x + x;
                data[idx * 3 + 0] = blended.Red();
                data[idx * 3 + 1] = blended.Green();
                data[idx * 3 + 2] = blended.Blue();
                alpha[idx] = 255;
            }
        }

        m_cached_square         = wxBitmap(image);
        m_cached_hue_for_square = m_hue;
        m_cached_square_size    = size;
    }

    gc.DrawBitmap(m_cached_square, 0, 0, size.x, size.y);

    // Draw square border
    double bw = get_border_width();
    gc.SetPen(wxPen(get_border_color(), bw));
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    gc.DrawRectangle(left, top, side_len, side_len);
}

void HSLColorPicker::paint_hue_handle(wxGraphicsContext& gc, const WheelGeometry& geom)
{
    double hue_rad = m_hue * M_PI / 180.0;
    double mid_r   = (geom.outer_radius + geom.inner_radius) / 2.0;
    double hx      = geom.cx + mid_r * std::cos(hue_rad - M_PI / 2.0);
    double hy      = geom.cy + mid_r * std::sin(hue_rad - M_PI / 2.0);

    wxColour hue_col = hsl_to_rgb(m_hue, 1.0, 0.5);
    double   bw      = get_border_width();

    // Rectangle dimensions: width ≈ handle diameter, height = ring width
    double rect_w = get_hue_handle_radius() * 2.0;
    double rect_h = geom.outer_radius - geom.inner_radius;

    // Rotation angle: tangent to the ring at the hue position
    double rotation = hue_rad; // radians, matching hue angle

    gc.PushState();
    gc.Translate(hx, hy);
    gc.Rotate(rotation);

    // Draw filled rounded rectangle centered at origin
    double corner_r = bw;
    gc.SetBrush(wxBrush(hue_col));
    gc.SetPen(wxPen(get_contrast_border_color(hue_col), bw));
    gc.DrawRoundedRectangle(-rect_w / 2.0, -rect_h / 2.0, rect_w, rect_h, corner_r);

    gc.PopState();
}

void HSLColorPicker::paint_square_handle(wxGraphicsContext& gc, const WheelGeometry& geom)
{
    double half_side = geom.inner_radius / std::sqrt(2.0) - m_wheel_panel->FromDIP(2);
    double left = geom.cx - half_side;
    double top = geom.cy - half_side;
    double side_len = half_side * 2.0;

    double px = left + m_saturation * side_len;
    double py = top + (1.0 - m_lightness) * side_len;

    wxColour cur = GetColor();
    double   hr  = get_square_handle_radius();
    double   bw  = get_border_width();

    gc.SetBrush(wxBrush(cur));
    gc.SetPen(wxPen(get_contrast_border_color(cur), bw));
    gc.DrawEllipse(px - hr, py - hr, hr * 2.0, hr * 2.0);
}

// ─── Mouse interaction ───────────────────────────────────────────────────────

HSLColorPicker::DragTarget HSLColorPicker::hit_test(int x, int y) const
{
    WheelGeometry geom = get_wheel_geometry();

    double dx   = x - geom.cx;
    double dy   = y - geom.cy;
    double dist = std::sqrt(dx * dx + dy * dy);

    // Check hue ring first
    if (dist >= geom.inner_radius && dist <= geom.outer_radius)
        return DragTarget::HueRing;

    // Check square
    double half_side = geom.inner_radius / std::sqrt(2.0) - m_wheel_panel->FromDIP(2);
    double left = geom.cx - half_side;
    double top = geom.cy - half_side;
    double side_len = half_side * 2.0;

    if (x >= left && x <= left + side_len && y >= top && y <= top + side_len)
        return DragTarget::Square;

    return DragTarget::None;
}

void HSLColorPicker::on_left_down(wxMouseEvent& event)
{
    int x = event.GetX();
    int y = event.GetY();

    m_drag_target = hit_test(x, y);
    if (m_drag_target == DragTarget::None)
        return;

    m_dragging = true;
    if (!m_wheel_panel->HasCapture())
        m_wheel_panel->CaptureMouse();

    if (m_drag_target == DragTarget::HueRing)
        update_hue_from_mouse(x, y);
    else
        update_sl_from_mouse(x, y);
}

void HSLColorPicker::on_left_up(wxMouseEvent&)
{
    if (!m_dragging)
        return;

    m_dragging    = false;
    m_drag_target = DragTarget::None;

    if (m_wheel_panel->HasCapture())
        m_wheel_panel->ReleaseMouse();

    m_wheel_panel->Refresh();

    if (m_on_color_changed) {
        m_on_color_changed(GetColor(), false);
    }
}

void HSLColorPicker::on_motion(wxMouseEvent& event)
{
    if (!m_dragging)
        return;

    int x = event.GetX();
    int y = event.GetY();

    if (m_drag_target == DragTarget::HueRing)
        update_hue_from_mouse(x, y);
    else if (m_drag_target == DragTarget::Square)
        update_sl_from_mouse(x, y);
}

void HSLColorPicker::update_hue_from_mouse(int x, int y)
{
    WheelGeometry geom = get_wheel_geometry();

    double dx    = x - geom.cx;
    double dy    = y - geom.cy;
    double angle = std::atan2(dy, dx) + M_PI / 2.0; // 0° at top
    if (angle < 0.0) angle += 2.0 * M_PI;

    m_hue = angle * 180.0 / M_PI;
    if (m_hue >= 360.0) m_hue -= 360.0;

    // Invalidate square cache since hue changed
    m_cached_hue_for_square = -1.0;

    update_from_hsl();
}

void HSLColorPicker::update_sl_from_mouse(int x, int y)
{
    WheelGeometry geom = get_wheel_geometry();
    double half_side = geom.inner_radius / std::sqrt(2.0) - m_wheel_panel->FromDIP(2);
    double left = geom.cx - half_side;
    double top = geom.cy - half_side;
    double side_len = half_side * 2.0;

    m_saturation = std::clamp((x - left) / side_len, 0.0, 1.0);
    m_lightness = std::clamp(1.0 - (y - top) / side_len, 0.0, 1.0);

    update_from_hsl();
}

// ─── Internal sync ───────────────────────────────────────────────────────────

void HSLColorPicker::update_from_hsl()
{
    update_hex_input();
    sync_preview_color();
    m_wheel_panel->Refresh(false);

    if (m_on_color_changed) {
        m_on_color_changed(GetColor(), m_dragging);
    }
}

void HSLColorPicker::update_hex_input()
{
    if (m_updating_hex || !m_hex_input)
        return;

    m_updating_hex = true;
    wxColour c = GetColor();
    m_hex_input->ChangeValue(colour_to_hex(c).Mid(1)); // strip leading '#'
    m_updating_hex = false;
}

void HSLColorPicker::sync_preview_color()
{
    if (!m_color_preview) return;
    m_color_preview->SetBackgroundColour(GetColor());
    m_color_preview->Refresh();
}

void HSLColorPicker::on_hex_input_changed(wxCommandEvent&)
{
    if (m_updating_hex) return;

    wxString text = m_hex_input->GetValue().Trim().Trim(false);
    if (text.length() != 6)
        return;

    wxColour c = hex_to_colour("#" + text);
    if (!c.IsOk())
        return;

    m_updating_hex = true;
    rgb_to_hsl(c, m_hue, m_saturation, m_lightness);
    m_cached_hue_for_square = -1.0; // invalidate square cache
    sync_preview_color();
    m_wheel_panel->Refresh(false);

    if (m_on_color_changed) {
        m_on_color_changed(c, false);
    }
    m_updating_hex = false;
}

void HSLColorPicker::on_hex_input_focus_lost(wxFocusEvent& event)
{
    event.Skip();

    // Reformat the hex input to the current color on focus lost
    if (m_updating_hex) return;

    wxString text = m_hex_input->GetValue().Trim().Trim(false);
    if (text.length() == 6) {
        wxColour c = hex_to_colour("#" + text);
        if (c.IsOk()) {
            m_updating_hex = true;
            rgb_to_hsl(c, m_hue, m_saturation, m_lightness);
            m_cached_hue_for_square = -1.0;
            sync_preview_color();
            m_wheel_panel->Refresh(false);

            if (m_on_color_changed)
                m_on_color_changed(c, false);
            m_updating_hex = false;
            return;
        }
    }

    // Invalid input — reset to current color
    update_hex_input();
}

} // namespace Slic3r::GUI
