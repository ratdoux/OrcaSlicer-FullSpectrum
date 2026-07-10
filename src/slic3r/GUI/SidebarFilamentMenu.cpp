#include "SidebarFilamentMenu.hpp"
#include "Widgets/Button.hpp"
#include "wxExtensions.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "MainFrame.hpp"
#include "MixedFilamentDialog.hpp"
#include "MixedFilamentBatchDialog.hpp"
#include "Widgets/FilamentCard.hpp"
#include "Widgets/FilamentCardMixed.hpp"
#include "libslic3r/MixedFilament.hpp"


namespace Slic3r::GUI {

SidebarFilamentMenu::SidebarFilamentMenu(wxWindow* parent, const wxColour& title_bg) : ::wxPanel(parent, wxID_ANY)
{
    build_ui(title_bg);
}

void SidebarFilamentMenu::on_filaments_change(size_t physical_count, std::vector<MixedFilamentDefinition>& mixed_filaments)
{
    on_physical_change(physical_count);
    on_mixed_change(mixed_filaments);
}

void SidebarFilamentMenu::on_physical_change(size_t physical_count)
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
        card->set_on_right_click_callback([this](int index, const wxPoint& screen_pos) {
            if (m_on_right_click_physical)
                m_on_right_click_physical(index, screen_pos);
        });
        int last_selected_preset = m_physical_cards.empty() ? 0 : m_physical_cards.back()->m_filament_combo_box->GetSelection();

        m_physical_cards.push_back(card);
        card->SetMinSize({FromDIP(20), -1});
        m_physical_sizer->Add(card, 0, wxEXPAND);

        card->m_filament_combo_box->update();
        card->m_filament_combo_box->SetSelection(last_selected_preset); // select the same filament as the last card
    }

    // REMOVE excess cards if physical_count decreased
    while (m_physical_count() > physical_count) {
        auto* card = m_physical_cards.back();

        m_physical_sizer->Detach(m_physical_count() - 1);
        card->Destroy();

        m_physical_cards.pop_back();
    }

    // Show/hide buttons based on whether there are filaments to delete/flush
    show_SEMM_buttons(true);

    update_physical_states();

    update_physical_filaments();


    if (physical_count == 1)
        m_physical_sizer->SetCols(1);
    else
        m_physical_sizer->SetCols(2);

    // Counter logic
    if (m_lbl_physical_counter) {
        if (physical_count > 6) {
            m_lbl_physical_counter->SetLabel(wxString::Format(" (%d)", (int)physical_count));
            m_lbl_physical_counter->Show(true);
        } else {
            m_lbl_physical_counter->Show(false);
        }
        m_lbl_physical_counter->GetParent()->Layout();
        m_physical_title_panel->Layout();
    }

    // Update layout
    const int max_content_height = m_physical_panel->GetSizer()->GetMinSize().y;
    const int current_height     = m_physical_panel->GetSize().y;
    const int target_height      = std::min(max_content_height, m_scrollbar_threshold);

    // Automatically adjust panel height, when grab panel is not shown
    const bool needs_automatic_adjustment = (
        max_content_height < m_scrollbar_threshold
        || current_height < m_scrollbar_threshold
        || current_height > max_content_height);   
    if (needs_automatic_adjustment) {
        m_physical_panel->SetMinSize(wxSize(-1, target_height));
        m_physical_panel->SetMaxSize(wxSize(-1, target_height));
    }

    m_physical_panel->Layout();
    m_physical_panel->FitInside();

    update_grab_panel_visibility(m_physical_panel, m_content_panel, m_physical_grab_panel, max_content_height);

    if (physical_count != static_cast<size_t>(current_count)) {
        m_physical_panel->Scroll(0, m_physical_panel->GetScrollRange(wxVERTICAL));
    }

    this->InvalidateBestSize();
    Layout();
    if (this->GetParent())
        this->GetParent()->Layout();
    m_title_panel->Refresh();
}

void SidebarFilamentMenu::on_mixed_change(std::vector<MixedFilamentDefinition>& mixed_filaments)
{
    int current_count = m_mixed_count();
    int new_count    = static_cast<int>(mixed_filaments.size());
    
    // ADD new cards if mixed_count increased
    for (int i = current_count; i < new_count; i++) {
        auto* card = new FilamentCardMixed(m_mixed_panel, &mixed_filaments[i], m_physical_filaments);
        card->set_on_box_edit_callback([this, i](bool edit_by_color) 
        { 
            edit_mixed_filament(i, edit_by_color);
        });
        card->set_on_edit_btn_callback([this, i, card](wxWindow* anchor) {
            show_mixed_filament_menu(i, wxDefaultPosition, anchor);
        });
        card->set_on_right_click_callback([this, i](const wxPoint& screen_pos) {
            show_mixed_filament_menu(i, screen_pos, nullptr);
        });

        m_mixed_cards.push_back(card);
        m_mixed_sizer->Add(card, 0, wxEXPAND);
    }

    // REMOVE excess cards if mixed_count decreased
    while (m_mixed_count() > new_count) {
        auto* card = m_mixed_cards.back();

        m_mixed_sizer->Detach(m_mixed_count() - 1);
        card->Destroy();

        m_mixed_cards.pop_back();
    }

    update_mixed_states(mixed_filaments);

    if (new_count == 1)
        m_mixed_sizer->SetCols(1);
    else
        m_mixed_sizer->SetCols(2);

    // Counter logic
    if (m_lbl_mixed_counter) {
        if (new_count > 6) {
            m_lbl_mixed_counter->SetLabel(wxString::Format(" (%d)", new_count));
            m_lbl_mixed_counter->Show(true);
        } else {
            m_lbl_mixed_counter->Show(false);
        }
        m_lbl_mixed_counter->GetParent()->Layout();
        m_mixed_title_panel->Layout();
    }

    // Bottom padding grabber-dependent visibility
    if (m_mixed_list_bottom_spacer) {
        m_mixed_list_bottom_spacer->Show(false);
    }

    // Update layout
    int max_content_height = m_mixed_panel->GetSizer()->GetMinSize().y;
    const int current_height     = m_mixed_panel->GetSize().y;
    const int target_height      = std::min(max_content_height, m_scrollbar_threshold);

    // Automatically adjust panel height, when grab panel is not shown
    const bool needs_automatic_adjustment = (
        max_content_height < m_scrollbar_threshold 
        || current_height < m_scrollbar_threshold
        || current_height > max_content_height);
    if (needs_automatic_adjustment) {
        m_mixed_panel->SetMinSize(wxSize(-1, target_height));
        m_mixed_panel->SetMaxSize(wxSize(-1, target_height));
    }

    if (m_mixed_list_bottom_spacer) {
        const bool scrollable = max_content_height >= m_scrollbar_threshold;
        m_mixed_list_bottom_spacer->Show(!scrollable);
    }

    m_mixed_panel->Layout();
    m_mixed_panel->FitInside();
    max_content_height = m_mixed_panel->GetSizer()->GetMinSize().y;
    update_grab_panel_visibility(m_mixed_panel, m_content_panel, m_mixed_grab_panel, max_content_height);

    if (new_count != current_count) {
        m_mixed_panel->Scroll(0, m_mixed_panel->GetScrollRange(wxVERTICAL));
    }

    this->InvalidateBestSize();
    Layout();
    if (this->GetParent())
        this->GetParent()->Layout();
    m_title_panel->Refresh();
}

void SidebarFilamentMenu::edit_mixed_filament(int index, bool edit_by_color)
{
    if (index < 0 || index >= m_mixed_count()) return;

    auto* card = m_mixed_cards[index];
    card->set_dialog_open(true);

    if (m_on_edit_mixed)
        m_on_edit_mixed(index, edit_by_color);

    card->set_dialog_open(false);
}

void SidebarFilamentMenu::delete_mixed_filament(int index)
{
    if (m_on_delete_mixed)
        m_on_delete_mixed(index);
}

void SidebarFilamentMenu::show_mixed_filament_menu(int index, const wxPoint& screen_pos, wxWindow* anchor)
{
    wxMenu menu;
    
    // Edit item
    append_menu_item(&menu, wxID_ANY, _L("Edit"), "", [this, index](wxCommandEvent&) {
        edit_mixed_filament(index, false);
    });
    
    // Delete item
    append_menu_item(&menu, wxID_ANY, _L("Delete"), _L("Delete this mixed filament"), [this, index](wxCommandEvent&) {
        delete_mixed_filament(index);
    }, "", nullptr, [this]() { return m_mixed_cards.size() > 0; });
    
    // Show popup
    if (anchor) {
        wxPoint pt{0, anchor->GetSize().GetHeight() + 10};
        pt = anchor->ClientToScreen(pt);
        pt = this->ScreenToClient(pt);
        PopupMenu(&menu, pt);
    } else {
        wxPoint client_pos = this->ScreenToClient(screen_pos);
        PopupMenu(&menu, client_pos);
    }
}

void SidebarFilamentMenu::update_physical_filaments() 
{
    if (!m_get_physical_filaments)
        return;

    m_physical_filaments = m_get_physical_filaments();
}

void SidebarFilamentMenu::build_ui(const wxColour& title_bg)
{
    const wxColour material_bg     = StateColor::darkModeColorFor(*wxWHITE);
    const wxColour material_divider = StateColor::darkModeColorFor(wxColour("#CECECE"));
    const wxColour material_title_chip_bg = StateColor::darkModeColorFor(wxColour("#F0F0F1"));
    const wxColour material_title_fg = StateColor::darkModeColorFor(wxColour("#7E7E7E"));

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
                     (m_btn_physical_add->GetPosition().x -
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

    // Icon
    m_btn_icon = new ScalableButton(m_title_panel, wxID_ANY, "filament");
    m_title_sizer->Add(m_btn_icon, 0, wxALIGN_CENTER | wxLEFT, FromDIP(SidebarProps::TitlebarMargin()));
    m_title_sizer->AddSpacer(FromDIP(SidebarProps::ElementSpacing()));

    // Label
    m_lbl_title = new Label(m_title_panel, _L("Material"), LB_PROPAGATE_MOUSE_EVENT);
    m_title_sizer->Add(m_lbl_title, 0, wxALIGN_CENTER, FromDIP(5));
    m_title_sizer->AddSpacer(FromDIP(10));

    m_title_sizer->AddStretchSpacer();

    // Flushing Volumes Button
    m_btn_flushing = new Button(m_title_panel, _L("Flushing volumes"));
    m_btn_flushing->SetStyle(ButtonStyle::Confirm, ButtonType::Compact);
    m_btn_flushing->SetId(wxID_RESET);
    m_btn_flushing->Bind(wxEVT_BUTTON, [this](auto&) {
        if (m_callbacks[ActionType::FlushingVolumes])
            m_callbacks[ActionType::FlushingVolumes]();
    });

    m_title_sizer->Add(m_btn_flushing, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(5));

    // Settings Button
    m_btn_settings = new ScalableButton(m_title_panel, wxID_ANY, "settings");
    m_btn_settings->SetToolTip(_L("Set filaments to use"));
    m_btn_settings->Bind(wxEVT_BUTTON, [this](auto&) {
        if (m_callbacks[ActionType::Settings])
            m_callbacks[ActionType::Settings]();
    });
    m_title_sizer->Add(m_btn_settings, 0, wxALIGN_CENTER | wxLEFT, FromDIP(SidebarProps::IconSpacing()));
    m_title_sizer->AddSpacer(FromDIP(SidebarProps::TitlebarMargin()));

    m_title_panel->SetSizer(m_title_sizer);
    m_title_panel->Layout();


    // ####################################
    // 2. Content Panel
    // ####################################
    m_content_panel = new wxPanel(this);
    m_content_panel->SetBackgroundColour(material_bg);
    m_content_sizer = new wxBoxSizer(wxVERTICAL);
    m_content_panel->SetSizer(m_content_sizer);

    // ####################################
    // 2.1 Physical Title Panel
    // ####################################
    m_physical_title_panel = new wxPanel(m_content_panel);
    m_physical_title_panel->SetBackgroundColour(material_bg);

    m_physical_title_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_physical_title_sizer->SetMinSize({-1, FromDIP(30)});

    m_physical_title_panel->SetSizer(m_physical_title_sizer);

    // nested sizer enables following shrink order: divider->title->buttons
    wxBoxSizer* physical_title_and_divider_sizer = new wxBoxSizer(wxHORIZONTAL);

    auto make_title_chip = [this, material_bg, material_title_chip_bg, material_title_fg](
                               wxWindow* parent, wxStaticText*& title_label, wxStaticText*& counter_label, const wxString& title) {
        StaticBox* chip = new StaticBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        chip->SetBackgroundColour(material_bg);
        chip->SetBackgroundColor(material_title_chip_bg);
        chip->SetBorderWidth(0);
        chip->SetCornerRadius(FromDIP(4));

        wxBoxSizer* chip_sizer = new wxBoxSizer(wxHORIZONTAL);
        chip->SetSizer(chip_sizer);

        title_label = new wxStaticText(chip, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
        title_label->SetForegroundColour(material_title_fg);
        title_label->SetBackgroundColour(material_title_chip_bg);
        title_label->SetFont(::Label::Body_14);

        counter_label = new wxStaticText(chip, wxID_ANY, "", wxDefaultPosition, wxDefaultSize);
        counter_label->SetForegroundColour(material_title_fg);
        counter_label->SetBackgroundColour(material_title_chip_bg);
        counter_label->SetFont(::Label::Body_14);

        chip_sizer->AddSpacer(FromDIP(4));
        chip_sizer->Add(title_label, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(2));
        chip_sizer->Add(counter_label, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(2));
        chip_sizer->AddSpacer(FromDIP(4));
        return chip;
    };

    // Physical title
    wxWindow* physical_title_chip = make_title_chip(m_physical_title_panel, m_lbl_physical_title, m_lbl_physical_counter, _L("Filament"));

    physical_title_and_divider_sizer->Add(physical_title_chip, 0, wxALIGN_CENTER);
    physical_title_and_divider_sizer->AddSpacer(FromDIP(SidebarProps::IconSpacing()));

    // Physical title divider
    m_physical_divider = new wxPanel(m_physical_title_panel);
    m_physical_divider->SetBackgroundColour(material_divider);
    m_physical_divider->SetMinSize({-1, 1});
    m_physical_divider->SetMaxSize({-1, 1});

    physical_title_and_divider_sizer->Add(m_physical_divider, 1, wxALIGN_CENTER | wxEXPAND, FromDIP(SidebarProps::ElementSpacing()));
    physical_title_and_divider_sizer->AddSpacer(FromDIP(SidebarProps::ElementSpacing()));

    m_physical_title_sizer->Add(physical_title_and_divider_sizer, 1, wxEXPAND);

    // Delete button
    m_btn_physical_del = new ScalableButton(m_physical_title_panel, wxID_ANY, "delete_filament");
    m_btn_physical_del->SetToolTip(_L("Remove last filament"));
    m_btn_physical_del->Bind(wxEVT_BUTTON, [this](auto&) {
        if (m_callbacks[ActionType::DeleteFilament])
            m_callbacks[ActionType::DeleteFilament]();
    });

    // ORCA Moved add button after delete button to prevent add button position change when remove icon automatically hidden
    m_physical_title_sizer->Add(m_btn_physical_del, 0, wxALIGN_CENTER | wxLEFT, FromDIP(SidebarProps::IconSpacing()));

    // Add button
    m_btn_physical_add = new ScalableButton(m_physical_title_panel, wxID_ANY, "add_filament");
    m_btn_physical_add->SetToolTip(_L("Add one filament"));
    m_btn_physical_add->Bind(wxEVT_BUTTON, [this](auto&) {
        if (m_callbacks[ActionType::AddFilament])
            m_callbacks[ActionType::AddFilament]();
    });

    m_physical_title_sizer->Add(m_btn_physical_add, 0, wxALIGN_CENTER | wxLEFT, FromDIP(SidebarProps::IconSpacing()));
    
    // AMS Sync button
    m_btn_ams = new ScalableButton(m_physical_title_panel, wxID_ANY, "ams_fila_sync");
    m_btn_ams->SetToolTip(_L("Synchronize filament list from AMS"));
    m_btn_ams->Bind(wxEVT_BUTTON, [this](auto&) {
        if (m_callbacks[ActionType::SyncAMS])
            m_callbacks[ActionType::SyncAMS]();
    });

    m_physical_title_sizer->Add(m_btn_ams, 0, wxALIGN_CENTER | wxLEFT, FromDIP(SidebarProps::IconSpacing()));
    
    m_content_sizer->Add(m_physical_title_panel, 0, wxEXPAND | wxALL, FromDIP(SidebarProps::TitlebarMargin()));


    // ####################################
    // 2.2 Physical Panel
    // ####################################

    // TODO: cant scroll when mouse over physical combo box

    m_physical_panel = new wxScrolledWindow(m_content_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_physical_panel->SetBackgroundColour(material_bg);
    m_physical_panel->SetScrollRate(0, 5);
    m_physical_panel->SetMinSize({-1, -1});
    m_physical_panel->SetMaxSize({-1, m_scrollbar_threshold});
    m_physical_panel->SetVirtualSize({-1, m_scrollbar_threshold}); // show scrollbar when content exceeds m_scrollbar_threshold

    m_physical_sizer = new wxGridSizer(1, FromDIP(2), FromDIP(SidebarProps::ContentMargin() * 2));
    
    wxBoxSizer* physical_intermediate_sizer = new wxBoxSizer(wxVERTICAL);
    physical_intermediate_sizer->Add(m_physical_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(SidebarProps::ContentMargin()));
    m_physical_panel->SetSizer(physical_intermediate_sizer);

    m_content_sizer->Add(m_physical_panel, 0, wxEXPAND);

    // grap panel
    m_physical_grab_panel = new wxPanel(m_content_panel, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(6)), wxBORDER_NONE);
    m_physical_grab_panel->SetBackgroundColour(material_bg);
    m_physical_grab_panel->SetCursor(wxCursor(wxCURSOR_SIZENS));

    m_physical_grab_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent& event) {
        wxPaintDC dc(m_physical_grab_panel);
        wxSize size = m_physical_grab_panel->GetClientSize();
        int thickness = m_physical_grab_line_thickness;
        int y = (size.y - thickness) / 2;
        dc.SetBrush(wxBrush(StateColor::darkModeColorFor(wxColour("#CECECE"))));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, y, size.x, thickness);
    });

    m_physical_grab_panel->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
        set_grab_panel_line_thickness(m_physical_grab_panel, 3);
        start_drag(event, m_physical_drag_state, m_physical_grab_panel, m_physical_panel);
    });
    m_physical_grab_panel->Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
        on_drag(event, m_physical_drag_state, m_physical_panel, m_content_panel, m_physical_grab_panel);
    });
    m_physical_grab_panel->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
        end_drag(event, m_physical_drag_state, m_physical_grab_panel);
        int thickness = m_physical_grab_panel->GetScreenRect().Contains(wxGetMousePosition()) ? 3 : 1;
        set_grab_panel_line_thickness(m_physical_grab_panel, thickness);
    });
    m_physical_grab_panel->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& event) {
        set_grab_panel_line_thickness(m_physical_grab_panel, 3);
        event.Skip();
    });
    m_physical_grab_panel->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
        if (!m_physical_drag_state->is_dragging &&
            !m_physical_grab_panel->GetScreenRect().Contains(wxGetMousePosition())) {
            set_grab_panel_line_thickness(m_physical_grab_panel, 1);
        }
        event.Skip();
    });

    m_content_sizer->Add(m_physical_grab_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(SidebarProps::TitlebarMargin()));

    // add one FilamentCard by default
    m_physical_cards.push_back(nullptr);
    m_physical_cards[0] = new FilamentCardPhysical(m_physical_panel, 0);
    m_physical_cards[0]->set_on_edit_callback([this](int index, wxWindow* anchor) {
        if (m_on_edit_physical)
            m_on_edit_physical(index);
    });
    m_physical_cards[0]->set_on_right_click_callback([this](int index, const wxPoint& screen_pos) {
        if (m_on_right_click_physical)
            m_on_right_click_physical(index, screen_pos);
    });
    m_physical_cards[0]->SetMinSize({FromDIP(20), -1});
    m_physical_sizer->Add(m_physical_cards[0], 0, wxEXPAND);

    m_physical_panel->Layout();

    // ####################################
    // 2.3 Add Mixed Filament (Big Button)
    // ####################################

    // add button (add panel for it!!!)

    // add function in on_filaments_change to show/Hide
    // 
    // hide all
    // if mixed_count > 0
    //      show m_mixed_title_panel & m_mixed_panel
    // else if physical_count > 1
    //      show m_m_btn_mixed_add_big

    // ####################################
    // 2.4 Mixed Title Panel
    // ####################################
    m_mixed_title_panel = new wxPanel(m_content_panel);
    m_mixed_title_panel->SetBackgroundColour(material_bg);

    m_mixed_title_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_mixed_title_sizer->SetMinSize({-1, FromDIP(30)});

    m_mixed_title_panel->SetSizer(m_mixed_title_sizer);

    // nested sizer enables following shrink order: divider->title->buttons
    wxBoxSizer* mixed_title_and_divider_sizer = new wxBoxSizer(wxHORIZONTAL);

    // Mixed title
    wxWindow* mixed_title_chip = make_title_chip(m_mixed_title_panel, m_lbl_mixed_title, m_lbl_mixed_counter, _L("Mixed Filament"));

    mixed_title_and_divider_sizer->Add(mixed_title_chip, 0, wxALIGN_CENTER);
    mixed_title_and_divider_sizer->AddSpacer(FromDIP(SidebarProps::IconSpacing()));

    // Mixed title divider
    m_mixed_divider = new wxPanel(m_mixed_title_panel);
    m_mixed_divider->SetBackgroundColour(material_divider);
    m_mixed_divider->SetMinSize({-1, 1});
    m_mixed_divider->SetMaxSize({-1, 1});

    mixed_title_and_divider_sizer->Add(m_mixed_divider, 1, wxALIGN_CENTER | wxEXPAND, FromDIP(SidebarProps::ElementSpacing()));
    mixed_title_and_divider_sizer->AddSpacer(FromDIP(SidebarProps::ElementSpacing()));

    m_mixed_title_sizer->Add(mixed_title_and_divider_sizer, 1, wxEXPAND);

    // Manage button
    m_btn_mixed_manage = new Button(m_mixed_title_panel, _L("Manage"));
    m_btn_mixed_manage->SetStyle(ButtonStyle::Confirm, ButtonType::Compact);
    m_btn_mixed_manage->Bind(wxEVT_BUTTON, [this](auto&) {
        MixedFilamentBatchDialog dlg(this, m_physical_filaments);
        if (dlg.ShowModal() == wxID_OK) {
            dlg.apply_batch_changes();
        }
    });
    m_mixed_title_sizer->Add(m_btn_mixed_manage, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(5));

    // Delete button
    m_btn_mixed_del = new ScalableButton(m_mixed_title_panel, wxID_ANY, "delete_filament");
    m_btn_mixed_del->SetToolTip(_L("Remove last mixed filament"));
    m_btn_mixed_del->Bind(wxEVT_BUTTON, [this](auto&) {
        auto* preset_bundle = wxGetApp().preset_bundle;
        if (preset_bundle) {
            auto &mgr = preset_bundle->mixed_filaments;
            const size_t num_physical = m_physical_filaments.size();
            std::vector<MixedFilamentDefinition> definitions = mgr.mixed_filament_definitions(num_physical);
            std::vector<MixedFilamentDefinition> old_mixed = definitions;

            int last_visible_idx = -1;
            for (int i = static_cast<int>(definitions.size()) - 1; i >= 0; --i) {
                if (!definitions[i].visibility.tombstoned) {
                    last_visible_idx = i;
                    break;
                }
            }

            if (last_visible_idx >= 0) {
                auto &target = definitions[last_visible_idx];
                auto canonical_pair = [](unsigned int a, unsigned int b) {
                    return std::make_pair(std::min(a, b), std::max(a, b));
                };
                const MixedFilamentPrimaryPairView target_pair_view = target.recipe.blend.primary_pair_or();
                const std::pair<unsigned int, unsigned int> target_pair = canonical_pair(target_pair_view.component_a.id, target_pair_view.component_b.id);
                const bool valid_auto_pair = target_pair.first >= 1 &&
                                             target_pair.second >= 1 &&
                                             target_pair.first <= num_physical &&
                                             target_pair.second <= num_physical &&
                                             target_pair.first != target_pair.second;

                if (target.source.kind == MixedFilamentSourceKind::Custom && target.source.origin_auto && valid_auto_pair) {
                    bool tombstoned_existing_auto = false;
                    for (size_t idx = 0; idx < definitions.size(); ++idx) {
                        if (idx == static_cast<size_t>(last_visible_idx))
                            continue;
                        MixedFilamentDefinition &candidate = definitions[idx];
                        if (candidate.source.kind == MixedFilamentSourceKind::Custom)
                            continue;
                        const MixedFilamentPrimaryPairView candidate_pair = candidate.recipe.blend.primary_pair_or();
                        if (canonical_pair(candidate_pair.component_a.id, candidate_pair.component_b.id) != target_pair)
                            continue;
                        candidate.visibility.tombstoned = true;
                        tombstoned_existing_auto = true;
                        break;
                    }

                    if (tombstoned_existing_auto) {
                        definitions.erase(definitions.begin() + last_visible_idx);
                    } else {
                        target.recipe.kind = MixedFilamentRecipeKind::WeightedBlend;
                        target.recipe.blend.components = {
                            {MixedFilamentPhysicalRef{target_pair.first}, 50},
                            {MixedFilamentPhysicalRef{target_pair.second}, 50}
                        };
                        target.recipe.manual_pattern.reset();
                        target.behavior.layer_cadence.component_a_layers = 1;
                        target.behavior.layer_cadence.component_b_layers = 1;
                        target.behavior.distribution = MixedFilamentDistributionMode::Simple;
                        target.source.kind = MixedFilamentSourceKind::AutoGenerated;
                        target.source.origin_auto = true;
                        target.visibility.tombstoned = true;
                    }
                } else if (target.source.kind == MixedFilamentSourceKind::Custom) {
                    definitions.erase(definitions.begin() + last_visible_idx);
                } else {
                    target.visibility.tombstoned = true;
                }

                std::vector<std::string> physical_colors;
                for (const auto& p : m_physical_filaments) {
                    physical_colors.push_back(p.first);
                }

                mgr.set_mixed_filament_definitions(definitions, physical_colors);

                const std::string serialized = mgr.serialize_custom_entries();
                if (ConfigOptionString *opt = preset_bundle->project_config.option<ConfigOptionString>("mixed_filament_definitions"))
                    opt->value = serialized;
                else
                    preset_bundle->project_config.set_key_value("mixed_filament_definitions", new ConfigOptionString(serialized));

                preset_bundle->update_mixed_filament_id_remap(old_mixed, num_physical, num_physical);

                if (auto* print_tab = wxGetApp().get_tab(Preset::TYPE_PRINT))
                    print_tab->update_dirty();
                if (wxGetApp().mainframe)
                    wxGetApp().mainframe->on_config_changed(&preset_bundle->project_config);

                wxGetApp().plater()->update_project_dirty_from_presets();
                wxGetApp().plater()->on_filaments_change(num_physical);
            }
        }
    });

    // ORCA Moved add button after delete button to prevent add button position change when remove icon automatically hidden
    m_mixed_title_sizer->Add(m_btn_mixed_del, 0, wxALIGN_CENTER | wxLEFT, FromDIP(SidebarProps::IconSpacing()));

    // Add button
    m_btn_mixed_add = new ScalableButton(m_mixed_title_panel, wxID_ANY, "add_filament");
    m_btn_mixed_add->SetToolTip(_L("Add one mixed filament"));
    m_btn_mixed_add->Bind(wxEVT_BUTTON, [this](auto&) {
        MixedFilamentDialog dlg(this, MixedFilamentDialog::Action::Add, m_physical_filaments);
        if (dlg.ShowModal() == wxID_OK) {
            MixedFilamentDefinition new_def = dlg.get_result();
            auto* preset_bundle = wxGetApp().preset_bundle;
            if (preset_bundle) {
                auto &mgr = preset_bundle->mixed_filaments;
                const size_t num_physical = m_physical_filaments.size();
                std::vector<MixedFilamentDefinition> old_mixed = mgr.mixed_filament_definitions(num_physical);

                std::vector<std::string> physical_colors;
                for (const auto& p : m_physical_filaments) {
                    physical_colors.push_back(p.first);
                }

                mgr.add_custom_filament_definition(new_def, physical_colors);

                const std::string serialized = mgr.serialize_custom_entries();
                if (ConfigOptionString *opt = preset_bundle->project_config.option<ConfigOptionString>("mixed_filament_definitions"))
                    opt->value = serialized;
                else
                    preset_bundle->project_config.set_key_value("mixed_filament_definitions", new ConfigOptionString(serialized));

                preset_bundle->update_mixed_filament_id_remap(old_mixed, num_physical, num_physical);

                if (auto* print_tab = wxGetApp().get_tab(Preset::TYPE_PRINT))
                    print_tab->update_dirty();
                if (wxGetApp().mainframe)
                    wxGetApp().mainframe->on_config_changed(&preset_bundle->project_config);

                wxGetApp().plater()->update_project_dirty_from_presets();
                wxGetApp().plater()->on_filaments_change(num_physical);
            }
        }
    });

    m_mixed_title_sizer->Add(m_btn_mixed_add, 0, wxALIGN_CENTER | wxLEFT, FromDIP(SidebarProps::IconSpacing()));

    m_content_sizer->Add(m_mixed_title_panel, 0, wxEXPAND | wxALL, FromDIP(SidebarProps::TitlebarMargin()));

    
    // ####################################
    // 2.5 Mixed Panel
    // ####################################
    m_mixed_panel = new wxScrolledWindow(m_content_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_mixed_panel->SetBackgroundColour(material_bg);
    m_mixed_panel->SetScrollRate(0, 5);
    m_mixed_panel->SetMinSize({-1, -1});
    m_mixed_panel->SetMaxSize({-1, m_scrollbar_threshold});
    m_mixed_panel->SetVirtualSize({-1, m_scrollbar_threshold}); // show scrollbar when content exceeds m_scrollbar_threshold

    m_mixed_sizer = new wxGridSizer(1, FromDIP(2), FromDIP(SidebarProps::ContentMargin() * 2));

    wxBoxSizer* mixed_intermediate_sizer = new wxBoxSizer(wxVERTICAL);
    mixed_intermediate_sizer->Add(m_mixed_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(SidebarProps::ContentMargin()));
    m_mixed_panel->SetSizer(mixed_intermediate_sizer);

    m_content_sizer->Add(m_mixed_panel, 0, wxEXPAND);
    
    // grap panel
    m_mixed_grab_panel = new wxPanel(m_content_panel, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(6)), wxBORDER_NONE);
    m_mixed_grab_panel->SetBackgroundColour(material_bg);
    m_mixed_grab_panel->SetCursor(wxCursor(wxCURSOR_SIZENS));

    m_mixed_grab_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent& event) {
        wxPaintDC dc(m_mixed_grab_panel);
        wxSize size = m_mixed_grab_panel->GetClientSize();
        int thickness = m_mixed_grab_line_thickness;
        int y = (size.y - thickness) / 2;
        dc.SetBrush(wxBrush(StateColor::darkModeColorFor(wxColour("#CECECE"))));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, y, size.x, thickness);
    });

    m_mixed_grab_panel->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
        set_grab_panel_line_thickness(m_mixed_grab_panel, 3);
        start_drag(event, m_mixed_drag_state, m_mixed_grab_panel, m_mixed_panel);
    });
    m_mixed_grab_panel->Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
        on_drag(event, m_mixed_drag_state, m_mixed_panel, m_content_panel, m_mixed_grab_panel);
    });
    m_mixed_grab_panel->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
        end_drag(event, m_mixed_drag_state, m_mixed_grab_panel);
        int thickness = m_mixed_grab_panel->GetScreenRect().Contains(wxGetMousePosition()) ? 3 : 1;
        set_grab_panel_line_thickness(m_mixed_grab_panel, thickness);
    });
    m_mixed_grab_panel->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& event) {
        set_grab_panel_line_thickness(m_mixed_grab_panel, 3);
        event.Skip();
    });
    m_mixed_grab_panel->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
        if (!m_mixed_drag_state->is_dragging &&
            !m_mixed_grab_panel->GetScreenRect().Contains(wxGetMousePosition())) {
            set_grab_panel_line_thickness(m_mixed_grab_panel, 1);
        }
        event.Skip();
    });

    m_content_sizer->Add(m_mixed_grab_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(SidebarProps::TitlebarMargin()));
    m_mixed_list_bottom_spacer = m_content_sizer->AddSpacer(FromDIP(4));
    
    // ####################################
    // 0. General Layout
    // ####################################

    m_content_panel->Layout();

    auto splitter_before_title = new ::StaticLine(this);
    splitter_before_title->SetLineColour(StateColor::darkModeColorFor(wxColour("#A6A9AA")));
    auto splitter_after_title = new ::StaticLine(this);
    splitter_after_title->SetLineColour(material_divider);

    m_main_sizer->Add(splitter_before_title, 0, wxEXPAND);
    m_main_sizer->Add(m_title_panel, 0, wxEXPAND);
    m_main_sizer->Add(splitter_after_title, 0, wxEXPAND);
    m_main_sizer->Add(m_content_panel, 0, wxEXPAND);
    SetSizer(m_main_sizer);

    show_SEMM_buttons(true);
}

void SidebarFilamentMenu::update_title(const wxString& label, const std::string& icon_name)
{
    wxString singular_label = label;
    if (label == _L("Filaments")) {
        singular_label = _L("Filament");
    } else if (label == _L("Pellets")) {
        singular_label = _L("Pellet");
    }
    m_lbl_physical_title->SetLabel(singular_label);
    m_lbl_physical_title->GetParent()->Layout();
    m_physical_title_panel->Layout();
    m_btn_icon->SetBitmap_(icon_name);
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

void SidebarFilamentMenu::update_mixed_states(std::vector<MixedFilamentDefinition>& mixed_filaments)
{
    m_mixed_filaments = mixed_filaments;

    int mixed_filament_count = static_cast<int>(m_mixed_filaments.size());
    int mixed_card_count     = m_mixed_count();

    for (int i = 0; i < mixed_card_count; i++) {
        if (i < mixed_filament_count)
            m_mixed_cards[i]->update_state(&m_mixed_filaments[i], true);
    }
}

void SidebarFilamentMenu::msw_rescale()
{
    m_title_sizer->SetMinSize(-1, 3 * wxGetApp().em_unit());

    m_btn_icon->msw_rescale();
    m_btn_physical_add->msw_rescale();
    m_btn_physical_del->msw_rescale();
    m_btn_ams->msw_rescale();
    m_btn_settings->msw_rescale();
    m_btn_flushing->Rescale();
    if (m_btn_mixed_manage)
        m_btn_mixed_manage->Rescale();

    for (auto* card : m_physical_cards) {
        card->m_filament_combo_box->msw_rescale();
    }
}

void SidebarFilamentMenu::sys_color_changed()
{
    m_btn_icon->msw_rescale();
    m_btn_physical_add->msw_rescale();
    m_btn_physical_del->msw_rescale();
    m_btn_ams->msw_rescale();
    m_btn_settings->msw_rescale();
    m_btn_flushing->Rescale();
    if (m_btn_mixed_manage)
        m_btn_mixed_manage->Rescale();

    for (auto* card : m_physical_cards) {
        card->m_filament_combo_box->sys_color_changed();
    }
}

void SidebarFilamentMenu::toggle_collapse(bool only_open = false)
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

void SidebarFilamentMenu::show_SEMM_buttons(bool is_single_extruder_multi_material = true)
{
    // Show add buttons only if the printer supports multi-material
    if (m_btn_physical_add)
        m_btn_physical_add->Show(is_single_extruder_multi_material);

    // Show/hide buttons based on whether there are filaments to delete/flush
    if (m_btn_physical_del)
        m_btn_physical_del->Show(is_single_extruder_multi_material && m_physical_count() > 1);

    if (m_btn_flushing)
        m_btn_flushing->Show(is_single_extruder_multi_material && m_physical_count() > 1);

    Layout();
}

void SidebarFilamentMenu::show_AMS_button(bool show)
{
    m_btn_ams->Show(show);
    Layout();
}

void SidebarFilamentMenu::start_drag(const wxMouseEvent& event, DragState* drag_state, wxPanel* grap_panel, wxScrolledWindow* panel)
{
    drag_state->is_dragging = true;
    drag_state->drag_start_y = wxGetMousePosition().y;
    drag_state->panel_start_height = panel->GetSize().y;
    drag_state->max_content_height = panel->GetSizer()->GetMinSize().y;
    grap_panel->CaptureMouse();
}

void SidebarFilamentMenu::end_drag(const wxMouseEvent& event, DragState* drag_state, wxPanel* grap_panel)
{
    if (drag_state->is_dragging) {
        drag_state->is_dragging = false;

        if (grap_panel->HasCapture())
            grap_panel->ReleaseMouse();
    }
}

void SidebarFilamentMenu::on_drag(const wxMouseEvent& event, DragState* drag_state, wxScrolledWindow* panel, wxPanel* parent_panel, wxPanel* grab_panel)
{
    if (drag_state->is_dragging && event.LeftIsDown()) {
        const int current_y = wxGetMousePosition().y;
        const int delta_y   = current_y - drag_state->drag_start_y;
        int new_height = drag_state->panel_start_height + delta_y;

        new_height = std::clamp(new_height, m_scrollbar_threshold, drag_state->max_content_height);

        if (panel->GetSize().y == new_height) {
            return;
        }

        // Throttle layout updates to avoid excessive CPU usage during dragging
        auto now     = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - drag_state->last_layout_time).count();

        // only update own layout instantly (15fps)
        if (elapsed < 64) {
            panel->SetMinSize({-1, new_height});
            panel->SetMaxSize({-1, new_height});

            return;
        }

        drag_state->last_layout_time = now;
        
        panel->SetMinSize({-1, new_height});
        panel->SetMaxSize({-1, new_height});

        parent_panel->Layout();
        update_grab_panel_visibility(panel, parent_panel, m_physical_grab_panel, drag_state->max_content_height);

        this->InvalidateBestSize();
        this->Layout();

        if (this->GetParent()) {
            this->GetParent()->Layout();
        }
    }
}

void SidebarFilamentMenu::update_grab_panel_visibility(wxScrolledWindow* panel, wxPanel* parent_panel, wxPanel* grab_panel, int max_content_height)
{
    if (!panel || !grab_panel)
        return;

    const bool show_grab_panel = max_content_height >= m_scrollbar_threshold;

    if (grab_panel->IsShown() != show_grab_panel) {
        grab_panel->Show(show_grab_panel);

        parent_panel->Layout();
        this->InvalidateBestSize();
        this->Layout();

        if (this->GetParent()) {
            this->GetParent()->Layout();
        }
    }
}

void SidebarFilamentMenu::set_grab_panel_line_thickness(wxPanel* grab_panel, int thickness)
{
    if (grab_panel == m_physical_grab_panel) {
        m_physical_grab_line_thickness = thickness;
    } else if (grab_panel == m_mixed_grab_panel) {
        m_mixed_grab_line_thickness = thickness;
    }
    grab_panel->Refresh();
}

} // namespace Slic3r::GUI
