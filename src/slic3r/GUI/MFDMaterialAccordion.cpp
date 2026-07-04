#include "MFDMaterialAccordion.hpp"

#include <wx/wx.h>
#include <wx/dcmemory.h>
#include <cmath>

#include "I18N.hpp"
#include "GUI_App.hpp"
#include "GUI_Factories.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/Button.hpp"
#include "wxExtensions.hpp"
#include "Widgets/FilamentCardMixed.hpp"

namespace Slic3r::GUI {

MFDMaterialAccordion::MFDMaterialAccordion(
    wxWindow*                                                   parent,
    const std::vector<std::pair<std::string, std::string>>&     physical_filaments,
    int clr_swatch_size_dip,
    int min_filament,
    int max_filament)
    : Accordion(parent, _L("Select Mixed Materials"))
    , m_physical_filaments(physical_filaments)
    , m_clr_swatch_size_dip(clr_swatch_size_dip)
    , m_min_filament(min_filament)
    , m_max_filament(max_filament)
{
    build_ui();
}

void MFDMaterialAccordion::build_ui()
{
    // Add/delete action buttons in the header.
    // They are registered as action controls so clicking them does NOT collapse the section.
    m_delete_btn = new ScalableButton(get_header_panel(), wxID_ANY, "delete_filament");
    m_delete_btn->SetBackgroundColour(GetBackgroundColour());
    m_delete_btn->SetToolTip(_L("Remove last material"));
    m_delete_btn->Enable(false);
    m_delete_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_on_remove_filament) m_on_remove_filament();
    });
    add_header_control(m_delete_btn, /*is_action_control=*/true);

    m_add_btn = new ScalableButton(get_header_panel(), wxID_ANY, "add_filament");
    m_add_btn->SetBackgroundColour(GetBackgroundColour());
    m_add_btn->SetToolTip(_L("Add material"));
    m_add_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_on_add_filament) m_on_add_filament();
    });
    add_header_control(m_add_btn, /*is_action_control=*/true);

    // Header summary panel (shown when collapsed)
    m_title_preview_panel = new wxPanel(get_header_panel(), wxID_ANY,
        wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_title_preview_panel->SetBackgroundColour(GetBackgroundColour());
    m_title_preview_panel->Show(false);
    add_header_control(m_title_preview_panel);

    // Body: the combobox container
    m_combobox_panel = new wxPanel(get_body_panel(), wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_combobox_sizer = new wxBoxSizer(wxVERTICAL);
    m_combobox_panel->SetSizer(m_combobox_sizer);
    get_body_sizer()->Add(m_combobox_panel, 0, wxEXPAND);
}

bool MFDMaterialAccordion::add_combobox_row(int selected_filament_index)
{
    const int current_count = static_cast<int>(m_comboboxes.size());
    const int new_count     = current_count + 1;

    if (new_count > m_max_filament)
        return false;

    const wxSize combobox_size(FromDIP(166), FromDIP(30));

    wxPanel*    row_panel = new wxPanel(m_combobox_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE);
    wxBoxSizer* row_sizer = new wxBoxSizer(wxHORIZONTAL);
    row_panel->SetSizer(row_sizer);

    // Label shows the slot number (1-based)
    wxStaticText* label = new wxStaticText(row_panel, wxID_ANY,
        wxString::Format(_L("Filament %d"), new_count));
    label->SetFont(::Label::Body_12);

    ComboBox* combobox = new ComboBox(row_panel, wxID_ANY, wxEmptyString,
        wxDefaultPosition, combobox_size, 0, nullptr, wxCB_READONLY);
    combobox->SetMinSize(combobox_size);
    combobox->SetKeepDropArrow(true);

    wxStaticText* weight_label = new wxStaticText(row_panel, wxID_ANY, "--%");
    weight_label->SetMinSize(wxSize(FromDIP(30), -1));
    weight_label->SetFont(::Label::Body_12);
    weight_label->Show(m_show_percentages);

    row_sizer->Add(label,        0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    row_sizer->Add(combobox,     1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    row_sizer->Add(weight_label, 0, wxALIGN_CENTER_VERTICAL);

    // Populate combobox items from physical filaments
    for (size_t i = 0; i < m_physical_filaments.size(); ++i) {
        const auto& [color_hex, name] = m_physical_filaments[i];
        wxColor  color(color_hex);
        wxString index = wxString::Format("%zu", i + 1);

        int swatch_sz = FromDIP(m_clr_swatch_size_dip);
        wxBitmap   bmp(swatch_sz, swatch_sz);
        wxMemoryDC dc(bmp);
        FilamentCardMixed::paint_clr_swatch(dc, wxSize(swatch_sz, swatch_sz), color, index, wxGetApp().dark_mode());

        combobox->Append(wxString(name), bmp);
    }

    if (selected_filament_index >= 0 && selected_filament_index < (int)m_physical_filaments.size())
        combobox->SetSelection(selected_filament_index);

    // Capture slot index for the callback
    size_t slot = static_cast<size_t>(current_count);
    combobox->Bind(wxEVT_COMBOBOX, [this, slot](wxCommandEvent&) {
        if (slot < m_comboboxes.size() && m_on_filament_changed) {
            m_on_filament_changed(slot, m_comboboxes[slot]->GetSelection());
        }
    });

    m_comboboxes.push_back(combobox);
    m_weight_labels.push_back(weight_label);

    m_combobox_sizer->Add(row_panel, 0, wxEXPAND | wxTOP, FromDIP(8));

    if (m_delete_btn) m_delete_btn->Enable(new_count > m_min_filament);
    if (m_add_btn)    m_add_btn->Enable(new_count < m_max_filament);

    m_combobox_panel->Layout();
    combobox->SetFocus();
    return true;
}

bool MFDMaterialAccordion::remove_last_combobox_row()
{
    const int current_count = static_cast<int>(m_comboboxes.size());
    if (current_count <= m_min_filament)
        return false;

    ComboBox* last_combobox = m_comboboxes.back();
    m_combobox_sizer->Detach(last_combobox->GetParent());
    last_combobox->GetParent()->Destroy();

    m_comboboxes.pop_back();
    m_weight_labels.pop_back();

    const int new_count = static_cast<int>(m_comboboxes.size());
    if (m_delete_btn) m_delete_btn->Enable(new_count > m_min_filament);
    if (m_add_btn)    m_add_btn->Enable(new_count < m_max_filament);

    m_combobox_panel->Layout();
    return true;
}

void MFDMaterialAccordion::clear_combobox_rows()
{
    for (ComboBox* combobox : m_comboboxes) {
        m_combobox_sizer->Detach(combobox->GetParent());
        combobox->GetParent()->Destroy();
    }
    m_comboboxes.clear();
    m_weight_labels.clear();
    m_combobox_panel->Layout();
}

void MFDMaterialAccordion::refresh_combobox_items(const std::vector<int>& selected_filaments)
{
    // Mark already-selected filaments so the user understands they would be swapped, not duplicated.
    for (ComboBox* combobox : m_comboboxes) {
        int count = combobox->GetCount();
        for (int n = 0; n < count; ++n) {
            const auto& [_, name] = m_physical_filaments[n];

            const bool is_already_selected = std::find(
                selected_filaments.begin(), selected_filaments.end(), n) != selected_filaments.end();
            const bool is_self = combobox->GetSelection() == n;

            wxString text;
            if (is_already_selected && !is_self) {
                text += _L("Switch");
                text += " - ";
                text += wxString(name);
            } else {
                text = wxString(name);
            }
            combobox->SetString(n, text);
        }
    }
}

void MFDMaterialAccordion::refresh_weight_labels(const std::vector<double>& weights)
{
    if (weights.size() != m_weight_labels.size())
        return;

    const int count = static_cast<int>(m_weight_labels.size());
    int total_sum = 0;
    std::vector<int> percentages(count);

    // Truncate to integer, then give the remainder to the last slot so the sum is always 100.
    for (int i = 0; i < count; ++i) {
        double raw = std::max(0.0, weights[i] * 100.0);
        percentages[i] = static_cast<int>(std::floor(raw));
        total_sum += percentages[i];
    }
    if (count > 0)
        percentages[count - 1] += 100 - total_sum;

    for (int i = 0; i < count; ++i) {
        m_weight_labels[i]->SetLabel(wxString::Format(_L("%02d%%"), percentages[i]));
        m_weight_labels[i]->Refresh();
    }
}

void MFDMaterialAccordion::set_combobox_selection(size_t slot, int phys_idx)
{
    if (slot < m_comboboxes.size())
        m_comboboxes[slot]->SetSelection(phys_idx);
}

void MFDMaterialAccordion::update_button_visibility(bool can_add_or_remove)
{
    bool collapsed = is_collapsed();
    m_can_add_or_remove = can_add_or_remove;

    // Update the label to reflect whether this is interactive or just showing the result.
    set_title(m_can_add_or_remove ? _L("Select Mixed Materials") : _L("Resulting Mixed Materials"));

    if (m_add_btn)
        m_add_btn->Show(m_can_add_or_remove && !collapsed);
    if (m_delete_btn)
        m_delete_btn->Show(m_can_add_or_remove && !collapsed);
    if (m_title_preview_panel)
        m_title_preview_panel->Show(collapsed);

    update_header_layout();
}

void MFDMaterialAccordion::update_title_preview(
    const std::vector<int>&    selected_filaments,
    const std::vector<double>& weights)
{
    if (!m_title_preview_panel)
        return;

    const int count = static_cast<int>(selected_filaments.size());

    // Calculate display percentages (floor-truncated, remainder to last)
    std::vector<int> percentages(count);
    int total_sum = 0;
    for (int i = 0; i < count; ++i) {
        double raw = std::max(0.0, weights[i] * 100.0);
        percentages[i] = static_cast<int>(std::floor(raw));
        total_sum += percentages[i];
    }
    if (count > 0)
        percentages[count - 1] += 100 - total_sum;

    // Rebuild swatches only when the filament selection changes to avoid flicker.
    bool filaments_changed = (selected_filaments != m_last_preview_filaments) ||
                             (m_title_swatches.size() != static_cast<size_t>(count));

    if (filaments_changed) {
        m_title_preview_panel->DestroyChildren();
        m_title_swatches.clear();
        m_title_percent_texts.clear();

        wxBoxSizer* sizer = new wxBoxSizer(wxHORIZONTAL);
        const int quad_size = FromDIP(18);

        for (int i = 0; i < count; ++i) {
            int phys_idx = selected_filaments[i];
            if (phys_idx < 0 || phys_idx >= static_cast<int>(m_physical_filaments.size()))
                continue;

            const auto& [color_hex, name] = m_physical_filaments[phys_idx];
            wxColor  color(color_hex);
            wxString idx_str = wxString::Format("%d", phys_idx + 1);

            wxPanel* swatch = new wxPanel(m_title_preview_panel, wxID_ANY,
                wxDefaultPosition, wxSize(quad_size, quad_size), wxBORDER_NONE);
            bind_header_events(swatch);
            swatch->SetMinSize(wxSize(quad_size, quad_size));
            swatch->SetBackgroundStyle(wxBG_STYLE_PAINT);
            swatch->SetToolTip(wxString::FromUTF8(name.c_str()));
            swatch->Bind(wxEVT_PAINT, [swatch, color, idx_str](wxPaintEvent&) {
                wxPaintDC dc(swatch);
                wxSize s = swatch->GetClientSize();
                wxColor c = color;
                wxString idx = idx_str;
                FilamentCardMixed::paint_clr_swatch(dc, s, c, idx, wxGetApp().dark_mode());
            });

            wxStaticText* pct_text = new wxStaticText(m_title_preview_panel, wxID_ANY,
                wxString::Format("%d%%", percentages[i]));
            bind_header_events(pct_text);
            pct_text->SetFont(::Label::Body_14);
            pct_text->SetForegroundColour("#333333");
            pct_text->SetToolTip(wxString::FromUTF8(name.c_str()));

            sizer->Add(swatch,   0, wxALIGN_CENTER_VERTICAL);
            if (m_show_percentages) {
                sizer->Add(pct_text, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
            } else {
                pct_text->Show(false);
            }
            if (i < count - 1)
                sizer->AddSpacer(FromDIP(12));

            m_title_swatches.push_back(swatch);
            m_title_percent_texts.push_back(pct_text);
        }

        m_title_preview_panel->SetSizer(sizer);
        m_last_preview_filaments = selected_filaments;
    } else {
        // Just update the percentages on cached labels to avoid rebuilding
        for (int i = 0; i < count; ++i) {
            if (i < static_cast<int>(m_title_percent_texts.size())) {
                if (m_show_percentages) {
                    m_title_percent_texts[i]->SetLabel(wxString::Format("%d%%", percentages[i]));
                    m_title_percent_texts[i]->Show(true);
                } else {
                    m_title_percent_texts[i]->Show(false);
                }
                m_title_percent_texts[i]->InvalidateBestSize();
            }
        }
    }

    m_title_preview_panel->Layout();
    update_header_layout();
}

void MFDMaterialAccordion::update_header_layout()
{
    Layout();
    if (GetParent()) GetParent()->Layout();
}

void MFDMaterialAccordion::on_collapsed_changed(bool collapsed)
{
    // When collapsed, hide the add/delete buttons and show the compact preview.
    // When expanded, do the reverse.
    if (m_add_btn)              m_add_btn->Show(m_can_add_or_remove && !collapsed);
    if (m_delete_btn)           m_delete_btn->Show(m_can_add_or_remove && !collapsed);
    if (m_title_preview_panel)  m_title_preview_panel->Show(collapsed);
    update_header_layout();
}

void MFDMaterialAccordion::show_percentages(bool show)
{
    if (m_show_percentages == show) return;
    m_show_percentages = show;

    for (wxStaticText* weight_label : m_weight_labels) {
        if (weight_label) weight_label->Show(show);
    }

    for (wxStaticText* pct_text : m_title_percent_texts) {
        if (pct_text) pct_text->Show(show);
    }

    m_last_preview_filaments.clear(); // invalidate cache to force reconstruction
    if (m_title_preview_panel) {
        m_title_preview_panel->Layout();
    }
    m_combobox_panel->Layout();
    Layout();
    update_header_layout();
}

} // namespace Slic3r::GUI
