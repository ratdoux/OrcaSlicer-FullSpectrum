#include "MixedFilamentDialog.hpp"
#include "MFDColorPickerAccordion.hpp"
#include "MFDPatternSelectorAccordion.hpp"
#include "MFDMaterialAccordion.hpp"
#include "MFDRatioAccordion.hpp"
#include "MFDRecommendationsAccordion.hpp"
#include "MFDPreviewAccordion.hpp"
#include "MFDGradientAccordion.hpp"
#include "MFDTheme.hpp"
#include "Widgets/Accordion.hpp"
#include "Widgets/FilamentCardMixed.hpp"


#include <wx/wx.h>
#include <wx/wrapsizer.h>
#include <wx/dcgraph.h>
#include <wx/dcbuffer.h>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>

#include "I18N.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Factories.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"

namespace Slic3r::GUI {

MixedFilamentDialog::MixedFilamentDialog(
    wxWindow*                   parent,
    MixedFilamentDialog::Action dialog_action,
    std::vector<std::pair<std::string, std::string>>& physical_filaments,
    int                         mixed_idx,
    bool                        start_by_color
) : DPIDialog(
        parent, 
        wxID_ANY, 
        dialog_action == Action::Add ? _L("Add Mixed Filament") : _L("Edit Mixed Filament"), 
        wxDefaultPosition, 
        wxDefaultSize, 
        wxDEFAULT_DIALOG_STYLE
    )
    , m_action(dialog_action)
    , m_physical_filaments(physical_filaments)
{ 
    max_filament = std::clamp((int)physical_filaments.size(), 2, 4);
    m_min_weight_ratio = 0.15;

    m_width_fixed       = this->wxWindow::FromDIP(400);
    m_height_start      = this->wxWindow::FromDIP(800);
    m_height_min        = this->wxWindow::FromDIP(400);
    m_clr_swatch_size   = this->wxWindow::FromDIP(20);

    SetBackgroundColour(MFDTheme::dialog_background());

    bool loaded_preset = false;
    auto* preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle && mixed_idx >= 0 && dialog_action == Action::Edit) {
        auto definitions = preset_bundle->mixed_filaments.mixed_filament_definitions(physical_filaments.size());
        if (mixed_idx < (int)definitions.size()) {
            const auto& def = definitions[mixed_idx];
            
            if (start_by_color) {
                m_current_tab = Tab::Mix;
                m_mix_method = MixMethod::ByColor;

                m_selected_filaments.clear();
                m_selected_filaments_weights.clear();
                for (size_t i = 0; i < def.recipe.blend.components.size() && i < (size_t)max_filament; ++i) {
                    const auto& comp = def.recipe.blend.components[i];
                    m_selected_filaments.push_back(static_cast<int>(comp.filament.id - 1));
                    m_selected_filaments_weights.push_back(comp.percent / 100.0);
                }
            } else if (def.recipe.kind == MixedFilamentRecipeKind::ManualPattern) {
                m_current_tab = Tab::Pattern;

                std::vector<unsigned int> pattern_indices;
                if (def.recipe.manual_pattern) {
                    for (const auto& group : def.recipe.manual_pattern->groups) {
                        for (const auto& ref : group) {
                            pattern_indices.push_back(ref.id);
                        }
                    }
                }

                struct FilamentUsage {
                    int index;
                    int count;
                    int first_pos;
                };

                std::vector<FilamentUsage> usages;
                for (size_t i = 0; i < physical_filaments.size(); ++i) {
                    usages.push_back({static_cast<int>(i), 0, 999999});
                }
                for (int pos = 0; pos < (int)pattern_indices.size(); ++pos) {
                    int idx = pattern_indices[pos] - 1;
                    if (idx >= 0 && idx < (int)usages.size()) {
                        usages[idx].count++;
                        if (usages[idx].first_pos == 999999) {
                            usages[idx].first_pos = pos;
                        }
                    }
                }

                std::vector<FilamentUsage> used_filaments;
                for (const auto& u : usages) {
                    if (u.count > 0) {
                        used_filaments.push_back(u);
                    }
                }

                std::sort(used_filaments.begin(), used_filaments.end(), [](const FilamentUsage& a, const FilamentUsage& b) {
                    if (a.count != b.count) {
                        return a.count > b.count;
                    }
                    return a.first_pos < b.first_pos;
                });

                int limit = max_filament;
                if ((int)used_filaments.size() > limit) {
                    used_filaments.resize(limit);
                }

                m_selected_filaments.clear();
                m_selected_filaments_weights.clear();

                if (!used_filaments.empty()) {
                    double total_count = 0;
                    for (const auto& u : used_filaments) {
                        total_count += u.count;
                    }
                    for (const auto& u : used_filaments) {
                        m_selected_filaments.push_back(u.index);
                        m_selected_filaments_weights.push_back(u.count / total_count);
                    }
                }

                if (m_selected_filaments.size() < 2) {
                    int free_idx = find_first_free_filament();
                    if (free_idx < 0) free_idx = 0;
                    m_selected_filaments.push_back(free_idx);
                    m_selected_filaments_weights = { 1.0, 0.0 };
                }
            } else if (def.recipe.kind == MixedFilamentRecipeKind::WeightedBlend &&
                       def.behavior.distribution != MixedFilamentDistributionMode::Simple) {
                m_current_tab = Tab::Gradient;

                m_selected_filaments.clear();
                m_selected_filaments_weights.clear();
                std::vector<int> component_percents;
                for (size_t i = 0; i < def.recipe.blend.components.size() && i < (size_t)max_filament; ++i) {
                    const auto& comp = def.recipe.blend.components[i];
                    m_selected_filaments.push_back(static_cast<int>(comp.filament.id - 1));
                    m_selected_filaments_weights.push_back(comp.percent / 100.0);
                    component_percents.push_back(comp.percent);
                }
                const size_t expected_stops = m_selected_filaments.size() >= 2 ? 2 * m_selected_filaments.size() - 1 : size_t(0);
                if (expected_stops >= 3 && def.behavior.gradient.stop_positions.size() == expected_stops) {
                    m_gradient_positions.clear();
                    m_gradient_positions.reserve(expected_stops);
                    for (const float position : def.behavior.gradient.stop_positions)
                        m_gradient_positions.emplace_back(std::clamp(double(position), 0.0, 1.0));
                } else {
                    m_gradient_positions = gradient_positions_from_component_percents(component_percents);
                }
            } else {
                m_current_tab = Tab::Mix;

                m_selected_filaments.clear();
                m_selected_filaments_weights.clear();
                for (size_t i = 0; i < def.recipe.blend.components.size() && i < (size_t)max_filament; ++i) {
                    const auto& comp = def.recipe.blend.components[i];
                    m_selected_filaments.push_back(static_cast<int>(comp.filament.id - 1));
                    m_selected_filaments_weights.push_back(comp.percent / 100.0);
                }
            }

            m_bias_value_mm = bias_value_from_definition(def);
            loaded_preset = true;
        }
    }

    if (!loaded_preset) {
        m_selected_filaments.push_back(0);
        if (m_physical_filaments.size() > 1) {
            m_selected_filaments.push_back(1);
        } else {
            m_selected_filaments.push_back(0);
        }
        m_selected_filaments_weights = get_default_weights(2);
    }
    m_selected_filaments_colors = get_selected_filaments_colors(m_selected_filaments);

    build_ui(parent);

    if (loaded_preset && m_current_tab == Tab::Pattern) {
        auto definitions = preset_bundle->mixed_filaments.mixed_filament_definitions(physical_filaments.size());
        if (mixed_idx < (int)definitions.size()) {
            const auto& def = definitions[mixed_idx];
            if (m_pattern_selector_accordion) {
                std::string pat_str;
                if (def.recipe.manual_pattern) {
                    std::ostringstream oss;
                    for (size_t g_idx = 0; g_idx < def.recipe.manual_pattern->groups.size(); ++g_idx) {
                        if (g_idx != 0)
                            oss << ',';
                        for (const auto& ref : def.recipe.manual_pattern->groups[g_idx]) {
                            if (ref.id >= 10) {
                                oss << '[' << ref.id << ']';
                            } else {
                                oss << ref.id;
                            }
                        }
                    }
                    pat_str = oss.str();
                }
                m_pattern_selector_accordion->set_pattern_string(wxString(pat_str));
            }
        }
    }

    update_tabs();
}

void MixedFilamentDialog::build_ui(wxWindow* parent)
{
    SetMinSize(wxSize(m_width_fixed, m_height_min));
    SetMaxSize(wxSize(m_width_fixed, wxDefaultCoord));
    Bind(wxEVT_SIZING, &MixedFilamentDialog::on_sizing, this);
    SetDoubleBuffered(true);
       
    m_main_sizer = new wxBoxSizer(wxVERTICAL);

    m_title_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE);
    m_title_panel->SetBackgroundColour(GetBackgroundColour());
    m_title_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_title_panel->SetSizer(m_title_sizer);
    m_title_panel->SetMinSize(wxSize(-1, FromDIP(30)));

    // "Mix", "Pattern" and "Gradient" Tabs
    // "Mix" Tab
    m_mix_tab_btn = new wxPanel(m_title_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_mix_tab_btn->SetMinSize(wxSize(FromDIP(40), FromDIP(30)));
    m_mix_tab_btn->SetBackgroundColour(GetBackgroundColour());

    m_mix_tab_btn->Bind(wxEVT_PAINT, [this](wxPaintEvent& event) {
        paintTabBtn(m_mix_tab_btn, true, false, FromDIP(6), _L("Mix"), "", m_current_tab == Tab::Mix, m_mix_tab_hovered); 
    });
    m_mix_tab_btn->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
        m_current_tab = Tab::Mix;
        update_tabs();
        update_preview();
    });
    m_mix_tab_btn->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) {
        m_mix_tab_hovered = true; m_mix_tab_btn->Refresh();
    });
    m_mix_tab_btn->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
        m_mix_tab_hovered = false; m_mix_tab_btn->Refresh();
    });

    // "Pattern" Tab
    m_pattern_tab_btn = new wxPanel(m_title_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_pattern_tab_btn->SetMinSize(wxSize(FromDIP(40), FromDIP(30)));
    m_pattern_tab_btn->SetBackgroundColour(GetBackgroundColour());

    m_pattern_tab_btn->Bind(wxEVT_PAINT, [this](wxPaintEvent& event) {
        paintTabBtn(m_pattern_tab_btn, false, false, FromDIP(6), _L("Pattern"), "", m_current_tab == Tab::Pattern, m_pattern_tab_hovered); 
    });
    m_pattern_tab_btn->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
        m_current_tab = Tab::Pattern;
        update_tabs();
        update_preview();
    });
    m_pattern_tab_btn->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) {
        m_pattern_tab_hovered = true; m_pattern_tab_btn->Refresh();
    });
    m_pattern_tab_btn->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
        m_pattern_tab_hovered = false; m_pattern_tab_btn->Refresh();
    });

    // "Gradient" Tab
    m_gradient_tab_btn = new wxPanel(m_title_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_gradient_tab_btn->SetMinSize(wxSize(FromDIP(40), FromDIP(30)));
    m_gradient_tab_btn->SetBackgroundColour(GetBackgroundColour());

    m_gradient_tab_btn->Bind(wxEVT_PAINT, [this](wxPaintEvent& event) {
        paintTabBtn(m_gradient_tab_btn, false, true, FromDIP(6), _L("Gradient"), "", m_current_tab == Tab::Gradient, m_gradient_tab_hovered); 
    });
    m_gradient_tab_btn->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
        m_current_tab = Tab::Gradient;
        update_tabs();
        update_preview();
    });
    m_gradient_tab_btn->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) {
        m_gradient_tab_hovered = true; m_gradient_tab_btn->Refresh();
    });
    m_gradient_tab_btn->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
        m_gradient_tab_hovered = false; m_gradient_tab_btn->Refresh();
    });
   
    m_title_sizer->Add(m_mix_tab_btn, 1, wxEXPAND);
    m_title_sizer->AddSpacer(FromDIP(4));
    m_title_sizer->Add(m_pattern_tab_btn, 1, wxEXPAND);
    m_title_sizer->AddSpacer(FromDIP(4));
    m_title_sizer->Add(m_gradient_tab_btn, 1, wxEXPAND);
    
    // "Method" radio buttons for "Mix" Tab
    m_mix_method_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_mix_method_panel->SetBackgroundColour(GetBackgroundColour());
    m_mix_method_sizer = new wxBoxSizer(wxVERTICAL);
    m_mix_method_panel->SetSizer(m_mix_method_sizer);
    build_mix_method_ui(m_mix_method_panel, m_mix_method_sizer);

    m_main_sizer->Add(m_title_panel, 0, wxEXPAND | wxALL, FromDIP(8));
    m_main_sizer->Add(m_mix_method_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));
    m_main_sizer->AddSpacer(FromDIP(4));

    wxPanel* title_divider = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    title_divider->SetBackgroundColour(MFDTheme::divider());
    m_main_sizer->Add(title_divider, 0, wxEXPAND);
        
    m_content_panel = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_content_panel->SetDoubleBuffered(true);
    m_content_panel->SetScrollRate(0, 20);
    m_content_panel->SetBackgroundColour(MFDTheme::content_background());
    m_content_sizer = new wxBoxSizer(wxVERTICAL);
    m_content_panel->SetSizer(m_content_sizer);

    m_content_panel->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        event.Skip();
        m_content_panel->Refresh();
    });

    m_color_picker_accordion = new MFDColorPickerAccordion(m_content_panel);
    m_color_picker_accordion->set_on_color_changed([this](const wxColour& c, bool drag) {
        on_color_picker_changed(c, drag);
    });

    m_pattern_selector_accordion = new MFDPatternSelectorAccordion(m_content_panel, m_physical_filaments);
    m_pattern_selector_accordion->set_on_pattern_changed([this](const std::vector<int>& indices) {
        update_preview();
    });
    m_pattern_selector_accordion->set_on_pattern_invalid([this]() {
        m_preview_accordion->clear_preview();
        if (m_list_preview_panel) m_list_preview_panel->Refresh();
    });

    m_material_accordion = new MFDMaterialAccordion(m_content_panel, m_physical_filaments, m_clr_swatch_size, min_filament, max_filament);
    m_material_accordion->set_on_add_filament([this]() { add_material_slot(); });
    m_material_accordion->set_on_remove_filament([this]() { remove_last_material_slot(); });
    m_material_accordion->set_on_filament_changed([this](size_t slot, int new_idx) { on_filament_selection_changed(slot, new_idx); });

    m_ratio_accordion = new MFDRatioAccordion(m_content_panel, m_selected_filaments_weights, m_selected_filaments_colors, m_min_weight_ratio);
    m_ratio_accordion->set_on_weights_changed([this]() {
        apply_min_weight_clamping();
        m_material_accordion->refresh_weight_labels(m_selected_filaments_weights);
        m_material_accordion->update_title_preview(m_selected_filaments, m_selected_filaments_weights);
        sync_bias_controls();
        update_preview();
        sync_color_picker_to_mix();
    });

    build_bias_ui();

    m_gradient_accordion = new MFDGradientAccordion(
        m_content_panel,
        m_selected_filaments,
        m_selected_filaments_colors,
        m_gradient_positions,
        m_gradient_min_ratio,
        m_physical_filaments
    );
    m_gradient_accordion->set_on_changed([this]() {
        update_preview();
        if (m_list_preview_panel) m_list_preview_panel->Refresh();
    });
    m_gradient_accordion->set_on_filament_changed([this](size_t slot, int new_phys_idx) {
        on_filament_selection_changed(slot, new_phys_idx);
    });

    m_recommendations_accordion = new MFDRecommendationsAccordion(m_content_panel, m_physical_filaments);
    m_recommendations_accordion->set_on_preset_selected([this](const std::vector<int>& phys, const std::vector<double>& w) {
        set_active_mix(phys, w);
    });

    m_preview_accordion = new MFDPreviewAccordion(m_content_panel);

    m_content_sizer->AddSpacer(FromDIP(8));
    m_content_sizer->Add(m_color_picker_accordion, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    m_content_sizer->Add(m_pattern_selector_accordion, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    m_content_sizer->Add(m_material_accordion, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    m_content_sizer->Add(m_ratio_accordion, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    m_content_sizer->Add(m_bias_accordion, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    m_content_sizer->Add(m_gradient_accordion, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    m_content_sizer->Add(m_preview_accordion, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    m_content_sizer->Add(m_recommendations_accordion, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    m_main_sizer->Add(m_content_panel, 1, wxEXPAND);

    m_footer_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE);
    m_footer_panel->SetBackgroundColour(GetBackgroundColour());
    m_footer_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_footer_panel->SetSizer(m_footer_sizer);
        
    // List preview widget taking left 50%
    m_list_preview_panel = new wxPanel(m_footer_panel, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(30)), wxBORDER_NONE);
    m_list_preview_panel->SetMinSize(wxSize(-1, FromDIP(30)));
    m_list_preview_panel->SetBackgroundColour(m_footer_panel->GetBackgroundColour());
    m_list_preview_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_list_preview_panel->SetDoubleBuffered(true);

    m_list_preview_panel->Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent&) {
        // Do nothing to prevent OS background erasure flickers
    });

    m_list_preview_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent& event) {
        wxAutoBufferedPaintDC context(m_list_preview_panel);
        const wxSize size = m_list_preview_panel->GetClientSize();
        const wxColor background_color = m_list_preview_panel->GetBackgroundColour();
        wxSize swatch_size(FromDIP(20), FromDIP(20));
        bool is_dark = wxGetApp().dark_mode();

        if (m_current_tab == Tab::Mix) {
            std::vector<unsigned int> indices;
            for (int idx : m_selected_filaments) {
                indices.push_back(idx + 1);
            }
            std::vector<int> percentages;
            for (double w : m_selected_filaments_weights) {
                percentages.push_back(static_cast<int>(std::round(w * 100)));
            }
            std::vector<wxColor> colors = m_selected_filaments_colors;

            FilamentCardMixed::paint_box_mix(
                context, size, background_color,
                indices, percentages, colors,
                is_dark, m_is_list_preview_hovered, swatch_size
            );
        } else if (m_current_tab == Tab::Pattern) {
            wxString pattern_str = m_pattern_selector_accordion->get_pattern_string();
            std::vector<int> pattern_indices;
            wxString error_msg;
            if (MFDPatternSelectorAccordion::parse_pattern(pattern_str, m_physical_filaments.size(), pattern_indices, error_msg)) {
                std::vector<unsigned int> indices;
                for (int idx : pattern_indices) {
                    indices.push_back(idx);
                }
                std::vector<wxColor> colors = get_colors_from_indices(pattern_indices);

                FilamentCardMixed::paint_box_pattern(
                    context, size, background_color,
                    indices, colors,
                    is_dark, m_is_list_preview_hovered, swatch_size
                );
            } else {
                context.SetBrush(wxBrush(background_color));
                context.SetPen(*wxTRANSPARENT_PEN);
                context.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());

                const int border_width = 1;
                const wxColor border_color = m_is_list_preview_hovered
                    ? wxColor(ColorRGB::ORCA().r_uchar(), ColorRGB::ORCA().g_uchar(), ColorRGB::ORCA().b_uchar(), 1)
                    : MFDTheme::input_border();

                context.SetBrush(*wxTRANSPARENT_BRUSH);
                context.SetPen(wxPen(border_color, border_width));
                context.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());
            }
        } else if (m_current_tab == Tab::Gradient) {
            std::vector<unsigned int> indices;
            for (int idx : m_selected_filaments) {
                indices.push_back(idx + 1);
            }
            FilamentCardMixed::paint_box_gradient(
                context, size, background_color,
                m_selected_filaments_colors,
                m_gradient_positions,
                indices,
                is_dark, m_is_list_preview_hovered, swatch_size
            );
        }
    });

    m_list_preview_panel->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& event) {
        m_is_list_preview_hovered = true;
        m_list_preview_panel->SetCursor(wxCursor(wxCURSOR_HAND));
        m_list_preview_panel->Refresh();
        event.Skip();
    });

    m_list_preview_panel->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
        m_is_list_preview_hovered = false;
        m_list_preview_panel->SetCursor(wxCursor(wxNullCursor));
        m_list_preview_panel->Refresh();
        event.Skip();
    });

    // Right half sizer for Cancel and OK buttons
    wxBoxSizer* right_sizer = new wxBoxSizer(wxHORIZONTAL);
    
    m_btn_cancel = new Button(m_footer_panel, _L("Cancel"), "", 0, 0, wxID_CANCEL);
    m_btn_cancel->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    m_btn_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        on_cancel();
    });

    m_btn_ok = new Button(m_footer_panel, _L("OK"), "", 0, 0, wxID_OK);
    m_btn_ok->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    m_btn_ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        on_ok();
    });

    right_sizer->AddStretchSpacer(1);
    right_sizer->Add(m_btn_cancel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    right_sizer->Add(m_btn_ok, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    m_footer_sizer->Add(m_list_preview_panel, 1, wxALIGN_CENTER_VERTICAL | wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));
    m_footer_sizer->Add(right_sizer, 1, wxALIGN_CENTER_VERTICAL | wxEXPAND);

    wxPanel* footer_divider = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    footer_divider->SetBackgroundColour(MFDTheme::divider());
    m_main_sizer->Add(footer_divider, 0, wxEXPAND);

    m_main_sizer->Add(m_footer_panel, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(8));


    SetSizer(m_main_sizer);
    Layout();
    m_content_panel->FitInside();
    SetSize(m_width_fixed, m_height_start);
    CentreOnParent();
    wxPoint pos = GetPosition();
    pos.x += m_width_fixed;
    SetPosition(pos);

    for (int idx : m_selected_filaments) {
        m_material_accordion->add_combobox_row(idx);
    }
    m_material_accordion->refresh_combobox_items(m_selected_filaments);

    generate_mix_presets();
    update_tabs();
    update_material_state();
    sync_color_picker_to_mix();
}
void MixedFilamentDialog::build_mix_method_ui(wxPanel* parent, wxBoxSizer* parent_sizer)
{
    wxPanel* title_panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    title_panel->SetBackgroundColour(parent->GetBackgroundColour());
    wxBoxSizer* title_sizer = new wxBoxSizer(wxHORIZONTAL);
    title_panel->SetSizer(title_sizer);

    wxStaticText* title_text = new wxStaticText(title_panel, wxID_ANY, _L("Method:"));
    title_text->SetFont(::Label::Head_14);
    MFDTheme::apply_text(title_text, MFDTheme::secondary_text(), title_panel->GetBackgroundColour());

    m_method_manual_radio = new wxRadioButton(title_panel, wxID_ANY, _L("Manual Ratio"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    m_method_manual_radio->SetFont(::Label::Body_14);
    m_method_manual_radio->SetValue(m_mix_method == MixMethod::ManualRatio);
    MFDTheme::apply_text(m_method_manual_radio, MFDTheme::primary_text(), title_panel->GetBackgroundColour());

    m_method_by_color_radio = new wxRadioButton(title_panel, wxID_ANY, _L("By Color"));
    m_method_by_color_radio->SetFont(::Label::Body_14);
    m_method_by_color_radio->SetValue(m_mix_method == MixMethod::ByColor);
    MFDTheme::apply_text(m_method_by_color_radio, MFDTheme::primary_text(), title_panel->GetBackgroundColour());

    m_method_manual_radio->Bind(wxEVT_RADIOBUTTON, [this](wxCommandEvent&) {
        m_mix_method = MixMethod::ManualRatio;
        update_tabs();
    });
    m_method_by_color_radio->Bind(wxEVT_RADIOBUTTON, [this](wxCommandEvent&) {
        m_mix_method = MixMethod::ByColor;
        update_tabs();
    });

    title_sizer->Add(title_text, 0, wxALIGN_CENTER_VERTICAL);
    title_sizer->Add(m_method_manual_radio, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));
    title_sizer->Add(m_method_by_color_radio, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    parent_sizer->Add(title_panel, 0, wxEXPAND);
}

void MixedFilamentDialog::build_bias_ui()
{
    m_bias_accordion = new Accordion(m_content_panel, _L("Mixed Filament Bias"));

    wxPanel* body = m_bias_accordion->get_body_panel();
    wxBoxSizer* body_sizer = m_bias_accordion->get_body_sizer();
    const wxColour card_bg = MFDTheme::card_background();
    body->SetBackgroundColour(card_bg);

    const wxString bias_tooltip =
        _L("Positive bias recesses the second filament in the pair; negative bias recesses the first filament.\n\n"
           "The color chip shows which filament the current value affects.\n\n"
           "Grouped wall patterns and Local-Z dithering ignore it.");

    wxPanel* row_panel = new wxPanel(body, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    row_panel->SetBackgroundColour(card_bg);
    wxBoxSizer* row_sizer = new wxBoxSizer(wxHORIZONTAL);
    row_panel->SetSizer(row_sizer);

    wxStaticText* label = new wxStaticText(row_panel, wxID_ANY, _L("Bias"));
    label->SetFont(::Label::Body_14);
    label->SetToolTip(bias_tooltip);
    MFDTheme::apply_text(label, MFDTheme::primary_text(), card_bg);

    m_bias_target_swatch = new wxPanel(row_panel, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(34), FromDIP(24)), wxBORDER_NONE);
    m_bias_target_swatch->SetMinSize(wxSize(FromDIP(34), FromDIP(24)));
    m_bias_target_swatch->SetBackgroundColour(card_bg);
    m_bias_target_swatch->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_bias_target_swatch->SetToolTip(bias_tooltip);
    m_bias_target_swatch->Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent&) {});
    m_bias_target_swatch->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(m_bias_target_swatch);
        const wxSize size = m_bias_target_swatch->GetClientSize();
        const wxColour bg = m_bias_target_swatch->GetBackgroundColour();
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(bg));
        dc.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());

        if (!bias_supported()) {
            dc.SetPen(wxPen(MFDTheme::input_border(), 1));
            dc.SetBrush(wxBrush(MFDTheme::input_background()));
            dc.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());
            return;
        }

        FilamentCardMixed::paint_clr_swatch(dc, size, bias_target_color(), bias_target_label(), wxGetApp().dark_mode(), 0);
    });

    const double initial_limit = current_bias_limit_mm();
    m_bias_value_mm = std::clamp(m_bias_value_mm, -initial_limit, initial_limit);
    const int initial_slider_limit = std::max(1, static_cast<int>(std::round(initial_limit * 1000.0)));
    m_bias_slider = new wxSlider(row_panel, wxID_ANY,
                                 static_cast<int>(std::round(m_bias_value_mm * 1000.0)),
                                 -initial_slider_limit,
                                 initial_slider_limit);
    m_bias_slider->SetTickFreq(std::max(1, initial_slider_limit / 4));
    m_bias_slider->SetToolTip(bias_tooltip);

    m_bias_value_input = new wxTextCtrl(row_panel, wxID_ANY,
                                        wxString::Format("%.3f", m_bias_value_mm),
                                        wxDefaultPosition,
                                        wxDefaultSize,
                                        wxTE_PROCESS_ENTER | wxTE_RIGHT);
    m_bias_value_input->SetFont(::Label::Body_14);
    m_bias_value_input->SetMinSize(wxSize(FromDIP(58), -1));
    m_bias_value_input->SetToolTip(bias_tooltip);
    MFDTheme::apply_input(m_bias_value_input);

    wxStaticText* unit_label = new wxStaticText(row_panel, wxID_ANY, _L("mm"));
    unit_label->SetFont(::Label::Body_14);
    unit_label->SetToolTip(bias_tooltip);
    MFDTheme::apply_text(unit_label, MFDTheme::primary_text(), card_bg);

    m_bias_slider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
        if (!m_bias_slider)
            return;

        apply_bias_value(double(m_bias_slider->GetValue()) / 1000.0);
    });

    auto apply_bias_text = [this](wxEvent& event) {
        if (m_bias_value_input) {
            double value = 0.0;
            if (m_bias_value_input->GetValue().ToDouble(&value))
                apply_bias_value(value);
            else
                m_bias_value_input->ChangeValue(wxString::Format("%.3f", m_bias_value_mm));
        }
        event.Skip();
    };
    m_bias_value_input->Bind(wxEVT_TEXT_ENTER, apply_bias_text);
    m_bias_value_input->Bind(wxEVT_KILL_FOCUS, apply_bias_text);

    row_sizer->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    row_sizer->Add(m_bias_target_swatch, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    row_sizer->Add(m_bias_slider, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    row_sizer->Add(m_bias_value_input, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    row_sizer->Add(unit_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    body_sizer->Add(row_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    sync_bias_controls();
}

void MixedFilamentDialog::sync_bias_controls()
{
    if (!m_bias_accordion)
        return;

    m_bias_accordion->Show(m_current_tab == Tab::Mix && mixed_filament_bias_enabled());

    const double limit = current_bias_limit_mm();
    m_bias_value_mm = std::clamp(m_bias_value_mm, -limit, limit);
    const int slider_limit = std::max(1, static_cast<int>(std::round(limit * 1000.0)));
    const int slider_value = std::clamp(static_cast<int>(std::round(m_bias_value_mm * 1000.0)),
                                        -slider_limit,
                                        slider_limit);

    if (m_bias_slider) {
        m_bias_slider->SetRange(-slider_limit, slider_limit);
        m_bias_slider->SetTickFreq(std::max(1, slider_limit / 4));
        if (m_bias_slider->GetValue() != slider_value)
            m_bias_slider->SetValue(slider_value);
        m_bias_slider->Enable(bias_supported());
    }

    if (m_bias_value_input) {
        const wxString formatted = wxString::Format("%.3f", m_bias_value_mm);
        if (m_bias_value_input->GetValue() != formatted)
            m_bias_value_input->ChangeValue(formatted);
        m_bias_value_input->Enable(bias_supported());
    }

    refresh_bias_target_swatch();
}

void MixedFilamentDialog::refresh_bias_target_swatch()
{
    if (m_bias_target_swatch) {
        m_bias_target_swatch->Enable(bias_supported());
        m_bias_target_swatch->Refresh();
    }
}

void MixedFilamentDialog::apply_bias_value(double value)
{
    const double limit = current_bias_limit_mm();
    m_bias_value_mm = std::clamp(value, -limit, limit);

    const int slider_limit = std::max(1, static_cast<int>(std::round(limit * 1000.0)));
    const int slider_value = std::clamp(static_cast<int>(std::round(m_bias_value_mm * 1000.0)),
                                        -slider_limit,
                                        slider_limit);

    if (m_bias_slider) {
        if (m_bias_slider->GetMin() != -slider_limit || m_bias_slider->GetMax() != slider_limit)
            m_bias_slider->SetRange(-slider_limit, slider_limit);
        if (m_bias_slider->GetValue() != slider_value)
            m_bias_slider->SetValue(slider_value);
    }

    if (m_bias_value_input)
        m_bias_value_input->ChangeValue(wxString::Format("%.3f", m_bias_value_mm));

    refresh_bias_target_swatch();
    update_preview();
}

bool MixedFilamentDialog::mixed_filament_bias_enabled() const
{
    auto* preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle)
        return false;

    const std::string key = "mixed_filament_component_bias_enabled";
    if (const ConfigOptionBool* opt = preset_bundle->project_config.option<ConfigOptionBool>(key))
        return opt->value;
    if (const ConfigOptionInt* opt = preset_bundle->project_config.option<ConfigOptionInt>(key))
        return opt->value != 0;

    const DynamicPrintConfig* print_cfg = &preset_bundle->prints.get_edited_preset().config;
    if (const ConfigOptionBool* opt = print_cfg->option<ConfigOptionBool>(key))
        return opt->value;
    if (const ConfigOptionInt* opt = print_cfg->option<ConfigOptionInt>(key))
        return opt->value != 0;

    return false;
}

bool MixedFilamentDialog::bias_supported() const
{
    return mixed_filament_bias_enabled() &&
           m_current_tab == Tab::Mix &&
           m_selected_filaments.size() == 2 &&
           m_selected_filaments[0] != m_selected_filaments[1];
}

std::vector<double> MixedFilamentDialog::nozzle_diameters() const
{
    std::vector<double> values(std::max<size_t>(1, m_physical_filaments.size()), 0.4);
    auto* preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle)
        return values;

    const ConfigOptionFloats* opt = preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
    if (!opt || opt->values.empty())
        return values;

    const size_t opt_count = opt->values.size();
    for (size_t i = 0; i < values.size(); ++i)
        values[i] = std::max(0.05, opt->get_at(unsigned(std::min(i, opt_count - 1))));

    return values;
}

double MixedFilamentDialog::bias_reference_nozzle_mm(unsigned int component_a, unsigned int component_b) const
{
    const std::vector<double> diameters = nozzle_diameters();
    double sum = 0.0;
    int count = 0;

    auto append_if_valid = [&diameters, &sum, &count](unsigned int component_id) {
        if (component_id >= 1 && component_id <= diameters.size()) {
            sum += std::max(0.05, diameters[size_t(component_id - 1)]);
            ++count;
        }
    };

    append_if_valid(component_a);
    append_if_valid(component_b);
    return count > 0 ? sum / double(count) : 0.4;
}

double MixedFilamentDialog::current_bias_limit_mm() const
{
    if (m_selected_filaments.size() < 2)
        return MixedFilamentManager::max_pair_bias_mm(0.4f);

    const double reference_nozzle_mm = bias_reference_nozzle_mm(static_cast<unsigned int>(m_selected_filaments[0] + 1),
                                                                static_cast<unsigned int>(m_selected_filaments[1] + 1));
    return MixedFilamentManager::max_pair_bias_mm(float(reference_nozzle_mm));
}

std::pair<float, float> MixedFilamentDialog::current_bias_surface_offsets(double value) const
{
    if (m_selected_filaments.size() < 2)
        return MixedFilamentManager::surface_offset_pair_from_signed_bias(float(value), 0.4f);

    const double reference_nozzle_mm = bias_reference_nozzle_mm(static_cast<unsigned int>(m_selected_filaments[0] + 1),
                                                                static_cast<unsigned int>(m_selected_filaments[1] + 1));
    return MixedFilamentManager::surface_offset_pair_from_signed_bias(float(value), float(reference_nozzle_mm));
}

double MixedFilamentDialog::bias_value_from_definition(const MixedFilamentDefinition& def) const
{
    if (def.recipe.kind != MixedFilamentRecipeKind::WeightedBlend || !def.recipe.blend.is_pair())
        return 0.0;

    const MixedFilamentPrimaryPairView pair = def.recipe.blend.primary_pair_or();
    const double reference_nozzle_mm = bias_reference_nozzle_mm(pair.component_a.id, pair.component_b.id);
    return MixedFilamentManager::bias_ui_value_from_surface_offsets(def.behavior.surface_bias.component_a_offset_mm,
                                                                    def.behavior.surface_bias.component_b_offset_mm,
                                                                    float(reference_nozzle_mm));
}

wxColour MixedFilamentDialog::bias_target_color() const
{
    if (m_selected_filaments_colors.size() < 2)
        return MFDTheme::input_background();

    if (m_bias_value_mm < -0.0005)
        return m_selected_filaments_colors[0];
    if (m_bias_value_mm > 0.0005)
        return m_selected_filaments_colors[1];

    return compute_mixed_color(m_physical_filaments, m_selected_filaments, m_selected_filaments_weights);
}

wxString MixedFilamentDialog::bias_target_label() const
{
    if (m_selected_filaments.size() < 2)
        return wxEmptyString;

    if (m_bias_value_mm < -0.0005)
        return wxString::Format("%d", m_selected_filaments[0] + 1);
    if (m_bias_value_mm > 0.0005)
        return wxString::Format("%d", m_selected_filaments[1] + 1);

    return _L("0");
}

std::vector<int> MixedFilamentDialog::gradient_component_percents() const
{
    const int count = static_cast<int>(m_selected_filaments.size());
    if (count <= 0)
        return {};

    const int expected_stops = 2 * count - 1;
    if (count == 1 || static_cast<int>(m_gradient_positions.size()) != expected_stops) {
        std::vector<double> equal_weights(size_t(count), 1.0);
        return normalize_gradient_percents(equal_weights);
    }

    std::vector<double> weights(size_t(count), 0.0);
    double left_boundary = 0.0;
    for (int i = 0; i < count; ++i) {
        const double right_boundary =
            i == count - 1 ? 1.0 : std::clamp(m_gradient_positions[size_t(2 * i + 1)], left_boundary, 1.0);
        weights[size_t(i)] = std::max(0.0, right_boundary - left_boundary);
        left_boundary = right_boundary;
    }

    return normalize_gradient_percents(weights);
}

std::vector<double> MixedFilamentDialog::gradient_positions_from_component_percents(const std::vector<int>& percents) const
{
    const int count = static_cast<int>(percents.size());
    if (count < 2)
        return {};

    std::vector<double> weights(size_t(count), 0.0);
    double sum = 0.0;
    for (int i = 0; i < count; ++i) {
        weights[size_t(i)] = double(std::max(0, percents[i]));
        sum += weights[size_t(i)];
    }
    if (sum <= 1e-9)
        weights.assign(size_t(count), 1.0);
    sum = std::accumulate(weights.begin(), weights.end(), 0.0);

    std::vector<double> boundaries(size_t(count + 1), 0.0);
    for (int i = 0; i < count; ++i)
        boundaries[size_t(i + 1)] = boundaries[size_t(i)] + weights[size_t(i)] / sum;
    boundaries.front() = 0.0;
    boundaries.back() = 1.0;

    std::vector<double> positions(size_t(2 * count - 1), 0.0);
    positions.front() = 0.0;
    positions.back() = 1.0;
    for (int i = 0; i < count - 1; ++i)
        positions[size_t(2 * i + 1)] = std::clamp(boundaries[size_t(i + 1)], 0.0, 1.0);
    for (int i = 1; i < count - 1; ++i)
        positions[size_t(2 * i)] = std::clamp((boundaries[size_t(i)] + boundaries[size_t(i + 1)]) * 0.5, 0.0, 1.0);

    return positions;
}

std::vector<int> MixedFilamentDialog::normalize_gradient_percents(const std::vector<double>& weights)
{
    std::vector<int> out(weights.size(), 0);
    if (weights.empty())
        return out;

    double sum = 0.0;
    for (const double weight : weights)
        sum += std::max(0.0, weight);

    if (sum <= 1e-9) {
        const int base = static_cast<int>(100 / weights.size());
        int remainder = static_cast<int>(100 % weights.size());
        for (int& value : out) {
            value = base + (remainder > 0 ? 1 : 0);
            if (remainder > 0)
                --remainder;
        }
        return out;
    }

    std::vector<double> remainders(weights.size(), 0.0);
    int assigned = 0;
    for (size_t i = 0; i < weights.size(); ++i) {
        const double exact = 100.0 * std::max(0.0, weights[i]) / sum;
        out[i] = static_cast<int>(std::floor(exact));
        remainders[i] = exact - out[i];
        assigned += out[i];
    }

    int missing = std::max(0, 100 - assigned);
    while (missing > 0) {
        size_t best_idx = 0;
        double best_remainder = -1.0;
        for (size_t i = 0; i < remainders.size(); ++i) {
            if (remainders[i] > best_remainder) {
                best_remainder = remainders[i];
                best_idx = i;
            }
        }
        ++out[best_idx];
        remainders[best_idx] = 0.0;
        --missing;
    }

    return out;
}

std::pair<int, int> MixedFilamentDialog::cadence_from_pair_percent(int component_b_percent)
{
    const int pct_b = std::clamp(component_b_percent, 0, 100);
    if (pct_b <= 0)
        return {1, 0};
    if (pct_b >= 100)
        return {0, 1};

    const int pct_a = 100 - pct_b;
    const bool b_is_major = pct_b >= pct_a;
    const int major_pct = b_is_major ? pct_b : pct_a;
    const int minor_pct = b_is_major ? pct_a : pct_b;
    const int major_layers = std::max(1, int(std::lround(double(major_pct) / double(std::max(1, minor_pct)))));

    int ratio_a = b_is_major ? 1 : major_layers;
    int ratio_b = b_is_major ? major_layers : 1;
    const int divisor = std::gcd(std::max(0, ratio_a), std::max(0, ratio_b));
    if (divisor > 1) {
        ratio_a /= divisor;
        ratio_b /= divisor;
    }
    return {ratio_a, ratio_b};
}

void MixedFilamentDialog::update_tabs()
{
    m_mix_tab_btn->Refresh();
    m_pattern_tab_btn->Refresh();
    m_gradient_tab_btn->Refresh();

    m_mix_method_panel->Show(m_current_tab == Tab::Mix);
    
    m_color_picker_accordion->Show(m_current_tab == Tab::Mix && m_mix_method == MixMethod::ByColor);
    m_pattern_selector_accordion->Show(m_current_tab == Tab::Pattern);
    
    m_material_accordion->Show(m_current_tab != Tab::Pattern);
    bool can_edit_materials = (m_current_tab == Tab::Mix && m_mix_method == MixMethod::ManualRatio) || (m_current_tab == Tab::Gradient);
    m_material_accordion->update_button_visibility(can_edit_materials);
    m_material_accordion->show_percentages(m_current_tab != Tab::Gradient);

    m_ratio_accordion->Show(m_current_tab == Tab::Mix && m_mix_method == MixMethod::ManualRatio);
    sync_bias_controls();
    m_gradient_accordion->Show(m_current_tab == Tab::Gradient);
    
    bool show_recs = (m_current_tab == Tab::Mix || m_current_tab == Tab::Gradient);
    m_recommendations_accordion->Show(show_recs);
    if (show_recs) {
        m_recommendations_accordion->set_mode(m_current_tab == Tab::Gradient ? MFDRecommendationsAccordion::Mode::Gradient : MFDRecommendationsAccordion::Mode::Mix);
    }
    
    m_preview_accordion->Show(true);

    if (m_current_tab == Tab::Mix && m_mix_method == MixMethod::ByColor) {
        m_color_picker_accordion->expand();
    }
    if (m_current_tab == Tab::Mix && m_mix_method == MixMethod::ManualRatio) {
        m_ratio_accordion->expand();
    }
    if (m_current_tab == Tab::Pattern) {
        m_pattern_selector_accordion->expand();
    }
    if (m_current_tab == Tab::Gradient) {
        m_gradient_accordion->expand();
    }

    m_content_panel->Layout();
    m_content_panel->FitInside();
    if (m_btn_ok) {
        m_btn_ok->Enable(m_current_tab != Tab::Gradient || m_selected_filaments.size() >= 2);
    }
    Layout();
    Refresh();
}

void MixedFilamentDialog::add_material_slot(int selected_filament_index)
{
    if (selected_filament_index < 0) {
        selected_filament_index = find_first_free_filament();
        if (selected_filament_index < 0) selected_filament_index = 0;
    }

    if (m_material_accordion->add_combobox_row(selected_filament_index)) {
        m_selected_filaments.push_back(selected_filament_index);
        update_material_state();
    }
}

void MixedFilamentDialog::remove_last_material_slot()
{
    if (m_material_accordion->remove_last_combobox_row()) {
        m_selected_filaments.pop_back();
        update_material_state();
    }
}

void MixedFilamentDialog::on_filament_selection_changed(size_t slot, int new_phys_idx)
{
    if (slot < m_selected_filaments.size()) {
        auto it = std::find(m_selected_filaments.begin(), m_selected_filaments.end(), new_phys_idx);
        if (it != m_selected_filaments.end() && std::distance(m_selected_filaments.begin(), it) != (std::ptrdiff_t)slot) {
            size_t old_slot = std::distance(m_selected_filaments.begin(), it);
            m_selected_filaments[old_slot] = m_selected_filaments[slot];
            m_material_accordion->set_combobox_selection(old_slot, m_selected_filaments[old_slot]);
        }
        m_selected_filaments[slot] = new_phys_idx;
        m_material_accordion->set_combobox_selection(slot, new_phys_idx);
        update_material_state();
    }
}

int MixedFilamentDialog::find_first_free_filament() const
{
    for (size_t i = 0; i < m_physical_filaments.size(); ++i) {
        if (std::find(m_selected_filaments.begin(), m_selected_filaments.end(), (int)i) == m_selected_filaments.end()) {
            return i;
        }
    }
    return -1;
}

void MixedFilamentDialog::update_material_state()
{
    m_material_accordion->refresh_combobox_items(m_selected_filaments);

    if (m_selected_filaments_weights.size() != m_selected_filaments.size()) {
        m_selected_filaments_weights = get_default_weights(m_selected_filaments.size());
    }

    m_selected_filaments_colors = get_selected_filaments_colors(m_selected_filaments);
    
    m_material_accordion->refresh_weight_labels(m_selected_filaments_weights);
    m_material_accordion->update_title_preview(m_selected_filaments, m_selected_filaments_weights);
    
    m_ratio_accordion->update_sizing();
    m_ratio_accordion->Refresh();

    if (m_gradient_accordion) {
        m_gradient_accordion->update_sizing();
        m_gradient_accordion->sync_data();
    }

    m_recommendations_accordion->update_recommendations(m_physical_filaments, m_min_weight_ratio);

    sync_bias_controls();
    update_preview();
    sync_color_picker_to_mix();
}

void MixedFilamentDialog::apply_min_weight_clamping()
{
    int count = m_selected_filaments_weights.size();
    if (count == 2) MFDRatioAccordion::clamp_weights_2(m_selected_filaments_weights, m_min_weight_ratio);
    else if (count == 3) MFDRatioAccordion::clamp_weights_3(m_selected_filaments_weights, m_min_weight_ratio);
    else if (count == 4) MFDRatioAccordion::clamp_weights_4(m_selected_filaments_weights, m_min_weight_ratio);
}

void MixedFilamentDialog::sync_color_picker_to_mix()
{
    if (m_syncing_from_color_picker || m_current_tab != Tab::Mix || m_mix_method != MixMethod::ByColor)
        return;

    wxColor mixed_color = compute_mixed_color(m_physical_filaments, m_selected_filaments, m_selected_filaments_weights);
    
    m_syncing_from_color_picker = true;
    m_color_picker_accordion->sync_color(mixed_color);
    update_color_match(mixed_color, false);
    m_syncing_from_color_picker = false;

    update_preview();
}

void MixedFilamentDialog::on_color_picker_changed(const wxColour& color, bool is_dragging)
{
    if (m_syncing_from_color_picker) return;
    m_syncing_from_color_picker = true;
    update_color_match(color, !is_dragging);
    m_syncing_from_color_picker = false;
}

void MixedFilamentDialog::update_color_match(const wxColour& selected_color, bool update_active_mix)
{
    const MixPreset* best = find_closest_mix(selected_color);
    if (!best) {
        m_current_deviation = -1.0;
        m_color_picker_accordion->clear_match();
        return;
    }

    if (update_active_mix) {
        set_active_mix(best->filament_indices, best->weights);
    } else {
        m_preview_accordion->update_preview_mix(best->weights, get_selected_filaments_colors(best->filament_indices), best->mixed_color);
        if (m_list_preview_panel) m_list_preview_panel->Refresh();
    }

    double dr = selected_color.Red()   - best->mixed_color.Red();
    double dg = selected_color.Green() - best->mixed_color.Green();
    double db = selected_color.Blue()  - best->mixed_color.Blue();
    m_current_deviation = std::sqrt(dr * dr + dg * dg + db * db);

    m_color_picker_accordion->update_match_status(selected_color, best->mixed_color, m_current_deviation, m_warning_deviation_threshold, m_max_deviation);
}

void MixedFilamentDialog::set_active_mix(const std::vector<int>& physical_filaments, const std::vector<double>& weights)
{
    this->Freeze();

    int target_count = physical_filaments.size();
    int current_count = m_material_accordion->get_combobox_count();

    if (current_count < target_count) {
        for (int i = current_count; i < target_count; ++i) {
            add_material_slot(physical_filaments[i]);
        }
    } else if (current_count > target_count) {
        while (m_material_accordion->get_combobox_count() > target_count) {
            remove_last_material_slot();
        }
    }

    for (int i = 0; i < target_count; ++i) {
        m_selected_filaments[i] = physical_filaments[i];
        m_material_accordion->set_combobox_selection(i, physical_filaments[i]);
    }

    if (!weights.empty()) {
        m_selected_filaments_weights = weights;
    } else {
        m_selected_filaments_weights = get_default_weights(target_count);
    }
    m_selected_filaments_colors  = get_selected_filaments_colors(m_selected_filaments);

    m_material_accordion->refresh_combobox_items(m_selected_filaments);
    m_material_accordion->refresh_weight_labels(m_selected_filaments_weights);
    m_material_accordion->update_title_preview(m_selected_filaments, m_selected_filaments_weights);
    
    m_ratio_accordion->update_sizing();
    m_ratio_accordion->Refresh();

    if (m_gradient_accordion) {
        m_gradient_accordion->update_sizing();
        m_gradient_accordion->sync_data();
    }

    if (m_current_tab == Tab::Gradient && m_gradient_accordion) {
        m_gradient_accordion->reset_to_defaults();
    }

    sync_bias_controls();
    m_content_panel->FitInside();

    this->Thaw();

    update_preview();
    sync_color_picker_to_mix();
}

wxColor MixedFilamentDialog::compute_preview_mixed_color() const
{
    if (!bias_supported())
        return compute_mixed_color(m_physical_filaments, m_selected_filaments, m_selected_filaments_weights);

    const int base_b_percent = std::clamp(static_cast<int>(std::round(m_selected_filaments_weights[1] * 100.0)), 0, 100);
    const auto [component_a_offset, component_b_offset] = current_bias_surface_offsets(m_bias_value_mm);
    const double reference_nozzle_mm = bias_reference_nozzle_mm(static_cast<unsigned int>(m_selected_filaments[0] + 1),
                                                                static_cast<unsigned int>(m_selected_filaments[1] + 1));
    const int apparent_b_percent = MixedFilamentManager::apparent_mix_b_percent(base_b_percent,
                                                                                component_a_offset,
                                                                                component_b_offset,
                                                                                float(reference_nozzle_mm));
    std::vector<double> apparent_weights = {
        double(100 - apparent_b_percent) / 100.0,
        double(apparent_b_percent) / 100.0
    };
    return compute_mixed_color(m_physical_filaments, m_selected_filaments, apparent_weights);
}

void MixedFilamentDialog::update_preview()
{
    if (!m_preview_accordion)
        return;

    if (m_current_tab == Tab::Mix) {
        wxColor mixed_color = compute_preview_mixed_color();
        m_preview_accordion->update_preview_mix(m_selected_filaments_weights, m_selected_filaments_colors, mixed_color);
    } else if (m_current_tab == Tab::Pattern) {
        wxString pattern_str = m_pattern_selector_accordion->get_pattern_string();
        std::vector<int> pattern_indices;
        wxString error_msg;
        if (MFDPatternSelectorAccordion::parse_pattern(pattern_str, m_physical_filaments.size(), pattern_indices, error_msg)) {
            std::vector<int> all_filaments(m_physical_filaments.size());
            for (size_t i = 0; i < all_filaments.size(); ++i) {
                all_filaments[i] = static_cast<int>(i);
            }
            std::vector<wxColor> colors = get_selected_filaments_colors(all_filaments);
            
            std::vector<double> weights(m_physical_filaments.size(), 0.0);
            for (int idx : pattern_indices) {
                weights[idx - 1] += 1.0;
            }
            double total = pattern_indices.size();
            for (double& w : weights) {
                w /= total;
            }
            
            wxColor mixed_color = compute_mixed_color(m_physical_filaments, all_filaments, weights);
            m_preview_accordion->update_preview_pattern(pattern_indices, colors, mixed_color);
        } else {
            m_preview_accordion->clear_preview();
        }
    } else if (m_current_tab == Tab::Gradient) {
        m_preview_accordion->update_preview_gradient(m_selected_filaments_colors, m_gradient_positions);
    }
    if (m_list_preview_panel) m_list_preview_panel->Refresh();
}

// static
wxColor MixedFilamentDialog::compute_mixed_color(
    const std::vector<std::pair<std::string, std::string>>& filaments,
    const std::vector<int>& indices, const std::vector<double>& weights)
{
    double r = 0, g = 0, b = 0;
    for (size_t i = 0; i < indices.size(); ++i) {
        wxColor col(filaments[indices[i]].first);
        r += weights[i] * col.Red();
        g += weights[i] * col.Green();
        b += weights[i] * col.Blue();
    }
    return wxColor(std::clamp((int)r, 0, 255), std::clamp((int)g, 0, 255), std::clamp((int)b, 0, 255));
}

void MixedFilamentDialog::generate_mix_presets()
{
    m_mix_presets.clear();
    int filament_count = m_physical_filaments.size();

    // 2-way mixes
    if (filament_count >= 2) {
        for (int i = 0; i < filament_count; ++i) {
            for (int j = i + 1; j < filament_count; ++j) {
                {
                    std::vector<double> weights = {0.5, 0.5};
                    std::vector<int> indices = {i, j};
                    m_mix_presets.push_back({indices, weights, compute_mixed_color(m_physical_filaments, indices, weights)});
                }
                {
                    std::vector<double> weights = {0.66, 0.34};
                    std::vector<int> indices = {i, j};
                    m_mix_presets.push_back({indices, weights, compute_mixed_color(m_physical_filaments, indices, weights)});
                }
                {
                    std::vector<double> weights = {0.34, 0.66};
                    std::vector<int> indices = {i, j};
                    m_mix_presets.push_back({indices, weights, compute_mixed_color(m_physical_filaments, indices, weights)});
                }
            }
        }
    }

    // 3-way mixes
    if (filament_count >= 3) {
        for (int i = 0; i < filament_count; ++i) {
            for (int j = i + 1; j < filament_count; ++j) {
                for (int k = j + 1; k < filament_count; ++k) {
                    std::vector<double> weights = {0.33, 0.33, 0.34};
                    std::vector<int> indices = {i, j, k};
                    m_mix_presets.push_back({indices, weights, compute_mixed_color(m_physical_filaments, indices, weights)});
                }
            }
        }
    }
}

const MixedFilamentDialog::MixPreset* MixedFilamentDialog::find_closest_mix(const wxColour& target) const
{
    if (m_mix_presets.empty())
        return nullptr;

    const MixPreset* best = nullptr;
    double best_dist = 1e18;

    for (const auto& preset : m_mix_presets) {
        double dr = target.Red()   - preset.mixed_color.Red();
        double dg = target.Green() - preset.mixed_color.Green();
        double db = target.Blue()  - preset.mixed_color.Blue();
        double dist = dr * dr + dg * dg + db * db;
        if (dist < best_dist) {
            best_dist = dist;
            best = &preset;
        }
    }

    return best;
}

std::vector<wxColor> MixedFilamentDialog::get_selected_filaments_colors(const std::vector<int>& filament_indices) const
{
    std::vector<wxColor> colors;
    colors.reserve(filament_indices.size());
    for (int idx : filament_indices) {
        if (idx >= 0 && idx < (int)m_physical_filaments.size()) {
            colors.emplace_back(m_physical_filaments[idx].first);
        } else {
            colors.emplace_back(*wxWHITE);
        }
    }
    return colors;
}

std::vector<double> MixedFilamentDialog::get_default_weights(int filament_count)
{
    switch (filament_count) {
        default:
        case 1: 
            return {1.0};
        case 2: 
            return {0.5, 0.5};
        case 3: 
            return {0.33, 0.33, 0.34};
        case 4: 
            return {0.25, 0.25, 0.25, 0.25};
    }
}



void MixedFilamentDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    m_width_fixed     = FromDIP(400);
    m_height_start    = FromDIP(800);
    m_height_min      = FromDIP(400);
    m_clr_swatch_size = FromDIP(20);

    SetMinSize(wxSize(m_width_fixed, m_height_min));
    Refresh();
}

void MixedFilamentDialog::on_sizing(wxSizeEvent& event)
{
    wxSize size = event.GetSize();
    size.SetWidth(m_width_fixed);
    if (size.GetHeight() < m_height_min) {
        size.SetHeight(m_height_min);
    }
    event.SetSize(size);
}

wxColour MixedFilamentDialog::getTabBorderColor(bool is_selected, bool is_hovered) const {
    if (is_selected) return m_orca_colour;
    if (is_hovered)  return m_orca_colour;
    return MFDTheme::card_border();
}

wxColour MixedFilamentDialog::getTabBackgroundColor(bool is_selected, bool is_hovered) const {
    if (is_selected) return wxColour(m_orca_colour.Red(), m_orca_colour.Green(), m_orca_colour.Blue(), 48);
    if (is_hovered)  return MFDTheme::content_background();
    return MFDTheme::dialog_background();
}

wxColour MixedFilamentDialog::getTabTextColor(bool is_selected, bool is_hovered) const {
    if (is_selected) return m_orca_colour;
    if (is_hovered)  return m_orca_colour;
    return MFDTheme::muted_text();
}

void MixedFilamentDialog::paintTabBtn(wxPanel* panel, bool round_left, bool round_right, double radius, wxString label, wxString icon_name, bool is_selected, bool is_hovered)
{
    wxSize size = panel->GetSize();
    wxAutoBufferedPaintDC dc(panel);
    wxGCDC                gcdc(dc);
    wxGraphicsContext*    gc = gcdc.GetGraphicsContext();
    if (!gc)
        return;

    wxColour background_color = panel->GetBackgroundColour();
    gc->SetBrush(wxBrush(background_color));
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->DrawRectangle(0, 0, size.x, size.y);

    wxColour fill_color   = getTabBackgroundColor(is_selected, is_hovered);
    wxColour border_color = getTabBorderColor(is_selected, is_hovered);
    wxColour text_color   = getTabTextColor(is_selected, is_hovered);

    const double border_width = 2;
    const double x            = border_width / 2;
    const double y            = border_width / 2;
    const double width        = size.x - border_width;
    const double height       = size.y - border_width;

    double tl = round_left ? radius : 0.0;
    double bl = round_left ? radius : 0.0;
    double tr = round_right ? radius : 0.0;
    double br = round_right ? radius : 0.0;

    wxGraphicsPath path = gc->CreatePath();
    path.MoveToPoint(x + tl, y);
    // Top edge
    path.AddLineToPoint(x + width - tr, y);

    // Top-right corner
    if (tr > 0)
        path.AddArc(x + width - tr, y + tr, tr, -M_PI / 2, 0, true);
    // Right edge
    path.AddLineToPoint(x + width, y + height - br);

    // Bottom-right corner
    if (br > 0)
        path.AddArc(x + width - br, y + height - br, br, 0, M_PI / 2, true);
    // Bottom edge
    path.AddLineToPoint(x + bl, y + height);

    // Bottom-left corner
    if (bl > 0)
        path.AddArc(x + bl, y + height - bl, bl, M_PI / 2, M_PI, true);
    // Left edge
    path.AddLineToPoint(x, y + tl);

    // Top-left corner
    if (tl > 0)
        path.AddArc(x + tl, y + tl, tl, M_PI, -M_PI / 2, true);
    path.CloseSubpath();

    gc->SetBrush(wxBrush(fill_color));
    gc->SetPen(wxPen(border_color, border_width));
    gc->DrawPath(path);


    wxFont font = this->GetFont();
    font.SetPointSize(font.GetPointSize() + 6);
    if (is_selected)
        font = font.Bold();
    gc->SetFont(font, text_color);

    // Measure text to center it
    double text_width, text_height;
    gc->GetTextExtent(label, &text_width, &text_height);

    // Center icon + text horizontally
    double total_width = text_width;
    double text_y      = ((size.y - text_height) / 2.0) - FromDIP(2); // center visually
    double start_x     = (size.x - total_width) / 2.0;
    double icon_width  = 0.0;

    // If specified, load icon
    if (icon_name != "") {
        wxBitmap icon_bmp  = ScalableBitmap(panel, icon_name.ToStdString(), 14).bmp();
        wxSize   icon_size = icon_bmp.GetSize();
        double icon_y      = (size.y - icon_size.y) / 2.0;

        total_width += icon_size.x + FromDIP(4);
        start_x = (size.x - total_width) / 2.0;
        icon_width = +icon_size.x + FromDIP(4);

        // Draw icon
        if (icon_bmp.IsOk()) {
            gc->DrawBitmap(icon_bmp, start_x, icon_y, icon_size.x, icon_size.y);
        }
    }

    // Draw text
    gc->DrawText(label, start_x + icon_width, text_y);
}

void MixedFilamentDialog::on_ok()
{
    EndModal(wxID_OK);
}

void MixedFilamentDialog::on_cancel()
{
    EndModal(wxID_CANCEL);
}

std::vector<wxColor> MixedFilamentDialog::get_colors_from_indices(const std::vector<int>& indices) const
{
    std::vector<wxColor> colors;
    colors.reserve(indices.size());
    for (int idx : indices) {
        if (idx > 0 && idx <= (int)m_physical_filaments.size()) {
            colors.push_back(wxColor(m_physical_filaments[idx - 1].first));
        } else {
            colors.push_back(*wxBLACK);
        }
    }
    return colors;
}

MixedFilamentDefinition MixedFilamentDialog::get_result() const
{
    MixedFilamentDefinition def;
    def.source.kind = MixedFilamentSourceKind::Custom;
    def.source.origin_auto = false;
    def.visibility.tombstoned = false;

    if (m_current_tab == Tab::Mix) {
        def.recipe.kind = MixedFilamentRecipeKind::WeightedBlend;
        def.behavior.distribution = MixedFilamentDistributionMode::Simple;
        for (size_t i = 0; i < m_selected_filaments.size(); ++i) {
            unsigned int phys_id = static_cast<unsigned int>(m_selected_filaments[i] + 1);
            int percent = std::clamp(static_cast<int>(std::round(m_selected_filaments_weights[i] * 100.0)), 0, 100);
            def.recipe.blend.components.push_back({{phys_id}, percent});
        }

        if (def.recipe.blend.is_pair()) {
            const MixedFilamentPrimaryPairView pair = def.recipe.blend.primary_pair_or();
            const double reference_nozzle_mm = bias_reference_nozzle_mm(pair.component_a.id, pair.component_b.id);
            const double bias_limit = MixedFilamentManager::max_pair_bias_mm(float(reference_nozzle_mm));
            const auto surface_offsets = MixedFilamentManager::surface_offset_pair_from_signed_bias(
                float(std::clamp(m_bias_value_mm, -bias_limit, bias_limit)),
                float(reference_nozzle_mm));
            def.behavior.surface_bias.component_a_offset_mm = surface_offsets.first;
            def.behavior.surface_bias.component_b_offset_mm = surface_offsets.second;
        }
    } else if (m_current_tab == Tab::Pattern) {
        def.recipe.kind = MixedFilamentRecipeKind::ManualPattern;
        def.behavior.distribution = MixedFilamentDistributionMode::Simple;

        wxString pattern_str = m_pattern_selector_accordion->get_pattern_string();
        std::vector<int> pattern_indices;
        wxString error_msg;
        if (MFDPatternSelectorAccordion::parse_pattern(pattern_str, m_physical_filaments.size(), pattern_indices, error_msg)) {
            std::vector<MixedFilamentPhysicalRef> refs;
            for (int idx : pattern_indices) {
                refs.push_back({static_cast<unsigned int>(idx)});
            }
            def.recipe.manual_pattern = MixedFilamentManualPattern{{refs}};

            // Rebuild blend components from pattern
            std::vector<double> weights(m_physical_filaments.size(), 0.0);
            for (int idx : pattern_indices) {
                if (idx > 0 && idx <= (int)m_physical_filaments.size()) {
                    weights[idx - 1] += 1.0;
                }
            }
            double total = pattern_indices.size();
            for (size_t i = 0; i < weights.size(); ++i) {
                if (weights[i] > 0.0) {
                    unsigned int phys_id = static_cast<unsigned int>(i + 1);
                    int percent = std::clamp(static_cast<int>(std::round((weights[i] / total) * 100.0)), 0, 100);
                    def.recipe.blend.components.push_back({{phys_id}, percent});
                }
            }
        }
    } else if (m_current_tab == Tab::Gradient) {
        def.recipe.kind = MixedFilamentRecipeKind::WeightedBlend;
        def.behavior.distribution = MixedFilamentDistributionMode::LayerCycle;
        def.behavior.gradient.enabled = true;
        def.behavior.gradient.component_a_start = MixedFilamentLegacyRow::k_default_gradient_dominant;
        def.behavior.gradient.component_a_end   = MixedFilamentLegacyRow::k_default_gradient_minority;
        def.behavior.gradient.stop_positions.clear();
        def.behavior.gradient.stop_positions.reserve(m_gradient_positions.size());
        for (const double position : m_gradient_positions)
            def.behavior.gradient.stop_positions.emplace_back(float(std::clamp(position, 0.0, 1.0)));
        const std::vector<int> percents = gradient_component_percents();
        for (size_t i = 0; i < m_selected_filaments.size(); ++i) {
            unsigned int phys_id = static_cast<unsigned int>(m_selected_filaments[i] + 1);
            int percent = i < percents.size() ? std::clamp(percents[i], 0, 100) : 0;
            def.recipe.blend.components.push_back({{phys_id}, percent});
        }

        if (def.recipe.blend.is_pair()) {
            const int component_b_percent = def.recipe.blend.primary_pair_or().component_b_percent;
            const auto [ratio_a, ratio_b] = cadence_from_pair_percent(component_b_percent);
            def.behavior.layer_cadence.component_a_layers = ratio_a;
            def.behavior.layer_cadence.component_b_layers = ratio_b;
        }
    }

    return def;
}

} // namespace Slic3r::GUI
