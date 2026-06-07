#include "MixedFilamentDialog.hpp"

#include <wx/wx.h>
#include <wx/wrapsizer.h>

#include "I18N.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Factories.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DropDown.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/FilamentCardMixed.hpp"
#include "Widgets/MixedFilamentRatioPanel.hpp"
#include <wx/dcgraph.h>

namespace Slic3r::GUI {

MixedFilamentDialog::MixedFilamentDialog(
    wxWindow*                   parent,
    MixedFilamentDialog::Action dialog_action,
    std::vector<std::pair<std::string, std::string>>&   physical_filaments
) : DPIDialog(
        parent, 
        wxID_ANY, 
        dialog_action == MixedFilamentDialog::Action::Add ? _L("Add Mixed Filament") : _L("Edit Mixed Filament"), 
        wxDefaultPosition, 
        wxDefaultSize, 
        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER
    ), m_physical_filaments(physical_filaments)
    , m_action(dialog_action)
{ 
    max_filament = std::clamp((int)physical_filaments.size(), 2, 4);
    m_min_weight_ratio = 0.15;

	m_width_fixed       = this->wxWindow::FromDIP(400);
    m_height_start      = this->wxWindow::FromDIP(600);
    m_height_min        = this->wxWindow::FromDIP(400);
    m_height_max_auto   = this->wxWindow::FromDIP(800);
    m_clr_swatch_size   = this->wxWindow::FromDIP(20);

    SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
	build_ui(parent);
}

void MixedFilamentDialog::build_ui(wxWindow* parent)
{
    SetMinSize(wxSize(m_width_fixed, m_height_min));
    SetMaxSize(wxSize(m_width_fixed, wxDefaultCoord));
    Bind(wxEVT_SIZING, &MixedFilamentDialog::on_sizing, this);
       
    m_main_sizer = new wxBoxSizer(wxVERTICAL);

    // Title
    m_title_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE);
    m_title_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_title_panel->SetSizer(m_title_sizer);
    m_title_panel->SetMinSize(wxSize(-1, FromDIP(30)));

    // Mix Tab
    m_mix_tab_btn = new wxPanel(m_title_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_mix_tab_btn->SetMinSize(wxSize(FromDIP(40), FromDIP(30)));
    m_mix_tab_btn->SetBackgroundColour(GetBackgroundColour());

    m_mix_tab_btn->Bind(wxEVT_PAINT, [this](wxPaintEvent& event) {
        paintTabBtn(m_mix_tab_btn, true, false, FromDIP(6), _L("Mix"), "" /*"ams_readonly"*/, m_current_tab == Tab::Mix, m_mix_tab_hovered); 
    });
    m_mix_tab_btn->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
        m_current_tab = Tab::Mix;
        update_tabs();
    });
    m_mix_tab_btn->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) {
            m_mix_tab_hovered = true;
        update_tabs();
    });
        m_mix_tab_btn->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
        m_mix_tab_hovered = false;
        update_tabs();
    });

    // Pattern Tab 
    m_pattern_tab_btn = new wxPanel(m_title_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_pattern_tab_btn->SetMinSize(wxSize(FromDIP(40), FromDIP(30)));
    m_pattern_tab_btn->SetBackgroundColour(GetBackgroundColour());

    m_pattern_tab_btn->Bind(wxEVT_PAINT, [this](wxPaintEvent& event) {
        paintTabBtn(m_pattern_tab_btn, false, true, FromDIP(6), _L("Pattern"), "" /*"ams_drying"*/, m_current_tab == Tab::Pattern, m_pattern_tab_hovered); 
    });
    m_pattern_tab_btn->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
        m_current_tab = Tab::Pattern;
        update_tabs();
    });
    m_pattern_tab_btn->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) {
        m_pattern_tab_hovered = true;
        update_tabs();
    });
    m_pattern_tab_btn->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
        m_pattern_tab_hovered = false;
        update_tabs();
    });
   
    m_title_sizer->Add(m_mix_tab_btn, 1, wxEXPAND);
    m_title_sizer->AddSpacer(FromDIP(4));
    m_title_sizer->Add(m_pattern_tab_btn, 1, wxEXPAND);
      
    m_main_sizer->Add(m_title_panel, 0, wxEXPAND | wxALL, FromDIP(8));
        
    // Content (scrollable)
    m_content_panel = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_content_panel->SetScrollRate(0, 20);
    m_content_panel->SetBackgroundColour(GetBackgroundColour());
    m_content_sizer = new wxBoxSizer(wxVERTICAL);
    m_content_panel->SetSizer(m_content_sizer);

    // refresh m_content_panel on resize (due to e.g. scrollbar appearing) to prevent rendering artifacts
    m_content_panel->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        event.Skip();
        m_content_panel->Refresh();
    });

    // Material Section
    m_material_panel = new wxPanel(m_content_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_material_sizer = new wxBoxSizer(wxVERTICAL);
    m_material_panel->SetSizer(m_material_sizer);

    // Material Title (incl buttons)
    m_material_title_panel = new wxPanel(m_material_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_material_title_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_material_title_panel->SetSizer(m_material_title_sizer);

    m_material_title_text = new wxStaticText(m_material_title_panel, wxID_ANY, _L("Select Mixed Materials"));
    m_material_title_text->SetForegroundColour("#7e7e7e");
    m_material_title_text->SetFont(::Label::Body_14);

    m_delete_material_btn = new ScalableButton(m_material_title_panel, wxID_ANY, "delete_filament");
    m_delete_material_btn->SetBackgroundColour(GetBackgroundColour());
    m_delete_material_btn->SetToolTip(_L("Remove last material"));
    m_delete_material_btn->Enable(false); // Enable when filament > min_filament
    m_delete_material_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { 
        remove_material_combobox(); 
    });

    m_add_material_btn = new ScalableButton(m_material_title_panel, wxID_ANY, "add_filament");
    m_add_material_btn->SetBackgroundColour(GetBackgroundColour());
    m_add_material_btn->SetToolTip(_L("Add material"));
    m_add_material_btn->Enable(true); // Enable when filament < max_filament
    m_add_material_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { 
        add_material_combobox(m_material_combobox_panel, m_material_combobox_sizer);
    });

    m_material_title_sizer->Add(m_material_title_text, 0, wxALIGN_CENTER_VERTICAL);
    m_material_title_sizer->AddStretchSpacer();
    m_material_title_sizer->Add(m_delete_material_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    m_material_title_sizer->Add(m_add_material_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));

    // Material Selections
    m_material_combobox_panel = new wxPanel(m_material_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_material_combobox_sizer = new wxBoxSizer(wxVERTICAL);
    m_material_combobox_panel->SetSizer(m_material_combobox_sizer);


    m_material_sizer->Add(m_material_title_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8)); 
    m_material_sizer->Add(m_material_combobox_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));

    m_content_sizer->Add(m_material_panel, 0, wxEXPAND | wxBOTTOM, FromDIP(8));

    // Ratio Section (groups ratio title, ratio panel, min weight slider)
    m_ratio_section_panel = new wxPanel(m_content_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_ratio_section_sizer = new wxBoxSizer(wxVERTICAL);
    m_ratio_section_panel->SetSizer(m_ratio_section_sizer);

    // Mix Gradient Selector
    m_mix_ratio_title_panel = new wxPanel(m_ratio_section_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);

    m_mix_ratio_title_text = new wxStaticText(m_mix_ratio_title_panel, wxID_ANY, _L("Select Ratio"));
    m_mix_ratio_title_text->SetForegroundColour("#7e7e7e");
    m_mix_ratio_title_text->SetFont(::Label::Body_14);

    m_ratio_section_sizer->Add(m_mix_ratio_title_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));

    m_mix_ratio_panel = new MixedFilamentRatioPanel(m_ratio_section_panel, m_selected_filaments_weights, m_selected_filaments_colors, m_min_weight_ratio,
                                                    [this]() { refresh_material_weight_labels(); });
    m_mix_ratio_sizer = new wxBoxSizer(wxVERTICAL);
    m_mix_ratio_panel->SetSizer(m_mix_ratio_sizer);

    m_ratio_section_sizer->Add(m_mix_ratio_panel, 0, wxEXPAND);

    // Min Weight Ratio Selector
    m_min_weight_panel = new wxPanel(m_ratio_section_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_min_weight_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_min_weight_panel->SetSizer(m_min_weight_sizer);

    m_min_weight_label = new wxStaticText(m_min_weight_panel, wxID_ANY, _L("Min Weight Ratio:"));
    m_min_weight_label->SetFont(::Label::Body_14);

    // Initial max is 100/2 = 50. Will be dynamically updated by update_min_weight_slider_bounds
    m_min_weight_slider      = new wxSlider(m_min_weight_panel, wxID_ANY, m_min_weight_ratio * 100, 0, 50);
    m_min_weight_slider->SetTickFreq(10);

    m_min_weight_value_input = new wxTextCtrl(m_min_weight_panel, wxID_ANY, wxString::Format("%d", static_cast<int>(m_min_weight_ratio * 100)));
    m_min_weight_value_input->SetFont(::Label::Body_14);
    m_min_weight_value_input->SetMinSize(wxSize(FromDIP(40), -1));

    m_min_weight_value_label = new wxStaticText(m_min_weight_panel, wxID_ANY, "%");
    m_min_weight_value_input->SetFont(::Label::Body_14);

    m_min_weight_slider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
        int new_percentage = m_min_weight_slider->GetValue();
        m_min_weight_value_input->SetValue(wxString::Format("%d", new_percentage));
        apply_min_weight(new_percentage);
    });

    m_min_weight_value_input->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
        long     new_percentage;
        wxString text = m_min_weight_value_input->GetValue();
        text.Replace("%", ""); // Forgive the user if they accidentally type the % symbol

        if (text.ToLong(&new_percentage)) {
            int max_percentage     = m_min_weight_slider->GetMax();
            int clamped_percentage = std::clamp(static_cast<int>(new_percentage), 0, max_percentage);

            // Only update if the value changed to avoid circular updates
            if (m_min_weight_slider->GetValue() != clamped_percentage) {
                m_min_weight_slider->SetValue(clamped_percentage);
                m_min_weight_value_input->SetValue(wxString::Format("%d", clamped_percentage)); // Update input in case it was clamped

                apply_min_weight(clamped_percentage);
            }
        }
    });

    // Clear non-numerical text
    auto format_text_input = [this](wxEvent& event) {
        int val = m_min_weight_slider->GetValue();
        m_min_weight_value_input->ChangeValue(wxString::Format("%d", val));
        event.Skip();
    };
    m_min_weight_value_input->Bind(wxEVT_KILL_FOCUS, format_text_input);
    m_min_weight_value_input->Bind(wxEVT_TEXT_ENTER, format_text_input);

    m_min_weight_sizer->Add(m_min_weight_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));
    m_min_weight_sizer->Add(m_min_weight_slider, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    m_min_weight_sizer->Add(m_min_weight_value_input, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    m_min_weight_sizer->Add(m_min_weight_value_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    m_ratio_section_sizer->Add(m_min_weight_panel, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(8));

    m_content_sizer->Add(m_ratio_section_panel, 0, wxEXPAND | wxTOP, FromDIP(8));

    // Mixing Recommendations
    m_recommendations_panel = new wxPanel(m_content_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_recommendations_sizer = new wxBoxSizer(wxVERTICAL);
    m_recommendations_panel->SetSizer(m_recommendations_sizer);

    m_recommendations_title_panel = new wxPanel(m_recommendations_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_recommendations_title_panel->SetBackgroundColour(this->GetBackgroundColour());
    m_recommendations_title_text = new wxStaticText(m_recommendations_title_panel, wxID_ANY, _L("Mixing Recommendations"));
    m_recommendations_title_text->SetForegroundColour("#7e7e7e");
    m_recommendations_title_text->SetFont(::Label::Body_14);

    m_recommendations_title_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_recommendations_title_panel->SetSizer(m_recommendations_title_sizer);
    m_recommendations_title_sizer->Add(m_recommendations_title_text, 0, wxALL, FromDIP(8));

    m_recommendations_mix_panel = new wxPanel(m_recommendations_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_recommendations_mix_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F5F5F5")));
    m_recommendations_mix_sizer = new wxBoxSizer(wxVERTICAL);
    m_recommendations_mix_panel->SetSizer(m_recommendations_mix_sizer);

    m_recommendations_sizer->Add(m_recommendations_title_panel, 0, wxEXPAND);
    fill_recommendations(m_recommendations_mix_panel, m_recommendations_mix_sizer);

    m_content_sizer->Add(m_recommendations_panel, 0, wxEXPAND | wxTOP, FromDIP(8));

    m_main_sizer->Add(m_content_panel, 1, wxEXPAND);

    // Footer
    m_footer_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE);
    m_footer_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_footer_panel->SetSizer(m_footer_sizer);
        
    Label* footer_placeholder_label = new Label(m_footer_panel, _L("Footer goes here..."));
    m_footer_sizer->Add(footer_placeholder_label, 0, wxALIGN_CENTER | wxALL, FromDIP(8));

    m_main_sizer->Add(m_footer_panel, 0, wxEXPAND);



    SetSizer(m_main_sizer);
	Layout();
    m_content_panel->FitInside();
    SetSize(m_width_fixed, m_height_start);
    CentreOnParent();

    update_tabs();
    add_material_combobox(m_material_combobox_panel, m_material_combobox_sizer);
    add_material_combobox(m_material_combobox_panel, m_material_combobox_sizer);

    update_content_max_height();
}

wxColour MixedFilamentDialog::getTabBorderColor(bool is_selected, bool is_hovered) const
{ 
    const wxColor selected_color(ColorRGB::ORCA().r_uchar(), ColorRGB::ORCA().g_uchar(), ColorRGB::ORCA().b_uchar());
    const wxColor normal_color("#4F4F4F");
    const wxColor hover_color(selected_color);

    if (is_selected)
        return selected_color;
    else if (is_hovered)
        return hover_color;
    else
        return normal_color;
}

wxColour MixedFilamentDialog::getTabBackgroundColor(bool is_selected, bool is_hovered) const 
{ 
    const wxColor selected_color(ColorRGB::ORCA().r_uchar(), ColorRGB::ORCA().g_uchar(), ColorRGB::ORCA().b_uchar());
    const wxColor hover_color(GetBackgroundColour());
    const wxColor normal_color(GetBackgroundColour());

    if (is_selected) 
        return selected_color;
    else if (is_hovered) 
        return hover_color;
    else
        return normal_color;
}

wxColour MixedFilamentDialog::getTabTextColor(bool is_selected, bool is_hovered) const 
{ 
    const wxColor selected_color(*wxWHITE);
    const wxColor hover_color(ColorRGB::ORCA().r_uchar(), ColorRGB::ORCA().g_uchar(), ColorRGB::ORCA().b_uchar());
    const wxColor normal_color("#4F4F4F");

    if (is_selected)
        return selected_color;
    else if (is_hovered)
        return hover_color;
    else
        return normal_color;
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
    wxColour text_color   = getTabTextColor(is_selected, is_hovered);;

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

void MixedFilamentDialog::update_tabs()
{
    // TODO: update content panel based on the current tab selection
     
        
    // redraw tabs
    m_mix_tab_btn->Refresh();
    m_pattern_tab_btn->Refresh();
}

void MixedFilamentDialog::add_material_combobox(wxPanel* parent, wxBoxSizer* sizer, int selected_filament_index) {
    const int current_count             = m_material_comboboxes.size();
    const int new_count                 = current_count + 1;
    if (selected_filament_index == -1) {
        selected_filament_index = find_first_free_filament();
    }
    if (new_count > max_filament || selected_filament_index == -1)
        return;

    parent->Freeze();

    const wxSize combobox_size  = wxSize(FromDIP(166), FromDIP(30));

    wxPanel* combobox_panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE);
    wxBoxSizer* combobox_sizer = new wxBoxSizer(wxHORIZONTAL);
    combobox_panel->SetSizer(combobox_sizer);

    // Label
    wxString      combobox_label_text = wxString::Format(_L("Filament %d"), new_count);
    wxStaticText* combobox_label      = new wxStaticText(combobox_panel, wxID_ANY, combobox_label_text);
    combobox_label->SetFont(::Label::Body_12);

    // Combobox
    ComboBox* combobox = new ComboBox(combobox_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, combobox_size, 0, nullptr,
                                        wxCB_READONLY);
    combobox->SetMinSize(combobox_size);
    combobox->SetKeepDropArrow(true);

    // Weight (updated in refresh_material_weight_labels)
    wxString      combobox_weight_label_text = wxString("--%");
    wxStaticText* combobox_weight_label      = new wxStaticText(combobox_panel, wxID_ANY, combobox_weight_label_text);
    combobox_weight_label->SetMinSize(wxSize(FromDIP(30), -1));
    combobox_weight_label->SetFont(::Label::Body_12);

    combobox_sizer->Add(combobox_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    combobox_sizer->Add(combobox, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    combobox_sizer->Add(combobox_weight_label, 0, wxALIGN_CENTER_VERTICAL);

    // Combobox items (updated in refresh_material_combobox_items)
    for (size_t i = 0; i < m_physical_filaments.size(); ++i) {
        const auto& [color_hex, name] = m_physical_filaments[i];
        wxColor color(color_hex);
        wxString index = wxString::Format("%zu", i + 1);

        wxBitmap   clr_swatch_bmp(m_clr_swatch_size, m_clr_swatch_size);
        wxMemoryDC dc(clr_swatch_bmp);
        FilamentCardMixed::paint_clr_swatch(dc, wxSize(m_clr_swatch_size, m_clr_swatch_size), color, index, wxGetApp().dark_mode());

        wxString text(name);
        combobox->Append(text, clr_swatch_bmp);
    }

    combobox->SetSelection(selected_filament_index);
    m_selected_filaments.push_back(selected_filament_index);

    size_t index = current_count;
    combobox->Bind(wxEVT_COMBOBOX, [this, index](wxCommandEvent&) {
        on_selected_filaments_changed(index);
    });

    m_material_comboboxes.push_back(combobox);
    m_material_weight_labels.push_back(combobox_weight_label);

    sizer->Add(combobox_panel, 0, wxEXPAND | wxTOP, FromDIP(8));

    m_delete_material_btn->Enable(new_count > min_filament);
    m_add_material_btn->Enable(new_count < max_filament);
                
    Layout();

    combobox->SetFocus(); // focus after layout

    m_selected_filaments_weights = get_default_weights(m_selected_filaments.size());
    m_selected_filaments_colors  = get_selected_filaments_colors(m_selected_filaments);
    
    update_min_weight_slider_bounds();
    refresh_material_combobox_items();
    refresh_material_weight_labels();
    m_mix_ratio_panel->update_sizing();
    m_mix_ratio_panel->Refresh();

    m_content_panel->FitInside();
    update_content_max_height();
    auto_resize_dialog_to_fit();

    parent->Thaw();
}

void MixedFilamentDialog::remove_material_combobox() {
    const int current_count = m_material_comboboxes.size();

    if (current_count <= min_filament)
        return;

    ComboBox* last_combobox = m_material_comboboxes.back();

    m_material_combobox_sizer->Detach(last_combobox->GetParent());
    last_combobox->GetParent()->Destroy();

    m_material_comboboxes.pop_back();
    m_material_weight_labels.pop_back();
    m_selected_filaments.pop_back();

    const int new_count = m_material_comboboxes.size();
    m_delete_material_btn->Enable(new_count > min_filament);
    m_add_material_btn->Enable(new_count < max_filament);

    Layout();

    m_selected_filaments_weights = get_default_weights(m_selected_filaments.size());
    m_selected_filaments_colors  = get_selected_filaments_colors(m_selected_filaments);

    update_min_weight_slider_bounds();
    refresh_material_combobox_items();
    refresh_material_weight_labels();
    m_mix_ratio_panel->update_sizing();
    m_mix_ratio_panel->Refresh();

    m_content_panel->FitInside();
    update_content_max_height();
    auto_resize_dialog_to_fit();
}

void MixedFilamentDialog::on_selected_filaments_changed(int index) 
{
    ComboBox* combobox = m_material_comboboxes[index];

    const int old_filament_index = m_selected_filaments[index];
    const int new_filament_index = combobox->GetSelection();

    if (old_filament_index == new_filament_index)
        return;

    // swap if another combobox has new_filament_index, else just assign
    for (size_t i = 0; i < m_selected_filaments.size(); ++i) {
        if (i == index)
            continue;

        if (m_selected_filaments[i] == new_filament_index) {
            // swap other combobox
            m_selected_filaments[i] = old_filament_index;
            m_material_comboboxes[i]->SetSelection(old_filament_index);
                
            break;
        }
    }

    m_selected_filaments[index] = new_filament_index;

    refresh_material_combobox_items();
    m_selected_filaments_colors  = get_selected_filaments_colors(m_selected_filaments);
    m_mix_ratio_panel->update_sizing();
    m_mix_ratio_panel->Refresh();
}

// returns -1 if not found
int MixedFilamentDialog::find_first_free_filament() const 
{ 
    for (int filament_index = 0; filament_index < max_filament; ++filament_index) {
        bool is_already_selected = false;
        for (int selected_filament_index : m_selected_filaments) {
            if (selected_filament_index == filament_index) {
                is_already_selected = true;
                break;
            }
        }

        if (is_already_selected == false) {
            return filament_index;
        }
    }

    return -1;
}

void MixedFilamentDialog::refresh_material_combobox_items() 
{
    for (ComboBox* combobox : m_material_comboboxes) {
        int count = combobox->GetCount();
        for (int n = 0; n < count; ++n) {
            const auto& [_, name] = m_physical_filaments[n];

            wxString text = wxString("");

            const bool is_already_selected = std::find(m_selected_filaments.begin(), m_selected_filaments.end(), n) !=
                                                m_selected_filaments.end();
            const bool is_self = combobox->GetSelection() == n;

            if (is_already_selected && !is_self) {
                text += _L("Switch with");
                text += " - ";
                text += wxString(name);
            } else {
                text += wxString(name);
            }

            combobox->SetString(n, text);
        }
    }
}

void MixedFilamentDialog::refresh_material_weight_labels() 
{
    if (m_selected_filaments_weights.size() != m_material_weight_labels.size())
        return;

    const int filament_count = m_material_weight_labels.size();

    std::vector<int>                       percentages(filament_count);
    std::vector<std::pair<double, size_t>> remainders(filament_count);
    int                                    total_sum = 0;

    // Calculate base truncated percentages and store fractional remainders
    // to make sure all percentages add up to 100%
    for (size_t i = 0; i < filament_count; ++i) {
        double raw_scaled = std::max(0.0 , m_selected_filaments_weights[i] * 100.0);
        percentages[i]    = static_cast<int>(std::floor(raw_scaled));
        total_sum += percentages[i];
        remainders[i] = {raw_scaled - percentages[i], i};
    }

    int diff = 100 - total_sum;

    for (int i = 0; i < filament_count; ++i) {
        int percentage = percentages[i];
        if (i == filament_count - 1) // last
            percentage += diff;

        m_material_weight_labels[i]->SetLabel(wxString::Format(_L("%02d%%"), percentage)); 
        m_material_weight_labels[i]->Refresh();
    }
}

void MixedFilamentDialog::update_min_weight_slider_bounds() 
{
    if (!m_min_weight_slider)
        return;

    int count = m_selected_filaments.size();
    if (count == 0)
        return;

    int max_val = std::floor(100.0 / count);
    m_min_weight_slider->SetRange(0, max_val);

    int cur_val = static_cast<int>(std::round(m_min_weight_ratio * 100.0));
    if (cur_val > max_val) {
        cur_val = max_val;
        m_min_weight_ratio = max_val / 100.0;
    }
    m_min_weight_slider->SetValue(cur_val);
    m_min_weight_value_input->SetValue(wxString::Format("%d", cur_val));
}

void MixedFilamentDialog::apply_min_weight(int new_percentage)
{
    m_min_weight_ratio = new_percentage / 100.0;

    int filament_count = m_selected_filaments_weights.size();
    if (filament_count == 0)
        return;

    bool recalculate_weights = false;
    for (double weight : m_selected_filaments_weights) {
        if (weight < m_min_weight_ratio) {
            recalculate_weights = true;
            break;
        }
    }

    if (recalculate_weights) {
        switch (filament_count) {
            case 2: MixedFilamentRatioPanel::clamp_weights_2(m_selected_filaments_weights, m_min_weight_ratio); break;
            case 3: MixedFilamentRatioPanel::clamp_weights_3(m_selected_filaments_weights, m_min_weight_ratio); break;
            case 4: MixedFilamentRatioPanel::clamp_weights_4(m_selected_filaments_weights, m_min_weight_ratio); break;
            default: break;
        }
    }

    refresh_material_weight_labels();
    m_mix_ratio_panel->Refresh();
}

std::vector<wxColor> MixedFilamentDialog::get_selected_filaments_colors(const std::vector<int>& filament_indices) const
{ 
    std::vector<wxColor> colors;
    colors.reserve(filament_indices.size());

    for (auto physical_filament_index : filament_indices)
    {
        if (physical_filament_index < 0 || physical_filament_index >= (int)m_physical_filaments.size()) {
            colors.push_back(wxColor(*wxBLACK));
        }
        else {
            const auto& [color_hex, name] = m_physical_filaments[physical_filament_index];
            colors.push_back(wxColor(color_hex));
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

void MixedFilamentDialog::update_content_max_height()
{
    // Compute the ideal total height where no scrollbar is needed
    m_content_panel->GetSizer()->Layout();
    int content_ideal_height = m_content_panel->GetSizer()->GetMinSize().GetHeight();

    // Add title panel height, footer panel height, and main sizer margins
    int title_height  = m_title_panel->GetBestSize().GetHeight() + FromDIP(8) * 2; // wxALL margin
    int footer_height = m_footer_panel->GetBestSize().GetHeight();
    int total_ideal   = title_height + content_ideal_height + footer_height;

    // Account for dialog frame/border decorations
    wxSize frame_size    = GetSize();
    wxSize client_size   = GetClientSize();
    int    frame_padding = frame_size.GetHeight() - client_size.GetHeight();
    total_ideal += frame_padding;

    SetMaxSize(wxSize(m_width_fixed, total_ideal));

    // If current size exceeds the new max, shrink to fit
    wxSize current_size = GetSize();
    if (current_size.GetHeight() > total_ideal) {
        SetSize(m_width_fixed, total_ideal);
    }
}

void MixedFilamentDialog::auto_resize_dialog_to_fit()
{
    // Automatically resize the dialog to fit content, capped at m_height_max_auto.
    // Manual resizing is not affected — SetMaxSize (from update_content_max_height)
    // allows the user to manually resize up to the full content height.
    int max_height = GetMaxSize().GetHeight();
    int target_height = std::min(max_height, m_height_max_auto);
    target_height = std::max(target_height, m_height_min);
    SetSize(m_width_fixed, target_height);
}

void MixedFilamentDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    m_width_fixed     = FromDIP(400);
    m_height_start    = FromDIP(600);
    m_height_min      = FromDIP(400);
    m_height_max_auto = FromDIP(800);

	SetMinSize(wxSize(m_width_fixed, m_height_min));

    update_content_max_height();

    // TODO: implement DPI change handling if necessary (e.g., adjust layout, fonts, etc.)
	Refresh();
}

void MixedFilamentDialog::on_sizing(wxSizeEvent& event)
{
    wxSize size = event.GetSize();

    // Fallback constraint enforcement
    size.SetWidth(m_width_fixed);
    if (size.GetHeight() < m_height_min) {
        size.SetHeight(m_height_min);
    }

    event.SetSize(size);
    // Dont call event.Skip(), as this would evoke the default sizer
}

void MixedFilamentDialog::fill_recommendations(wxPanel* container, wxBoxSizer* container_sizer)
{
    if (!container)
        return;

    int filament_count = m_physical_filaments.size();

    auto format_tooltip = [this](const std::vector<int>& phys_indices, const std::vector<double>& weights, const wxColor& mixed_color) -> wxString {
        wxString tooltip = wxString("");
        for (size_t i = 0; i < phys_indices.size(); ++i) {
            if (i > 0) {
                tooltip += " + ";
            }
            int pct = static_cast<int>(std::round(weights[i] * 100.0));
            tooltip += wxString::Format("%d%% Filament [%d]", pct, phys_indices[i] + 1);
        }
        tooltip +=wxString::Format(" = #%02X%02X%02X", mixed_color.Red(), mixed_color.Green(), mixed_color.Blue());
        return tooltip;
    };

    auto get_mixed_color = [this](const std::vector<int>& phys_indices, const std::vector<double>& weights) -> wxColor {
        double r = 0, g = 0, b = 0;
        for (size_t i = 0; i < phys_indices.size(); ++i) {
            wxColor col(m_physical_filaments[phys_indices[i]].first);
            r += weights[i] * col.Red();
            g += weights[i] * col.Green();
            b += weights[i] * col.Blue();
        }
        return wxColor(std::clamp((int)r, 0, 255), std::clamp((int)g, 0, 255), std::clamp((int)b, 0, 255));
    };

    
    auto create_mix_tile = [this](wxPanel* parent, const wxColor& color, const wxString& tooltip, const std::vector<int>& physical_indices, const std::vector<double>& weights) -> wxPanel* {
        wxPanel* tile = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(24), FromDIP(24)), wxBORDER_NONE);
        tile->SetBackgroundColour(color);
        tile->SetCursor(wxCursor(wxCURSOR_HAND));
        tile->SetToolTip(tooltip);
        tile->Bind(wxEVT_LEFT_UP, [weights, physical_indices, this](wxMouseEvent&) {
            if (weights[0] < m_min_weight_ratio || weights[1] < m_min_weight_ratio) {
                m_min_weight_ratio = std::min(weights[0], weights[1]);
                m_min_weight_slider->SetValue(static_cast<int>(std::round(m_min_weight_ratio * 100.0)));
                m_min_weight_value_input->SetValue(wxString::Format("%d", static_cast<int>(std::round(m_min_weight_ratio * 100.0))));
            }
            
            set_active_mix(physical_indices, weights);
        });
        return tile;
    };

    if (filament_count >= 2) {
        wxStaticText* label = new wxStaticText(container, wxID_ANY, _L("2-Way Mixes"));
        label->SetForegroundColour("#7e7e7e");
        label->SetFont(::Label::Body_12.Bold());
        container_sizer->Add(label, 0, wxLEFT | wxTOP | wxRIGHT, FromDIP(8));

        // 50/50 Mixes Row
        wxWrapSizer* wrap_sizer_50 = new wxWrapSizer(wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS);
        for (int i = 0; i < filament_count; ++i) {
            for (int j = i + 1; j < filament_count; ++j) {
                std::vector<double> weights     = {0.5, 0.5};
                std::vector<int> phys_indices   = {i, j};
                wxColor mixed_color             = get_mixed_color(phys_indices, weights);
                wxString tooltip                = format_tooltip(phys_indices, weights, mixed_color);

                wxPanel* tile = create_mix_tile(container, mixed_color, tooltip, phys_indices, weights);
                wrap_sizer_50->Add(tile, 0, wxALL, FromDIP(4));
            }
        }
        container_sizer->Add(wrap_sizer_50, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        // 66/34 Mixes Row
        wxWrapSizer* wrap_sizer_66 = new wxWrapSizer(wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS);
        for (int i = 0; i < filament_count; ++i) {
            for (int j = i + 1; j < filament_count; ++j) {
                std::vector<double> weights     = {0.66, 0.34};
                std::vector<int> phys_indices   = {i, j};
                wxColor mixed_color             = get_mixed_color(phys_indices, weights);
                wxString tooltip                = format_tooltip(phys_indices, weights, mixed_color);

                wxPanel* tile = create_mix_tile(container, mixed_color, tooltip, phys_indices, weights);
                wrap_sizer_66->Add(tile, 0, wxALL, FromDIP(4));
            }
        }
        container_sizer->Add(wrap_sizer_66, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    }

    if (filament_count >= 3) {
        wxStaticText* label = new wxStaticText(container, wxID_ANY, _L("3-Way Mixes"));
        label->SetForegroundColour("#7e7e7e");
        label->SetFont(::Label::Body_12.Bold());
        container_sizer->Add(label, 0, wxLEFT | wxTOP | wxRIGHT, FromDIP(8));

        wxWrapSizer* wrap_sizer = new wxWrapSizer(wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS);
        for (int i = 0; i < filament_count; ++i) {
            for (int j = i + 1; j < filament_count; ++j) {
                for (int k = j + 1; k < filament_count; ++k) {
                    std::vector<double> weights     = {0.33, 0.33, 0.34};
                    std::vector<int> phys_indices   = {i, j, k};
                    wxColor mixed_color             = get_mixed_color(phys_indices, weights);
                    wxString tooltip                = format_tooltip(phys_indices, weights, mixed_color);

                    wxPanel* tile = create_mix_tile(container, mixed_color, tooltip, phys_indices, weights);
                    wrap_sizer->Add(tile, 0, wxALL, FromDIP(4));
                }
            }
        }
        container_sizer->Add(wrap_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    }

    m_recommendations_sizer->Add(container, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
}

void MixedFilamentDialog::set_active_mix(const std::vector<int>& physical_filaments, const std::vector<double>& weights)
{
    this->Freeze();

    int target_count = physical_filaments.size();
    int current_count = m_material_comboboxes.size();

    if (current_count < target_count) {
        for (int i = current_count; i < target_count; ++i) {
            add_material_combobox(m_material_combobox_panel, m_material_combobox_sizer, physical_filaments[i]);
        }
    } else if (current_count > target_count) {
        while (m_material_comboboxes.size() > target_count) {
            remove_material_combobox();
        }
    }

    for (int i = 0; i < target_count; ++i) {
        m_selected_filaments[i] = physical_filaments[i];
        m_material_comboboxes[i]->SetSelection(physical_filaments[i]);
    }

    m_selected_filaments_weights = weights;
    m_selected_filaments_colors  = get_selected_filaments_colors(m_selected_filaments);

    update_min_weight_slider_bounds();
    refresh_material_combobox_items();
    refresh_material_weight_labels();
    m_mix_ratio_panel->update_sizing();
    m_mix_ratio_panel->Refresh();

    m_content_panel->FitInside();
    update_content_max_height();
    auto_resize_dialog_to_fit();

    this->Thaw();
}

} // namespace Slic3r::GUI