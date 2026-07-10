#include "MFDRatioAccordion.hpp"

#include <wx/wx.h>
#include <wx/dcgraph.h>
#include <wx/dcmemory.h>
#include <cmath>
#include <algorithm>
#include <memory>

#include "I18N.hpp"
#include "GUI_App.hpp"
#include "Widgets/Label.hpp"
#include "MFDTheme.hpp"

namespace Slic3r::GUI {

// ---------------------------------------------------------------------------
// TriPoint static helpers (ported verbatim from MixedFilamentRatioPanel)
// ---------------------------------------------------------------------------

double TriPoint::signed_area2(TriPoint a, TriPoint b, TriPoint c)
{
    return ((b.x - a.x) * (c.y - a.y)) - ((c.x - a.x) * (b.y - a.y));
}

void TriPoint::barycentric(TriPoint p, TriPoint v0, TriPoint v1, TriPoint v2,
                            double& w0, double& w1, double& w2)
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
    if (sum > 0.0) { w0 /= sum; w1 /= sum; w2 /= sum; }
}

double TriPoint::dot(TriPoint a, TriPoint b) { return a.x * b.x + a.y * b.y; }

TriPoint TriPoint::project_to_segment(TriPoint p, TriPoint a, TriPoint b)
{
    TriPoint ab{b.x - a.x, b.y - a.y};
    double len2 = dot(ab, ab);
    if (len2 < 1e-12) return a;
    double t = dot(TriPoint{p.x - a.x, p.y - a.y}, ab) / len2;
    t = std::clamp(t, 0.0, 1.0);
    return TriPoint{a.x + ab.x * t, a.y + ab.y * t};
}

TriPoint TriPoint::project_to_triangle(TriPoint p, TriPoint a, TriPoint b, TriPoint c)
{
    double w0, w1, w2;
    barycentric(p, a, b, c, w0, w1, w2);
    if (w0 >= 0.0 && w1 >= 0.0 && w2 >= 0.0)
        return p;
    TriPoint p_ab = project_to_segment(p, a, b);
    TriPoint p_bc = project_to_segment(p, b, c);
    TriPoint p_ca = project_to_segment(p, c, a);
    auto dist2 = [&](const TriPoint& q) {
        double dx = q.x - p.x, dy = q.y - p.y;
        return dx*dx + dy*dy;
    };
    double d_ab = dist2(p_ab), d_bc = dist2(p_bc), d_ca = dist2(p_ca);
    if (d_ab <= d_bc && d_ab <= d_ca) return p_ab;
    if (d_bc <= d_ca) return p_bc;
    return p_ca;
}

// ---------------------------------------------------------------------------
// MFDRatioAccordion
// ---------------------------------------------------------------------------

MFDRatioAccordion::MFDRatioAccordion(wxWindow* parent,
                                     std::vector<double>&  filament_weights,
                                     std::vector<wxColor>& filament_colors,
                                     double&               min_weight_ratio)
    : Accordion(parent, _L("Select Ratio"))
    , m_filament_weights(filament_weights)
    , m_filament_colors(filament_colors)
    , m_min_weight_ratio(min_weight_ratio)
{
    build_ui();
}

void MFDRatioAccordion::build_ui()
{
    build_ratio_canvas();
    build_min_weight_row();
}

void MFDRatioAccordion::build_ratio_canvas()
{
    wxPanel*    body   = get_body_panel();
    wxBoxSizer* sizer  = get_body_sizer();

    // The canvas size is determined by update_sizing() after filament count is known.
    m_canvas = new wxPanel(body, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_canvas->SetBackgroundColour(MFDTheme::card_background());
    m_canvas->SetBackgroundStyle(wxBG_STYLE_PAINT);

    m_canvas->Bind(wxEVT_PAINT,       &MFDRatioAccordion::on_canvas_paint,      this);
    m_canvas->Bind(wxEVT_LEFT_DOWN,   &MFDRatioAccordion::on_canvas_left_down,  this);
    m_canvas->Bind(wxEVT_LEFT_UP,     &MFDRatioAccordion::on_canvas_left_up,    this);
    m_canvas->Bind(wxEVT_MOTION,      &MFDRatioAccordion::on_canvas_motion,     this);
    m_canvas->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
        m_dragging = false;
        m_canvas->Refresh();
    });

    sizer->Add(m_canvas, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
}

void MFDRatioAccordion::build_min_weight_row()
{
    wxPanel*    body   = get_body_panel();
    wxBoxSizer* sizer  = get_body_sizer();

    m_min_weight_panel = new wxPanel(body, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_min_weight_panel->SetBackgroundColour(MFDTheme::card_background());
    wxBoxSizer* row_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_min_weight_panel->SetSizer(row_sizer);

    wxStaticText* label = new wxStaticText(m_min_weight_panel, wxID_ANY, _L("Min Weight Ratio:"));
    label->SetFont(::Label::Body_14);
    MFDTheme::apply_text(label, MFDTheme::primary_text(), m_min_weight_panel->GetBackgroundColour());

    // Initial max is 50 (100 / 2 filaments); dynamically updated in update_sizing().
    m_min_weight_slider = new wxSlider(m_min_weight_panel, wxID_ANY,
        static_cast<int>(m_min_weight_ratio * 100), 0, 50);
    m_min_weight_slider->SetTickFreq(10);

    m_min_weight_value_input = new wxTextCtrl(m_min_weight_panel, wxID_ANY,
        wxString::Format("%d", static_cast<int>(m_min_weight_ratio * 100)));
    m_min_weight_value_input->SetFont(::Label::Body_14);
    m_min_weight_value_input->SetMinSize(wxSize(FromDIP(40), -1));
    MFDTheme::apply_input(m_min_weight_value_input);

    wxStaticText* pct_label = new wxStaticText(m_min_weight_panel, wxID_ANY, "%");
    pct_label->SetFont(::Label::Body_14);
    MFDTheme::apply_text(pct_label, MFDTheme::primary_text(), m_min_weight_panel->GetBackgroundColour());

    row_sizer->Add(label,                  0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    row_sizer->Add(m_min_weight_slider,    1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    row_sizer->Add(m_min_weight_value_input, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    row_sizer->Add(pct_label,              0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    m_min_weight_slider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
        int pct = m_min_weight_slider->GetValue();
        m_min_weight_value_input->SetValue(wxString::Format("%d", pct));
        // Fire the weights changed callback so the dialog can clamp weights
        // and update the preview.
        m_min_weight_ratio = pct / 100.0;
        if (m_on_weights_changed) m_on_weights_changed();
        if (m_canvas) m_canvas->Refresh();
    });

    m_min_weight_value_input->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
        long pct_long;
        wxString text = m_min_weight_value_input->GetValue();
        text.Replace("%", "");
        if (text.ToLong(&pct_long)) {
            int max_pct     = m_min_weight_slider->GetMax();
            int clamped_pct = std::clamp(static_cast<int>(pct_long), 0, max_pct);
            if (m_min_weight_slider->GetValue() != clamped_pct) {
                m_min_weight_slider->SetValue(clamped_pct);
                m_min_weight_value_input->SetValue(wxString::Format("%d", clamped_pct));
                m_min_weight_ratio = clamped_pct / 100.0;
                if (m_on_weights_changed) m_on_weights_changed();
                if (m_canvas) m_canvas->Refresh();
            }
        }
    });

    // On focus-loss or Enter, format the text field to ensure clean display.
    auto format_input = [this](wxEvent& event) {
        int val = m_min_weight_slider->GetValue();
        m_min_weight_value_input->ChangeValue(wxString::Format("%d", val));
        event.Skip();
    };
    m_min_weight_value_input->Bind(wxEVT_KILL_FOCUS, format_input);
    m_min_weight_value_input->Bind(wxEVT_TEXT_ENTER, format_input);

    sizer->Add(m_min_weight_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    update_sizing();
}

void MFDRatioAccordion::update_sizing()
{
    if (!m_canvas)
        return;

    // Update slider bounds based on current filament count
    const int count = static_cast<int>(m_filament_weights.size());
    if (m_min_weight_slider && count > 0) {
        int max_pct = static_cast<int>(std::floor(100.0 / count));
        m_min_weight_slider->SetRange(0, max_pct);
        int cur_pct = static_cast<int>(std::round(m_min_weight_ratio * 100.0));
        cur_pct = std::clamp(cur_pct, 0, max_pct);
        m_min_weight_slider->SetValue(cur_pct);
        if (m_min_weight_value_input)
            m_min_weight_value_input->SetValue(wxString::Format("%d", cur_pct));
    }

    if (count == 2) {
        // 2-filament: slim horizontal bar
        m_canvas->SetMinSize(wxSize(-1, FromDIP(60)));
        m_canvas->SetMaxSize(wxSize(-1, FromDIP(60)));
    } else {
        // 3- or 4-filament: square canvas
        m_canvas->SetMinSize(wxSize(FromDIP(220), FromDIP(220)));
        m_canvas->SetMaxSize(wxSize(-1, -1));
    }

    // Invalidate caches so they get recomputed on the next paint
    m_cached_background_3 = wxBitmap();
    m_cached_background_4 = wxBitmap();
    m_cached_min_weight_ratio_overlay = wxBitmap();
    m_last_colors.clear();
    m_last_min_weight_ratio = -1.0;
    m_last_count = -1;

    get_body_panel()->Layout();
    if (GetParent()) GetParent()->Layout();
}

// ---------------------------------------------------------------------------
// Mouse events
// ---------------------------------------------------------------------------

void MFDRatioAccordion::on_canvas_left_down(wxMouseEvent& event)
{
    m_dragging = true;
    if (!m_canvas->HasCapture()) m_canvas->CaptureMouse();
    update_weights_from_mouse(event.GetX(), event.GetY());
}

void MFDRatioAccordion::on_canvas_left_up(wxMouseEvent&)
{
    if (!m_dragging) return;
    m_dragging = false;
    if (m_canvas->HasCapture()) m_canvas->ReleaseMouse();
    m_canvas->Refresh();
}

void MFDRatioAccordion::on_canvas_motion(wxMouseEvent& event)
{
    if (!m_dragging) return;
    update_weights_from_mouse(event.GetX(), event.GetY());
}

void MFDRatioAccordion::update_weights_from_mouse(int x, int y)
{
    if (m_filament_weights.empty()) return;
    const int count = static_cast<int>(m_filament_weights.size());
    switch (count) {
    case 2: update_weights_from_mouse_2(x); break;
    case 3: update_weights_from_mouse_3(x, y); break;
    case 4: update_weights_from_mouse_4(x, y); break;
    default: break;
    }
    if (m_on_weights_changed) m_on_weights_changed();
}

void MFDRatioAccordion::update_weights_from_mouse_2(int x)
{
    double margin = get_margin();
    int    width  = std::max(1.0, m_canvas->GetClientSize().x - margin * 2);
    double adj_x  = x - margin;
    double w1 = adj_x / width;
    double w0 = 1.0 - w1;
    m_filament_weights = {w0, w1};
    clamp_weights_2(m_filament_weights, m_min_weight_ratio);
    m_canvas->Refresh(false);
}

void MFDRatioAccordion::update_weights_from_mouse_3(int x, int y)
{
    auto [c0, c1, c2] = triangle_vertices();
    auto inner_pt = [&](double bw0, double bw1, double bw2) {
        return TriPoint{bw0*c0.x + bw1*c1.x + bw2*c2.x, bw0*c0.y + bw1*c1.y + bw2*c2.y};
    };
    double m = m_min_weight_ratio;
    TriPoint ci0 = inner_pt(1.0-2.0*m, m, m);
    TriPoint ci1 = inner_pt(m, 1.0-2.0*m, m);
    TriPoint ci2 = inner_pt(m, m, 1.0-2.0*m);
    TriPoint mouse{(double)x, (double)y};
    mouse = TriPoint::project_to_triangle(mouse, ci0, ci1, ci2);
    double w0, w1, w2;
    TriPoint::barycentric(mouse, c0, c1, c2, w0, w1, w2);
    m_filament_weights = {w0, w1, w2};
    m_canvas->Refresh(false);
}

void MFDRatioAccordion::update_weights_from_mouse_4(int x, int y)
{
    auto [c0, c1, c2, c3] = quad_vertices();
    double side = c1.x - c0.x;
    if (side <= 0.0) return;
    TriPoint mouse{(double)x, (double)y};
    mouse.x = std::clamp(mouse.x, c0.x, c1.x);
    mouse.y = std::clamp(mouse.y, c0.y, c2.y);
    double u = (mouse.x - c0.x) / side;
    double v = (mouse.y - c0.y) / side;
    m_filament_weights = {(1.0-u)*(1.0-v), u*(1.0-v), (1.0-u)*v, u*v};
    clamp_weights_4(m_filament_weights, m_min_weight_ratio);
    m_canvas->Refresh(false);
}

// ---------------------------------------------------------------------------
// Static clamp helpers
// ---------------------------------------------------------------------------

void MFDRatioAccordion::clamp_weights_2(std::vector<double>& w, double m)
{
    if (w.size() != 2) return;
    double u = std::clamp(w[1], m, 1.0 - m);
    w[0] = 1.0 - u;
    w[1] = u;
}

void MFDRatioAccordion::clamp_weights_3(std::vector<double>& w, double m)
{
    if (w.size() != 3) return;
    bool recalc = false;
    for (double weight : w) { if (weight < m) { recalc = true; break; } }
    if (!recalc) return;
    std::vector<double> diffs(3, 0.0);
    double diff_sum = 0.0;
    for (int i = 0; i < 3; ++i) {
        double d = std::max(0.0, w[i] - m);
        diffs[i] = d;
        diff_sum += d;
    }
    if (diff_sum > 1e-9) {
        for (int i = 0; i < 3; ++i)
            w[i] = m + (1.0 - 3.0*m) * (diffs[i] / diff_sum);
    } else {
        for (int i = 0; i < 3; ++i) w[i] = 1.0/3.0;
    }
}

void MFDRatioAccordion::clamp_weights_4(std::vector<double>& w, double m)
{
    if (w.size() != 4) return;
    double u = w[1] + w[3];
    double v = w[2] + w[3];
    if (m > 0.0) {
        if (m >= 0.25) { u = v = 0.5; }
        else {
            double w0 = (1.0-u)*(1.0-v);
            double w1 = u*(1.0-v);
            double w2 = (1.0-u)*v;
            double w3 = u*v;
            bool inside = (w0>=m && w1>=m && w2>=m && w3>=m);
            if (!inside) {
                struct LocalCurve { double u_start, u_end; int type; };
                auto get_v = [](int type, double uv, double mv) -> double {
                    switch(type) {
                    case 1: return mv/uv;
                    case 2: return mv/(1.0-uv);
                    case 3: return 1.0 - mv/uv;
                    case 4: return 1.0 - mv/(1.0-uv);
                    default: return 0.0;
                    }
                };
                auto closest = [&](double um, double vm, const LocalCurve& curve) -> TriPoint {
                    double l = curve.u_start, r = curve.u_end;
                    for (int iter = 0; iter < 15; ++iter) {
                        double m1 = l + (r-l)/3.0, m2 = r - (r-l)/3.0;
                        double v1 = get_v(curve.type, m1, m);
                        double v2 = get_v(curve.type, m2, m);
                        double d1 = (m1-um)*(m1-um)+(v1-vm)*(v1-vm);
                        double d2 = (m2-um)*(m2-um)+(v2-vm)*(v2-vm);
                        if (d1 < d2) r = m2; else l = m1;
                    }
                    double ub = (l+r)*0.5;
                    return TriPoint{ub, get_v(curve.type, ub, m)};
                };
                LocalCurve curves[4] = {
                    {2.0*m, 0.5, 1}, {0.5, 1.0-2.0*m, 2},
                    {2.0*m, 0.5, 3}, {0.5, 1.0-2.0*m, 4}
                };
                TriPoint best = closest(u, v, curves[0]);
                double best_d = (best.x-u)*(best.x-u)+(best.y-v)*(best.y-v);
                for (int i = 1; i < 4; ++i) {
                    TriPoint pt = closest(u, v, curves[i]);
                    double d = (pt.x-u)*(pt.x-u)+(pt.y-v)*(pt.y-v);
                    if (d < best_d) { best_d = d; best = pt; }
                }
                u = best.x; v = best.y;
            }
        }
    } else {
        u = std::clamp(u, 0.0, 1.0);
        v = std::clamp(v, 0.0, 1.0);
    }
    w[0] = (1.0-u)*(1.0-v);
    w[1] = u*(1.0-v);
    w[2] = (1.0-u)*v;
    w[3] = u*v;
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void MFDRatioAccordion::on_canvas_paint(wxPaintEvent&)
{
    if (m_filament_weights.empty() || m_filament_colors.empty()) return;
    wxAutoBufferedPaintDC dc(m_canvas);
    dc.SetBackground(wxBrush(get_background_color()));
    dc.Clear();
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;
    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
    wxSize size = m_canvas->GetClientSize();
    if (size.x <= 0 || size.y <= 0) return;
    const int count = static_cast<int>(m_filament_weights.size());
    if (count == 2) {
        paint_2(*gc, size);
        paint_min_ratio_overlay(*gc, size);
        paint_2_handle(*gc, size);
    } else if (count == 3) {
        paint_3(*gc, size);
        paint_min_ratio_overlay(*gc, size);
        paint_3_handle(*gc, size);
    } else if (count == 4) {
        paint_4(*gc, size);
        paint_min_ratio_overlay(*gc, size);
        paint_4_handle(*gc, size);
    }
}

void MFDRatioAccordion::paint_2(wxGraphicsContext& gc, const wxSize& size)
{
    auto& colors = m_filament_colors;
    auto& weights = m_filament_weights;
    if (colors.size() != 2 || colors.size() != weights.size()) return;
    const double m = get_margin();
    const double bw = get_border_width();
    const double sx = m, sy = m;
    const double w  = std::max(1.0, size.x - m*2.0);
    const double h  = std::max(1.0, size.y - m*2.0);
    gc.SetBrush(wxBrush(get_background_color()));
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.DrawRectangle(0, 0, w, h);
    wxGraphicsBrush gb = gc.CreateLinearGradientBrush(sx, sy, sx+w, sy, colors[0], colors[1]);
    gc.SetBrush(gb);
    gc.DrawRectangle(sx, sy, w, h);
    gc.SetPen(wxPen(get_border_color(), bw));
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    gc.DrawRectangle(sx, sy, w, h);
}

void MFDRatioAccordion::paint_2_handle(wxGraphicsContext& gc, const wxSize& size)
{
    auto& colors = m_filament_colors;
    auto& weights = m_filament_weights;
    if (colors.size() != 2 || colors.size() != weights.size()) return;
    const double bw = get_border_width();
    const double hr = get_handle_radius();
    const double m  = get_margin();
    const double sx = m, sy = m;
    const double w  = std::max(1.0, size.x - m*2.0);
    const double h  = std::max(1.0, size.y - m*2.0);
    const double hx = sx + weights[1] * w;
    double t = weights[1];
    wxColour hc((unsigned char)(colors[0].Red()*(1-t)+colors[1].Red()*t),
                (unsigned char)(colors[0].Green()*(1-t)+colors[1].Green()*t),
                (unsigned char)(colors[0].Blue()*(1-t)+colors[1].Blue()*t));
    gc.SetPen(wxPen(get_contrast_border_color(hc), bw));
    gc.StrokeLine(hx, sy, hx, sy+h);
    gc.SetBrush(wxBrush(hc));
    gc.SetPen(wxPen(get_contrast_border_color(hc), bw));
    gc.DrawEllipse(hx-hr, (sy+h/2.0)-hr, hr*2.0, hr*2.0);
}

void MFDRatioAccordion::paint_3(wxGraphicsContext& gc, const wxSize& size)
{
    auto& colors = m_filament_colors;
    auto& weights = m_filament_weights;
    if (colors.size() != 3 || weights.size() != 3) return;
    const double bw = get_border_width();
    const auto [c0, c1, c2] = triangle_vertices();
    if (!m_cached_background_3.IsOk() || m_cached_background_3.GetSize() != size || m_last_colors != colors) {
        wxImage img(size.x, size.y, false);
        wxColour bg = get_background_color();
        for (int y = 0; y < size.y; ++y)
            for (int x = 0; x < size.x; ++x)
                img.SetRGB(x, y, bg.Red(), bg.Green(), bg.Blue());
        const int mnx = std::max(0,(int)std::floor(std::min({c0.x,c1.x,c2.x})));
        const int mxx = std::min(size.x-1,(int)std::ceil(std::max({c0.x,c1.x,c2.x})));
        const int mny = std::max(0,(int)std::floor(std::min({c0.y,c1.y,c2.y})));
        const int mxy = std::min(size.y-1,(int)std::ceil(std::max({c0.y,c1.y,c2.y})));
        for (int y = mny; y <= mxy; ++y) {
            for (int x = mnx; x <= mxx; ++x) {
                TriPoint p{x+0.5, y+0.5};
                double w0,w1,w2;
                TriPoint::barycentric(p,c0,c1,c2,w0,w1,w2);
                if (w0<-1e-6||w1<-1e-6||w2<-1e-6) continue;
                img.SetRGB(x,y,
                    (int)std::clamp(w0*colors[0].Red()+w1*colors[1].Red()+w2*colors[2].Red(),0.0,255.0),
                    (int)std::clamp(w0*colors[0].Green()+w1*colors[1].Green()+w2*colors[2].Green(),0.0,255.0),
                    (int)std::clamp(w0*colors[0].Blue()+w1*colors[1].Blue()+w2*colors[2].Blue(),0.0,255.0));
            }
        }
        m_cached_background_3 = wxBitmap(img);
        m_last_colors = colors;
    }
    gc.DrawBitmap(m_cached_background_3, 0, 0, size.x, size.y);
    gc.SetPen(wxPen(get_border_color(), bw));
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    wxGraphicsPath path = gc.CreatePath();
    path.MoveToPoint(c0.x,c0.y); path.AddLineToPoint(c1.x,c1.y);
    path.AddLineToPoint(c2.x,c2.y); path.AddLineToPoint(c0.x,c0.y);
    path.CloseSubpath();
    gc.StrokePath(path);
}

void MFDRatioAccordion::paint_3_handle(wxGraphicsContext& gc, const wxSize&)
{
    auto& colors = m_filament_colors;
    auto& weights = m_filament_weights;
    if (colors.size()!=3||weights.size()!=3) return;
    const double bw = get_border_width();
    const double hr = get_handle_radius();
    const auto [c0,c1,c2] = triangle_vertices();
    const double px = weights[0]*c0.x+weights[1]*c1.x+weights[2]*c2.x;
    const double py = weights[0]*c0.y+weights[1]*c1.y+weights[2]*c2.y;
    wxColour hc(
        (unsigned char)std::clamp(weights[0]*colors[0].Red()+weights[1]*colors[1].Red()+weights[2]*colors[2].Red(),0.0,255.0),
        (unsigned char)std::clamp(weights[0]*colors[0].Green()+weights[1]*colors[1].Green()+weights[2]*colors[2].Green(),0.0,255.0),
        (unsigned char)std::clamp(weights[0]*colors[0].Blue()+weights[1]*colors[1].Blue()+weights[2]*colors[2].Blue(),0.0,255.0));
    gc.SetBrush(wxBrush(hc));
    gc.SetPen(wxPen(get_contrast_border_color(hc),bw));
    gc.DrawEllipse(px-hr,py-hr,hr*2.0,hr*2.0);
}

void MFDRatioAccordion::paint_4(wxGraphicsContext& gc, const wxSize& size)
{
    auto& colors = m_filament_colors;
    auto& weights = m_filament_weights;
    if (colors.size()!=4||weights.size()!=4) return;
    const double bw = get_border_width();
    const auto [c0,c1,c2,c3] = quad_vertices();
    const double side = c1.x - c0.x;
    if (side<=0.0) return;
    if (!m_cached_background_4.IsOk()||m_cached_background_4.GetSize()!=size||m_last_colors!=colors) {
        wxImage img(size.x,size.y,false);
        wxColour bg = get_background_color();
        for(int y=0;y<size.y;++y) for(int x=0;x<size.x;++x) img.SetRGB(x,y,bg.Red(),bg.Green(),bg.Blue());
        const int mnx=std::max(0,(int)std::floor(c0.x)),mxx=std::min(size.x-1,(int)std::ceil(c1.x));
        const int mny=std::max(0,(int)std::floor(c0.y)),mxy=std::min(size.y-1,(int)std::ceil(c2.y));
        for(int y=mny;y<=mxy;++y) {
            for(int x=mnx;x<=mxx;++x) {
                double u=((double)x+0.5-c0.x)/side, v=((double)y+0.5-c0.y)/side;
                if(u<0||u>1||v<0||v>1) continue;
                double w0=(1-u)*(1-v),w1=u*(1-v),w2=(1-u)*v,w3=u*v;
                img.SetRGB(x,y,
                    (int)std::clamp(w0*colors[0].Red()+w1*colors[1].Red()+w2*colors[2].Red()+w3*colors[3].Red(),0.0,255.0),
                    (int)std::clamp(w0*colors[0].Green()+w1*colors[1].Green()+w2*colors[2].Green()+w3*colors[3].Green(),0.0,255.0),
                    (int)std::clamp(w0*colors[0].Blue()+w1*colors[1].Blue()+w2*colors[2].Blue()+w3*colors[3].Blue(),0.0,255.0));
            }
        }
        m_cached_background_4 = wxBitmap(img);
        m_last_colors = colors;
    }
    gc.DrawBitmap(m_cached_background_4,0,0,size.x,size.y);
    gc.SetPen(wxPen(get_border_color(),bw));
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    gc.DrawRectangle(c0.x,c0.y,side,side);
}

void MFDRatioAccordion::paint_4_handle(wxGraphicsContext& gc, const wxSize&)
{
    auto& colors = m_filament_colors;
    auto& weights = m_filament_weights;
    if (colors.size()!=4||weights.size()!=4) return;
    const double bw=get_border_width();
    const double hr=get_handle_radius();
    const auto [c0,c1,c2,c3]=quad_vertices();
    const double side=c1.x-c0.x;
    if(side<=0.0) return;
    const double u=weights[1]+weights[3], v=weights[2]+weights[3];
    const double hx=c0.x+u*side, hy=c0.y+v*side;
    wxColour hc(
        (unsigned char)std::clamp(weights[0]*colors[0].Red()+weights[1]*colors[1].Red()+weights[2]*colors[2].Red()+weights[3]*colors[3].Red(),0.0,255.0),
        (unsigned char)std::clamp(weights[0]*colors[0].Green()+weights[1]*colors[1].Green()+weights[2]*colors[2].Green()+weights[3]*colors[3].Green(),0.0,255.0),
        (unsigned char)std::clamp(weights[0]*colors[0].Blue()+weights[1]*colors[1].Blue()+weights[2]*colors[2].Blue()+weights[3]*colors[3].Blue(),0.0,255.0));
    gc.SetBrush(wxBrush(hc));
    gc.SetPen(wxPen(get_contrast_border_color(hc),bw));
    gc.DrawEllipse(hx-hr,hy-hr,hr*2.0,hr*2.0);
}

void MFDRatioAccordion::paint_min_ratio_overlay(wxGraphicsContext& gc, const wxSize& size)
{
    const int count = static_cast<int>(m_filament_weights.size());
    if (count < 2 || m_min_weight_ratio <= 0.0) return;

    bool needs_update = !m_cached_min_weight_ratio_overlay.IsOk()
        || m_cached_min_weight_ratio_overlay.GetSize() != size
        || m_last_min_weight_ratio != m_min_weight_ratio
        || m_last_count != count;

    if (needs_update) {
        wxBitmap bmp(size.x, size.y, 32);
        bmp.UseAlpha();
        wxMemoryDC mdc;
        mdc.SelectObject(bmp);
        mdc.SetBackground(*wxTRANSPARENT_BRUSH);
        mdc.Clear();
        std::unique_ptr<wxGraphicsContext> mctx(wxGraphicsContext::Create(mdc));
        if (!mctx) return;
        wxColor bg = m_canvas->GetBackgroundColour();
        mctx->SetBrush(wxBrush(wxColor(bg.Red(),bg.Green(),bg.Blue(),128)));
        wxDash dashes[2]={4,4};
        wxPen pen(*wxWHITE,1,wxPENSTYLE_USER_DASH);
        pen.SetDashes(2,dashes);
        mctx->SetPen(pen);
        wxGraphicsPath fp=mctx->CreatePath(), ip=mctx->CreatePath();
        double m=m_min_weight_ratio;
        if (count==2) {
            double mg=get_margin();
            double w=std::max(1.0,size.x-mg*2), h=std::max(1.0,size.y-mg*2);
            fp.AddRectangle(mg,mg,w,h);
            ip.AddRectangle(mg+w*m,mg,w*(1-2*m),h);
            fp.AddPath(ip);
            mctx->FillPath(fp,wxODDEVEN_RULE);
            mctx->StrokeLine(mg+w*m,mg,mg+w*m,mg+h);
            mctx->StrokeLine(mg+w*(1-m),mg,mg+w*(1-m),mg+h);
        } else if (count==3) {
            auto [c0,c1,c2]=triangle_vertices();
            fp.MoveToPoint(c0.x,c0.y); fp.AddLineToPoint(c1.x,c1.y);
            fp.AddLineToPoint(c2.x,c2.y); fp.CloseSubpath();
            auto ipt=[&](double bw0,double bw1,double bw2) {
                return wxPoint2DDouble(bw0*c0.x+bw1*c1.x+bw2*c2.x, bw0*c0.y+bw1*c1.y+bw2*c2.y);
            };
            ip.MoveToPoint(ipt(1-2*m,m,m)); ip.AddLineToPoint(ipt(m,1-2*m,m));
            ip.AddLineToPoint(ipt(m,m,1-2*m)); ip.CloseSubpath();
            fp.AddPath(ip);
            mctx->FillPath(fp,wxODDEVEN_RULE);
            mctx->StrokePath(ip);
        } else if (count==4) {
            auto [c0,c1,c2,c3]=quad_vertices();
            double side=c1.x-c0.x;
            fp.AddRectangle(c0.x,c0.y,side,side);
            auto get_v_min=[m](double u){return std::max(m/std::max(u,1e-5),m/std::max(1-u,1e-5));};
            auto get_v_max=[m](double u){return std::min(1-m/std::max(u,1e-5),1-m/std::max(1-u,1e-5));};
            std::vector<wxPoint2DDouble> top_pts,bot_pts;
            const int steps=16;
            for(int i=0;i<=steps;++i) {
                double u=2*m+(1-4*m)*(i/(double)steps);
                top_pts.push_back({c0.x+u*side,c0.y+get_v_min(u)*side});
                bot_pts.push_back({c0.x+u*side,c0.y+get_v_max(u)*side});
            }
            if(!top_pts.empty()) {
                ip.MoveToPoint(top_pts.front());
                for(auto& pt:top_pts) ip.AddLineToPoint(pt);
                for(auto it=bot_pts.rbegin();it!=bot_pts.rend();++it) ip.AddLineToPoint(*it);
                ip.CloseSubpath();
            }
            fp.AddPath(ip);
            mctx->FillPath(fp,wxODDEVEN_RULE);
            mctx->StrokePath(ip);
        }
        mdc.SelectObject(wxNullBitmap);
        m_last_min_weight_ratio = m_min_weight_ratio;
        m_last_count = count;
        m_cached_min_weight_ratio_overlay = bmp;
    }
    gc.DrawBitmap(m_cached_min_weight_ratio_overlay, 0, 0, size.x, size.y);
}

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------

std::tuple<TriPoint,TriPoint,TriPoint> MFDRatioAccordion::triangle_vertices() const
{
    wxSize size = m_canvas->GetClientSize();
    double mg = get_margin();
    double max_w = std::max(1.0, size.x - mg*2);
    double max_h = std::max(1.0, size.y - mg*2);
    double side = std::min(max_w, max_h * 2.0 / std::sqrt(3.0));
    double tri_h = side * std::sqrt(3.0) / 2.0;
    double cx = size.x / 2.0;
    double top_y = (size.y - tri_h) / 2.0;
    double bot_y = top_y + tri_h;
    return {TriPoint{cx, top_y}, TriPoint{cx - side/2.0, bot_y}, TriPoint{cx + side/2.0, bot_y}};
}

std::tuple<TriPoint,TriPoint,TriPoint,TriPoint> MFDRatioAccordion::quad_vertices() const
{
    wxSize size = m_canvas->GetClientSize();
    double mg = get_margin();
    double side = std::min(size.x, size.y) - mg*2;
    double left = (size.x - side) / 2.0;
    double top  = (size.y - side) / 2.0;
    return {
        TriPoint{left, top},
        TriPoint{left+side, top},
        TriPoint{left, top+side},
        TriPoint{left+side, top+side}
    };
}

// ---------------------------------------------------------------------------
// Color helpers
// ---------------------------------------------------------------------------

wxColour MFDRatioAccordion::get_contrast_border_color(const wxColour& bg) const
{
    double luma = (0.299*bg.Red() + 0.587*bg.Green() + 0.114*bg.Blue()) / 255.0;
    if (luma <= 0.75) return *wxWHITE;
    if (luma >= 0.90) return wxColour(0x4F,0x4F,0x4F);
    double t = (luma - 0.75) / 0.15;
    auto c = (unsigned char)(255 - t*(255-0x4F));
    return wxColour(c, c, c);
}

wxColour MFDRatioAccordion::get_border_color() const
{
    return MFDTheme::input_border();
}

wxColour MFDRatioAccordion::get_background_color() const
{
    return MFDTheme::card_background();
}

} // namespace Slic3r::GUI
