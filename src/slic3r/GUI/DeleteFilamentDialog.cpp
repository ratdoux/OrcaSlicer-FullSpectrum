#include "DeleteFilamentDialog.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "I18N.hpp"
#include "wxExtensions.hpp"
#include <wx/stattext.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>

namespace Slic3r { namespace GUI {

DeleteFilamentDialog::DeleteFilamentDialog(
    wxWindow* parent,
    const wxString& title,
    const wxString& message,
    bool has_color_or_painting,
    const std::vector<FilamentChoiceItem>& target_choices,
    int default_selection)
    : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY, title,
                wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX),
      m_has_color_or_painting(has_color_or_painting),
      m_target_choices(target_choices)
{
    SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));

    const int dlg_width = 440;
    auto* main_sizer = new wxBoxSizer(wxVERTICAL);
    auto* topsizer   = new wxBoxSizer(wxHORIZONTAL);
    auto* rightsizer = new wxBoxSizer(wxVERTICAL);

    // Left info icon 
    wxBitmap info_bmp = create_scaled_bitmap("info", this, 48);
    auto* logo = new wxStaticBitmap(this, wxID_ANY, info_bmp.IsOk() ? info_bmp : wxNullBitmap);
    topsizer->Add(FromDIP(20), 0, 0, wxEXPAND, 0);
    topsizer->Add(logo, 0, wxTOP, FromDIP(20));
    topsizer->Add(FromDIP(25), 0, 0, wxEXPAND, 0);

    const int content_width = dlg_width - 160;

    // Message
    if (!message.empty()) {
        wxStaticText* msg_text = new wxStaticText(this, wxID_ANY, message);
        msg_text->Wrap(FromDIP(content_width));
        rightsizer->Add(msg_text, 0, wxEXPAND | wxBOTTOM, FromDIP(10));
    }

    if (!m_has_color_or_painting) {
        wxStaticText* not_in_use_label = new wxStaticText(this, wxID_ANY, _L("Filaments not in use"));
        wxFont font = not_in_use_label->GetFont();
        font.MakeItalic();
        not_in_use_label->SetFont(font);
        not_in_use_label->SetForegroundColour(wxColour(120, 120, 120));
        rightsizer->Add(not_in_use_label, 0, wxEXPAND | wxBOTTOM, FromDIP(10));
    } else {
        wxStaticText* target_label = new wxStaticText(this, wxID_ANY, _L("Transfer color/painting information to:"));
        rightsizer->Add(target_label, 0, wxEXPAND | wxBOTTOM, FromDIP(5));

        m_target_combo = new ComboBox(this, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(content_width), -1), 0, nullptr, wxCB_READONLY);
        m_target_combo->SetKeepDropArrow(true);

        for (const auto& item : m_target_choices) {
            m_target_combo->Append(item.name, item.bitmap);
        }

        if (!m_target_choices.empty()) {
            int sel = (default_selection >= 0 && default_selection < (int)m_target_choices.size()) ? default_selection : 0;
            m_target_combo->SetSelection(sel);
        }

        rightsizer->Add(m_target_combo, 0, wxEXPAND | wxBOTTOM, FromDIP(10));
    }

    topsizer->Add(rightsizer, 1, wxTOP | wxRIGHT | wxEXPAND, FromDIP(20));
    main_sizer->Add(topsizer, 1, wxEXPAND);

    // Spacing above buttons (roughly the height of the buttons)
    main_sizer->AddSpacer(FromDIP(24));

    // Buttons
    auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    btn_sizer->AddStretchSpacer(1);

    auto* btn_delete = new Button(this, _L("Delete"), "", 0, 0, wxID_OK);
    btn_delete->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    btn_delete->SetCursor(wxCURSOR_HAND);
    btn_delete->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
    btn_sizer->Add(btn_delete, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(ButtonProps::ChoiceButtonGap()));

    auto* btn_cancel = new Button(this, _L("Cancel"), "", 0, 0, wxID_CANCEL);
    btn_cancel->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    btn_cancel->SetCursor(wxCURSOR_HAND);
    btn_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    btn_sizer->Add(btn_cancel, 0, wxALIGN_CENTER_VERTICAL);

    main_sizer->Add(btn_sizer, 0, wxBOTTOM | wxRIGHT | wxEXPAND, FromDIP(15));

    this->SetSizer(main_sizer);
    this->Layout();
    main_sizer->Fit(this);
    this->CenterOnParent();

    // Focus Delete button so Enter immediately confirms
    btn_delete->SetFocus();
    SetDefaultItem(btn_delete);

    this->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
        if (event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_NUMPAD_ENTER) {
            EndModal(wxID_OK);
            return;
        }
        event.Skip();
    });

    wxGetApp().UpdateDlgDarkUI(this);
}

DeleteFilamentDialog::~DeleteFilamentDialog() {}

int DeleteFilamentDialog::get_target_filament_id() const
{
    if (!m_has_color_or_painting || m_target_combo == nullptr)
        return -1;
    int sel = m_target_combo->GetSelection();
    if (sel >= 0 && sel < (int)m_target_choices.size())
        return static_cast<int>(m_target_choices[sel].filament_id);
    return -1;
}

void DeleteFilamentDialog::on_dpi_changed(const wxRect &suggested_rect) {}

}} // namespace Slic3r::GUI
