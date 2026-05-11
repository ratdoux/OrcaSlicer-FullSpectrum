#include "FilamentCard.hpp"

#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Factories.hpp"
#include "slic3r/GUI/MainFrame.hpp"


namespace Slic3r::GUI {

FilamentCardPhysical::FilamentCardPhysical(wxWindow* parent, const int index)
    : ::wxPanel(parent, wxID_ANY)
{
    m_index = index;

    SetBackgroundColour(*wxWHITE);
    build_ui();
}

wxPoint FilamentCardPhysical::get_edit_btn_client_position()
{
    wxPoint pt{0, m_filament_edit_btn->GetSize().GetHeight() + 10};
    pt = m_filament_edit_btn->ClientToScreen(pt);
    pt = wxGetApp().mainframe->ScreenToClient(pt);

    return pt;
}

void FilamentCardPhysical::update_state()
{
    m_filament_combo_box->update();
}

void FilamentCardPhysical::build_ui()
{
    m_sizer = new wxBoxSizer(wxHORIZONTAL);

    //if ((m_config.index % 2) == 0)
    //    m_sizer->AddSpacer(FromDIP(SidebarProps::ContentMargin()));

    m_filament_combo_box = new PlaterPresetComboBox(this, Preset::TYPE_FILAMENT);
    m_filament_combo_box->set_filament_idx(m_index);

    if (m_filament_combo_box->clr_picker != nullptr) {
        m_filament_combo_box->clr_picker->SetLabel(wxString::Format("%d", m_index + 1));
        m_sizer->Add(m_filament_combo_box->clr_picker, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT,
                     FromDIP(SidebarProps::ElementSpacing()) - FromDIP(2));
    }

    m_sizer->Add(m_filament_combo_box, 1, wxALL | wxEXPAND, FromDIP(2))->SetMinSize({-1, FromDIP(30)});

    m_filament_edit_btn = new ScalableButton(this, wxID_ANY, "menu_filament");
    m_filament_edit_btn->SetToolTip(_L("Click to edit preset"));
    m_filament_edit_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_on_edit)
            m_on_edit(m_index, m_filament_edit_btn);
    });
    m_filament_combo_box->edit_btn = m_filament_edit_btn;
    m_sizer->Add(m_filament_edit_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(SidebarProps::ElementSpacing()) - FromDIP(2));

    m_filament_combo_box->update();

    SetSizer(m_sizer);
    Layout();
}

} // namespace Slic3r::GUI
