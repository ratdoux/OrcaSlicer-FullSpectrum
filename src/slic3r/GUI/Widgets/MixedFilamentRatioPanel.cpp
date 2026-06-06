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


MixedFilamentRatioPanel::MixedFilamentRatioPanel(
    wxWindow*            parent,
    std::vector<double>& filament_weights,
    std::vector<wxColor>& filament_colors,
    double& min_weight_ratio,
    std::function<void()> on_weights_changed_callback)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, parent->FromDIP(220)), wxBORDER_NONE),
    m_filament_weights(filament_weights), m_filament_colors(filament_colors), m_min_weight_ratio(min_weight_ratio), m_on_weights_changed_callback(on_weights_changed_callback)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);

    Bind(wxEVT_PAINT, &MixedFilamentRatioPanel::on_paint, this);

    Bind(wxEVT_LEFT_DOWN, &MixedFilamentRatioPanel::on_left_down, this);

    Bind(wxEVT_LEFT_UP, &MixedFilamentRatioPanel::on_left_up, this);

    Bind(wxEVT_MOTION, &MixedFilamentRatioPanel::on_motion, this);

    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { m_dragging = false; });

    update_sizing();
};

void MixedFilamentRatioPanel::update_sizing()
{
    const int filament_count = m_filament_weights.size();

    if (filament_count == 2) {
        // Height locked to 60, width allowed to expand (-1)
        SetMinSize(wxSize(-1, FromDIP(60)));
        SetMaxSize(wxSize(-1, FromDIP(60)));
    } else {
        // Height and Width scale proportionally, setting a square minimum
        SetMinSize(wxSize(FromDIP(220), FromDIP(220)));
        SetMaxSize(wxSize(-1, -1));
    }

    if (GetParent()) {
        GetParent()->Layout();
    }
}

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

void MixedFilamentRatioPanel::on_left_down(wxMouseEvent& event)
{
    m_dragging = true;

    if (!HasCapture())
        CaptureMouse();

    update_weights_from_mouse(event.GetX(), event.GetY());
}

void MixedFilamentRatioPanel::on_motion(wxMouseEvent& event)
{
    if (!m_dragging)
        return;

    update_weights_from_mouse(event.GetX(), event.GetY());
}

void MixedFilamentRatioPanel::on_left_up(wxMouseEvent&)
{
    if (!m_dragging)
        return;

    m_dragging = false;

    if (HasCapture())
        ReleaseMouse();

    Refresh(); // necessary to have different paint for on-drag and not on-drag states
}

void MixedFilamentRatioPanel::on_paint(wxPaintEvent&)
{
    if (m_filament_weights.empty() || m_filament_colors.empty()) {
        return;
    }

    wxAutoBufferedPaintDC buffered_pain_context(this);
    buffered_pain_context.SetBackground(wxBrush(get_background_color()));
    buffered_pain_context.Clear();

    std::unique_ptr<wxGraphicsContext> context(wxGraphicsContext::Create(buffered_pain_context));
    if (!context)
        return;

    context->SetAntialiasMode(wxAntialiasMode::wxANTIALIAS_DEFAULT);

    wxSize    size           = GetClientSize();
    const int filament_count = m_filament_weights.size();

    if (size.x <= 0 || size.y <= 0)
        return;

    if (filament_count == 2) {
        paint_2(*context, size, m_filament_colors, m_filament_weights);
    } else if (filament_count == 3) {
        paint_3(*context, size, m_filament_colors, m_filament_weights);
    } else if (filament_count == 4) {
        paint_4(*context, size, m_filament_colors, m_filament_weights);
    }

    paint_min_ratio_overlay(*context, size);

    // paint handle after overlay to ensure its always on top and visible
    if (filament_count == 2) {
        paint_2_handle(*context, size, m_filament_colors, m_filament_weights);
    } else if (filament_count == 3) {
        paint_3_handle(*context, size, m_filament_colors, m_filament_weights);
    } else if (filament_count == 4) {
        paint_4_handle(*context, size, m_filament_colors, m_filament_weights);
    }
}

void MixedFilamentRatioPanel::update_weights_from_mouse(int x, int y)
{
    if (m_filament_weights.empty())
        return;

    const int filament_count = m_filament_weights.size();

    switch (filament_count) {
    case 2: update_weights_from_mouse_2(x); break;
    case 3: update_weights_from_mouse_3(x, y); break;
    case 4: update_weights_from_mouse_4(x, y); break;
    default: break;
    }

    if (m_on_weights_changed_callback) {
        m_on_weights_changed_callback();
    }
}

void MixedFilamentRatioPanel::clamp_weights_2(std::vector<double>& weights, double min_weight_ratio)
{
    if (weights.size() != 2)
        return;

    double u = weights[1];
    double m = min_weight_ratio;
    u = std::clamp(u, m, 1.0 - m);

    weights[0] = 1.0 - u;
    weights[1] = u;
}

void MixedFilamentRatioPanel::update_weights_from_mouse_2(int x)
{
    double margin = get_margin();
    int width = std::max(1.0, GetClientSize().x - margin * 2);
    double adjusted_x = (double) x - margin;

    double weight_1 = adjusted_x / width;
    double weight_0 = 1.0 - weight_1;

    m_filament_weights = {weight_0, weight_1};
    clamp_weights_2(m_filament_weights, m_min_weight_ratio);

    Refresh(false);
}

void MixedFilamentRatioPanel::paint_2(wxGraphicsContext& gc, const wxSize& size, std::vector<wxColor>& colors, std::vector<double>& weights)
{
    if (colors.size() != 2 || colors.size() != weights.size())
        return;

    const double   margin           = get_margin();
    const double   border_width     = get_border_width();
    const wxColour border_color     = get_border_color();
    const wxColour background_color = get_background_color();

    const double start_x = margin;
    const double start_y = margin;
    const double width   = std::max(1.0, size.x - margin * 2.0);
    const double height  = std::max(1.0, size.y - margin * 2.0);

    // Background
    gc.SetBrush(wxBrush(background_color));
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.DrawRectangle(0, 0, width, height);

    // Gradient 
    wxGraphicsBrush gradientBrush = gc.CreateLinearGradientBrush(start_x, start_y, start_x + width, start_y, colors[0], colors[1]);
    gc.SetBrush(gradientBrush);
    gc.DrawRectangle(start_x, start_y, width, height);

    // Border
    gc.SetPen(wxPen(border_color, border_width));
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    gc.DrawRectangle(start_x, start_y, width, height);
}

void MixedFilamentRatioPanel::paint_2_handle(wxGraphicsContext&          gc,
                                             const wxSize&               size,
                                             const std::vector<wxColor>& colors,
                                             const std::vector<double>&  weights)
{
    if (colors.size() != 2 || colors.size() != weights.size())
        return;

    const double border_width  = get_border_width();
    const double handle_radius = get_handle_radius();
    const double margin        = get_margin();

    const double start_x = margin;
    const double start_y = margin;
    const double width   = std::max(1.0, size.x - margin * 2.0);
    const double height  = std::max(1.0, size.y - margin * 2.0);

    const double handle_x = start_x + (weights[1] * width);

    // White line behind the handle
    gc.SetPen(wxPen(*wxWHITE, border_width));
    gc.StrokeLine(handle_x, start_y, handle_x, start_y + height);

    // Calculate handle color
    double   t = weights[1];
    wxColour handle_color(static_cast<unsigned char>(colors[0].Red() * (1.0 - t) + colors[1].Red() * t),
                          static_cast<unsigned char>(colors[0].Green() * (1.0 - t) + colors[1].Green() * t),
                          static_cast<unsigned char>(colors[0].Blue() * (1.0 - t) + colors[1].Blue() * t));

    gc.SetBrush(wxBrush(handle_color));
    gc.SetPen(wxPen(*wxWHITE, border_width));

    gc.DrawEllipse(handle_x - handle_radius, (start_y + height / 2.0) - handle_radius, handle_radius * 2.0, handle_radius * 2.0);
}

void MixedFilamentRatioPanel::clamp_weights_3(std::vector<double>& weights, double min_weight_ratio)
{
    if (weights.size() != 3)
        return;

    double m                   = min_weight_ratio;
    bool   recalculate_weights = false;
    for (double weight : weights) {
        if (weight < m) {
            recalculate_weights = true;
            break;
        }
    }

    if (recalculate_weights) {
        std::vector<double> diffs(3, 0.0);
        double              diff_sum = 0.0;
        for (int i = 0; i < 3; ++i) {
            double diff = std::max(0.0, weights[i] - m);
            diffs[i]    = diff;
            diff_sum += diff;
        }

        if (diff_sum > 1e-9) {
            for (int i = 0; i < 3; ++i) {
                weights[i] = m + (1.0 - 3.0 * m) * (diffs[i] / diff_sum);
            }
        } else {
            for (int i = 0; i < 3; ++i) {
                weights[i] = 1.0 / 3.0;
            }
        }
    }
}

void MixedFilamentRatioPanel::update_weights_from_mouse_3(int x, int y)
{
    auto [corner_0, corner_1, corner_2] = triangle_vertices();

    auto inner_point = [&](double bw0, double bw1, double bw2) {
        return TriPoint{bw0 * corner_0.x + bw1 * corner_1.x + bw2 * corner_2.x, bw0 * corner_0.y + bw1 * corner_1.y + bw2 * corner_2.y};
    };

    double   m              = m_min_weight_ratio;
    TriPoint corner_inner_0 = inner_point(1.0 - 2.0 * m, m, m);
    TriPoint corner_inner_1 = inner_point(m, 1.0 - 2.0 * m, m);
    TriPoint corner_inner_2 = inner_point(m, m, 1.0 - 2.0 * m);

    TriPoint mouse{(double) x, (double) y};

    mouse = TriPoint::project_to_triangle(mouse, corner_inner_0, corner_inner_1, corner_inner_2);

    double weight_0, weight_1, weight_2;

    TriPoint::barycentric(mouse, corner_0, corner_1, corner_2, weight_0, weight_1, weight_2);

    m_filament_weights = {weight_0, weight_1, weight_2};

    Refresh(false);
}

void MixedFilamentRatioPanel::paint_3(wxGraphicsContext& gc, const wxSize& size, std::vector<wxColor>& colors, std::vector<double>& weights)
{
    if (colors.size() != 3 || weights.size() != 3)
        return;

    const double   border_width  = get_border_width();
    const wxColour border_color  = get_border_color();
    const wxColour background_color = get_background_color();
    const double   handle_radius = get_handle_radius();

    const auto [corner_0, corner_1, corner_2] = triangle_vertices();

    // Background (Caching is kept here because barycentric gradients aren't native to standard graphics APIs)
    if (!m_cached_background_3.IsOk() || m_cached_background_3.GetSize() != size || m_last_colors != colors) {
        wxImage image(size.x, size.y, false);

        for (int y = 0; y < size.y; ++y) {
            for (int x = 0; x < size.x; ++x) {
                image.SetRGB(x, y, background_color.Red(), background_color.Green(), background_color.Blue());
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
                if (w0 < eps || w1 < eps || w2 < eps) continue;

                const int r = (int) std::clamp(w0 * colors[0].Red() + w1 * colors[1].Red() + w2 * colors[2].Red(), 0.0, 255.0);
                const int g = (int) std::clamp(w0 * colors[0].Green() + w1 * colors[1].Green() + w2 * colors[2].Green(), 0.0, 255.0);
                const int b = (int) std::clamp(w0 * colors[0].Blue() + w1 * colors[1].Blue() + w2 * colors[2].Blue(), 0.0, 255.0);
                image.SetRGB(x, y, r, g, b);
            }
        }
        m_cached_background_3 = wxBitmap(image);
        m_last_colors         = colors;
    }

    gc.DrawBitmap(m_cached_background_3, 0, 0, size.x, size.y);

    // Border (Using paths for smooth anti-aliased triangles)
    gc.SetPen(wxPen(border_color, border_width));
    gc.SetBrush(*wxTRANSPARENT_BRUSH);

    wxGraphicsPath path = gc.CreatePath();
    path.MoveToPoint(corner_0.x, corner_0.y);
    path.AddLineToPoint(corner_1.x, corner_1.y);
    path.AddLineToPoint(corner_2.x, corner_2.y);
    path.AddLineToPoint(corner_0.x, corner_0.y);
    path.CloseSubpath();
    gc.StrokePath(path);
}

void MixedFilamentRatioPanel::paint_3_handle(
    wxGraphicsContext& gc,
    const wxSize&,
    const std::vector<wxColor>& colors,
    const std::vector<double>& weights)
{
    if (colors.size() != 3 || weights.size() != 3)
        return;

    const double border_width  = get_border_width();
    const double handle_radius = get_handle_radius();

    const auto [corner_0, corner_1, corner_2] = triangle_vertices();
    
    // Handle
    const double px = weights[0] * corner_0.x + weights[1] * corner_1.x + weights[2] * corner_2.x;
    const double py = weights[0] * corner_0.y + weights[1] * corner_1.y + weights[2] * corner_2.y;

    // Handle Color
    wxColour handle_color(
        (unsigned char)std::clamp(weights[0] * colors[0].Red() + weights[1] * colors[1].Red() + weights[2] * colors[2].Red(), 0.0, 255.0),
        (unsigned char)std::clamp(weights[0] * colors[0].Green() + weights[1] * colors[1].Green() + weights[2] * colors[2].Green(), 0.0, 255.0),
        (unsigned char)std::clamp(weights[0] * colors[0].Blue() + weights[1] * colors[1].Blue() + weights[2] * colors[2].Blue(), 0.0, 255.0)
    );

    gc.SetBrush(wxBrush(handle_color));
    gc.SetPen(wxPen(*wxWHITE, border_width));
    gc.DrawEllipse(px - handle_radius, py - handle_radius, handle_radius * 2.0, handle_radius * 2.0);
}


void MixedFilamentRatioPanel::clamp_weights_4(std::vector<double>& weights, double min_weight_ratio)
{
    if (weights.size() != 4)
        return;

    double u = weights[1] + weights[3];
    double v = weights[2] + weights[3];

    double m = min_weight_ratio;

    if (m > 0.0) {
        if (m >= 0.25) {
            u = 0.5;
            v = 0.5;
        } else {
            // Check if (u, v) is already inside the allowed boundary
            double w0 = (1.0 - u) * (1.0 - v);
            double w1 = u * (1.0 - v);
            double w2 = (1.0 - u) * v;
            double w3 = u * v;

            bool inside = (w0 >= m && w1 >= m && w2 >= m && w3 >= m);

            // fast ternary search implementation to find the closest point on the boundary curves if outside
            if (!inside) {
                struct LocalCurve
                {
                    double u_start;
                    double u_end;
                    int    type; // 1, 2, 3, or 4
                };

                auto get_v_func = [](int type, double u_val, double m_val) -> double {
                    switch (type) {
                    case 1: return m_val / u_val;
                    case 2: return m_val / (1.0 - u_val);
                    case 3: return 1.0 - m_val / u_val;
                    case 4: return 1.0 - m_val / (1.0 - u_val);
                    default: return 0.0;
                    }
                };

                auto closest_point_on_curve = [&](double u_mouse, double v_mouse, const LocalCurve& curve) {
                    double l = curve.u_start;
                    double r = curve.u_end;
                    // 15 iterations of ternary search for high precision
                    for (int iter = 0; iter < 15; ++iter) {
                        double m1 = l + (r - l) / 3.0;
                        double m2 = r - (r - l) / 3.0;

                        double v1 = get_v_func(curve.type, m1, m);
                        double v2 = get_v_func(curve.type, m2, m);

                        double d1 = (m1 - u_mouse) * (m1 - u_mouse) + (v1 - v_mouse) * (v1 - v_mouse);
                        double d2 = (m2 - u_mouse) * (m2 - u_mouse) + (v2 - v_mouse) * (v2 - v_mouse);

                        if (d1 < d2) {
                            r = m2;
                        } else {
                            l = m1;
                        }
                    }
                    double u_best = (l + r) * 0.5;
                    return TriPoint{u_best, get_v_func(curve.type, u_best, m)};
                };

                // Define the 4 boundary curves
                LocalCurve curves[4] = {
                    {2.0 * m, 0.5, 1},       // Curve 1 (bottom-left boundary)
                    {0.5, 1.0 - 2.0 * m, 2}, // Curve 2 (bottom-right boundary)
                    {2.0 * m, 0.5, 3},       // Curve 3 (top-left boundary)
                    {0.5, 1.0 - 2.0 * m, 4}  // Curve 4 (top-right boundary)
                };

                TriPoint best_point  = closest_point_on_curve(u, v, curves[0]);
                double   min_dist_sq = (best_point.x - u) * (best_point.x - u) + (best_point.y - v) * (best_point.y - v);

                for (int i = 1; i < 4; ++i) {
                    TriPoint pt      = closest_point_on_curve(u, v, curves[i]);
                    double   dist_sq = (pt.x - u) * (pt.x - u) + (pt.y - v) * (pt.y - v);
                    if (dist_sq < min_dist_sq) {
                        min_dist_sq = dist_sq;
                        best_point  = pt;
                    }
                }
                u = best_point.x;
                v = best_point.y;
            }
        }
    } else {
        u = std::clamp(u, 0.0, 1.0);
        v = std::clamp(v, 0.0, 1.0);
    }

    weights[0] = (1.0 - u) * (1.0 - v);
    weights[1] = u * (1.0 - v);
    weights[2] = (1.0 - u) * v;
    weights[3] = u * v;
}

void MixedFilamentRatioPanel::update_weights_from_mouse_4(int x, int y)
{
    auto [corner_0, corner_1, corner_2, corner_3] = quad_vertices();

    double side = corner_1.x - corner_0.x;
    if (side <= 0.0)
        return;

    TriPoint mouse{(double) x, (double) y};

    mouse.x = std::clamp(mouse.x, corner_0.x, corner_1.x);
    mouse.y = std::clamp(mouse.y, corner_0.y, corner_2.y);

    double u = (mouse.x - corner_0.x) / side;
    double v = (mouse.y - corner_0.y) / side;

    m_filament_weights = {
        (1.0 - u) * (1.0 - v),
        u * (1.0 - v),
        (1.0 - u) * v,
        u * v
    };
    clamp_weights_4(m_filament_weights, m_min_weight_ratio);

    Refresh(false);
}

void MixedFilamentRatioPanel::paint_4(wxGraphicsContext& gc, const wxSize& size, std::vector<wxColor>& colors, std::vector<double>& weights)
{
    if (colors.size() != 4 || weights.size() != 4)
        return;

    const double   border_width  = get_border_width();
    const wxColour border_color  = get_border_color();
    const wxColour background_color = get_background_color();
    const double   handle_radius = get_handle_radius();

    const auto [corner_0, corner_1, corner_2, corner_3] = quad_vertices();
    double side = corner_1.x - corner_0.x;
    if (side <= 0.0)
        return;

    // Background (Caching is kept here for the complex distance/barycentric math)
    if (!m_cached_background_4.IsOk() || m_cached_background_4.GetSize() != size || m_last_colors != colors) {
        wxImage image(size.x, size.y, false);

        for (int y = 0; y < size.y; ++y) {
            for (int x = 0; x < size.x; ++x) {
                image.SetRGB(x, y, background_color.Red(), background_color.Green(), background_color.Blue());
            }
        }

        const int min_x = std::max(0, (int)std::floor(corner_0.x));
        const int max_x = std::min(size.x - 1, (int) std::ceil(corner_1.x));
        const int min_y = std::max(0, (int) std::floor(corner_0.y));
        const int max_y = std::min(size.y - 1, (int) std::ceil(corner_2.y));

        for (int y = min_y; y <= max_y; ++y) {
            for (int x = min_x; x <= max_x; ++x) {
                // Get normalized coordinates for the current pixel
                double u = ((double) x + 0.5 - corner_0.x) / side;
                double v = ((double) y + 0.5 - corner_0.y) / side;

                // Mask strictly to the square
                if (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0)
                    continue;

                double weight_0 = (1.0 - u) * (1.0 - v);
                double weight_1 = u * (1.0 - v);
                double weight_2 = (1.0 - u) * v;
                double weight_3 = u * v;

                const int r = (int) std::clamp(
                    weight_0 * colors[0].Red() 
                    + weight_1 * colors[1].Red() 
                    + weight_2 * colors[2].Red() 
                    + weight_3 * colors[3].Red(),
                    0.0, 255.0);
                const int g = (int) std::clamp(
                    weight_0 * colors[0].Green() 
                    + weight_1 * colors[1].Green() 
                    + weight_2 * colors[2].Green() 
                    + weight_3 * colors[3].Green(),
                    0.0, 255.0);
                const int b = (int) std::clamp(
                    weight_0 * colors[0].Blue() 
                    + weight_1 * colors[1].Blue() 
                    + weight_2 * colors[2].Blue() 
                    + weight_3 * colors[3].Blue(),
                    0.0, 255.0);

                image.SetRGB(x, y, r, g, b);
            }
        }
        m_cached_background_4 = wxBitmap(image);
        m_last_colors         = colors;
    }

    gc.DrawBitmap(m_cached_background_4, 0, 0, size.x, size.y);

    // Border
    gc.SetPen(wxPen(border_color, border_width));
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    gc.DrawRectangle(corner_0.x, corner_0.y, side, side);
}

void MixedFilamentRatioPanel::paint_4_handle(
    wxGraphicsContext& gc,
    const wxSize&,
    const std::vector<wxColor>& colors,
    const std::vector<double>& weights)
{
    if (colors.size() != 4 || weights.size() != 4)
        return;

    const double border_width  = get_border_width();
    const double handle_radius = get_handle_radius();

    const auto [corner_0, corner_1, corner_2, corner_3] = quad_vertices();

    const double side = corner_1.x - corner_0.x;
    if (side <= 0.0)
        return;
    
    // Handle Location
    // Mathematically reversing the bilinear equations to get U and V back from the weights
    const double u = weights[1] + weights[3];
    const double v = weights[2] + weights[3];

    const double handle_x = corner_0.x + u * side;
    const double handle_y = corner_0.y + v * side;

    // Handle Color
    wxColour handle_color(
        (unsigned char)std::clamp(weights[0] * colors[0].Red() + weights[1] * colors[1].Red() + weights[2] * colors[2].Red() + weights[3] * colors[3].Red(), 0.0, 255.0),
        (unsigned char)std::clamp(weights[0] * colors[0].Green() + weights[1] * colors[1].Green() + weights[2] * colors[2].Green() + weights[3] * colors[3].Green(), 0.0, 255.0),
        (unsigned char)std::clamp(weights[0] * colors[0].Blue() + weights[1] * colors[1].Blue() + weights[2] * colors[2].Blue() + weights[3] * colors[3].Blue(), 0.0, 255.0)
    );

    gc.SetBrush(wxBrush(handle_color));
    gc.SetPen(wxPen(*wxWHITE, border_width));
    gc.DrawEllipse(handle_x - handle_radius, handle_y - handle_radius, handle_radius * 2.0, handle_radius * 2.0);
}

void MixedFilamentRatioPanel::paint_min_ratio_overlay(wxGraphicsContext& context, const wxSize& size) 
{
    const int count = m_filament_weights.size();

    if (count < 2 || m_min_weight_ratio <= 0.0)
        return;

    // Determine if the cache needs to be regenerated
    bool size_changed  = !m_cached_min_weight_ratio_overlay.IsOk() || m_cached_min_weight_ratio_overlay.GetSize() != size;
    bool ratio_changed = (m_last_min_weight_ratio != m_min_weight_ratio);
    bool count_changed = (m_last_count != count);

    if (size_changed || ratio_changed || count_changed) {

        wxBitmap   bitmap(size.x, size.y, 32);
        bitmap.UseAlpha();
        wxMemoryDC memory_dc;
        memory_dc.SelectObject(bitmap);
        memory_dc.SetBackground(*wxTRANSPARENT_BRUSH);
        memory_dc.Clear();

        std::unique_ptr<wxGraphicsContext> mem_context(wxGraphicsContext::Create(memory_dc));
        if (!mem_context)
            return;
        
        // 50% opacity background color
        wxColor bg = GetBackgroundColour();
        mem_context->SetBrush(wxBrush(wxColor(bg.Red(), bg.Green(), bg.Blue(), 128)));

        // Checkered white line setup
        wxDash dashes[2] = {4, 4};
        wxPen  edge_pen(*wxWHITE, 1, wxPENSTYLE_USER_DASH);
        edge_pen.SetDashes(2, dashes);
        mem_context->SetPen(edge_pen);

        wxGraphicsPath full_path  = mem_context->CreatePath();
        wxGraphicsPath inner_path = mem_context->CreatePath();

        double m = m_min_weight_ratio;

        if (count == 2) {
            double margin = get_margin();
            double width  = std::max(1.0, size.x - margin * 2.0);
            double height = std::max(1.0, size.y - margin * 2.0);

            full_path.AddRectangle(margin, margin, width, height);
            inner_path.AddRectangle(margin + width * m, margin, width * (1.0 - 2.0 * m), height);

            full_path.AddPath(inner_path);
            mem_context->FillPath(full_path, wxODDEVEN_RULE);

            // Draw boundaries
            mem_context->StrokeLine(margin + width * m, margin, margin + width * m, margin + height);
            mem_context->StrokeLine(margin + width * (1.0 - m), margin, margin + width * (1.0 - m), margin + height);
        } else if (count == 3) {
            auto [corner_0, corner_1, corner_2] = triangle_vertices();

            full_path.MoveToPoint(corner_0.x, corner_0.y);
            full_path.AddLineToPoint(corner_1.x, corner_1.y);
            full_path.AddLineToPoint(corner_2.x, corner_2.y);
            full_path.CloseSubpath();

            auto inner_point = [&](double bw0, double bw1, double bw2) {
                return wxPoint2DDouble(bw0 * corner_0.x + bw1 * corner_1.x + bw2 * corner_2.x,
                                       bw0 * corner_0.y + bw1 * corner_1.y + bw2 * corner_2.y);
            };

            inner_path.MoveToPoint(inner_point(1.0 - 2.0 * m, m, m));
            inner_path.AddLineToPoint(inner_point(m, 1.0 - 2.0 * m, m));
            inner_path.AddLineToPoint(inner_point(m, m, 1.0 - 2.0 * m));
            inner_path.CloseSubpath();

            full_path.AddPath(inner_path);
            mem_context->FillPath(full_path, wxODDEVEN_RULE);
            mem_context->StrokePath(inner_path);
        } else if (count == 4) {
            auto [corner_0, corner_1, corner_2, c3] = quad_vertices();
            double side                             = corner_1.x - corner_0.x;

            full_path.AddRectangle(corner_0.x, corner_0.y, side, side);

            auto get_v_min = [m](double u) { return std::max(m / std::max(u, 1e-5), m / std::max(1.0 - u, 1e-5)); };
            auto get_v_max = [m](double u) { return std::min(1.0 - m / std::max(u, 1e-5), 1.0 - m / std::max(1.0 - u, 1e-5)); };

            // Generate points for the hyperbola boundaries approximating the valid squircle
            std::vector<wxPoint2DDouble> top_pts, bot_pts;
            const int                    steps = 16;

            for (int i = 0; i <= steps; ++i) {
                double u = 2.0 * m + (1.0 - 4.0 * m) * (i / (double) steps);
                top_pts.push_back({corner_0.x + u * side, corner_0.y + get_v_min(u) * side});
                bot_pts.push_back({corner_0.x + u * side, corner_0.y + get_v_max(u) * side});
            }

            if (!top_pts.empty()) {
                inner_path.MoveToPoint(top_pts.front());
                for (const auto& pt : top_pts)
                    inner_path.AddLineToPoint(pt);
                for (auto it = bot_pts.rbegin(); it != bot_pts.rend(); ++it)
                    inner_path.AddLineToPoint(*it);
                inner_path.CloseSubpath();
            }

            full_path.AddPath(inner_path);
            mem_context->FillPath(full_path, wxODDEVEN_RULE);
            mem_context->StrokePath(inner_path);
        }

        memory_dc.SelectObject(wxNullBitmap); // deselect before drawing to main context
    
        m_last_min_weight_ratio           = m_min_weight_ratio;
        m_last_count                      = count;
        m_cached_min_weight_ratio_overlay = bitmap;
    }

    context.DrawBitmap(m_cached_min_weight_ratio_overlay, 0, 0, size.x, size.y);
}

std::tuple<MixedFilamentRatioPanel::TriPoint, MixedFilamentRatioPanel::TriPoint, MixedFilamentRatioPanel::TriPoint>
MixedFilamentRatioPanel::triangle_vertices() const
{
    wxSize size   = GetClientSize();
    double margin = get_margin();

    double max_width  = std::max(1.0, size.x - margin * 2.0);
    double max_height = std::max(1.0, size.y - margin * 2.0);

    double side = std::min(max_width, max_height * 2.0 / std::sqrt(3.0));

    double trie_height = side * std::sqrt(3.0) / 2.0;

    double center_x = size.x / 2.0;
    double top_y    = (size.y - trie_height) / 2.0;
    double bottom_y = top_y + trie_height;

    TriPoint corner_0{center_x, top_y};
    TriPoint corner_1{center_x - side / 2.0, bottom_y};
    TriPoint corner_2{center_x + side / 2.0, bottom_y};

    return {corner_0, corner_1, corner_2};
}

std::tuple<MixedFilamentRatioPanel::TriPoint,
           MixedFilamentRatioPanel::TriPoint,
           MixedFilamentRatioPanel::TriPoint,
           MixedFilamentRatioPanel::TriPoint>
MixedFilamentRatioPanel::quad_vertices() const
{
    wxSize size   = GetClientSize();
    double margin = get_margin();

    // Fit a square inside the panel
    double side = std::min(size.x, size.y) - margin * 2;

    // Center it
    double left = (size.x - side) / 2.0;
    double top  = (size.y - side) / 2.0;

    return {
        TriPoint{left, top},              // Corner 0: Top-Left
        TriPoint{left + side, top},       // Corner 1: Top-Right
        TriPoint{left, top + side},       // Corner 2: Bottom-Left
        TriPoint{left + side, top + side} // Corner 3: Bottom-Right
    };
}



} // namespace Slic3r::GUI