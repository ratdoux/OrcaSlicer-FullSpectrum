#include "MixedFilamentRatioPanel.hpp"

#include <wx/wx.h>

#include "Slic3r/GUI/I18N.hpp"
#include "Slic3r/GUI/GUI.hpp"
#include "Slic3r/GUI/GUI_App.hpp"
#include "Slic3r/GUI/GUI_Factories.hpp"
#include "Button.hpp"
#include "CheckBox.hpp"
#include "ComboBox.hpp"
#include "DropDown.hpp"
#include "Label.hpp"
#include "FilamentCardMixed.hpp"
#include <wx/dcgraph.h>

namespace Slic3r::GUI {


MixedFilamentRatioPanel::MixedFilamentRatioPanel(wxWindow*            parent,
                                                 std::vector<double>& filament_weights,
                                                 std::vector<wxColor>& filament_colors)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, parent->FromDIP(220)), wxBORDER_NONE),
    m_filament_weights(filament_weights), m_filament_colors(filament_colors)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);

    Bind(wxEVT_PAINT, &MixedFilamentRatioPanel::on_paint, this);

    Bind(wxEVT_LEFT_DOWN, &MixedFilamentRatioPanel::on_left_down, this);

    Bind(wxEVT_LEFT_UP, &MixedFilamentRatioPanel::on_left_up, this);

    Bind(wxEVT_MOTION, &MixedFilamentRatioPanel::on_motion, this);

    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { m_dragging = false; });

    // TODO handle update from 2-3-4 filaments
};

// static
double MixedFilamentRatioPanel::TriPoint::signed_area2(TriPoint a, TriPoint b, TriPoint c)
{
    return ((b.x - a.x) * (c.y - a.y)) - ((c.x - a.x) * (b.y - a.y));
}

// static
void MixedFilamentRatioPanel::TriPoint::barycentric(TriPoint p, TriPoint v0, TriPoint v1, TriPoint v2, double& w0, double& w1, double& w2)
{
    double total = signed_area2(v0, v1, v2);

    if (std::abs(total) < 1e-9) {
        w0 = w1 = w2 = 1.0 / 3.0;
        return;
    }

    w0 = signed_area2(p, v1, v2) / total;
    w1 = signed_area2(v0, p, v2) / total;
    w2 = 1.0 - w0 - w1;


    double sum = w0 + w1 + w2;

    if (sum > 0.0) {
        w0 /= sum;
        w1 /= sum;
        w2 /= sum;
    }
}

// static
double MixedFilamentRatioPanel::TriPoint::dot(TriPoint a, TriPoint b) { return a.x * b.x + a.y * b.y; }

// static
MixedFilamentRatioPanel::TriPoint MixedFilamentRatioPanel::TriPoint::project_to_segment(TriPoint p, TriPoint a, TriPoint b)
{
    TriPoint ab_line{b.x - a.x, b.y - a.y};
    double   len2 = dot(ab_line, ab_line);

    if (len2 < 1e-12)
        return a;

    double t = dot(TriPoint{p.x - a.x, p.y - a.y}, ab_line) / len2;
    t        = std::clamp(t, 0.0, 1.0);

    return TriPoint{a.x + ab_line.x * t, a.y + ab_line.y * t};
}

// static
MixedFilamentRatioPanel::TriPoint MixedFilamentRatioPanel::TriPoint::project_to_triangle(TriPoint p, TriPoint a, TriPoint b, TriPoint c)
{
    double weight_0, weight_1, weight_2;

    barycentric(p, a, b, c, weight_0, weight_1, weight_2);

    if (weight_0 >= 0.0 && weight_1 >= 0.0 && weight_2 >= 0.0)
        return p;

    TriPoint p_ab = project_to_segment(p, a, b);
    TriPoint p_bc = project_to_segment(p, b, c);
    TriPoint p_ca = project_to_segment(p, c, a);

    auto dist2 = [&](const TriPoint& q) {
        double delta_x = q.x - p.x;
        double delta_y = q.y - p.y;
        return delta_x * delta_x + delta_y * delta_y;
    };

    double d_ab = dist2(p_ab);
    double d_bc = dist2(p_bc);
    double d_ca = dist2(p_ca);

    if (d_ab <= d_bc && d_ab <= d_ca) {
        return p_ab;
    }

    if (d_bc <= d_ca) {
        return p_bc;
    }

    return p_ca;
}

void MixedFilamentRatioPanel::on_paint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC context(this);
    wxSize                size = GetClientSize();

    if (m_filament_weights.empty() || m_filament_colors.empty()) {
        return;
    }

    const int filament_count = m_filament_weights.size();

    if (filament_count == 2) {
        paint_2(context, size, m_filament_colors, m_filament_weights);
    } else if (filament_count == 3) {
        paint_3(context, size, m_filament_colors, m_filament_weights);
    } else if (filament_count == 4) {
        paint_4(context, size, m_filament_colors, m_filament_weights);
    }
}

void MixedFilamentRatioPanel::on_left_down(wxMouseEvent& event)
{
    m_dragging = true;

    if (!HasCapture())
        CaptureMouse();

    update_from_mouse(event.GetX(), event.GetY());
}

void MixedFilamentRatioPanel::on_motion(wxMouseEvent& event)
{
    if (!m_dragging)
        return;

    update_from_mouse(event.GetX(), event.GetY());
}

void MixedFilamentRatioPanel::on_left_up(wxMouseEvent&)
{
    if (!m_dragging)
        return;

    m_dragging = false;

    if (HasCapture())
        ReleaseMouse();
}

void MixedFilamentRatioPanel::update_from_mouse(int x, int y)
{
    if (m_filament_weights.empty())
        return;

    const int filament_count = m_filament_weights.size();

    switch (filament_count) {
    case 2: update_from_mouse_2(x); break;
    case 3: update_from_mouse_3(x, y); break;
    case 4: update_from_mouse_4(x, y); break;
    default: break;
    }
}

std::tuple<MixedFilamentRatioPanel::TriPoint, MixedFilamentRatioPanel::TriPoint, MixedFilamentRatioPanel::TriPoint>
MixedFilamentRatioPanel::triangle_vertices() const
{
    wxSize size   = GetClientSize();
    double margin = FromDIP(24);
    double side   = std::min(size.x, size.y) - margin * 2;

    double trie_height = side * std::sqrt(3.0) / 2.0;

    double center_x = size.x / 2.0;
    double top_y    = (size.y - trie_height) / 2.0;
    double bottom_y = top_y + trie_height;

    TriPoint corner_0{center_x, top_y};
    TriPoint corner_1{center_x - side / 2.0, bottom_y};
    TriPoint corner_2{center_x + side / 2.0, bottom_y};

    return {corner_0, corner_1, corner_2};
}

void MixedFilamentRatioPanel::update_from_mouse_2(int x)
{
    int width = GetClientSize().x;

    double weight_1 = (double) std::clamp(x, 0, width - 1) / (double) std::max(1, width - 1);

    m_filament_weights = {1.0 - weight_1, weight_1};

    Refresh(false);
}

void MixedFilamentRatioPanel::update_from_mouse_3(int x, int y)
{
    auto [corner_0, corner_1, corner_2] = triangle_vertices();

    TriPoint mouse{(double) x, (double) y};

    mouse = TriPoint::project_to_triangle(mouse, corner_0, corner_1, corner_2);

    double weight_0, weight_1, weight_2;

    TriPoint::barycentric(mouse, corner_0, corner_1, corner_2, weight_0, weight_1, weight_2);

    m_filament_weights = {weight_0, weight_1, weight_2};

    Refresh(false);
}

void MixedFilamentRatioPanel::update_from_mouse_4(int x, int y)
{
    auto [corner_0, corner_1, corner_2] = triangle_vertices();

    TriPoint mouse{(double) x, (double) y};
    mouse = TriPoint::project_to_triangle(mouse, corner_0, corner_1, corner_2);

    // triangular space
    double tri_weight_0, tri_weight_1, tri_weight_2;
    TriPoint::barycentric(mouse, corner_0, corner_1, corner_2, tri_weight_0, tri_weight_1, tri_weight_2);

    double weight_3 = 3.0 * std::min({tri_weight_0, tri_weight_1, tri_weight_2});

    double weight_0 = tri_weight_0 - weight_3 / 3.0;
    double weight_1 = tri_weight_1 - weight_3 / 3.0;
    double weight_2 = tri_weight_2 - weight_3 / 3.0;

    m_filament_weights = {weight_0, weight_1, weight_2, weight_3};

    Refresh(false);
}

void MixedFilamentRatioPanel::paint_2(wxDC& context, const wxSize& size, std::vector<wxColor>& colors, std::vector<double>& weights)
{
    context.SetBrush(wxBrush(GetBackgroundColour()));
    context.SetPen(*wxTRANSPARENT_PEN);
    context.DrawRectangle(0, 0, size.x, size.y);

    if (colors.size() != 2 || colors.size() != weights.size())
        return;

    const int width  = size.x;
    const int height = size.y;

    // Gradient
    for (int x = 0; x < width; ++x) {
        double t = static_cast<double>(x) / std::max(1, width - 1);

        unsigned char r = static_cast<unsigned char>(colors[0].Red() * (1.0 - t) + colors[1].Red() * t);
        unsigned char g = static_cast<unsigned char>(colors[0].Green() * (1.0 - t) + colors[1].Green() * t);
        unsigned char b = static_cast<unsigned char>(colors[0].Blue() * (1.0 - t) + colors[1].Blue() * t);

        context.SetPen(wxPen(wxColour(r, g, b)));
        context.DrawLine(x, 0, x, height);
    }

    // Border
    context.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#BDBDBD")), 1));
    context.SetBrush(*wxTRANSPARENT_BRUSH);
    context.DrawRectangle(0, 0, width - 1, height - 1);

    // Handle
    int handle_x = std::round(weights[1] * (width - 1));

    const int radius = FromDIP(6);

    context.SetBrush(*wxWHITE_BRUSH);
    context.SetPen(wxPen(wxColour("#262E30"), FromDIP(2)));
    context.DrawCircle(handle_x, height / 2, radius);

    // TODO draw line under border?
}

void MixedFilamentRatioPanel::paint_3(wxDC& context, const wxSize& size, std::vector<wxColor>& colors, std::vector<double>& weights)
{
    if (colors.size() != 3 || weights.size() != 3)
        return;

    const auto [corner_0, corner_1, corner_2] = triangle_vertices();

    // background
    if (!m_cached_background_3.IsOk() || m_last_size != size || m_last_colors != colors) {
        wxImage image(size.x, size.y, false);

        const wxColour bg = GetBackgroundColour();

        for (int y = 0; y < size.y; ++y) {
            for (int x = 0; x < size.x; ++x) {
                image.SetRGB(x, y, bg.Red(), bg.Green(), bg.Blue());
            }
        }

        const int min_x = std::max(0, (int) std::floor(std::min({corner_0.x, corner_1.x, corner_2.x})));
        const int max_x = std::min(size.x - 1, (int) std::ceil(std::max({corner_0.x, corner_1.x, corner_2.x})));

        const int min_y = std::max(0, (int) std::floor(std::min({corner_0.y, corner_1.y, corner_2.y})));
        const int max_y = std::min(size.y - 1, (int) std::ceil(std::max({corner_0.y, corner_1.y, corner_2.y})));

        for (int y = min_y; y <= max_y; ++y) {
            for (int x = min_x; x <= max_x; ++x) {
                const TriPoint p{(double) x + 0.5, (double) y + 0.5};

                double w0, w1, w2;

                TriPoint::barycentric(p, corner_0, corner_1, corner_2, w0, w1, w2);

                constexpr double eps = -1e-6;

                if (w0 < eps || w1 < eps || w2 < eps)
                    continue;

                const int r = (int) std::clamp(w0 * colors[0].Red() + w1 * colors[1].Red() + w2 * colors[2].Red(), 0.0, 255.0);
                const int g = (int) std::clamp(w0 * colors[0].Green() + w1 * colors[1].Green() + w2 * colors[2].Green(), 0.0, 255.0);
                const int b = (int) std::clamp(w0 * colors[0].Blue() + w1 * colors[1].Blue() + w2 * colors[2].Blue(), 0.0, 255.0);

                image.SetRGB(x, y, r, g, b);
            }
        }

        m_cached_background_3 = wxBitmap(image);
        m_last_size           = size;
        m_last_colors         = colors;
    }

    context.DrawBitmap(m_cached_background_3, 0, 0);

    // border
    const wxColour border_colour(120, 120, 120);

    context.SetPen(wxPen(border_colour, FromDIP(1)));
    context.SetBrush(*wxTRANSPARENT_BRUSH);

    wxPoint triangle[3] = {wxPoint(wxRound(corner_0.x), wxRound(corner_0.y)), wxPoint(wxRound(corner_1.x), wxRound(corner_1.y)),
                           wxPoint(wxRound(corner_2.x), wxRound(corner_2.y))};

    context.DrawPolygon(3, triangle);

    // handle
    const double px = weights[0] * corner_0.x + weights[1] * corner_1.x + weights[2] * corner_2.x;
    const double py = weights[0] * corner_0.y + weights[1] * corner_1.y + weights[2] * corner_2.y;

    const int radius = FromDIP(6);

    context.SetBrush(*wxWHITE_BRUSH);
    context.SetPen(wxPen(wxColour(60, 60, 60), FromDIP(2)));

    context.DrawCircle(wxPoint(wxRound(px), wxRound(py)), radius);
}

void MixedFilamentRatioPanel::paint_4(wxDC& context, const wxSize& size, std::vector<wxColor>& colors, std::vector<double>& weights)
{
    if (colors.size() != 4 || weights.size() != 4)
        return;

    const auto [corner_0, corner_1, corner_2] = triangle_vertices();
    const TriPoint centroid{(corner_0.x + corner_1.x + corner_2.x) / 3.0, (corner_0.y + corner_1.y + corner_2.y) / 3.0};

    // background
    if (!m_cached_background_4.IsOk() || m_last_size != size || m_last_colors != colors) {
        const double max_radius = std::sqrt((corner_0.x - centroid.x) * (corner_0.x - centroid.x) +
                                            (corner_0.y - centroid.y) * (corner_0.y - centroid.y));
        wxImage image(size.x, size.y, false);
        const wxColour bg = GetBackgroundColour();

        for (int y = 0; y < size.y; ++y) {
            for (int x = 0; x < size.x; ++x) {
                image.SetRGB(x, y, bg.Red(), bg.Green(), bg.Blue());
            }
        }

        const int min_x = std::max(0, (int) std::floor(std::min({corner_0.x, corner_1.x, corner_2.x})));
        const int max_x = std::min(size.x - 1, (int) std::ceil(std::max({corner_0.x, corner_1.x, corner_2.x})));

        const int min_y = std::max(0, (int) std::floor(std::min({corner_0.y, corner_1.y, corner_2.y})));
        const int max_y = std::min(size.y - 1, (int) std::ceil(std::max({corner_0.y, corner_1.y, corner_2.y})));

        for (int y = min_y; y <= max_y; ++y) {
            for (int x = min_x; x <= max_x; ++x) {
                const TriPoint p{(double) x + 0.5, (double) y + 0.5};
                double         tri_w0, tri_w1, tri_w2;

                TriPoint::barycentric(p, corner_0, corner_1, corner_2, tri_w0, tri_w1, tri_w2);

                constexpr double eps = -1e-6;

                if (tri_w0 < eps || tri_w1 < eps || tri_w2 < eps)
                    continue;

                const double dx = p.x - centroid.x;
                const double dy = p.y - centroid.y;

                const double radius = std::sqrt(dx * dx + dy * dy);

                const double outer  = std::clamp(radius / max_radius, 0.0, 1.0);
                const double center = 1.0 - outer;

                const double w0 = tri_w0 * outer;
                const double w1 = tri_w1 * outer;
                const double w2 = tri_w2 * outer;
                const double w3 = center;

                const int r = (int) std::clamp(w0 * colors[0].Red() + w1 * colors[1].Red() + w2 * colors[2].Red() + w3 * colors[3].Red(),
                                               0.0, 255.0);
                const int g = (int) std::clamp(w0 * colors[0].Green() + w1 * colors[1].Green() + w2 * colors[2].Green() +
                                                   w3 * colors[3].Green(),
                                               0.0, 255.0);
                const int b = (int) std::clamp(w0 * colors[0].Blue() + w1 * colors[1].Blue() + w2 * colors[2].Blue() + w3 * colors[3].Blue(),
                                               0.0, 255.0);

                image.SetRGB(x, y, r, g, b);
            }
        }

        m_cached_background_4 = wxBitmap(image);
        m_last_size           = size;
        m_last_colors         = colors;
    }

    context.DrawBitmap(m_cached_background_4, 0, 0);

    const wxColour border_colour(120, 120, 120);

    context.SetPen(wxPen(border_colour, FromDIP(1)));
    context.SetBrush(*wxTRANSPARENT_BRUSH);

    wxPoint triangle[3] = {wxPoint(wxRound(corner_0.x), wxRound(corner_0.y)), wxPoint(wxRound(corner_1.x), wxRound(corner_1.y)),
                           wxPoint(wxRound(corner_2.x), wxRound(corner_2.y))};

    context.DrawPolygon(3, triangle);

    context.SetPen(wxPen(border_colour, FromDIP(1)));
    context.SetBrush(*wxTRANSPARENT_BRUSH);
    context.DrawCircle(wxPoint(wxRound(centroid.x), wxRound(centroid.y)), FromDIP(4));

    //
    // Handle reconstruction
    //
    // weights[3] = center
    // remaining weights are distributed on the outer triangle.
    //
    const double outer = std::max(0.0, 1.0 - weights[3]);

    double tri_w0 = 1.0 / 3.0;
    double tri_w1 = 1.0 / 3.0;
    double tri_w2 = 1.0 / 3.0;

    if (outer > 1e-9) {
        tri_w0 = weights[0] / outer;
        tri_w1 = weights[1] / outer;
        tri_w2 = weights[2] / outer;
    }

    const double outer_x = tri_w0 * corner_0.x + tri_w1 * corner_1.x + tri_w2 * corner_2.x;
    const double outer_y = tri_w0 * corner_0.y + tri_w1 * corner_1.y + tri_w2 * corner_2.y;

    const double handle_x = centroid.x + (outer_x - centroid.x) * outer;
    const double handle_y = centroid.y + (outer_y - centroid.y) * outer;

    context.SetBrush(*wxWHITE_BRUSH);
    context.SetPen(wxPen(wxColour(60, 60, 60), FromDIP(2)));

    context.DrawCircle(wxPoint(wxRound(handle_x), wxRound(handle_y)), FromDIP(6));
}

} // namespace Slic3r::GUI