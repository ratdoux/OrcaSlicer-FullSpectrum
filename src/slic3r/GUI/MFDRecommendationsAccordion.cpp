#include "MFDRecommendationsAccordion.hpp"

#include <wx/wx.h>
#include <wx/wrapsizer.h>
#include <algorithm>
#include <cmath>

#include "I18N.hpp"
#include "Widgets/Label.hpp"

namespace Slic3r::GUI {

MFDRecommendationsAccordion::MFDRecommendationsAccordion(
    wxWindow*                                                   parent,
    const std::vector<std::pair<std::string, std::string>>&     physical_filaments)
    : Accordion(parent, _L("Mixing Recommendations"))
    , m_physical_filaments(physical_filaments)
{
    build_ui();
}

void MFDRecommendationsAccordion::build_ui()
{
    wxPanel*    body  = get_body_panel();
    wxBoxSizer* sizer = get_body_sizer();
    fill_recommendations(body, sizer);
}

void MFDRecommendationsAccordion::fill_recommendations(
    wxPanel* container, wxBoxSizer* container_sizer)
{
    if (!container)
        return;

    const int filament_count = static_cast<int>(m_physical_filaments.size());

    if (filament_count >= 2) {
        wxStaticText* label_2way = new wxStaticText(container, wxID_ANY, _L("2-Way Mixes"));
        label_2way->SetForegroundColour("#7e7e7e");
        label_2way->SetFont(::Label::Body_12.Bold());
        container_sizer->Add(label_2way, 0, wxLEFT | wxTOP | wxRIGHT, FromDIP(8));

        // 50/50 mixes - the most common starting point for any 2-filament blend.
        wxWrapSizer* wrap_50 = new wxWrapSizer(wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS);
        for (int i = 0; i < filament_count; ++i) {
            for (int j = i + 1; j < filament_count; ++j) {
                std::vector<double> w  = {0.5, 0.5};
                std::vector<int>    pi = {i, j};
                wxColor mc = get_mixed_color(pi, w);
                wrap_50->Add(create_mix_tile(container, mc, format_tooltip(pi, w, mc), pi, w),
                             0, wxALL, FromDIP(4));
            }
        }
        container_sizer->Add(wrap_50, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        // 66/34 mixes - shows a dominant vs. accent filament option.
        wxWrapSizer* wrap_66 = new wxWrapSizer(wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS);
        for (int i = 0; i < filament_count; ++i) {
            for (int j = i + 1; j < filament_count; ++j) {
                std::vector<double> w  = {0.66, 0.34};
                std::vector<int>    pi = {i, j};
                wxColor mc = get_mixed_color(pi, w);
                wrap_66->Add(create_mix_tile(container, mc, format_tooltip(pi, w, mc), pi, w),
                             0, wxALL, FromDIP(4));
            }
        }
        container_sizer->Add(wrap_66, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    }

    if (filament_count >= 3) {
        wxStaticText* label_3way = new wxStaticText(container, wxID_ANY, _L("3-Way Mixes"));
        label_3way->SetForegroundColour("#7e7e7e");
        label_3way->SetFont(::Label::Body_12.Bold());
        container_sizer->Add(label_3way, 0, wxLEFT | wxTOP | wxRIGHT, FromDIP(8));

        wxWrapSizer* wrap_3 = new wxWrapSizer(wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS);
        for (int i = 0; i < filament_count; ++i) {
            for (int j = i + 1; j < filament_count; ++j) {
                for (int k = j + 1; k < filament_count; ++k) {
                    std::vector<double> w  = {0.33, 0.33, 0.34};
                    std::vector<int>    pi = {i, j, k};
                    wxColor mc = get_mixed_color(pi, w);
                    wrap_3->Add(create_mix_tile(container, mc, format_tooltip(pi, w, mc), pi, w),
                                0, wxALL, FromDIP(4));
                }
            }
        }
        container_sizer->Add(wrap_3, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    }
}

wxPanel* MFDRecommendationsAccordion::create_mix_tile(
    wxPanel*                    parent,
    const wxColor&              color,
    const wxString&             tooltip,
    const std::vector<int>&     physical_indices,
    const std::vector<double>&  weights)
{
    wxPanel* tile = new wxPanel(parent, wxID_ANY, wxDefaultPosition,
                                wxSize(FromDIP(24), FromDIP(24)), wxBORDER_NONE);
    tile->SetBackgroundColour(color);
    tile->SetCursor(wxCursor(wxCURSOR_HAND));
    tile->SetToolTip(tooltip);

    // Clicking a tile fires the callback so the dialog can apply the preset
    // to its state and propagate the change to other sections.
    tile->Bind(wxEVT_LEFT_UP, [this, physical_indices, weights](wxMouseEvent&) {
        if (m_on_preset_selected)
            m_on_preset_selected(physical_indices, weights);
    });

    return tile;
}

wxString MFDRecommendationsAccordion::format_tooltip(
    const std::vector<int>&    phys_indices,
    const std::vector<double>& weights,
    const wxColor&             mixed_color) const
{
    wxString tooltip;
    for (size_t i = 0; i < phys_indices.size(); ++i) {
        if (i > 0)
            tooltip += " + ";
        int pct = static_cast<int>(std::round(weights[i] * 100.0));
        tooltip += wxString::Format("%d%% Filament [%d]", pct, phys_indices[i] + 1);
    }
    tooltip += wxString::Format(" = #%02X%02X%02X",
        mixed_color.Red(), mixed_color.Green(), mixed_color.Blue());
    return tooltip;
}

wxColor MFDRecommendationsAccordion::get_mixed_color(
    const std::vector<int>&    phys_indices,
    const std::vector<double>& weights) const
{
    double r = 0, g = 0, b = 0;
    for (size_t i = 0; i < phys_indices.size(); ++i) {
        wxColor col(m_physical_filaments[phys_indices[i]].first);
        r += weights[i] * col.Red();
        g += weights[i] * col.Green();
        b += weights[i] * col.Blue();
    }
    return wxColor(std::clamp(static_cast<int>(r), 0, 255),
                   std::clamp(static_cast<int>(g), 0, 255),
                   std::clamp(static_cast<int>(b), 0, 255));
}

void MFDRecommendationsAccordion::update_recommendations(
    const std::vector<std::pair<std::string, std::string>>& physical_filaments, 
    double current_min_weight_ratio)
{
    // currently a no-op as the recommendations are static based on physical filaments
    // If you want to dynamically re-build recommendations, call fill_recommendations here.
}

} // namespace Slic3r::GUI
