#include "MFDBatchRecommendedAccordion.hpp"
#include "MixedFilamentBatchDialog.hpp"
#include "Widgets/FilamentCardMixed.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MFDTheme.hpp"
#include <wx/dcgraph.h>
#include <numeric>

namespace Slic3r::GUI {

// ==========================================
// CollapsibleSubSection Implementation
// ==========================================

CollapsibleSubSection::CollapsibleSubSection(wxWindow* parent, const wxString& title)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL)
{
    build_ui(title);
}

void CollapsibleSubSection::build_ui(const wxString& title)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &CollapsibleSubSection::on_paint, this);
    SetDoubleBuffered(true);

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(main_sizer);

    // Header Panel
    m_header = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(24)), wxBORDER_NONE);
    m_header->SetBackgroundColour(MFDTheme::card_background());
    m_header->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_header->SetCursor(wxCursor(wxCURSOR_HAND));
    m_header->SetDoubleBuffered(true);

    wxBoxSizer* header_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_header->SetSizer(header_sizer);

    // Spacer for chevron drawing space
    header_sizer->AddSpacer(FromDIP(20));

    // Title
    m_lbl_title = new wxStaticText(m_header, wxID_ANY, title);
    m_lbl_title->SetFont(::Label::Body_13);
    MFDTheme::apply_text(m_lbl_title, MFDTheme::muted_text(), m_header->GetBackgroundColour());

    // Info Count
    m_lbl_info = new wxStaticText(m_header, wxID_ANY, "");
    m_lbl_info->SetFont(::Label::Body_12);
    MFDTheme::apply_text(m_lbl_info, MFDTheme::secondary_text(), m_header->GetBackgroundColour());

    // Custom Checkbox
    m_check_box = new BatchCheckBox(m_header, [this]() {
        if (on_check_clicked)
            on_check_clicked();
    });

    header_sizer->Add(m_lbl_title, 0, wxALIGN_CENTER_VERTICAL);
    header_sizer->AddStretchSpacer(1);
    header_sizer->Add(m_lbl_info, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    header_sizer->Add(m_check_box, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));

    // Body Panel
    m_body = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_body->SetBackgroundColour(MFDTheme::card_background());

    main_sizer->Add(m_header, 0, wxEXPAND | wxALL, FromDIP(4));
    main_sizer->Add(m_body, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(4));

    // Hover highlights tracking
    auto header_hover = std::make_shared<bool>(false);
    
    auto enter_header = [this, header_hover](wxMouseEvent& e) {
        *header_hover = true;
        m_lbl_title->SetForegroundColour(MFDTheme::primary_text());
        m_header->Refresh();
        e.Skip();
    };
    auto leave_header = [this, header_hover](wxMouseEvent& e) {
        wxPoint pos = wxGetMousePosition();
        wxRect rect = m_header->GetScreenRect();
        if (!rect.Contains(pos)) {
            *header_hover = false;
            m_lbl_title->SetForegroundColour(MFDTheme::muted_text());
            m_header->Refresh();
        }
        e.Skip();
    };

    m_header->Bind(wxEVT_ENTER_WINDOW, enter_header);
    m_header->Bind(wxEVT_LEAVE_WINDOW, leave_header);
    m_lbl_title->Bind(wxEVT_ENTER_WINDOW, enter_header);
    m_lbl_title->Bind(wxEVT_LEAVE_WINDOW, leave_header);
    m_lbl_info->Bind(wxEVT_ENTER_WINDOW, enter_header);
    m_lbl_info->Bind(wxEVT_LEAVE_WINDOW, leave_header);

    m_header->Bind(wxEVT_PAINT, [this, header_hover](wxPaintEvent&) {
        wxPaintDC pdc(m_header);
        wxGCDC dc(pdc);
        wxSize s = m_header->GetClientSize();

        dc.SetBackground(wxBrush(MFDTheme::card_background()));
        dc.Clear();

        // Draw antialiased thin chevron arrow
        wxColor chevron_color;
        if (wxGetApp().dark_mode()) {
            chevron_color = *header_hover ? *wxWHITE : wxColour(150, 150, 150);
        } else {
            chevron_color = *header_hover ? wxColour(26, 26, 26) : wxColour(102, 102, 102);
        }
        dc.SetPen(wxPen(chevron_color, FromDIP(1)));
        int cx = FromDIP(10);
        int cy = s.y / 2;
        if (m_collapsed) {
            // Point up (caret shape ^)
            dc.DrawLine(cx - 4, cy + 2, cx, cy - 2);
            dc.DrawLine(cx, cy - 2, cx + 4, cy + 2);
        } else {
            // Point down (V shape)
            dc.DrawLine(cx - 4, cy - 2, cx, cy + 2);
            dc.DrawLine(cx, cy + 2, cx + 4, cy - 2);
        }
    });

    m_header->Bind(wxEVT_LEFT_UP, &CollapsibleSubSection::on_header_click, this);
    m_lbl_title->Bind(wxEVT_LEFT_UP, &CollapsibleSubSection::on_header_click, this);
    m_lbl_info->Bind(wxEVT_LEFT_UP, &CollapsibleSubSection::on_header_click, this);

    m_body->Show(!m_collapsed);
}

void CollapsibleSubSection::on_paint(wxPaintEvent&)
{
    wxPaintDC pdc(this);
    wxGCDC dc(pdc);
    wxSize size = GetSize();

    // Draw parent background color first
    wxColour parent_bg = GetParent() ? GetParent()->GetBackgroundColour() : GetBackgroundColour();
    dc.SetBrush(wxBrush(parent_bg));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(0, 0, size.x, size.y);

    // Rounded card with slightly darker grey border
    wxColour card_bg = MFDTheme::card_background();
    dc.SetBrush(wxBrush(card_bg));
    dc.SetPen(wxPen(MFDTheme::card_border(), 1));
    dc.DrawRoundedRectangle(0, 0, size.x, size.y, 4);
}

void CollapsibleSubSection::on_header_click(wxMouseEvent&)
{
    toggle();
}

void CollapsibleSubSection::toggle()
{
    m_collapsed = !m_collapsed;
    m_body->Show(!m_collapsed);
    m_header->Refresh();

    Layout();
    
    // Bubble layout up directly to the scrolled window
    wxWindow* p = GetParent();
    wxScrolledWindow* scrolled = nullptr;
    while (p) {
        if (auto* s = dynamic_cast<wxScrolledWindow*>(p)) {
            scrolled = s;
            break;
        }
        p = p->GetParent();
    }
    if (scrolled) {
        scrolled->Layout();
        scrolled->FitInside();
    } else {
        Layout();
    }

    if (on_toggle)
        on_toggle(m_collapsed);
}

void CollapsibleSubSection::set_check_state(int state)
{
    if (m_check_box)
        m_check_box->set_state(state);
}

int CollapsibleSubSection::get_check_state() const
{
    return m_check_box ? m_check_box->get_state() : 0;
}

void CollapsibleSubSection::set_info(int add_count, int del_count)
{
    wxString txt = "";
    if (add_count > 0 || del_count > 0) {
        if (add_count > 0) txt += wxString::Format("+%d", add_count);
        if (del_count > 0) {
            if (!txt.IsEmpty()) txt += " ";
            txt += wxString::Format("-%d", del_count);
        }
    }
    m_lbl_info->SetLabel(txt);
    m_header->Layout();
}

// ==========================================
// MFDBatchRecommendedAccordion Implementation
// ==========================================

MFDBatchRecommendedAccordion::MFDBatchRecommendedAccordion(wxWindow* parent, std::vector<BatchMixItem>& mix_items,
                                                           const std::vector<std::pair<std::string, std::string>>& physical_filaments)
    : Accordion(parent, _L("Mixing Recommendations"))
    , m_mix_items(mix_items)
    , m_physical_filaments(physical_filaments)
{
    m_physical_enabled.resize(m_physical_filaments.size(), true);
    build_ui();
}

void MFDBatchRecommendedAccordion::build_ui()
{
    wxPanel* body = get_body_panel();
    wxBoxSizer* sizer = get_body_sizer();

    // Explainer Label
    m_explainer_lbl = new wxStaticText(body, wxID_ANY, _L("Toggle filaments to enable or disable them for the recommended mixes"));
    m_explainer_lbl->SetFont(::Label::Body_13);
    MFDTheme::apply_text(m_explainer_lbl, MFDTheme::secondary_text(), body->GetBackgroundColour());
    m_explainer_lbl->SetMinSize(wxSize(FromDIP(400), -1));

    sizer->Add(m_explainer_lbl, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));

    // Physical Swatches Selector Row
    m_phys_sizer = new wxWrapSizer(wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS);
    build_physical_row();
    sizer->Add(m_phys_sizer, 0, wxEXPAND | wxALL, FromDIP(8));
    sizer->AddSpacer(FromDIP(4));

    // Three subaccordions
    build_subaccordions();
}

void MFDBatchRecommendedAccordion::build_physical_row()
{
    wxPanel* body = get_body_panel();
    m_phys_tiles.clear();

    for (size_t idx = 0; idx < m_physical_filaments.size(); ++idx) {
        PhysicalFilamentTile* tile = new PhysicalFilamentTile(
            body,
            idx,
            m_physical_filaments[idx].first,
            m_physical_filaments[idx].second,
            m_physical_enabled,
            [this]() {
                // Re-evaluate recommended swatches visibility dynamically based on enabled physical filaments
                auto filter_tiles = [this](auto& items_list) {
                    for (auto& pair : items_list) {
                        bool ok = true;
                        for (int phys : pair.first->physical_indices) {
                            if (phys >= 0 && phys < m_physical_enabled.size() && !m_physical_enabled[phys]) {
                                ok = false;
                                break;
                            }
                        }
                        pair.second->Show(ok);
                    }
                };
                
                filter_tiles(m_items_50_50);
                filter_tiles(m_items_34_66);
                filter_tiles(m_items_3way);

                // Perform instant layout update
                update_states();

                // Notify parent dialog
                if (m_on_item_toggled)
                    m_on_item_toggled();
            }
        );

        m_phys_tiles.push_back(tile);
        m_phys_sizer->Add(tile, 0, wxALL, FromDIP(2));
    }
}

void MFDBatchRecommendedAccordion::build_subaccordions()
{
    wxPanel* body = get_body_panel();
    wxBoxSizer* sizer = get_body_sizer();

    // 50/50
    m_sub_50_50 = new CollapsibleSubSection(body, _L("50/50 Mixes"));
    m_wrap_50_50 = new wxWrapSizer(wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS);
    m_sub_50_50->get_body()->SetSizer(m_wrap_50_50);
    build_tiles_for_subsection(m_sub_50_50, m_wrap_50_50, 0, m_items_50_50);
    sizer->Add(m_sub_50_50, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    // 34/66
    m_sub_34_66 = new CollapsibleSubSection(body, _L("34/66 Mixes"));
    m_wrap_34_66 = new wxWrapSizer(wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS);
    m_sub_34_66->get_body()->SetSizer(m_wrap_34_66);
    build_tiles_for_subsection(m_sub_34_66, m_wrap_34_66, 1, m_items_34_66);
    sizer->Add(m_sub_34_66, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    // 3-Way
    m_sub_3way = new CollapsibleSubSection(body, _L("33/33/34 Mixes"));
    m_wrap_3way = new wxWrapSizer(wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS);
    m_sub_3way->get_body()->SetSizer(m_wrap_3way);
    build_tiles_for_subsection(m_sub_3way, m_wrap_3way, 2, m_items_3way);
    sizer->Add(m_sub_3way, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
}

void MFDBatchRecommendedAccordion::build_tiles_for_subsection(CollapsibleSubSection* sub, wxWrapSizer* sizer,
                                                               int mix_type, std::vector<std::pair<BatchMixItem*, BatchSwatchTile*>>& items_list)
{
    wxPanel* sub_body = sub->get_body();
    items_list.clear();

    for (size_t idx = 0; idx < m_mix_items.size(); ++idx) {
        auto& item = m_mix_items[idx];
        if (!item.is_recommended)
            continue;

        // Categorize recommended mixes:
        // mix_type: 0 = 50/50 (2 components, equal), 1 = 34/66 (2 components, unequal), 2 = 3-Way
        bool match = false;
        if (item.physical_indices.size() == 2) {
            bool is_equal = item.percentages[0] == item.percentages[1];
            if (mix_type == 0 && is_equal) match = true;
            if (mix_type == 1 && !is_equal) match = true;
        } else if (item.physical_indices.size() == 3) {
            if (mix_type == 2) match = true;
        }

        if (!match)
            continue;

        BatchSwatchTile* tile = new BatchSwatchTile(sub_body, &item, [this]() {
            update_states();
            if (m_on_item_toggled)
                m_on_item_toggled();
        });

        items_list.push_back({&item, tile});
        sizer->Add(tile, 0, wxALL, FromDIP(2));
    }

    // Set up checkmark click callback for this subaccordion
    sub->on_check_clicked = [this, sub, mix_type]() {
        auto& list = *((mix_type == 0) ? &m_items_50_50 : ((mix_type == 1) ? &m_items_34_66 : &m_items_3way));
        // Find if this list has existing mixes
        bool has_existing = false;
        for (auto& pair : list) {
            if (pair.first->is_existing) {
                has_existing = true;
                break;
            }
        }

        int current = sub->get_check_state();
        int next = 0;
        if (has_existing) {
            // Cycle: Box (1) -> Checked (2) -> Empty (0) -> Box (1)
            if (current == 1) next = 2;
            else if (current == 2) next = 0;
            else next = 1;
        } else {
            // Cycle: Empty (0) -> Checked (2) -> Empty (0)
            if (current == 0) next = 2;
            else next = 0;
        }

        // Apply state to all visible recommended items in this subaccordion
        for (auto& pair : list) {
            if (!pair.second->IsShown())
                continue;
            BatchMixItem* item = pair.first;
            if (next == 2) {
                if (item->is_existing) item->is_deleted = false;
                else item->is_added = true;
            } else if (next == 0) {
                if (item->is_existing) item->is_deleted = true;
                else item->is_added = false;
            } else {
                // Box (keep existing only)
                if (item->is_existing) item->is_deleted = false;
                else item->is_added = false;
            }
            pair.second->Refresh();
        }

        update_states();
        if (m_on_item_toggled)
            m_on_item_toggled();
    };

    // Subaccordion collapse callback (info count is updated via update_subaccordion_state)
    sub->on_toggle = [this, sub, mix_type](bool collapsed) {
        // No-op since count is shown all the time and managed by update_subaccordion_state
    };
}

void MFDBatchRecommendedAccordion::update_subaccordion_state(CollapsibleSubSection* sub, const std::vector<std::pair<BatchMixItem*, BatchSwatchTile*>>& items_list)
{
    if (items_list.empty()) {
        sub->set_check_state(0);
        sub->set_info(0, 0);
        return;
    }

    bool all_added = true;
    bool all_removed = true;
    bool only_existing = true;
    bool has_existing = false;
    int visible_count = 0;

    int add_cnt = 0;
    int del_cnt = 0;

    for (const auto& pair : items_list) {
        if (!pair.second->IsShown())
            continue;
        
        visible_count++;
        BatchMixItem* item = pair.first;
        bool active = (item->is_existing && !item->is_deleted) || (!item->is_existing && item->is_added);
        if (active) {
            all_removed = false;
            if (!item->is_existing)
                add_cnt++;
        } else {
            all_added = false;
            if (item->is_existing)
                del_cnt++;
        }

        if (item->is_existing) {
            has_existing = true;
            if (item->is_deleted) {
                only_existing = false;
            }
        } else {
            if (item->is_added) {
                only_existing = false;
            }
        }
    }

    if (visible_count == 0) {
        sub->set_check_state(0);
        sub->set_info(0, 0);
        return;
    }

    int state = 0;
    if (all_added) {
        state = 2; // Checked
    } else if (all_removed) {
        state = 0; // Empty
    } else if (has_existing && only_existing) {
        state = 1; // Box
    } else {
        state = 1; // Indeterminate/Box state
    }

    sub->set_check_state(state);
    
    // Update count info badge (shown all the time)
    sub->set_info(add_cnt, del_cnt);

    // Refresh all visible item swatches
    for (const auto& pair : items_list) {
        if (pair.second->IsShown())
            pair.second->Refresh();
    }
}

void MFDBatchRecommendedAccordion::update_states()
{
    // Update physical swatches
    for (auto* tile : m_phys_tiles) {
        tile->Refresh();
    }

    // Update the three subaccordions
    update_subaccordion_state(m_sub_50_50, m_items_50_50);
    update_subaccordion_state(m_sub_34_66, m_items_34_66);
    update_subaccordion_state(m_sub_3way, m_items_3way);

    // Layout the child subsections first so their heights are correct
    m_sub_50_50->Layout();
    m_sub_34_66->Layout();
    m_sub_3way->Layout();

    Layout();

    // Bubble layout directly to scrolled window
    wxWindow* p = GetParent();
    wxScrolledWindow* scrolled = nullptr;
    while (p) {
        if (auto* s = dynamic_cast<wxScrolledWindow*>(p)) {
            scrolled = s;
            break;
        }
        p = p->GetParent();
    }
    if (scrolled) {
        scrolled->Layout();
        scrolled->FitInside();
    }
}

void MFDBatchRecommendedAccordion::wrap_explainer(int client_width)
{
    if (m_explainer_lbl) {
        int wrap_w = client_width - FromDIP(48);
        if (wrap_w > FromDIP(100)) {
            m_explainer_lbl->Wrap(wrap_w);
        }
    }
}

} // namespace Slic3r::GUI
