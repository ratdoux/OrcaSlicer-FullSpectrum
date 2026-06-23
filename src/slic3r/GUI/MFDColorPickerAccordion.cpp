#include "MFDColorPickerAccordion.hpp"

#include <wx/wx.h>
#include <wx/dcgraph.h>
#include <cmath>
#include <memory>

#include "I18N.hpp"
#include "GUI_App.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/HSLColorPicker.hpp"

namespace Slic3r::GUI {

MFDColorPickerAccordion::MFDColorPickerAccordion(wxWindow* parent)
    : Accordion(parent, _L("Color Picker"))
{
    build_ui();
}

void MFDColorPickerAccordion::build_ui()
{
    wxPanel*    body  = get_body_panel();
    wxBoxSizer* sizer = get_body_sizer();

    // HSL color picker wheel
    m_hsl_color_picker = new HSLColorPicker(body, *wxRED,
        [this](const wxColour& color, bool is_dragging) {
            if (m_syncing)
                return; // Avoid circular update when sync_color() is called.
            if (m_on_color_changed)
                m_on_color_changed(color, is_dragging);
        });
    sizer->Add(m_hsl_color_picker, 0, wxEXPAND);
    sizer->AddSpacer(FromDIP(8));

    // Match-status row: hex display + color swatch
    m_match_section_panel = new wxPanel(body, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    wxBoxSizer* match_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_match_section_panel->SetSizer(match_sizer);

    wxStaticText* hash_label = new wxStaticText(m_match_section_panel, wxID_ANY, "#");
    hash_label->SetFont(::Label::Body_14);

    m_matched_hex_display = new wxTextCtrl(m_match_section_panel, wxID_ANY, "------",
        wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
    m_matched_hex_display->SetFont(::Label::Body_14);
    m_matched_hex_display->SetMinSize(wxSize(FromDIP(70), -1));

    m_matched_color_preview = new wxPanel(m_match_section_panel, wxID_ANY,
        wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    m_matched_color_preview->SetMinSize(wxSize(FromDIP(70), FromDIP(26)));
    m_matched_color_preview->SetBackgroundColour(*wxLIGHT_GREY);
    m_matched_color_preview->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_matched_color_preview->Bind(wxEVT_PAINT, &MFDColorPickerAccordion::paint_matched_color_preview, this);

    match_sizer->Add(hash_label,              0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(2));
    match_sizer->Add(m_matched_hex_display,   1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    match_sizer->Add(m_matched_color_preview, 1, wxALIGN_CENTER_VERTICAL);

    sizer->Add(m_match_section_panel, 0, wxEXPAND | wxBOTTOM, FromDIP(8));

    // Hover tracking on the match row so the paint handler shows the percentage.
    auto on_enter = [this](wxMouseEvent& e) {
        m_match_section_hovered = true;
        if (m_matched_color_preview) m_matched_color_preview->Refresh();
        e.Skip();
    };
    auto on_leave = [this](wxMouseEvent& e) {
        m_match_section_hovered = false;
        if (m_matched_color_preview) m_matched_color_preview->Refresh();
        e.Skip();
    };
    m_match_section_panel->Bind(wxEVT_ENTER_WINDOW, on_enter);
    m_match_section_panel->Bind(wxEVT_LEAVE_WINDOW, on_leave);
    m_matched_hex_display->Bind(wxEVT_ENTER_WINDOW, on_enter);
    m_matched_hex_display->Bind(wxEVT_LEAVE_WINDOW, on_leave);
    m_matched_color_preview->Bind(wxEVT_ENTER_WINDOW, on_enter);
    m_matched_color_preview->Bind(wxEVT_LEAVE_WINDOW, on_leave);

    // --- Header summary widgets (visible while collapsed) ---
    m_title_selected_hex = new wxStaticText(get_header_panel(), wxID_ANY, "#------");
    m_title_selected_hex->SetFont(::Label::Body_14);
    m_title_selected_hex->SetForegroundColour("#333333");
    m_title_selected_hex->Show(false);
    add_header_control(m_title_selected_hex);

    m_title_selected_preview = new wxPanel(get_header_panel(), wxID_ANY,
        wxDefaultPosition, wxSize(FromDIP(36), FromDIP(18)), wxBORDER_SIMPLE);
    m_title_selected_preview->SetBackgroundColour(*wxLIGHT_GREY);
    m_title_selected_preview->Show(false);
    add_header_control(m_title_selected_preview);

    // Warning icon is only shown when deviation exceeds the threshold.
    m_title_warning = new wxStaticText(get_header_panel(), wxID_ANY,
        wxString::FromUTF8("\xe2\x9a\xa0")); // ⚠
    wxFont warn_font = ::Label::Body_14;
    warn_font.SetPointSize(warn_font.GetPointSize() + 4);
    m_title_warning->SetFont(warn_font);
    m_title_warning->Show(false);
    add_header_control(m_title_warning);
}

void MFDColorPickerAccordion::sync_color(const wxColour& color)
{
    if (!m_hsl_color_picker)
        return;
    m_syncing = true;
    m_hsl_color_picker->SetColor(color);
    m_syncing = false;
}

void MFDColorPickerAccordion::update_match_status(
    const wxColour& selected_color,
    const wxColour& matched_color,
    double          deviation,
    double          warning_threshold,
    double          max_deviation)
{
    m_current_deviation           = deviation;
    m_warning_deviation_threshold = warning_threshold;
    m_max_deviation               = max_deviation;

    bool has_warning = deviation > warning_threshold;

    wxString hex = HSLColorPicker::colour_to_hex(matched_color).Mid(1); // strip leading '#'
    if (m_matched_hex_display)
        m_matched_hex_display->SetValue(hex);
    if (m_matched_color_preview) {
        m_matched_color_preview->SetBackgroundColour(matched_color);
        m_matched_color_preview->Refresh();
    }

    // Build tooltip text
    wxString close_tip   = _L("Color deviation: %.0f");
    wxString warning_tip = _L("Color deviation: %.0f. For a better match, consider adding more physical filaments.");
    wxString tip = has_warning ? wxString::Format(warning_tip, deviation)
                               : wxString::Format(close_tip, deviation);
    if (m_matched_hex_display)    m_matched_hex_display->SetToolTip(tip);
    if (m_matched_color_preview)  m_matched_color_preview->SetToolTip(tip);
    if (m_match_section_panel)    m_match_section_panel->SetToolTip(tip);

    // Update header summary
    if (m_title_selected_hex) {
        m_title_selected_hex->SetLabel(HSLColorPicker::colour_to_hex(selected_color));
        m_title_selected_hex->InvalidateBestSize();
    }
    if (m_title_selected_preview) {
        m_title_selected_preview->SetBackgroundColour(selected_color);
        m_title_selected_preview->Refresh();
    }
    if (m_title_warning)
        m_title_warning->Show(is_collapsed() && has_warning);

    on_collapsed_changed(is_collapsed());
}

void MFDColorPickerAccordion::clear_match()
{
    m_current_deviation = -1.0;
    if (m_matched_hex_display) {
        m_matched_hex_display->SetValue("------");
        m_matched_hex_display->SetToolTip("");
    }
    if (m_matched_color_preview) {
        m_matched_color_preview->SetBackgroundColour(*wxLIGHT_GREY);
        m_matched_color_preview->SetToolTip("");
        m_matched_color_preview->Refresh();
    }
    if (m_match_section_panel)
        m_match_section_panel->SetToolTip("");
    if (m_title_selected_hex)
        m_title_selected_hex->SetLabel("#------");
    if (m_title_selected_preview) {
        m_title_selected_preview->SetBackgroundColour(*wxLIGHT_GREY);
        m_title_selected_preview->Refresh();
    }
    if (m_title_warning)
        m_title_warning->Show(false);
    on_collapsed_changed(is_collapsed());
}

void MFDColorPickerAccordion::on_collapsed_changed(bool collapsed)
{
    // Show the selected color summary only while the body is hidden.
    if (m_title_selected_hex)     m_title_selected_hex->Show(collapsed);
    if (m_title_selected_preview) m_title_selected_preview->Show(collapsed);
    if (m_title_warning)
        m_title_warning->Show(collapsed && (m_current_deviation > m_warning_deviation_threshold));
    if (get_body_panel())
        get_body_panel()->GetParent()->Layout();
}

void MFDColorPickerAccordion::paint_matched_color_preview(wxPaintEvent&)
{
    wxPaintDC dc(m_matched_color_preview);
    wxSize    size = m_matched_color_preview->GetSize();
    if (size.x <= 0 || size.y <= 0) return;

    wxColour bg_color = m_matched_color_preview->GetBackgroundColour();
    dc.SetBackground(wxBrush(bg_color));
    dc.Clear();

    if (m_current_deviation < 0)
        return;

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;

    bool is_warning = m_current_deviation > m_warning_deviation_threshold;

    // Adapt text color to background luminance for readability.
    double luma = (0.299 * bg_color.Red() + 0.587 * bg_color.Green() + 0.114 * bg_color.Blue()) / 255.0;
    wxColour text_color = (luma > 0.6) ? wxColour("#2B2B2B") : *wxWHITE;

    wxFont font = dc.GetFont();
    if (!font.IsOk()) font = ::Label::Body_14;
    font.SetPointSize(font.GetPointSize() + 2);

    wxFont icon_font = font;
    icon_font.SetPointSize(font.GetPointSize() + 6);
    icon_font = icon_font.Bold();

    if (m_match_section_hovered) {
        int match_pct = 100 - static_cast<int>(std::round((m_current_deviation / m_max_deviation) * 100.0));
        match_pct = std::clamp(match_pct, 0, 100);

        if (is_warning) {
            // Show warning icon + percentage on hover
            wxString icon_str = wxString::FromUTF8("\xe2\x9a\xa0");
            wxString pct_str  = " " + wxString::Format(_L("%d%% Match"), match_pct);

            double iw, ih, pw, ph;
            gc->SetFont(icon_font, *wxWHITE);
            gc->GetTextExtent(icon_str, &iw, &ih);
            gc->SetFont(font, text_color);
            gc->GetTextExtent(pct_str, &pw, &ph);

            double total_w = iw + pw;
            double sx = (size.x - total_w) / 2.0;

            gc->SetFont(icon_font, *wxWHITE);
            gc->DrawText(icon_str, sx, (size.y - ih) / 2.0);
            gc->SetFont(font, text_color);
            gc->DrawText(pct_str, sx + iw, (size.y - ph) / 2.0);
        } else {
            wxString pct_str = wxString::Format(_L("%d%% Match"), match_pct);
            double pw, ph;
            gc->SetFont(font, text_color);
            gc->GetTextExtent(pct_str, &pw, &ph);
            gc->DrawText(pct_str, (size.x - pw) / 2.0, (size.y - ph) / 2.0);
        }
    } else {
        // Collapsed non-hover: show warning icon only if threshold exceeded.
        if (is_warning) {
            wxString icon_str = wxString::FromUTF8("\xe2\x9a\xa0");
            double   iw, ih;
            gc->SetFont(icon_font, *wxWHITE);
            gc->GetTextExtent(icon_str, &iw, &ih);
            gc->DrawText(icon_str, (size.x - iw) / 2.0, (size.y - ih) / 2.0);
        }
    }
}

} // namespace Slic3r::GUI
