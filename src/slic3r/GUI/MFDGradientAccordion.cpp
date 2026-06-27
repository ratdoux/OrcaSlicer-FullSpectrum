#include "MFDGradientAccordion.hpp"

#include <wx/wx.h>
#include <wx/dcgraph.h>
#include <wx/dcmemory.h>
#include <cmath>
#include <algorithm>
#include <memory>

#include "I18N.hpp"
#include "GUI_App.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/FilamentCardMixed.hpp"

namespace Slic3r::GUI {

MFDGradientAccordion::MFDGradientAccordion(
    wxWindow* parent,
    std::vector<int>& selected_filaments,
    std::vector<wxColor>& filament_colors,
    std::vector<double>& gradient_positions,
    double& min_ratio,
    const std::vector<std::pair<std::string, std::string>>& physical_filaments
)
    : Accordion(parent, _L("Linear Gradient"))
    , m_selected_filaments(selected_filaments)
    , m_filament_colors(filament_colors)
    , m_gradient_positions(gradient_positions)
    , m_min_ratio(min_ratio)
    , m_physical_filaments(physical_filaments)
{
    build_ui();
}

void MFDGradientAccordion::build_ui()
{
    build_canvas();
    build_edit_row();
    build_min_ratio_row();
}

void MFDGradientAccordion::build_canvas()
{
    wxPanel* body = get_body_panel();
    wxBoxSizer* sizer = get_body_sizer();

    m_canvas = new wxPanel(body, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_canvas->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    m_canvas->SetBackgroundStyle(wxBG_STYLE_PAINT);

    m_canvas->Bind(wxEVT_PAINT,       &MFDGradientAccordion::on_canvas_paint,      this);
    m_canvas->Bind(wxEVT_LEFT_DOWN,   &MFDGradientAccordion::on_canvas_left_down,  this);
    m_canvas->Bind(wxEVT_LEFT_UP,     &MFDGradientAccordion::on_canvas_left_up,    this);
    m_canvas->Bind(wxEVT_MOTION,      &MFDGradientAccordion::on_canvas_motion,     this);
    m_canvas->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
        m_dragging = false;
        m_canvas->Refresh();
    });

    sizer->Add(m_canvas, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
}

void MFDGradientAccordion::build_edit_row()
{
    wxPanel* body = get_body_panel();
    wxBoxSizer* sizer = get_body_sizer();

    m_edit_panel = new wxPanel(body, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    wxBoxSizer* panel_sizer = new wxBoxSizer(wxVERTICAL);
    m_edit_panel->SetSizer(panel_sizer);

    const wxSize combobox_size(FromDIP(166), FromDIP(30));
    m_filament_combo = new ComboBox(m_edit_panel, wxID_ANY, wxEmptyString,
        wxDefaultPosition, combobox_size, 0, nullptr, wxCB_READONLY);
    m_filament_combo->SetMinSize(combobox_size);
    m_filament_combo->SetKeepDropArrow(true);

    // Populate combobox items from physical filaments
    for (size_t i = 0; i < m_physical_filaments.size(); ++i) {
        const auto& [color_hex, name] = m_physical_filaments[i];
        wxColor  color(color_hex);
        wxString index = wxString::Format("%zu", i + 1);

        int swatch_sz = FromDIP(20);
        wxBitmap   bmp(swatch_sz, swatch_sz);
        wxMemoryDC dc(bmp);
        FilamentCardMixed::paint_clr_swatch(dc, wxSize(swatch_sz, swatch_sz), color, index, wxGetApp().dark_mode());

        m_filament_combo->Append(wxString(name), bmp);
    }

    wxBoxSizer* row_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_pos_label = new wxStaticText(m_edit_panel, wxID_ANY, _L("Position:"));
    m_pos_label->SetFont(::Label::Body_14);

    m_pos_input = new wxTextCtrl(m_edit_panel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    m_pos_input->SetFont(::Label::Body_14);
    m_pos_input->SetMinSize(wxSize(FromDIP(40), -1));

    m_pct_label = new wxStaticText(m_edit_panel, wxID_ANY, "%");
    m_pct_label->SetFont(::Label::Body_14);

    row_sizer->Add(m_pos_label,       0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    row_sizer->Add(m_pos_input,       0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    row_sizer->Add(m_pct_label,       0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    panel_sizer->Add(m_filament_combo, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    panel_sizer->Add(row_sizer, 0, wxEXPAND);

    // Binds
    m_filament_combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
        int sel = m_filament_combo->GetSelection();
        if (sel != wxNOT_FOUND) {
            int stop_idx = m_selected_stop_index;
            if (stop_idx % 2 == 0) { // Filament
                size_t filament_slot = stop_idx / 2;
                if (m_on_filament_changed) {
                    m_on_filament_changed(filament_slot, sel);
                }
            }
        }
    });

    auto on_enter_or_kill_focus = [this](wxEvent& event) {
        apply_text_input_change();
        event.Skip();
    };
    m_pos_input->Bind(wxEVT_TEXT_ENTER, on_enter_or_kill_focus);
    m_pos_input->Bind(wxEVT_KILL_FOCUS, on_enter_or_kill_focus);

    sizer->Add(m_edit_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
}

void MFDGradientAccordion::build_min_ratio_row()
{
    wxPanel* body = get_body_panel();
    wxBoxSizer* sizer = get_body_sizer();

    m_min_ratio_panel = new wxPanel(body, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    wxBoxSizer* row_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_min_ratio_panel->SetSizer(row_sizer);

    wxStaticText* label = new wxStaticText(m_min_ratio_panel, wxID_ANY, _L("Min Stop Gap:"));
    label->SetFont(::Label::Body_14);

    m_min_ratio_slider = new wxSlider(m_min_ratio_panel, wxID_ANY, static_cast<int>(m_min_ratio * 100), 0, 50);
    m_min_ratio_slider->SetTickFreq(10);

    m_min_ratio_value_input = new wxTextCtrl(m_min_ratio_panel, wxID_ANY, wxString::Format("%d", static_cast<int>(m_min_ratio * 100)), wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    m_min_ratio_value_input->SetFont(::Label::Body_14);
    m_min_ratio_value_input->SetMinSize(wxSize(FromDIP(40), -1));

    wxStaticText* pct_label = new wxStaticText(m_min_ratio_panel, wxID_ANY, "%");
    pct_label->SetFont(::Label::Body_14);

    row_sizer->Add(label,                   0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    row_sizer->Add(m_min_ratio_slider,      1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    row_sizer->Add(m_min_ratio_value_input, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    row_sizer->Add(pct_label,               0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    m_min_ratio_slider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
        int pct = m_min_ratio_slider->GetValue();
        m_min_ratio_value_input->SetValue(wxString::Format("%d", pct));
        m_min_ratio = pct / 100.0;
        clamp_all_stops();
        if (m_on_changed) m_on_changed();
        m_canvas->Refresh();
        sync_edit_panel();
    });

    auto apply_min_ratio_text = [this](wxEvent& event) {
        long pct_long;
        if (m_min_ratio_value_input->GetValue().ToLong(&pct_long)) {
            int clamped_pct = std::clamp(static_cast<int>(pct_long), 0, m_min_ratio_slider->GetMax());
            m_min_ratio_slider->SetValue(clamped_pct);
            m_min_ratio_value_input->ChangeValue(wxString::Format("%d", clamped_pct));
            m_min_ratio = clamped_pct / 100.0;
            clamp_all_stops();
            if (m_on_changed) m_on_changed();
            m_canvas->Refresh();
            sync_edit_panel();
        }
        event.Skip();
    };
    m_min_ratio_value_input->Bind(wxEVT_TEXT_ENTER, apply_min_ratio_text);
    m_min_ratio_value_input->Bind(wxEVT_KILL_FOCUS, apply_min_ratio_text);

    sizer->Add(m_min_ratio_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    update_sizing();
}

void MFDGradientAccordion::update_sizing()
{
    if (!m_canvas)
        return;

    const int count = static_cast<int>(m_filament_colors.size());
    const int num_stops = 2 * count - 1;

    if (m_min_ratio_slider && count > 0) {
        int max_pct = static_cast<int>(std::floor(100.0 / (num_stops - 1)));
        m_min_ratio_slider->SetRange(0, max_pct);
        int cur_pct = static_cast<int>(std::round(m_min_ratio * 100.0));
        cur_pct = std::clamp(cur_pct, 0, max_pct);
        m_min_ratio_slider->SetValue(cur_pct);
        m_min_ratio = cur_pct / 100.0;
        if (m_min_ratio_value_input)
            m_min_ratio_value_input->SetValue(wxString::Format("%d", cur_pct));
    }

    m_canvas->SetMinSize(wxSize(-1, FromDIP(60)));
    m_canvas->SetMaxSize(wxSize(-1, FromDIP(60)));

    if (count != m_last_count) {
        reset_points_to_defaults(count);
        m_selected_stop_index = 0; // Default to first filament
        m_last_count = count;
    }

    clamp_all_stops();
    sync_edit_panel();

    get_body_panel()->Layout();
    if (GetParent()) GetParent()->Layout();
}

void MFDGradientAccordion::sync_data()
{
    clamp_all_stops();
    sync_edit_panel();
    m_canvas->Refresh();
}

void MFDGradientAccordion::reset_points_to_defaults(int count)
{
    m_gradient_positions.clear();
    if (count < 2) return;
    int num_intervals = 2 * count - 2;
    for (int i = 0; i <= num_intervals; ++i) {
        m_gradient_positions.push_back(static_cast<double>(i) / num_intervals);
    }
}

void MFDGradientAccordion::clamp_all_stops()
{
    const int count = static_cast<int>(m_filament_colors.size());
    const int num_stops = 2 * count - 1;
    if (m_gradient_positions.size() != num_stops) {
        reset_points_to_defaults(count);
    }

    double L = m_min_ratio;

    // Forward pass
    m_gradient_positions[0] = 0.0;
    for (int i = 1; i < num_stops; ++i) {
        m_gradient_positions[i] = std::max(m_gradient_positions[i], m_gradient_positions[i - 1] + L);
    }

    // Backward pass
    m_gradient_positions[num_stops - 1] = 1.0;
    for (int i = num_stops - 2; i >= 0; --i) {
        m_gradient_positions[i] = std::min(m_gradient_positions[i], m_gradient_positions[i + 1] - L);
    }

    // Final boundary validation
    if (m_gradient_positions[0] < 0.0) {
        m_gradient_positions[0] = 0.0;
        for (int i = 1; i < num_stops; ++i) {
            m_gradient_positions[i] = std::max(m_gradient_positions[i], m_gradient_positions[i - 1] + L);
        }
    }
}

void MFDGradientAccordion::sync_edit_panel()
{
    if (!m_edit_panel) return;

    const int count = static_cast<int>(m_filament_colors.size());
    int stop_idx = m_selected_stop_index;

    if (stop_idx < 0 || stop_idx >= (int)m_gradient_positions.size()) {
        stop_idx = 0;
        m_selected_stop_index = 0;
    }

    refresh_combobox_items();

    if (stop_idx % 2 == 0) {
        // Selected a Filament
        size_t filament_slot = stop_idx / 2;
        m_filament_combo->Show(true);

        if (filament_slot < m_selected_filaments.size()) {
            m_filament_combo->SetSelection(m_selected_filaments[filament_slot]);
        }

        m_pos_label->SetLabel(_L("Position:"));
        m_pos_input->Enable(filament_slot > 0 && filament_slot < m_selected_filaments.size() - 1);
    } else {
        // Selected a Midpoint
        m_filament_combo->Show(false);

        m_pos_label->SetLabel(_L("Position (relative):"));
        m_pos_input->Enable(true);
    }

    update_percentage_input();
    m_edit_panel->Layout();
    get_body_panel()->Layout();
    if (GetParent()) GetParent()->Layout();
}

void MFDGradientAccordion::update_percentage_input()
{
    int stop_idx = m_selected_stop_index;
    if (stop_idx % 2 == 0) {
        // Filament: global percentage
        double global_pos = m_gradient_positions[stop_idx];
        m_pos_input->ChangeValue(wxString::Format("%.1f", global_pos * 100.0));
    } else {
        // Midpoint: relative percentage in between adjacent filaments
        double p_left = m_gradient_positions[stop_idx - 1];
        double p_mid  = m_gradient_positions[stop_idx];
        double p_right = m_gradient_positions[stop_idx + 1];
        double range = p_right - p_left;
        double relative_pos = (range > 1e-6) ? (p_mid - p_left) / range : 0.5;
        m_pos_input->ChangeValue(wxString::Format("%.1f", relative_pos * 100.0));
    }
}

void MFDGradientAccordion::apply_text_input_change()
{
    double val;
    if (m_pos_input->GetValue().ToDouble(&val)) {
        double pct = val / 100.0;
        int stop_idx = m_selected_stop_index;
        double L = m_min_ratio;

        if (stop_idx % 2 == 0) {
            // Filament: update global position and adjust midpoints to maintain relative positioning
            size_t slot = stop_idx / 2;
            if (slot > 0 && slot < m_filament_colors.size() - 1) {
                double p_left_fil = m_gradient_positions[stop_idx - 2];
                double range_l = m_gradient_positions[stop_idx] - p_left_fil;
                double r_left = (range_l > 1e-6) ? (m_gradient_positions[stop_idx - 1] - p_left_fil) / range_l : 0.5;

                double p_right_fil = m_gradient_positions[stop_idx + 2];
                double range_r = p_right_fil - m_gradient_positions[stop_idx];
                double r_right = (range_r > 1e-6) ? (m_gradient_positions[stop_idx + 1] - m_gradient_positions[stop_idx]) / range_r : 0.5;

                double min_bound = p_left_fil + 2.0 * L;
                double max_bound = p_right_fil - 2.0 * L;
                m_gradient_positions[stop_idx] = std::clamp(pct, min_bound, max_bound);

                m_gradient_positions[stop_idx - 1] = p_left_fil + r_left * (m_gradient_positions[stop_idx] - p_left_fil);
                m_gradient_positions[stop_idx + 1] = m_gradient_positions[stop_idx] + r_right * (p_right_fil - m_gradient_positions[stop_idx]);

                if (m_on_changed) m_on_changed();
                m_canvas->Refresh();
            }
        } else {
            // Midpoint: update relative position
            double p_left = m_gradient_positions[stop_idx - 1];
            double p_right = m_gradient_positions[stop_idx + 1];
            double range = p_right - p_left;
            double global_pos = p_left + pct * range;
            double min_bound = p_left + L;
            double max_bound = p_right - L;
            m_gradient_positions[stop_idx] = std::clamp(global_pos, min_bound, max_bound);
            if (m_on_changed) m_on_changed();
            m_canvas->Refresh();
        }
    }
    update_percentage_input();
}

void MFDGradientAccordion::on_canvas_paint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(m_canvas);
    dc.SetBackground(wxBrush(get_background_color()));
    dc.Clear();

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;

    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

    wxSize size = m_canvas->GetClientSize();
    if (size.x <= 0 || size.y <= 0) return;

    const int count = static_cast<int>(m_filament_colors.size());
    const int num_stops = 2 * count - 1;
    if (m_gradient_positions.size() != num_stops) return;

    const double margin = get_margin();
    const double bw = get_border_width();
    const double w = std::max(1.0, size.x - margin * 2.0);
    const double h = std::max(1.0, size.y - margin * 2.0);
    const double sx = margin;
    const double sy = margin;

    // Draw background gradient bar
    for (int i = 0; i < count - 1; ++i) {
        double p_start = m_gradient_positions[2 * i];
        double p_mid   = m_gradient_positions[2 * i + 1];
        double p_end   = m_gradient_positions[2 * i + 2];

        double x_start = sx + p_start * w;
        double x_mid   = sx + p_mid * w;
        double x_end   = sx + p_end * w;

        wxColor c_start = m_filament_colors[i];
        wxColor c_end   = m_filament_colors[i + 1];
        wxColor c_mid(
            (c_start.Red() + c_end.Red()) / 2,
            (c_start.Green() + c_end.Green()) / 2,
            (c_start.Blue() + c_end.Blue()) / 2
        );

        if (x_mid > x_start) {
            wxGraphicsBrush gb1 = gc->CreateLinearGradientBrush(x_start, sy, x_mid, sy, c_start, c_mid);
            gc->SetBrush(gb1);
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->DrawRectangle(x_start, sy, x_mid - x_start + 0.5, h);
        }
        if (x_end > x_mid) {
            wxGraphicsBrush gb2 = gc->CreateLinearGradientBrush(x_mid, sy, x_end, sy, c_mid, c_end);
            gc->SetBrush(gb2);
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->DrawRectangle(x_mid, sy, x_end - x_mid, h);
        }
    }

    // Outline gradient bar
    gc->SetPen(wxPen(get_border_color(), bw));
    gc->SetBrush(*wxTRANSPARENT_BRUSH);
    gc->DrawRectangle(sx, sy, w, h);

    // Draw central line
    double cy = sy + h / 2.0;
    gc->SetPen(wxPen(*wxWHITE, FromDIP(1.5)));
    gc->StrokeLine(sx, cy, sx + w, cy);

    double L = m_min_ratio;

    // Draw limit markers for the selected handle (only when actively grabbing/dragging)
    int sel_idx = m_selected_stop_index;
    if (m_dragging && sel_idx > 0 && sel_idx < num_stops - 1) {
        double left_limit = 0.0;
        double right_limit = 1.0;
        if (sel_idx % 2 == 0) {
            // Filament: bounded by adjacent filaments and 2.0 * L (since there is a midpoint stop in between)
            left_limit = m_gradient_positions[sel_idx - 2] + 2.0 * L;
            right_limit = m_gradient_positions[sel_idx + 2] - 2.0 * L;
        } else {
            // Midpoint: bounded by adjacent filaments and L
            left_limit = m_gradient_positions[sel_idx - 1] + L;
            right_limit = m_gradient_positions[sel_idx + 1] - L;
        }

        double xl = sx + left_limit * w;
        double xr = sx + right_limit * w;

        // Draw vertical dashed lines
        wxDash dashes[2] = {4, 4};
        wxPen pen(*wxWHITE, 1.5, wxPENSTYLE_USER_DASH);
        pen.SetDashes(2, dashes);
        gc->SetPen(pen);
        gc->StrokeLine(xl, sy, xl, sy + h);
        gc->StrokeLine(xr, sy, xr, sy + h);
    }

    // Draw handles
    for (int i = 0; i < num_stops; ++i) {
        double pos = m_gradient_positions[i];
        double hx = sx + pos * w;

        if (i % 2 == 0) {
            // Filament handle: Circle
            int fil_idx = i / 2;
            wxColor color = m_filament_colors[fil_idx];
            bool is_grabbed = m_dragging && (i == sel_idx);
            double hr = get_handle_radius(is_grabbed);

            // Outline / Shadow (larger border width if selected)
            wxColour border_color = get_contrast_border_color(color);
            double current_bw = (i == sel_idx) ? (bw + FromDIP(2.5)) : bw;
            gc->SetPen(wxPen(border_color, current_bw));
            gc->SetBrush(wxBrush(color));
            gc->DrawEllipse(hx - hr, cy - hr, hr * 2.0, hr * 2.0);
        } else {
            // Midpoint handle: Solid thick white vertical tick
            bool is_grabbed = m_dragging && (i == sel_idx);
            bool is_selected = (i == sel_idx);
            
            double tick_w = is_grabbed ? FromDIP(5.5) : FromDIP(3.0);
            double tick_h = is_selected ? FromDIP(9.0) : FromDIP(6.0);

            gc->SetPen(wxPen(*wxWHITE, tick_w));
            gc->StrokeLine(hx, cy - tick_h, hx, cy + tick_h);
        }
    }
}

void MFDGradientAccordion::on_canvas_left_down(wxMouseEvent& event)
{
    wxSize size = m_canvas->GetClientSize();
    const double margin = get_margin();
    const double w = std::max(1.0, size.x - margin * 2.0);
    const double sx = margin;

    int click_x = event.GetX();
    int count = static_cast<int>(m_filament_colors.size());
    int num_stops = 2 * count - 1;

    // 1. Check if clicked on a Filament handle (higher priority)
    int clicked_filament_idx = -1;
    double fil_hit_radius = FromDIP(10);
    for (int i = 0; i < count; ++i) {
        double hx = sx + m_gradient_positions[2 * i] * w;
        if (std::abs(click_x - hx) < fil_hit_radius) {
            clicked_filament_idx = 2 * i;
            break;
        }
    }

    if (clicked_filament_idx != -1) {
        m_selected_stop_index = clicked_filament_idx;
        m_dragging = (clicked_filament_idx > 0 && clicked_filament_idx < num_stops - 1);
        if (m_dragging && !m_canvas->HasCapture()) {
            m_canvas->CaptureMouse();
        }
        sync_edit_panel();
        m_canvas->Refresh();
        return;
    }

    // 2. Otherwise, click snaps to the midpoint of the clicked segment
    for (int i = 0; i < count - 1; ++i) {
        double x_left = sx + m_gradient_positions[2 * i] * w;
        double x_right = sx + m_gradient_positions[2 * i + 2] * w;
        if (click_x >= x_left && click_x <= x_right) {
            int mid_idx = 2 * i + 1;
            m_selected_stop_index = mid_idx;
            
            // Snap midpoint to click coordinate
            double p_click = (click_x - sx) / w;
            double L = m_min_ratio;
            double min_bound = m_gradient_positions[2 * i] + L;
            double max_bound = m_gradient_positions[2 * i + 2] - L;
            m_gradient_positions[mid_idx] = std::clamp(p_click, min_bound, max_bound);

            m_dragging = true;
            if (!m_canvas->HasCapture()) {
                m_canvas->CaptureMouse();
            }
            if (m_on_changed) m_on_changed();
            sync_edit_panel();
            m_canvas->Refresh();
            return;
        }
    }
}

void MFDGradientAccordion::on_canvas_left_up(wxMouseEvent&)
{
    if (m_dragging) {
        m_dragging = false;
        if (m_canvas->HasCapture()) {
            m_canvas->ReleaseMouse();
        }
        m_canvas->Refresh();
    }
}

void MFDGradientAccordion::on_canvas_motion(wxMouseEvent& event)
{
    if (!m_dragging) return;
    update_positions_from_mouse(event.GetX());
}

void MFDGradientAccordion::update_positions_from_mouse(int x)
{
    wxSize size = m_canvas->GetClientSize();
    const double margin = get_margin();
    const double w = std::max(1.0, size.x - margin * 2.0);
    const double sx = margin;

    double p_new = (x - sx) / w;
    int idx = m_selected_stop_index;
    double L = m_min_ratio;

    if (idx % 2 == 0) {
        // Filament: update global position and adjust midpoints to maintain relative positioning
        double p_left_fil = m_gradient_positions[idx - 2];
        double range_l = m_gradient_positions[idx] - p_left_fil;
        double r_left = (range_l > 1e-6) ? (m_gradient_positions[idx - 1] - p_left_fil) / range_l : 0.5;

        double p_right_fil = m_gradient_positions[idx + 2];
        double range_r = p_right_fil - m_gradient_positions[idx];
        double r_right = (range_r > 1e-6) ? (m_gradient_positions[idx + 1] - m_gradient_positions[idx]) / range_r : 0.5;

        double min_bound = p_left_fil + 2.0 * L;
        double max_bound = p_right_fil - 2.0 * L;
        m_gradient_positions[idx] = std::clamp(p_new, min_bound, max_bound);

        m_gradient_positions[idx - 1] = p_left_fil + r_left * (m_gradient_positions[idx] - p_left_fil);
        m_gradient_positions[idx + 1] = m_gradient_positions[idx] + r_right * (p_right_fil - m_gradient_positions[idx]);
    } else {
        // Midpoint: simple drag
        double min_bound = m_gradient_positions[idx - 1] + L;
        double max_bound = m_gradient_positions[idx + 1] - L;
        m_gradient_positions[idx] = std::clamp(p_new, min_bound, max_bound);
    }

    if (m_on_changed) m_on_changed();
    m_canvas->Refresh();
    update_percentage_input();
}

wxColour MFDGradientAccordion::get_contrast_border_color(const wxColour& bg) const
{
    return bg.GetLuminance() > 0.5 ? wxColour(100, 100, 100) : *wxWHITE;
}

wxColour MFDGradientAccordion::get_border_color() const
{
    return StateColor::darkModeColorFor(wxColour("#CECECE"));
}

wxColour MFDGradientAccordion::get_background_color() const
{
    return GetBackgroundColour();
}

void MFDGradientAccordion::refresh_combobox_items()
{
    if (!m_filament_combo) return;

    for (size_t i = 0; i < m_physical_filaments.size(); ++i) {
        const auto& [_, name] = m_physical_filaments[i];
        const bool is_already_selected = std::find(
            m_selected_filaments.begin(), m_selected_filaments.end(), (int)i) != m_selected_filaments.end();
        
        int stop_idx = m_selected_stop_index;
        bool is_self = false;
        if (stop_idx % 2 == 0) {
            size_t slot = stop_idx / 2;
            if (slot < m_selected_filaments.size()) {
                is_self = (m_selected_filaments[slot] == (int)i);
            }
        }

        wxString text;
        if (is_already_selected && !is_self) {
            text += _L("Switch");
            text += " - ";
            text += wxString(name);
        } else {
            text = wxString(name);
        }
        m_filament_combo->SetString(i, text);
    }
}

} // namespace Slic3r::GUI
