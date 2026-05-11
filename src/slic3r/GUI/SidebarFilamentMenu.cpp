#include "SidebarFilamentMenu.hpp"
#include "Widgets/Button.hpp"
#include "wxExtensions.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "Widgets/FilamentCard.hpp"

namespace Slic3r::GUI {

SidebarFilamentMenu::SidebarFilamentMenu(wxWindow* parent, const wxColour& title_bg) : ::wxPanel(parent, wxID_ANY)
{
    build_ui(title_bg);
}

wxPoint SidebarFilamentMenu::get_edit_btn_client_position(int index)
{
    // TODO handle mixed filament count
    if (index >= m_physical_count())
        return wxPoint(0, 0);

    return m_physical_cards[index]->get_edit_btn_client_position();
}

bool SidebarFilamentMenu::switch_to_tab(int index)
{
    // TODO handle mixed filament count
    if (index >= m_physical_count())
        return false;

    return m_physical_cards[index]->m_filament_combo_box->switch_to_tab();
}

void SidebarFilamentMenu::update_physical_states()
{
    for (auto* card : m_physical_cards)
        card->update_state();
}

void SidebarFilamentMenu::msw_rescale()
{
    m_title_sizer->SetMinSize(-1, 3 * wxGetApp().em_unit());

    m_btn_icon->msw_rescale();
    m_btn_add->msw_rescale();
    m_btn_del->msw_rescale();
    m_btn_ams->msw_rescale();
    m_btn_settings->msw_rescale();
    m_btn_flushing->Rescale();

    for (auto* card : m_physical_cards) {
        card->m_filament_combo_box->msw_rescale();
    }
}

void SidebarFilamentMenu::sys_color_changed()
{
    m_btn_icon->msw_rescale();
    m_btn_add->msw_rescale();
    m_btn_del->msw_rescale();
    m_btn_ams->msw_rescale();
    m_btn_settings->msw_rescale();
    m_btn_flushing->Rescale();

    for (auto* card : m_physical_cards) {
        card->m_filament_combo_box->sys_color_changed();
    }
}

void SidebarFilamentMenu::toggle_collapse(bool only_open)
{
    if (only_open && m_content_panel->GetMaxHeight() == 0) {
        m_content_panel->SetMaxSize({-1, -1});
        return;
    }

    if (m_content_panel->GetMaxHeight() == 0)
        m_content_panel->SetMaxSize({-1, -1});
    else
        m_content_panel->SetMaxSize({-1, 0});
}

void SidebarFilamentMenu::show_SEMM_buttons(bool is_single_extruder_multi_material)
{
    // Show add buttons only if the printer supports multi-material
    if (m_btn_add)
        m_btn_add->Show(is_single_extruder_multi_material);

    // Show/hide buttons based on whether there are filaments to delete/flush
    if (m_btn_del)
        m_btn_del->Show(is_single_extruder_multi_material && m_physical_count() > 1);

    if (m_btn_flushing)
        m_btn_flushing->Show(is_single_extruder_multi_material && m_physical_count() > 1);

    Layout();
}

void SidebarFilamentMenu::show_AMS_button(bool show) {
    m_btn_ams->Show(show);
    Layout();
}

void SidebarFilamentMenu::on_filaments_change(size_t physical_count)
{
    int current_count = m_physical_count();

    if (physical_count == 1 || current_count == 1) {
        if (!m_physical_cards.empty())
            m_physical_cards[0]->m_filament_combo_box->GetDropDown().Invalidate();
    }

    // ADD new cards if physical_count increased
    for (int i = current_count; i < physical_count; ++i) {
        auto* card = new FilamentCardPhysical(m_physical_panel, i);
        card->set_on_edit_callback([this](int index, wxWindow* anchor) {
            if (m_on_edit_physical)
                m_on_edit_physical(index);
        });
        int last_selected_preset = m_physical_cards.empty() ? 0 : m_physical_cards.back()->m_filament_combo_box->GetSelection();

        m_physical_cards.push_back(card);
        m_physical_sizer->Add(card, 0, wxEXPAND);

        card->m_filament_combo_box->update();
        card->m_filament_combo_box->SetSelection(last_selected_preset); // select the same filament as the last card
    }

    // REMOVE excess cards if physical_count decreased
    while (m_physical_count() > physical_count) {
        auto* card = m_physical_cards.back();
        m_physical_sizer->Remove(m_physical_count() - 1);

        card->Destroy();
        m_physical_cards.pop_back();
    }

    // Show/hide buttons based on whether there are filaments to delete/flush
    show_SEMM_buttons(true);

    update_physical_states();

    if (physical_count == 1)
        m_physical_sizer->SetCols(1);
    else
        m_physical_sizer->SetCols(2);

    Layout();
    m_title_panel->Refresh();
}

void SidebarFilamentMenu::update_title(const wxString& label, const std::string& icon_name)
{
    m_lbl_title->SetLabel(label);
    m_btn_icon->SetBitmap_(icon_name);
}

void SidebarFilamentMenu::build_ui(const wxColour& title_bg)
{
    m_main_sizer = new wxBoxSizer(wxVERTICAL);

    // ####################################
    // 1. Title Bar
    // ####################################
    m_title_panel = new StaticBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE);
    m_title_panel->SetBackgroundColor(title_bg);
    m_title_panel->SetBackgroundColor2(0xF1F1F1);
    m_title_panel->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) {
        if (m_callbacks[ActionType::CollapseToggle]) {
            int btn_flushing_x =
                (m_btn_flushing->IsShown() ?
                     m_btn_flushing->GetPosition().x :
                     (m_btn_add->GetPosition().x -
                      FromDIP(30)));

            // ORCA exclude area of del button from titlebar collapse/expand feature to fix
            // undesired collapse when user spams del filament button
            if (e.GetPosition().x > btn_flushing_x)
                return;

            toggle_collapse(false);

            m_callbacks[ActionType::CollapseToggle]();
        }
    });

    m_title_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_title_sizer->SetMinSize({-1, FromDIP(30)});

    // Icon and Label
    m_btn_icon = new ScalableButton(m_title_panel, wxID_ANY, "filament");

    m_lbl_title = new Label(m_title_panel, _L("Filaments"), LB_PROPAGATE_MOUSE_EVENT);

    // Title Buttons
    m_btn_flushing = new Button(m_title_panel, _L("Flushing volumes"));
    m_btn_flushing->SetStyle(ButtonStyle::Confirm, ButtonType::Compact);
    m_btn_flushing->SetId(wxID_RESET);
    m_btn_flushing->Bind(wxEVT_BUTTON, [this](auto&) {
        if (m_callbacks[ActionType::FlushingVolumes])
            m_callbacks[ActionType::FlushingVolumes]();
    });

    m_btn_add = new ScalableButton(m_title_panel, wxID_ANY, "add_filament");
    m_btn_add->SetToolTip(_L("Add one filament"));
    m_btn_add->Bind(wxEVT_BUTTON, [this](auto&) {
        if (m_callbacks[ActionType::AddFilament])
            m_callbacks[ActionType::AddFilament]();
    });

    m_btn_del = new ScalableButton(m_title_panel, wxID_ANY, "delete_filament");
    m_btn_del->SetToolTip(_L("Remove last filament"));
    m_btn_del->Bind(wxEVT_BUTTON, [this](auto&) {
        if (m_callbacks[ActionType::DeleteFilament])
            m_callbacks[ActionType::DeleteFilament]();
    });

    m_btn_ams = new ScalableButton(m_title_panel, wxID_ANY, "ams_fila_sync");
    m_btn_ams->SetToolTip(_L("Synchronize filament list from AMS"));
    m_btn_ams->Bind(wxEVT_BUTTON, [this](auto&) {
        if (m_callbacks[ActionType::SyncAMS])
            m_callbacks[ActionType::SyncAMS]();
    });

    m_btn_settings = new ScalableButton(m_title_panel, wxID_ANY, "settings");
    m_btn_settings->SetToolTip(_L("Set filaments to use"));
    m_btn_settings->Bind(wxEVT_BUTTON, [this](auto&) {
        if (m_callbacks[ActionType::Settings])
            m_callbacks[ActionType::Settings]();
    });

    // Title Layout
    m_title_sizer->Add(m_btn_icon, 0, wxALIGN_CENTER | wxLEFT, FromDIP(SidebarProps::TitlebarMargin()));
    m_title_sizer->AddSpacer(FromDIP(SidebarProps::ElementSpacing()));
    m_title_sizer->Add(m_lbl_title, 0, wxALIGN_CENTER, FromDIP(5));
    m_title_sizer->AddSpacer(FromDIP(10));

    m_title_sizer->AddStretchSpacer();
    m_title_sizer->Add(m_btn_flushing, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(5));

    // ORCA Moved add button after delete button to prevent add button position change when remove icon automatically hidden
    m_title_sizer->Add(m_btn_del, 0, wxALIGN_CENTER | wxLEFT, FromDIP(SidebarProps::IconSpacing()));
    m_title_sizer->Add(m_btn_add, 0, wxALIGN_CENTER | wxLEFT, FromDIP(SidebarProps::IconSpacing()));
    m_title_sizer->AddSpacer(FromDIP(20));

    m_title_sizer->Add(m_btn_ams, 0, wxALIGN_CENTER | wxLEFT, FromDIP(SidebarProps::IconSpacing()));
    m_title_sizer->Add(m_btn_settings, 0, wxALIGN_CENTER | wxLEFT, FromDIP(SidebarProps::IconSpacing()));
    m_title_sizer->AddSpacer(FromDIP(SidebarProps::TitlebarMargin()));

    show_SEMM_buttons(true);

    m_title_panel->SetSizer(m_title_sizer);
    m_title_panel->Layout();


    // ####################################
    // 2. Content Panel
    // ####################################
    m_content_panel = new wxPanel(this);
    m_content_sizer = new wxBoxSizer(wxVERTICAL);
    m_content_panel->SetSizer(m_content_sizer);

    m_physical_panel = new wxPanel(m_content_panel);
    m_physical_sizer = new wxGridSizer(1, FromDIP(2), FromDIP(SidebarProps::ContentMargin() * 2));
    m_physical_panel->SetSizer(m_physical_sizer);

    // add one FilamentCard by default
    m_physical_cards.push_back(nullptr);
    m_physical_cards[0] = new FilamentCardPhysical(m_physical_panel, 0);
    m_physical_cards[0]->set_on_edit_callback([this](int index, wxWindow* anchor) {
        if (m_on_edit_physical)
            m_on_edit_physical(index);
    });
    m_physical_sizer->Add(m_physical_cards[0], 0, wxEXPAND);

    m_physical_panel->Layout();

    m_content_sizer->Add(m_physical_panel, 0, wxALL | wxEXPAND, FromDIP(SidebarProps::ContentMargin()));
    //m_content_sizer->Add(m_mixed_panel, 0, wxALL | wxEXPAND, FromDIP(SidebarProps::ContentMargin()));

    m_content_panel->Layout();

    auto splitter_before_title = new ::StaticLine(this);
    splitter_before_title->SetLineColour("#A6A9AA");
    auto splitter_after_title = new ::StaticLine(this);
    splitter_after_title->SetLineColour("#CECECE");

    m_main_sizer->Add(splitter_before_title, 0, wxEXPAND);
    m_main_sizer->Add(m_title_panel, 0, wxEXPAND);
    m_main_sizer->Add(splitter_after_title, 0, wxEXPAND);
    m_main_sizer->Add(m_content_panel, 0, wxEXPAND);
    SetSizer(m_main_sizer);
}


} // namespace Slic3r::GUI
