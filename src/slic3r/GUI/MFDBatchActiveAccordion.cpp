#include "MFDBatchActiveAccordion.hpp"
#include "MixedFilamentBatchDialog.hpp"
#include "Widgets/FilamentCardMixed.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MFDTheme.hpp"
#include <wx/dcgraph.h>

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
        body->Layout();
        return;
    }

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
    body->Layout();
}

void MFDBatchActiveAccordion::update_states()
{
    for (auto* tile : m_tiles) {
        tile->Refresh();
    }
}

} // namespace Slic3r::GUI
