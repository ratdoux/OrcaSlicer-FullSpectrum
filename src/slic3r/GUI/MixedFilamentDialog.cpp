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
#include <wx/dcgraph.h>

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

		build_ui(parent);
    }

	void MixedFilamentDialog::build_ui(wxWindow* parent)
	{
        SetMinSize(wxSize(m_width_fixed, m_height_min));
        SetMaxSize(wxSize(m_width_fixed, wxDefaultCoord));
        Bind(wxEVT_SIZING, &MixedFilamentDialog::on_sizing, this);
       
        SetBackgroundColour(parent->GetBackgroundColour());
        m_main_sizer = new wxBoxSizer(wxVERTICAL);

        // Title
        m_title_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE);
        m_title_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_title_panel->SetSizer(m_title_sizer);
        m_title_panel->SetMinSize(wxSize(-1, FromDIP(24)));

        // Mix Tab
        m_mix_tab_btn = new wxPanel(m_title_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        m_mix_tab_btn->SetMinSize(wxSize(FromDIP(40), FromDIP(24)));
        m_mix_tab_btn->SetBackgroundColour(parent->GetBackgroundColour());

        m_mix_tab_btn->Bind(wxEVT_PAINT, [this](wxPaintEvent& event) {
            paintRoundedPanel(m_mix_tab_btn, true, false, FromDIP(6), _L("Mix"), "ams_readonly", m_current_tab == Tab::Mix, m_mix_tab_hovered); 
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
         m_pattern_tab_btn->SetMinSize(wxSize(FromDIP(40), FromDIP(24)));
         m_pattern_tab_btn->SetBackgroundColour(parent->GetBackgroundColour());

         m_pattern_tab_btn->Bind(wxEVT_PAINT, [this](wxPaintEvent& event) {
             paintRoundedPanel(m_pattern_tab_btn, false, true, FromDIP(6), _L("Pattern"), "ams_drying", m_current_tab == Tab::Pattern, m_pattern_tab_hovered); 
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
        
        // Content
        m_content_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE);
        m_content_sizer = new wxBoxSizer(wxVERTICAL);
        m_content_panel->SetSizer(m_content_sizer);

        Label* placeholder_label = new Label(m_content_panel, _L("Content goes here... "));
        m_content_sizer->Add(placeholder_label, 0, wxALIGN_CENTER | wxLEFT, FromDIP(8));

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
        SetSize(m_width_fixed, m_height_start);
        CentreOnParent();

        update_tabs();
    }

    wxColour MixedFilamentDialog::getTabBorderColor(bool is_selected, bool is_hovered) const
    { 
        if (is_selected)
            return wxColor(ColorRGB::ORCA().r_uchar(), ColorRGB::ORCA().g_uchar(), ColorRGB::ORCA().b_uchar());
        else if (is_hovered)
            return wxColor("#707070");
        else
            return wxColor("#A0A0A0");
    }

    void MixedFilamentDialog::paintRoundedPanel(wxPanel* panel, bool round_left, bool round_right, double radius, wxString label, wxString icon_name, bool is_selected, bool is_hovered) 
    {
        wxSize size = panel->GetSize();
        wxAutoBufferedPaintDC dc(panel);
        wxGCDC                gcdc(dc);
        wxGraphicsContext*    gc = gcdc.GetGraphicsContext();
        if (!gc)
            return;

        wxColour background_color = panel->GetParent()->GetBackgroundColour();
        gc->SetBrush(wxBrush(background_color));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->DrawRectangle(0, 0, size.x, size.y);

        wxColour fill_color = panel->GetBackgroundColour();
        wxColour border_color = getTabBorderColor(is_selected, is_hovered);
        wxColour text_color   = is_selected ? *wxWHITE : *wxBLACK;

        const double border_width = 1;
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
        gc->SetPen(wxPen(border_color, 1));
        gc->DrawPath(path);

        // Load icon
        wxBitmap icon_bmp  = ScalableBitmap(panel, icon_name.ToStdString(), 14).bmp();
        wxSize   icon_size = icon_bmp.GetSize();

        // Measure text
        gc->SetFont(panel->GetFont(), text_color);
        double text_width, text_height;
        gc->GetTextExtent(label, &text_width, &text_height);

        // Center icon + text horizontally
        double total_width = icon_size.x + FromDIP(4) + text_width;
        double start_x     = (size.x - total_width) / 2.0;
        double icon_y      = (size.y - icon_size.y) / 2.0;
        double text_y      = (size.y - text_height) / 2.0;

        // Draw icon
        if (icon_bmp.IsOk()) {
            gc->DrawBitmap(icon_bmp, start_x, icon_y, icon_size.x, icon_size.y);
        }

        // Draw text
        gc->DrawText(label, start_x + icon_size.x + FromDIP(4), text_y);
    }

    void MixedFilamentDialog::update_tabs()
    {
        wxColor orca_color(ColorRGB::ORCA().r_uchar(), ColorRGB::ORCA().g_uchar(), ColorRGB::ORCA().b_uchar());
        wxColor normal_color("#CECECE");
        wxColor hover_color("#D8D8D8");

        bool is_mix_selected = m_current_tab == Tab::Mix;
        if (is_mix_selected) {
            m_mix_tab_btn->SetBackgroundColour(orca_color);
        } else if (m_mix_tab_hovered) {
            m_mix_tab_btn->SetBackgroundColour(hover_color);
        } else {
            m_mix_tab_btn->SetBackgroundColour(normal_color);
        }
        m_mix_tab_btn->Refresh();

        bool is_pattern_selected = m_current_tab == Tab::Pattern;
        if (is_pattern_selected) {
            m_pattern_tab_btn->SetBackgroundColour(orca_color);
        } else if (m_pattern_tab_hovered) {
            m_pattern_tab_btn->SetBackgroundColour(hover_color);
        } else {
            m_pattern_tab_btn->SetBackgroundColour(normal_color);
        }

        m_pattern_tab_btn->Refresh();

        // TODO: update content panel based on the current tab selection
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
        // Dont call event.Skip(), as this would evoke the default sizer
    }


} // namespace Slic3r::GUI 