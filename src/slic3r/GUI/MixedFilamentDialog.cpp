#include "MixedFilamentDialog.hpp"

#include <wx/wx.h>

#include "I18N.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Factories.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DropDown.hpp"
#include "Widgets/Label.hpp"

namespace Slic3r::GUI {

	MixedFilamentDialog::MixedFilamentDialog(
        wxWindow*                   parent,
        MixedFilamentDialog::Action dialog_action,
        std::vector<std::string>&   physical_colors) 
		: DPIDialog(
            parent, 
            wxID_ANY, 
            dialog_action == MixedFilamentDialog::Action::Add ? _L("Add Mixed Filament") : _L("Edit Mixed Filament"), 
            wxDefaultPosition, 
            wxDefaultSize, 
            wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER
        ),
        m_physical_colors(physical_colors), m_action(dialog_action)
	{ 
		m_width_fixed  = this->wxWindow::FromDIP(400);
        m_height_start = this->wxWindow::FromDIP(600);
        m_height_min   = this->wxWindow::FromDIP(400);

		build_ui();
    }

	void MixedFilamentDialog::build_ui()
	{
        SetMinSize(wxSize(m_width_fixed, m_height_min));
        SetMaxSize(wxSize(m_width_fixed, wxDefaultCoord));
        SetSize(m_width_fixed, m_height_start);
        Bind(wxEVT_SIZING, &MixedFilamentDialog::on_sizing, this);
       
        m_main_sizer = new wxBoxSizer(wxVERTICAL);

        // Title
        m_title_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE);
        m_title_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_title_panel->SetSizer(m_title_sizer);

        m_mix_btn = new ScalableButton(m_title_panel, wxID_ANY, "ams_readonly", _L("Mix"));
        m_mix_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
            m_current_tab = Tab::Mix;
            update_tabs();
        });
        m_mix_btn->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) { on_tab_hover_enter(e, Tab::Mix); });
        m_mix_btn->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) { on_tab_hover_leave(e); });

        m_pattern_btn = new ScalableButton(m_title_panel, wxID_ANY, "ams_drying", _L("Pattern"));
        m_pattern_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
            m_current_tab = Tab::Pattern;
            update_tabs();
        });
        m_pattern_btn->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) { on_tab_hover_enter(e, Tab::Pattern); });
        m_pattern_btn->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) { on_tab_hover_leave(e); });       

        m_title_sizer->AddStretchSpacer();
        m_title_sizer->Add(m_mix_btn, 0, wxALIGN_CENTER);
        m_title_sizer->AddSpacer(FromDIP(6));
        m_title_sizer->Add(m_pattern_btn, 0, wxALIGN_CENTER);
        m_title_sizer->AddStretchSpacer();

        m_main_sizer->Add(m_title_panel, 0, wxEXPAND);
        
        // Content
        m_content_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE);
        m_content_sizer = new wxBoxSizer(wxVERTICAL);
        m_content_panel->SetSizer(m_content_sizer);

        Label* placeholder_label = new Label(m_content_panel, _L("Content goes here... ") + std::to_string(static_cast<int>(m_current_tab)));
        m_content_sizer->Add(placeholder_label, 0, wxALIGN_CENTER | wxALL, FromDIP(20));

        m_main_sizer->Add(m_content_panel, 1, wxEXPAND);

        // Footer
        m_footer_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE);
        m_footer_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_footer_panel->SetSizer(m_footer_sizer);
        
        Label* footer_placeholder_label = new Label(m_footer_panel, _L("Footer goes here..."));
        m_footer_sizer->Add(footer_placeholder_label, 0, wxALIGN_CENTER | wxALL, FromDIP(20));

        m_main_sizer->Add(m_footer_panel, 0, wxEXPAND);



        SetSizer(m_main_sizer);
		Layout();
        CentreOnParent();
    }

    void MixedFilamentDialog::update_tabs()
    {
        wxColor active_bg   = wxColor(ColorRGB::ORCA().r_uchar(), ColorRGB::ORCA().g_uchar(), ColorRGB::ORCA().b_uchar(), 255);
        wxColor inactive_bg = wxColor("#CECECE");

        m_mix_btn->SetBackgroundColour(m_current_tab == Tab::Mix ? active_bg : inactive_bg);
        m_pattern_btn->SetBackgroundColour(m_current_tab == Tab::Pattern ? active_bg : inactive_bg);

        m_mix_btn->SetForegroundColour(m_current_tab == Tab::Mix ? *wxWHITE : *wxBLACK);
        m_pattern_btn->SetForegroundColour(m_current_tab == Tab::Pattern ? *wxWHITE : *wxBLACK);

        m_title_panel->Refresh();
        m_title_panel->Layout();
        // TODO: update content panel based on the current tab selection
    }

    void MixedFilamentDialog::on_tab_hover_enter(wxMouseEvent& event, Tab tab) 
    { 
        wxWindow* btn = dynamic_cast<wxWindow*>(event.GetEventObject());

        if (tab != m_current_tab) {
            btn->SetBackgroundColour(wxColor("#D8D8D8"));
            btn->Refresh();
        }

        event.Skip();
    }

    void MixedFilamentDialog::on_tab_hover_leave(wxMouseEvent& event) 
    {
        update_tabs();
        event.Skip();
    }

    void MixedFilamentDialog::on_dpi_changed(const wxRect& suggested_rect)
	{
        m_width_fixed  = FromDIP(400);
        m_height_start = FromDIP(600);
        m_height_min   = FromDIP(400);

		SetMinSize(wxSize(m_width_fixed, m_height_min));
        SetMaxSize(wxSize(m_width_fixed, wxDefaultCoord));

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
        event.Skip();
    }

} // namespace Slic3r::GUI 