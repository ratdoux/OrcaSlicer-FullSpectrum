#include "MFDBatchActiveAccordion.hpp"
#include "MixedFilamentBatchDialog.hpp"
#include "Widgets/FilamentCardMixed.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MFDTheme.hpp"
#include "Plater.hpp"
#include "PresetBundle.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/MixedFilament.hpp"
#include <wx/dcgraph.h>
#include <wx/checkbox.h>

namespace Slic3r::GUI {

MFDBatchActiveAccordion::MFDBatchActiveAccordion(wxWindow* parent, std::vector<BatchMixItem>& mix_items)
    : Accordion(parent, _L("Active Mixed Filaments"))
    , m_mix_items(mix_items)
{
    build_ui();
}

void MFDBatchActiveAccordion::build_ui()
{
    wxPanel* body = get_body_panel();
    wxBoxSizer* sizer = get_body_sizer();

    bool has_active = false;
    for (const auto& item : m_mix_items) {
        if (item.is_existing) {
            has_active = true;
            break;
        }
    }

    if (!has_active) {
        wxStaticText* placeholder = new wxStaticText(body, wxID_ANY, _L("No active mixed filaments"));
        placeholder->SetFont(::Label::Body_13);
        MFDTheme::apply_text(placeholder, MFDTheme::muted_text(), body->GetBackgroundColour());
        sizer->Add(placeholder, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(8));
    } else {
        m_wrap_sizer = new wxWrapSizer(wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS);

        m_tiles.clear();

        for (size_t idx = 0; idx < m_mix_items.size(); ++idx) {
            auto& item = m_mix_items[idx];
            if (!item.is_existing)
                continue;

            BatchSwatchTile* tile = new BatchSwatchTile(body, &item, [this]() {
                if (m_on_item_toggled)
                    m_on_item_toggled();
            });
            m_tiles.push_back(tile);
            m_wrap_sizer->Add(tile, 0, wxALL, FromDIP(2));
        }

        sizer->Add(m_wrap_sizer, 0, wxEXPAND | wxALL, FromDIP(8));
    }

    // Section title
    wxStaticText* auto_gen_title = new wxStaticText(body, wxID_ANY, _L("Settings"));
    auto_gen_title->SetFont(::Label::Body_13);
    MFDTheme::apply_text(auto_gen_title, MFDTheme::secondary_text(), body->GetBackgroundColour());
    sizer->Add(auto_gen_title, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));

    // Custom Checkbox Row
    wxPanel* cb_panel = new wxPanel(body, wxID_ANY);
    cb_panel->SetBackgroundColour(body->GetBackgroundColour());
    wxBoxSizer* cb_sizer = new wxBoxSizer(wxHORIZONTAL);

    const bool auto_gen_enabled = wxGetApp().app_config != nullptr ?
        wxGetApp().app_config->get_bool("auto_generate_gradients") : true;

    auto toggle_auto_gen = [this]() {
        const bool new_val = (m_batch_check_box->get_state() != 2);
        m_batch_check_box->set_state(new_val ? 2 : 0);
        if (wxGetApp().app_config != nullptr) {
            wxGetApp().app_config->set_bool("auto_generate_gradients", new_val);
            wxGetApp().app_config->save();
        }
        MixedFilamentManager::set_auto_generate_enabled(new_val);
        if (wxGetApp().preset_bundle != nullptr && wxGetApp().plater() != nullptr) {
            const size_t num_physical = wxGetApp().preset_bundle->filament_presets.size();
            wxGetApp().plater()->set_auto_generated_gradient_decision(num_physical, new_val);
        }
        if (m_on_item_toggled)
            m_on_item_toggled();
    };

    m_batch_check_box = new BatchCheckBox(cb_panel, toggle_auto_gen);
    m_batch_check_box->set_state(auto_gen_enabled ? 2 : 0);

    wxStaticText* cb_label = new wxStaticText(cb_panel, wxID_ANY, _L("Auto-generate 50/50 Mixes"));
    cb_label->SetFont(::Label::Body_13);
    cb_label->SetCursor(wxCursor(wxCURSOR_HAND));
    MFDTheme::apply_text(cb_label, MFDTheme::primary_text(), cb_panel->GetBackgroundColour());
    cb_label->Bind(wxEVT_LEFT_UP, [toggle_auto_gen](wxMouseEvent&) {
        toggle_auto_gen();
    });

    cb_sizer->Add(m_batch_check_box, 0, wxALIGN_CENTER_VERTICAL);
    cb_sizer->AddSpacer(FromDIP(10));
    cb_sizer->Add(cb_label, 0, wxALIGN_CENTER_VERTICAL);
    cb_panel->SetSizer(cb_sizer);

    sizer->Add(cb_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(8));

    body->Layout();
}

void MFDBatchActiveAccordion::update_states()
{
    for (auto* tile : m_tiles) {
        tile->Refresh();
    }
}

} // namespace Slic3r::GUI
