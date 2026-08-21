#include "MFDRecommendationsAccordion.hpp"

#include <wx/wx.h>
#include <wx/wrapsizer.h>
#include <algorithm>
#include <cmath>

#include "I18N.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/FilamentCardMixed.hpp"
#include "GUI_App.hpp"
#include "MFDTheme.hpp"
#include "MixedColorMatchHelpers.hpp"

namespace Slic3r::GUI {

MFDRecommendationsAccordion::MFDRecommendationsAccordion(
    wxWindow*                                                   parent,
    const std::vector<std::pair<std::string, std::string>>&     physical_filaments)
    : Accordion(parent, _L("Mix Recommendations"))
    , m_physical_filaments(physical_filaments)
{
    std::vector<std::string> physical_colors;
    physical_colors.reserve(m_physical_filaments.size());
    for (const auto& filament : m_physical_filaments)
        physical_colors.emplace_back(filament.first);
    m_display_context = build_mixed_filament_display_context(physical_colors);
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

    // 1. Create and populate m_mix_panel
    m_mix_panel = new wxPanel(container, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_mix_panel->SetBackgroundColour(MFDTheme::card_background());
    wxBoxSizer* mix_sizer = new wxBoxSizer(wxVERTICAL);
    m_mix_panel->SetSizer(mix_sizer);

    if (filament_count >= 2) {
        wxStaticText* label_2way = new wxStaticText(m_mix_panel, wxID_ANY, _L("2-Material Mixes"));
        label_2way->SetFont(::Label::Body_12.Bold());
        MFDTheme::apply_text(label_2way, MFDTheme::muted_text(), m_mix_panel->GetBackgroundColour());
        mix_sizer->Add(label_2way, 0, wxLEFT | wxTOP | wxRIGHT, FromDIP(8));

        wxWrapSizer* wrap_50 = new wxWrapSizer(wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS);
        for (int i = 0; i < filament_count; ++i) {
            for (int j = i + 1; j < filament_count; ++j) {
                std::vector<double> w  = {0.5, 0.5};
                std::vector<int>    pi = {i, j};
                wxColor mc = get_mixed_color(pi, w);
                wrap_50->Add(create_mix_tile(m_mix_panel, mc, format_tooltip(pi, w, mc), pi, w),
                             0, wxALL, FromDIP(2));
            }
        }
        mix_sizer->Add(wrap_50, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        wxWrapSizer* wrap_66 = new wxWrapSizer(wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS);
        for (int i = 0; i < filament_count; ++i) {
            for (int j = i + 1; j < filament_count; ++j) {
                std::vector<double> w  = {0.66, 0.34};
                std::vector<int>    pi = {i, j};
                wxColor mc = get_mixed_color(pi, w);
                wrap_66->Add(create_mix_tile(m_mix_panel, mc, format_tooltip(pi, w, mc), pi, w),
                             0, wxALL, FromDIP(2));
            }
        }
        mix_sizer->Add(wrap_66, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    }

    if (filament_count >= 3) {
        wxStaticText* label_3way = new wxStaticText(m_mix_panel, wxID_ANY, _L("3-Material Mixes"));
        label_3way->SetFont(::Label::Body_12.Bold());
        MFDTheme::apply_text(label_3way, MFDTheme::muted_text(), m_mix_panel->GetBackgroundColour());
        mix_sizer->Add(label_3way, 0, wxLEFT | wxTOP | wxRIGHT, FromDIP(8));

        wxWrapSizer* wrap_3 = new wxWrapSizer(wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS);
        for (int i = 0; i < filament_count; ++i) {
            for (int j = i + 1; j < filament_count; ++j) {
                for (int k = j + 1; k < filament_count; ++k) {
                    std::vector<double> w  = {0.33, 0.33, 0.34};
                    std::vector<int>    pi = {i, j, k};
                    wxColor mc = get_mixed_color(pi, w);
                    wrap_3->Add(create_mix_tile(m_mix_panel, mc, format_tooltip(pi, w, mc), pi, w),
                                0, wxALL, FromDIP(2));
                }
            }
        }
        mix_sizer->Add(wrap_3, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    }
    m_mix_panel->Layout();
    container_sizer->Add(m_mix_panel, 1, wxEXPAND);

    // 2. Create and populate m_gradient_panel
    m_gradient_panel = new wxPanel(container, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_gradient_panel->SetBackgroundColour(MFDTheme::card_background());
    wxBoxSizer* gradient_sizer = new wxBoxSizer(wxVERTICAL);
    m_gradient_panel->SetSizer(gradient_sizer);

    // 2-material gradients
    if (filament_count >= 2) {
        wxStaticText* label_2way = new wxStaticText(m_gradient_panel, wxID_ANY, _L("2-Material Gradients"));
        label_2way->SetFont(::Label::Body_12.Bold());
        MFDTheme::apply_text(label_2way, MFDTheme::muted_text(), m_gradient_panel->GetBackgroundColour());
        gradient_sizer->Add(label_2way, 0, wxLEFT | wxTOP | wxRIGHT, FromDIP(8));

        wxWrapSizer* wrap_2 = new wxWrapSizer(wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS);
        for (int i = 0; i < filament_count; ++i) {
            for (int j = i + 1; j < filament_count; ++j) {
                wrap_2->Add(create_gradient_tile(m_gradient_panel, {i, j}), 0, wxALL, FromDIP(2));
                wrap_2->Add(create_gradient_tile(m_gradient_panel, {j, i}), 0, wxALL, FromDIP(2));
            }
        }
        gradient_sizer->Add(wrap_2, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    }

    // 3-material gradients
    wxWrapSizer* wrap_3 = nullptr;
    for (int i = 0; i < filament_count; ++i) {
        for (int j = 0; j < filament_count; ++j) {
            if (i == j) continue;
            for (int k = 0; k < filament_count; ++k) {
                if (i == k || j == k) continue;

                wxColor c1(m_physical_filaments[i].first);
                wxColor c2(m_physical_filaments[j].first);
                wxColor c3(m_physical_filaments[k].first);
                if (is_color_in_between(c1, c2, c3)) {
                    if (!wrap_3) {
                        wxStaticText* label_3way = new wxStaticText(m_gradient_panel, wxID_ANY, _L("3-Material Gradients"));
                        label_3way->SetFont(::Label::Body_12.Bold());
                        MFDTheme::apply_text(label_3way, MFDTheme::muted_text(), m_gradient_panel->GetBackgroundColour());
                        gradient_sizer->Add(label_3way, 0, wxLEFT | wxTOP | wxRIGHT, FromDIP(8));
                        wrap_3 = new wxWrapSizer(wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS);
                    }
                    wrap_3->Add(create_gradient_tile(m_gradient_panel, {i, j, k}), 0, wxALL, FromDIP(2));
                }
            }
        }
    }
    if (wrap_3) {
        gradient_sizer->Add(wrap_3, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    }



    m_gradient_panel->Layout();
    m_gradient_panel->Show(false); // Initially hide gradient recommendations
    container_sizer->Add(m_gradient_panel, 1, wxEXPAND);
}

wxPanel* MFDRecommendationsAccordion::create_mix_tile(
    wxPanel*                    parent,
    const wxColor&              color,
    const wxString&             tooltip,
    const std::vector<int>&     physical_indices,
    const std::vector<double>&  weights)
{
    wxPanel* tile = new wxPanel(parent, wxID_ANY, wxDefaultPosition,
                                wxSize(FromDIP(28), FromDIP(28)), wxBORDER_NONE);
    tile->SetMinSize(wxSize(FromDIP(28), FromDIP(28)));
    tile->SetBackgroundColour(parent ? parent->GetBackgroundColour() : MFDTheme::card_background());
    tile->SetBackgroundStyle(wxBG_STYLE_PAINT);
    tile->SetCursor(wxCursor(wxCURSOR_HAND));
    tile->SetToolTip(tooltip);

    auto is_hovered = std::make_shared<bool>(false);

    tile->Bind(wxEVT_PAINT, [tile, color, is_hovered](wxPaintEvent&) {
        wxPaintDC dc(tile);
        wxSize s = tile->GetClientSize();
        wxColor c = color;
        wxString idx = "";
        int padding = *is_hovered ? 0 : tile->FromDIP(2);

        dc.SetBackground(wxBrush(tile->GetParent()->GetBackgroundColour()));
        dc.Clear();

        FilamentCardMixed::paint_clr_swatch(dc, s, c, idx, wxGetApp().dark_mode(), padding);
    });

    tile->Bind(wxEVT_ENTER_WINDOW, [tile, is_hovered](wxMouseEvent& e) {
        *is_hovered = true;
        tile->Refresh(); e.Skip();
    });

    tile->Bind(wxEVT_LEAVE_WINDOW, [tile, is_hovered](wxMouseEvent& e) {
        *is_hovered = false;
        tile->Refresh(); e.Skip();
    });

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
    std::vector<std::string> physical_colors;
    physical_colors.reserve(m_physical_filaments.size());
    for (const auto& f : m_physical_filaments)
        physical_colors.emplace_back(f.first);
    const MixedFilamentDisplayContext ctx = build_mixed_filament_display_context(physical_colors);

    std::vector<int> pct_ints;
    pct_ints.reserve(weights.size());
    for (double w : weights)
        pct_ints.push_back(static_cast<int>(std::round(w * 100.0)));

    char hex_buf[16];
    std::snprintf(hex_buf, sizeof(hex_buf), "#%02X%02X%02X", mixed_color.Red(), mixed_color.Green(), mixed_color.Blue());

    ColorNames::DescriptionOptions opts;
    opts.include_components = true;

    std::string tip = ColorNames::mixed_filament_name(phys_indices, pct_ints, std::string(hex_buf), ctx.physical_material_types,
                                                      ctx.physical_colors, opts);
    return from_u8(tip);
}

wxColor MFDRecommendationsAccordion::get_mixed_color(
    const std::vector<int>&    phys_indices,
    const std::vector<double>& weights) const
{
    const size_t count = std::min(phys_indices.size(), weights.size());
    std::vector<unsigned int> component_ids;
    std::vector<int>          component_weights;
    component_ids.reserve(count);
    component_weights.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        if (phys_indices[index] < 0 || phys_indices[index] >= int(m_physical_filaments.size()) ||
            !std::isfinite(weights[index]) || weights[index] <= 0.0)
            continue;
        component_ids.emplace_back(unsigned(phys_indices[index] + 1));
        component_weights.emplace_back(std::max(1, int(std::lround(weights[index] * 1000000.0))));
    }
    return blend_mixed_filament_components(component_ids, component_weights, m_display_context);
}

void MFDRecommendationsAccordion::update_recommendations(
    const std::vector<std::pair<std::string, std::string>>& physical_filaments, 
    double current_min_weight_ratio)
{
    wxUnusedVar(physical_filaments);
    wxUnusedVar(current_min_weight_ratio);
}

void MFDRecommendationsAccordion::set_mode(Mode mode)
{
    if (m_mode != mode) {
        m_mode = mode;
        set_title(mode == Mode::Gradient ? _L("Gradient Recommendations") : _L("Mix Recommendations"));
        
        if (m_mix_panel)      m_mix_panel->Show(mode == Mode::Mix);
        if (m_gradient_panel) m_gradient_panel->Show(mode == Mode::Gradient);
        
        get_body_panel()->Layout();
        if (GetParent()) {
            GetParent()->Layout();
            GetParent()->Refresh();
        }
    }
}

wxPanel* MFDRecommendationsAccordion::create_gradient_tile(
    wxPanel*                parent,
    const std::vector<int>& physical_indices)
{
    wxPanel* tile = new wxPanel(parent, wxID_ANY, wxDefaultPosition,
                                wxSize(FromDIP(28), FromDIP(28)), wxBORDER_NONE);
    tile->SetMinSize(wxSize(FromDIP(28), FromDIP(28)));
    tile->SetBackgroundColour(parent ? parent->GetBackgroundColour() : MFDTheme::card_background());
    tile->SetBackgroundStyle(wxBG_STYLE_PAINT);
    tile->SetCursor(wxCursor(wxCURSOR_HAND));
    tile->SetToolTip(format_gradient_tooltip(physical_indices));

    auto is_hovered = std::make_shared<bool>(false);

    std::vector<unsigned int> component_ids;
    component_ids.reserve(physical_indices.size());
    for (const int index : physical_indices) {
        if (index >= 0 && index < int(m_physical_filaments.size()))
            component_ids.emplace_back(unsigned(index + 1));
    }
    const std::vector<wxColor> colors =
        build_mixed_filament_gradient_preview(component_ids, {}, m_display_context).sampled_colors;

    tile->Bind(wxEVT_PAINT, [tile, colors, is_hovered](wxPaintEvent&) {
        wxPaintDC dc(tile);
        wxSize s = tile->GetClientSize();
        wxString idx = "";
        int padding = *is_hovered ? 0 : tile->FromDIP(2);

        dc.SetBackground(wxBrush(tile->GetParent()->GetBackgroundColour()));
        dc.Clear();

        FilamentCardMixed::paint_clr_swatch_gradient(dc, s, colors, idx, wxGetApp().dark_mode(), padding);
    });

    tile->Bind(wxEVT_ENTER_WINDOW, [tile, is_hovered](wxMouseEvent& e) {
        *is_hovered = true;
        tile->Refresh(); e.Skip();
    });

    tile->Bind(wxEVT_LEAVE_WINDOW, [tile, is_hovered](wxMouseEvent& e) {
        *is_hovered = false;
        tile->Refresh(); e.Skip();
    });

    tile->Bind(wxEVT_LEFT_UP, [this, physical_indices](wxMouseEvent&) {
        if (m_on_preset_selected)
            m_on_preset_selected(physical_indices, {});
    });

    return tile;
}

wxString MFDRecommendationsAccordion::format_gradient_tooltip(const std::vector<int>& phys_indices) const
{
    std::vector<std::string> physical_colors;
    physical_colors.reserve(m_physical_filaments.size());
    for (const auto& f : m_physical_filaments)
        physical_colors.emplace_back(f.first);
    const MixedFilamentDisplayContext ctx = build_mixed_filament_display_context(physical_colors);

    ColorNames::DescriptionOptions opts;
    opts.include_components = true;

    std::string tip = ColorNames::mixed_filament_name(phys_indices, {}, "", ctx.physical_material_types, ctx.physical_colors, opts);
    return from_u8(tip);
}

bool MFDRecommendationsAccordion::is_color_in_between(const wxColor& c1, const wxColor& c2, const wxColor& c3, double max_dev)
{
    double vx = c3.Red() - c1.Red();
    double vy = c3.Green() - c1.Green();
    double vz = c3.Blue() - c1.Blue();
    double v_len2 = vx * vx + vy * vy + vz * vz;
    if (v_len2 < 1e-6) return false;

    double ux = c2.Red() - c1.Red();
    double uy = c2.Green() - c1.Green();
    double uz = c2.Blue() - c1.Blue();

    double dot = ux * vx + uy * vy + uz * vz;
    double p = dot / v_len2;

    if (p <= 0.05 || p >= 0.95) return false;

    double px = ux - p * vx;
    double py = uy - p * vy;
    double pz = uz - p * vz;
    double dist = std::sqrt(px * px + py * py + pz * pz);

    return dist <= max_dev;
}

} // namespace Slic3r::GUI
