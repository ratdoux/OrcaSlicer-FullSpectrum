#include "MixedFilamentDialog.hpp"
#include "MFDColorPickerAccordion.hpp"
#include "MFDPatternSelectorAccordion.hpp"
#include "MFDMaterialAccordion.hpp"
#include "MFDRatioAccordion.hpp"
#include "MFDRecommendationsAccordion.hpp"
#include "MFDPreviewAccordion.hpp"
#include "Widgets/FilamentCardMixed.hpp"


#include <wx/wx.h>
#include <wx/wrapsizer.h>
#include <wx/dcgraph.h>
#include <algorithm>
#include <cmath>

#include "I18N.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Factories.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"

namespace Slic3r::GUI {

MixedFilamentDialog::MixedFilamentDialog(
    wxWindow*                   parent,
    MixedFilamentDialog::Action dialog_action,
    std::vector<std::pair<std::string, std::string>>& physical_filaments
) : DPIDialog(
        parent, 
        wxID_ANY, 
        dialog_action == Action::Add ? _L("Add Mixed Filament") : _L("Edit Mixed Filament"), 
        wxDefaultPosition, 
        wxDefaultSize, 
        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER
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

    SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));

    m_selected_filaments.push_back(0);
    if (m_physical_filaments.size() > 1) {
        m_selected_filaments.push_back(1);
    } else {
        m_selected_filaments.push_back(0);
    }
    m_selected_filaments_weights = get_default_weights(2);
    m_selected_filaments_colors = get_selected_filaments_colors(m_selected_filaments);

    build_ui(parent);
}

void MixedFilamentDialog::build_ui(wxWindow* parent)
{
    SetMinSize(wxSize(m_width_fixed, m_height_min));
    SetMaxSize(wxSize(m_width_fixed, wxDefaultCoord));
    Bind(wxEVT_SIZING, &MixedFilamentDialog::on_sizing, this);
    SetDoubleBuffered(true);
       
    m_main_sizer = new wxBoxSizer(wxVERTICAL);

    m_title_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE);
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
    m_mix_method_sizer = new wxBoxSizer(wxVERTICAL);
    m_mix_method_panel->SetSizer(m_mix_method_sizer);
    build_mix_method_ui(m_mix_method_panel, m_mix_method_sizer);

    m_main_sizer->Add(m_title_panel, 0, wxEXPAND | wxALL, FromDIP(8));
    m_main_sizer->Add(m_mix_method_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));
    m_main_sizer->AddSpacer(FromDIP(4));

    wxPanel* title_divider = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    title_divider->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#EBEBEB")));
    m_main_sizer->Add(title_divider, 0, wxEXPAND);
        
    m_content_panel = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_content_panel->SetDoubleBuffered(true);
    m_content_panel->SetScrollRate(0, 20);
    m_content_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F5F5F5")));
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
        m_preview_layer_stack = compute_pattern_layer_stack(indices, 20);
        
        std::vector<int> all_filaments(m_physical_filaments.size());
        for (size_t i = 0; i < all_filaments.size(); ++i) all_filaments[i] = static_cast<int>(i);
        m_preview_colors = get_selected_filaments_colors(all_filaments);
        
        std::vector<double> weights(m_physical_filaments.size(), 0.0);
        for (int idx : indices) weights[idx - 1] += 1.0;
        double total = indices.size();
        for (double& w : weights) w /= total;
        
        wxColor mixed_color = compute_mixed_color(m_physical_filaments, all_filaments, weights);
        m_preview_accordion->update_preview(m_preview_layer_stack, m_preview_colors, mixed_color);
        if (m_list_preview_panel) m_list_preview_panel->Refresh();
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
        update_preview();
        sync_color_picker_to_mix();
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
    m_content_sizer->Add(m_preview_accordion, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    m_content_sizer->Add(m_recommendations_accordion, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    m_main_sizer->Add(m_content_panel, 1, wxEXPAND);

    m_footer_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE);
    m_footer_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_footer_panel->SetSizer(m_footer_sizer);
        
    // List preview widget taking left 50%
    m_list_preview_panel = new wxPanel(m_footer_panel, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(30)), wxBORDER_NONE);
    m_list_preview_panel->SetMinSize(wxSize(-1, FromDIP(30)));
    m_list_preview_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);

    m_list_preview_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent& event) {
        wxPaintDC context(m_list_preview_panel);
        const wxSize size = m_list_preview_panel->GetClientSize();
        const wxColor background_color = GetBackgroundColour();
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
                    : wxColor("#CECECE");

                context.SetBrush(*wxTRANSPARENT_BRUSH);
                context.SetPen(wxPen(border_color, border_width));
                context.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());
            }
        } else if (m_current_tab == Tab::Gradient) {
            // Draw placeholder/TBD for Gradient (background and border only)
            context.SetBrush(wxBrush(background_color));
            context.SetPen(*wxTRANSPARENT_PEN);
            context.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());

            const int border_width = 1;
            const wxColor border_color = m_is_list_preview_hovered
                ? wxColor(ColorRGB::ORCA().r_uchar(), ColorRGB::ORCA().g_uchar(), ColorRGB::ORCA().b_uchar(), 1)
                : wxColor("#CECECE");

            context.SetBrush(*wxTRANSPARENT_BRUSH);
            context.SetPen(wxPen(border_color, border_width));
            context.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());
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
    footer_divider->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#EBEBEB")));
    m_main_sizer->Add(footer_divider, 0, wxEXPAND);

    m_footer_panel->SetBackgroundColour(GetBackgroundColour());
    m_main_sizer->Add(m_footer_panel, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(8));


    SetSizer(m_main_sizer);
    Layout();
    m_content_panel->FitInside();
    SetSize(m_width_fixed, m_height_start);
    CentreOnParent();
    wxPoint pos = GetPosition();
    pos.x += m_width_fixed;
    SetPosition(pos);

    m_material_accordion->add_combobox_row(0);
    if (m_physical_filaments.size() > 1) {
        m_material_accordion->add_combobox_row(1);
    } else {
        m_material_accordion->add_combobox_row(0);
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
    wxBoxSizer* title_sizer = new wxBoxSizer(wxHORIZONTAL);
    title_panel->SetSizer(title_sizer);

    wxStaticText* title_text = new wxStaticText(title_panel, wxID_ANY, _L("Method:"));
    title_text->SetFont(::Label::Head_14);
    title_text->SetForegroundColour("#333333");

    m_method_manual_radio = new wxRadioButton(title_panel, wxID_ANY, _L("Manual Ratio"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    m_method_manual_radio->SetFont(::Label::Body_14);
    m_method_manual_radio->SetValue(m_mix_method == MixMethod::ManualRatio);

    m_method_by_color_radio = new wxRadioButton(title_panel, wxID_ANY, _L("By Color"));
    m_method_by_color_radio->SetFont(::Label::Body_14);
    m_method_by_color_radio->SetValue(m_mix_method == MixMethod::ByColor);

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

    m_ratio_accordion->Show(m_current_tab == Tab::Mix && m_mix_method == MixMethod::ManualRatio);
    m_recommendations_accordion->Show(m_current_tab == Tab::Mix);
    m_preview_accordion->Show(m_current_tab != Tab::Gradient);

    if (m_current_tab == Tab::Mix && m_mix_method == MixMethod::ByColor) {
        m_color_picker_accordion->expand();
    }
    if (m_current_tab == Tab::Mix && m_mix_method == MixMethod::ManualRatio) {
        m_ratio_accordion->expand();
    }
    if (m_current_tab == Tab::Pattern) {
        m_preview_accordion->expand();
    }

    if (m_current_tab == Tab::Gradient) {
        // Gradient not implemented yet
    }

    m_content_panel->Layout();
    m_content_panel->FitInside();
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

    m_recommendations_accordion->update_recommendations(m_physical_filaments, m_min_weight_ratio);

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
        update_preview(best->filament_indices, best->weights);
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

    m_selected_filaments_weights = weights;
    m_selected_filaments_colors  = get_selected_filaments_colors(m_selected_filaments);

    m_material_accordion->refresh_combobox_items(m_selected_filaments);
    m_material_accordion->refresh_weight_labels(m_selected_filaments_weights);
    m_material_accordion->update_title_preview(m_selected_filaments, m_selected_filaments_weights);
    
    m_ratio_accordion->update_sizing();
    m_ratio_accordion->Refresh();

    m_content_panel->FitInside();

    this->Thaw();

    update_preview();
    sync_color_picker_to_mix();
}
void MixedFilamentDialog::update_preview()
{
    if (m_current_tab == Tab::Pattern) {
        wxString pattern_str = m_pattern_selector_accordion->get_pattern_string();
        std::vector<int> pattern_indices;
        wxString error_msg;
        if (MFDPatternSelectorAccordion::parse_pattern(pattern_str, m_physical_filaments.size(), pattern_indices, error_msg)) {
            m_preview_layer_stack = compute_pattern_layer_stack(pattern_indices, 20);
            
            std::vector<int> all_filaments(m_physical_filaments.size());
            for (size_t i = 0; i < all_filaments.size(); ++i) {
                all_filaments[i] = static_cast<int>(i);
            }
            m_preview_colors = get_selected_filaments_colors(all_filaments);
            
            std::vector<double> weights(m_physical_filaments.size(), 0.0);
            for (int idx : pattern_indices) {
                weights[idx - 1] += 1.0;
            }
            double total = pattern_indices.size();
            for (double& w : weights) {
                w /= total;
            }
            
            wxColor mixed_color = compute_mixed_color(m_physical_filaments, all_filaments, weights);
            m_preview_accordion->update_preview(m_preview_layer_stack, m_preview_colors, mixed_color);
        } else {
            m_preview_accordion->clear_preview();
        }
    } else {
        update_preview(m_selected_filaments, m_selected_filaments_weights);
    }
    if (m_list_preview_panel) m_list_preview_panel->Refresh();
}

void MixedFilamentDialog::update_preview(const std::vector<int>& filaments, const std::vector<double>& weights)
{
    m_preview_layer_stack = compute_layer_stack(weights, 20);
    m_preview_colors = get_selected_filaments_colors(filaments);
    wxColor mixed_color = compute_mixed_color(m_physical_filaments, filaments, weights);
    m_preview_accordion->update_preview(m_preview_layer_stack, m_preview_colors, mixed_color);
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

std::vector<MFDPreviewLayerEntry> MixedFilamentDialog::compute_layer_stack(const std::vector<double>& weights, int total_layers)
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

std::vector<MFDPreviewLayerEntry> MixedFilamentDialog::compute_pattern_layer_stack(const std::vector<int>& pattern_indices, int total_layers)
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
    return StateColor::darkModeColorFor(wxColour("#EBEBEB"));
}

wxColour MixedFilamentDialog::getTabBackgroundColor(bool is_selected, bool is_hovered) const {
    if (is_selected) return wxColour(m_orca_colour.Red(), m_orca_colour.Green(), m_orca_colour.Blue(), 48);
    if (is_hovered)  return StateColor::darkModeColorFor(wxColour("#F5F5F5"));
    return StateColor::darkModeColorFor(*wxWHITE);
}

wxColour MixedFilamentDialog::getTabTextColor(bool is_selected, bool is_hovered) const {
    if (is_selected) return m_orca_colour;
    if (is_hovered)  return m_orca_colour;
    return wxColour("#999999");
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

} // namespace Slic3r::GUI
