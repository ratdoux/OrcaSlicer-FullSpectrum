#include "MFDPreviewAccordion.hpp"

#include <wx/wx.h>
#include <wx/dcgraph.h>
#include <algorithm>
#include <cmath>

#include "I18N.hpp"
#include "GUI_App.hpp"
#include "MFDTheme.hpp"

namespace Slic3r::GUI {

MFDPreviewAccordion::MFDPreviewAccordion(wxWindow* parent)
    : Accordion(parent, _L("Preview"))
{
    build_ui();
}

void MFDPreviewAccordion::build_ui()
{
    wxPanel*    body   = get_body_panel();
    wxBoxSizer* sizer  = get_body_sizer();

    wxBoxSizer* row_sizer = new wxBoxSizer(wxHORIZONTAL);

    // Layer stack panel: draws one colored bar per layer entry.
    m_layers_panel = new wxPanel(body, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(120)), wxBORDER_NONE);
    m_layers_panel->SetMinSize(wxSize(-1, FromDIP(120)));
    m_layers_panel->SetBackgroundColour(body->GetBackgroundColour());
    m_layers_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_layers_panel->Bind(wxEVT_PAINT, &MFDPreviewAccordion::paint_layers_panel, this);

    // Solid color panel: shows the blended result color as a flat rectangle.
    m_color_panel = new wxPanel(body, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(120)), wxBORDER_NONE);
    m_color_panel->SetMinSize(wxSize(-1, FromDIP(120)));
    m_color_panel->SetBackgroundColour(body->GetBackgroundColour());
    m_color_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_color_panel->Bind(wxEVT_PAINT, &MFDPreviewAccordion::paint_color_panel, this);

    row_sizer->Add(m_layers_panel, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));
    row_sizer->Add(m_color_panel,  1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));
    sizer->Add(row_sizer, 0, wxEXPAND);

    // Header preview widgets: a mini layer strip and a small color swatch,
    // visible only when the accordion is collapsed.
    m_title_layers = new wxPanel(get_header_panel(), wxID_ANY, wxDefaultPosition, wxSize(FromDIP(144), FromDIP(18)), wxBORDER_NONE);
    m_title_layers->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_title_layers->Show(false);
    m_title_layers->Bind(wxEVT_PAINT, &MFDPreviewAccordion::paint_title_layers, this);
    add_header_control(m_title_layers);

    m_title_swatch = new wxPanel(get_header_panel(), wxID_ANY, wxDefaultPosition, wxSize(FromDIP(36), FromDIP(18)), wxBORDER_SIMPLE);
    m_title_swatch->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_title_swatch->SetBackgroundColour(*wxLIGHT_GREY);
    m_title_swatch->Show(false);
    m_title_swatch->Bind(wxEVT_PAINT, &MFDPreviewAccordion::paint_title_swatch, this);
    add_header_control(m_title_swatch);
}

void MFDPreviewAccordion::update_preview_mix(
    const std::vector<double>&               weights,
    const std::vector<wxColor>&              colors,
    const wxColor&                           mixed_color)
{
    m_preview_mode = PreviewMode::Mix;
    m_layer_stack  = compute_layer_stack(weights, 20);
    m_colors       = colors;

    if (m_color_panel) {
        m_color_panel->SetBackgroundColour(mixed_color);
        m_color_panel->Refresh();
    }
    if (m_title_swatch) {
        m_title_swatch->SetBackgroundColour(mixed_color);
        m_title_swatch->Refresh();
    }
    if (m_layers_panel)
        m_layers_panel->Refresh();
    if (m_title_layers)
        m_title_layers->Refresh();
}

void MFDPreviewAccordion::update_preview_pattern(
    const std::vector<int>&                  pattern_indices,
    const std::vector<wxColor>&              colors,
    const wxColor&                           mixed_color)
{
    m_preview_mode = PreviewMode::Pattern;
    m_layer_stack  = compute_pattern_layer_stack(pattern_indices, 20);
    m_colors       = colors;

    if (m_color_panel) {
        m_color_panel->SetBackgroundColour(mixed_color);
        m_color_panel->Refresh();
    }
    if (m_title_swatch) {
        m_title_swatch->SetBackgroundColour(mixed_color);
        m_title_swatch->Refresh();
    }
    if (m_layers_panel)
        m_layers_panel->Refresh();
    if (m_title_layers)
        m_title_layers->Refresh();
}

void MFDPreviewAccordion::update_preview_gradient(
    const std::vector<wxColor>&              colors,
    const std::vector<double>&               positions,
    const std::vector<wxColor>&              predicted_colors)
{
    m_preview_mode = PreviewMode::Gradient;
    m_colors = colors;
    m_gradient_positions = positions;
    m_gradient_predicted_colors = predicted_colors;

    if (m_color_panel)
        m_color_panel->Refresh();
    if (m_title_swatch)
        m_title_swatch->Refresh();
    if (m_layers_panel)
        m_layers_panel->Refresh();
    if (m_title_layers)
        m_title_layers->Refresh();
}

void MFDPreviewAccordion::set_preview_mode(PreviewMode mode)
{
    if (m_preview_mode != mode) {
        m_preview_mode = mode;
        Refresh();
    }
}

void MFDPreviewAccordion::clear_preview()
{
    m_layer_stack.clear();
    m_colors.clear();
    m_gradient_positions.clear();
    m_gradient_predicted_colors.clear();

    if (m_color_panel) {
        m_color_panel->SetBackgroundColour(*wxLIGHT_GREY);
        m_color_panel->Refresh();
    }
    if (m_title_swatch) {
        m_title_swatch->SetBackgroundColour(*wxLIGHT_GREY);
        m_title_swatch->Refresh();
    }
    if (m_layers_panel)
        m_layers_panel->Refresh();
    if (m_title_layers)
        m_title_layers->Refresh();
}

void MFDPreviewAccordion::on_collapsed_changed(bool collapsed)
{
    // Show the compact header previews only when the body is hidden,
    // giving the user a quick visual of the current mix at a glance.
    if (m_title_swatch)  m_title_swatch->Show(collapsed);
    if (m_title_layers)  m_title_layers->Show(collapsed);
    if (m_title_layers)  m_title_layers->Refresh();
}

MFDPreviewAccordion::GradientSample MFDPreviewAccordion::sample_gradient(double t) const
{
    GradientSample sample;
    if (m_colors.empty() || m_gradient_positions.empty()) {
        sample.color_a = *wxLIGHT_GREY;
        sample.color_b = *wxLIGHT_GREY;
        sample.weight_b = 0.0;
        return sample;
    }
    int count = static_cast<int>(m_colors.size());
    if (count == 1 || m_gradient_positions.size() < 3) {
        sample.color_a = m_colors[0];
        sample.color_b = m_colors[0];
        sample.weight_b = 0.0;
        sample.index_a = 0;
        sample.index_b = 0;
        return sample;
    }

    t = std::clamp(t, 0.0, 1.0);

    for (int i = 0; i < count - 1; ++i) {
        double p_start = m_gradient_positions[2 * i];
        double p_end   = m_gradient_positions[2 * i + 2];
        if (t >= p_start && t <= p_end) {
            double p_mid = m_gradient_positions[2 * i + 1];
            sample.index_a = i;
            sample.index_b = i + 1;
            sample.color_a = m_colors[i];
            sample.color_b = m_colors[i + 1];
            if (t <= p_mid) {
                double den = p_mid - p_start;
                sample.weight_b = (den > 1e-6) ? (0.5 * (t - p_start) / den) : 0.0;
            } else {
                double den = p_end - p_mid;
                sample.weight_b = (den > 1e-6) ? (0.5 + 0.5 * (t - p_mid) / den) : 1.0;
            }
            return sample;
        }
    }

    if (t <= m_gradient_positions.front()) {
        sample.index_a = 0;
        sample.index_b = 0;
        sample.color_a = m_colors.front();
        sample.color_b = m_colors.front();
        sample.weight_b = 0.0;
    } else {
        sample.index_a = count - 1;
        sample.index_b = count - 1;
        sample.color_a = m_colors.back();
        sample.color_b = m_colors.back();
        sample.weight_b = 0.0;
    }
    return sample;
}

void MFDPreviewAccordion::draw_layer_stack_common(
    wxGraphicsContext* gc,
    const wxSize&      size,
    bool               vertical,
    double             padding,
    double             corner_radius)
{
    // Grey contrasting background
    gc->SetBrush(wxBrush(MFDTheme::card_border()));
    gc->SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#E0E0E0")), 1));
    gc->DrawRoundedRectangle(0, 0, size.x, size.y, corner_radius);

    // Clip to the contrasting background area
    gc->Clip(padding, padding, size.x - 2 * padding, size.y - 2 * padding);

    if (m_preview_mode == PreviewMode::Gradient) {
        if (m_colors.empty() || m_gradient_positions.empty())
            return;

        double W = size.x - 2 * padding;
        double H = size.y - 2 * padding;
        
        double cycle_size = FromDIP(6.0);
        double total_span = vertical ? H : W;
        int num_cycles = static_cast<int>(std::round(total_span / cycle_size));
        if (num_cycles < 2) num_cycles = 2;
        cycle_size = total_span / num_cycles;
        double half_cycle = cycle_size / 2.0;

        for (int c = 0; c < num_cycles; ++c) {
            double t = 0.0;
            if (vertical) {
                double y_cycle_bottom = (size.y - padding) - c * cycle_size;
                double y_center = y_cycle_bottom - half_cycle;
                t = (size.y - padding - y_center) / H;
            } else {
                double x_left_start = padding + c * cycle_size;
                double x_center = x_left_start + half_cycle;
                t = (x_center - padding) / W;
            }

            GradientSample sample = sample_gradient(t);

            double size_A = cycle_size * (1.0 - sample.weight_b);
            double size_B = cycle_size * sample.weight_b;

            if (vertical) {
                double y_cycle_bottom = (size.y - padding) - c * cycle_size;
                if (size_A > 0) {
                    gc->SetBrush(wxBrush(sample.color_a));
                    gc->SetPen(*wxTRANSPARENT_PEN);
                    gc->DrawRectangle(padding, y_cycle_bottom - size_A - 0.5, W, size_A + 0.5);
                }
                if (size_B > 0) {
                    gc->SetBrush(wxBrush(sample.color_b));
                    gc->SetPen(*wxTRANSPARENT_PEN);
                    gc->DrawRectangle(padding, y_cycle_bottom - size_A - size_B - 0.5, W, size_B + 0.5);
                }
            } else {
                double x_left_start = padding + c * cycle_size;
                if (size_A > 0) {
                    gc->SetBrush(wxBrush(sample.color_a));
                    gc->SetPen(*wxTRANSPARENT_PEN);
                    gc->DrawRectangle(x_left_start, padding, size_A + 0.5, H);
                }
                if (size_B > 0) {
                    gc->SetBrush(wxBrush(sample.color_b));
                    gc->SetPen(*wxTRANSPARENT_PEN);
                    gc->DrawRectangle(x_left_start + size_A, padding, size_B + 0.5, H);
                }
            }
        }
    } else { // preview_mode == Mix or Pattern
        if (m_layer_stack.empty())
            return;

        int    total_layers = static_cast<int>(m_layer_stack.size());
        double W = size.x - 2 * padding;
        double H = size.y - 2 * padding;
        double step_size = (vertical ? H : W) / total_layers;
        double radius = vertical ? (step_size / 2.0) : FromDIP(1.0);

        for (int i = 0; i < total_layers; ++i) {
            const auto& entry = m_layer_stack[i];
            if (entry.filament_index < 0 || entry.filament_index >= static_cast<int>(m_colors.size()))
                continue;

            wxColor col = m_colors[entry.filament_index];
            gc->SetBrush(wxBrush(col));
            gc->SetPen(*wxTRANSPARENT_PEN);

            if (vertical) {
                double draw_w = W * entry.scale;
                double draw_x = padding + (W - draw_w) / 2.0;
                double draw_y = (size.y - padding) - (i + 1) * step_size;
                gc->DrawRoundedRectangle(draw_x, draw_y, draw_w, step_size, radius);
            } else {
                double draw_h = H * entry.scale;
                double draw_y = padding + (H - draw_h) / 2.0;
                double draw_x = padding + i * step_size;
                gc->DrawRoundedRectangle(draw_x, draw_y, step_size, draw_h, radius);
            }
        }
    }
}

void MFDPreviewAccordion::draw_gradient_common(
    wxGraphicsContext* gc,
    const wxSize&      size,
    bool               vertical)
{
    const std::vector<wxColor>& display_colors =
        m_gradient_predicted_colors.size() >= 2 ? m_gradient_predicted_colors : m_colors;
    if (display_colors.size() < 2)
        return;

    double W = size.x;
    double H = size.y;

    const int segment_count = int(display_colors.size()) - 1;
    for (int index = 0; index < segment_count; ++index) {
        const double start = double(index) / double(segment_count);
        const double end   = double(index + 1) / double(segment_count);
        const wxColor& color_start = display_colors[size_t(index)];
        const wxColor& color_end   = display_colors[size_t(index + 1)];

        if (vertical) {
            const double y_start = size.y - start * H;
            const double y_end   = size.y - end * H;
            wxGraphicsBrush brush = gc->CreateLinearGradientBrush(0, y_start, 0, y_end, color_start, color_end);
            gc->SetBrush(brush);
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->DrawRectangle(0, y_end, W, y_start - y_end + 0.5);
        } else {
            const double x_start = start * W;
            const double x_end   = end * W;
            wxGraphicsBrush brush = gc->CreateLinearGradientBrush(x_start, 0, x_end, 0, color_start, color_end);
            gc->SetBrush(brush);
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->DrawRectangle(x_start, 0, x_end - x_start + 0.5, H);
        }
    }
}

void MFDPreviewAccordion::paint_layers_panel(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(m_layers_panel);
    wxGCDC gcdc(dc);
    wxGraphicsContext* gc = gcdc.GetGraphicsContext();
    if (!gc) return;

    wxSize size = m_layers_panel->GetSize();

    // Card background
    gc->SetBrush(wxBrush(m_layers_panel->GetBackgroundColour()));
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->DrawRectangle(0, 0, size.x, size.y);

    draw_layer_stack_common(gc, size, true, FromDIP(2), FromDIP(4));
}

void MFDPreviewAccordion::paint_title_layers(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(m_title_layers);
    wxGCDC gcdc(dc);
    wxGraphicsContext* gc = gcdc.GetGraphicsContext();
    if (!gc) return;

    wxSize size = m_title_layers->GetSize();

    // Card background
    gc->SetBrush(wxBrush(m_title_layers->GetBackgroundColour()));
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->DrawRectangle(0, 0, size.x, size.y);

    draw_layer_stack_common(gc, size, false, FromDIP(2), FromDIP(2));
}

void MFDPreviewAccordion::paint_color_panel(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(m_color_panel);
    wxGCDC gcdc(dc);
    wxGraphicsContext* gc = gcdc.GetGraphicsContext();
    if (!gc) return;

    wxSize size = m_color_panel->GetSize();

    if (m_preview_mode == PreviewMode::Gradient) {
        draw_gradient_common(gc, size, true);
    } else {
        // Solid color with a border
        gc->SetBrush(wxBrush(m_color_panel->GetBackgroundColour()));
        gc->SetPen(wxPen(MFDTheme::card_border(), 1));
        gc->DrawRectangle(0, 0, size.x, size.y);
    }
}

void MFDPreviewAccordion::paint_title_swatch(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(m_title_swatch);
    wxGCDC gcdc(dc);
    wxGraphicsContext* gc = gcdc.GetGraphicsContext();
    if (!gc) return;

    wxSize size = m_title_swatch->GetClientSize();

    if (m_preview_mode == PreviewMode::Gradient) {
        draw_gradient_common(gc, size, false); 
    } else {
        gc->SetBrush(wxBrush(m_title_swatch->GetBackgroundColour()));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->DrawRectangle(0, 0, size.x, size.y);
    }
}

std::vector<MFDPreviewLayerEntry> MFDPreviewAccordion::compute_layer_stack(const std::vector<double>& weights, int total_layers)
{
    std::vector<MFDPreviewLayerEntry> stack;
    int n_filaments = weights.size();
    if (n_filaments == 0) return stack;
    if (n_filaments == 1) {
        for (int i = 0; i < total_layers; ++i) stack.push_back({0, 1.0});
        return stack;
    }

    std::vector<int> counts(n_filaments, 0);
    int total_assigned = 0;
    std::vector<double> remainders(n_filaments, 0.0);

    for (int i = 0; i < n_filaments; ++i) {
        double exact = weights[i] * total_layers;
        counts[i] = static_cast<int>(std::floor(exact));
        remainders[i] = exact - counts[i];
        total_assigned += counts[i];
    }

    while (total_assigned < total_layers) {
        int best_idx = 0;
        double max_rem = -1.0;
        for (int i = 0; i < n_filaments; ++i) {
            if (remainders[i] > max_rem) {
                max_rem = remainders[i];
                best_idx = i;
            }
        }
        counts[best_idx]++;
        remainders[best_idx] -= 1.0;
        total_assigned++;
    }

    auto generate_pattern = [&]() {
        std::vector<int> current_counts = counts;
        std::vector<int> pattern;
        pattern.reserve(total_layers);
        for (int step = 0; step < total_layers; ++step) {
            int best_idx = -1;
            double max_score = -1e9;
            for (int i = 0; i < n_filaments; ++i) {
                if (current_counts[i] > 0) {
                    double score = static_cast<double>(current_counts[i]) / counts[i];
                    if (score > max_score) {
                        max_score = score;
                        best_idx = i;
                    }
                }
            }
            if (best_idx != -1) {
                pattern.push_back(best_idx);
                current_counts[best_idx]--;
            }
        }
        return pattern;
    };

    std::vector<int> pattern = generate_pattern();
    for (int idx : pattern) {
        stack.push_back({idx, 1.0});
    }

    return stack;
}

std::vector<MFDPreviewLayerEntry> MFDPreviewAccordion::compute_pattern_layer_stack(const std::vector<int>& pattern_indices, int total_layers)
{
    std::vector<MFDPreviewLayerEntry> stack;
    if (pattern_indices.empty()) return stack;
    
    int num_indices = pattern_indices.size();
    for (int i = 0; i < total_layers; ++i) {
        int idx = pattern_indices[i % num_indices] - 1;
        stack.push_back({idx, 1.0});
    }
    return stack;
}

} // namespace Slic3r::GUI
