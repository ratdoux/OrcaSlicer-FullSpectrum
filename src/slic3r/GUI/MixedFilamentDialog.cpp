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
#include "Widgets/FilamentCardMixed.hpp"
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
        ),
        m_physical_filaments(physical_filaments), m_action(dialog_action)
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

        // Material Section
        m_material_panel = new wxPanel(m_content_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        m_material_sizer = new wxBoxSizer(wxVERTICAL);
        m_material_panel->SetSizer(m_material_sizer);

        // Material Title (incl buttons)
        m_material_title_panel  = new wxPanel(m_content_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        m_material_title_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_material_title_panel->SetSizer(m_material_title_sizer);

        m_material_title_text = new wxStaticText(m_material_title_panel, wxID_ANY, _L("Select Mixed Materials"));
        m_material_title_text->SetForegroundColour("#7e7e7e");
        m_material_title_text->SetFont(::Label::Body_14);

        m_delete_material_btn = new ScalableButton(m_material_title_panel, wxID_ANY, "delete_filament");
        m_delete_material_btn->SetToolTip(_L("Remove last material"));
        m_delete_material_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { remove_material_combobox(); });
        m_delete_material_btn->Enable(false); // Enable when filament 3 or 4

        m_add_material_btn = new ScalableButton(m_material_title_panel, wxID_ANY, "add_filament");
        m_add_material_btn->SetToolTip(_L("Add material"));
        m_delete_material_btn->Enable(false); // Enable when filament 2 or 3
        m_add_material_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { add_material_combobox(); });

        m_material_title_sizer->Add(m_material_title_text, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
        m_material_title_sizer->AddStretchSpacer();
        m_material_title_sizer->Add(m_delete_material_btn, 0, wxALIGN_CENTER_VERTICAL);
        m_material_title_sizer->Add(m_add_material_btn, 0, wxALIGN_CENTER_VERTICAL);

        // Material Selections
        m_material_combobox_panel = new wxPanel(m_material_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        m_material_combobox_sizer = new wxBoxSizer(wxVERTICAL);
        m_material_combobox_panel->SetSizer(m_material_combobox_sizer);


        m_material_sizer->Add(m_material_title_panel, 0, wxEXPAND); // TODO margins?
        m_material_sizer->Add(m_material_combobox_panel, 0, wxEXPAND);

        m_content_sizer->Add(m_material_panel, 0);

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
        add_material_combobox();
        add_material_combobox();
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

        wxFont font = panel->GetFont();
        font.SetPointSize(font.GetPointSize() + 2);
        if (is_selected || is_hovered) {
            font = font.Bold();
        }
        gc->SetFont(font, text_color);

        // Measure text to center it
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

    // TODO fix add and delete buttons not working

    void MixedFilamentDialog::add_material_combobox() {
        const int current_count = m_material_comboboxes.size();
        const int new_index     = current_count + 1;
        int       swatch_size   = FromDIP(20);

        wxSize combobox_size = wxSize(FromDIP(166), FromDIP(24));
        ComboBox* combobox = new ComboBox(m_material_combobox_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, combobox_size, 0, nullptr,
                                          wxCB_READONLY);
        combobox->SetMinSize(combobox_size);
        //combobox->SetKeepDropArrow(true);

        for (size_t i = 0; i < m_physical_filaments.size(); ++i) {

            const auto& [color_hex, name] = m_physical_filaments[i];
            wxColor color(color_hex);
            wxString index = wxString::Format("%zu", i + 1);

            wxBitmap clr_swatch_bmp(swatch_size, swatch_size);
            wxMemoryDC dc(clr_swatch_bmp);
            FilamentCardMixed::paint_clr_swatch(dc, wxSize(swatch_size, swatch_size), color, index, wxGetApp().dark_mode());


            wxString text = wxString::Format("%s", name);
            combobox->Append(text, clr_swatch_bmp);
        }

        if (!m_physical_filaments.empty()) 
            combobox->SetSelection(0);

        size_t index = new_index;
        combobox->Bind(wxEVT_COMBOBOX, [this, index](wxCommandEvent&) {
            on_selected_filaments_changed(index);
        });

        m_material_comboboxes.push_back(combobox);
        m_material_combobox_sizer->Add(combobox, 0, wxEXPAND | wxTOP, FromDIP(8));

        m_delete_material_btn->Enable(new_index > 1);
        m_add_material_btn->Enable(new_index < 4);
        Layout();
    }

    void MixedFilamentDialog::remove_material_combobox() {
        if (m_material_comboboxes.size() <= 1 || m_selected_filaments.size() <= 1)
            return;

        const size_t last_index = m_selected_filaments.size() - 1;
        ComboBox* last_combobox = m_material_comboboxes[last_index];

        m_material_combobox_sizer->Detach(last_index);
        last_combobox->Destroy();

        m_selected_filaments.pop_back();

        const int new_count = (last_index - 1);
        m_delete_material_btn->Enable(new_count > 1);
        m_add_material_btn->Enable(new_count < 3);
        Layout();
    }

    void MixedFilamentDialog::on_selected_filaments_changed(int selection_index) 
    {
        
        // TODO if select already selected filament, switch them
        // TODO redraw comboboxes
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