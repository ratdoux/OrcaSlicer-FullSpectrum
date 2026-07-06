#include "MFDPatternSelectorAccordion.hpp"

#include <wx/wx.h>
#include <wx/wrapsizer.h>
#include <wx/dcgraph.h>
#include <cctype>

#include "I18N.hpp"
#include "GUI_App.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/FilamentCardMixed.hpp"

namespace Slic3r::GUI {

MFDPatternSelectorAccordion::MFDPatternSelectorAccordion(
    wxWindow*                                                   parent,
    const std::vector<std::pair<std::string, std::string>>&     physical_filaments)
    : Accordion(parent, _L("Select Pattern"))
    , m_physical_filaments(physical_filaments)
{
    build_ui();
}

void MFDPatternSelectorAccordion::build_ui()
{
    wxPanel*    body  = get_body_panel();
    wxBoxSizer* sizer = get_body_sizer();

    // --- Filament swatch row ---
    m_filament_row = new wxPanel(body, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_filament_row->SetBackgroundColour(body->GetBackgroundColour());
    wxWrapSizer* filaments_sizer = new wxWrapSizer(wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS);
    m_filament_row->SetSizer(filaments_sizer);
    build_filament_row(m_filament_row, filaments_sizer);
    sizer->Add(m_filament_row, 0, wxEXPAND | wxBOTTOM, this->FromDIP(16));

    // --- Input row: text field + backspace button ---
    wxBoxSizer* input_row = new wxBoxSizer(wxHORIZONTAL);

    m_pattern_input = new wxTextCtrl(body, wxID_ANY, "12",
        wxDefaultPosition, wxSize(-1, this->FromDIP(30)), wxTE_PROCESS_ENTER);
    m_pattern_input->SetFont(::Label::Body_14);
    m_pattern_input->Bind(wxEVT_TEXT, &MFDPatternSelectorAccordion::on_text_changed, this);

    wxPanel* bs_btn = new wxPanel(body, wxID_ANY, wxDefaultPosition,
        wxSize(this->FromDIP(38), this->FromDIP(30)), wxBORDER_NONE);
    bs_btn->SetMinSize(wxSize(this->FromDIP(38), this->FromDIP(30)));
    bs_btn->SetBackgroundStyle(wxBG_STYLE_PAINT);
    bs_btn->SetToolTip(_L("Delete last entry"));

    auto bs_hovered = std::make_shared<bool>(false);

    // Custom-painted backspace icon (chevron/backspace shape with X inside)
    auto paint_backspace = [this](wxGraphicsContext* gc, const wxSize& size, bool hovered) {
        wxColour clr = hovered ? *wxWHITE : *wxBLACK;
        gc->SetPen(wxPen(clr, this->FromDIP(1.5)));
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        double w = this->FromDIP(16), h = this->FromDIP(12);
        double cx = size.x / 2.0, cy = size.y / 2.0;
        double x0 = cx - w/2, x1 = cx - w/2 + this->FromDIP(4), x2 = cx + w/2;
        double y0 = cy - h/2, y1 = cy, y2 = cy + h/2;
        wxGraphicsPath p = gc->CreatePath();
        p.MoveToPoint(x0, y1); p.AddLineToPoint(x1, y0);
        p.AddLineToPoint(x2, y0); p.AddLineToPoint(x2, y2);
        p.AddLineToPoint(x1, y2); p.CloseSubpath();
        gc->StrokePath(p);
        double ixw = this->FromDIP(3), ixcx = cx + this->FromDIP(2);
        gc->StrokeLine(ixcx - ixw, cy - ixw, ixcx + ixw, cy + ixw);
        gc->StrokeLine(ixcx - ixw, cy + ixw, ixcx + ixw, cy - ixw);
    };

    bs_btn->Bind(wxEVT_PAINT, [this, bs_btn, paint_backspace, bs_hovered](wxPaintEvent&) {
        wxPaintDC dc(bs_btn);
        wxGCDC gcdc(dc);
        wxGraphicsContext* gc = gcdc.GetGraphicsContext();
        if (!gc) return;
        wxSize size = bs_btn->GetClientSize();
        bool hovered = *bs_hovered;
        if (hovered) {
            gc->SetBrush(wxBrush(wxColour(224, 80, 80)));
        } else {
            gc->SetBrush(wxBrush(bs_btn->GetParent()->GetBackgroundColour()));
        }
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->DrawRectangle(0, 0, size.x, size.y);
        paint_backspace(gc, size, hovered);
    });
    bs_btn->Bind(wxEVT_ENTER_WINDOW, [bs_btn, bs_hovered](wxMouseEvent& e) {
        bs_btn->SetCursor(wxCursor(wxCURSOR_HAND));
        *bs_hovered = true;
        bs_btn->Refresh(); e.Skip();
    });
    bs_btn->Bind(wxEVT_LEAVE_WINDOW, [bs_btn, bs_hovered](wxMouseEvent& e) {
        *bs_hovered = false;
        bs_btn->Refresh(); e.Skip();
    });
    bs_btn->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) {
        handle_backspace(); e.Skip();
    });

    input_row->Add(m_pattern_input, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    input_row->AddSpacer(this->FromDIP(4));
    input_row->Add(bs_btn, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(input_row, 0, wxEXPAND | wxBOTTOM, this->FromDIP(4));

    // --- Warning label (hidden until a parse error occurs) ---
    m_pattern_warning = new wxStaticText(body, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE);
    m_pattern_warning->SetForegroundColour(*wxRED);
    m_pattern_warning->SetFont(::Label::Body_12);
    m_pattern_warning->Hide();
    sizer->Add(m_pattern_warning, 0, wxEXPAND);

    // --- Header summary text (shown when collapsed) ---
    m_title_preview_text = new wxStaticText(get_header_panel(), wxID_ANY, wxEmptyString);
    m_title_preview_text->SetFont(::Label::Body_14);
    m_title_preview_text->SetForegroundColour("#333333");
    m_title_preview_text->Show(false);
    add_header_control(m_title_preview_text);
}

void MFDPatternSelectorAccordion::build_filament_row(
    wxPanel* parent, wxBoxSizer* /*parent_sizer*/)
{
    for (size_t i = 0; i < m_physical_filaments.size(); ++i) {
        const auto& [color_hex, name] = m_physical_filaments[i];
        wxColor   color(color_hex);
        wxString  display_index = wxString::Format("%zu", i + 1);
        wxString  tooltip_name  = wxString::FromUTF8(name.c_str());

        wxPanel* swatch = new wxPanel(parent, wxID_ANY, wxDefaultPosition,
            wxSize(this->FromDIP(28), this->FromDIP(28)), wxBORDER_NONE);
        swatch->SetMinSize(wxSize(this->FromDIP(28), this->FromDIP(28)));
        swatch->SetBackgroundStyle(wxBG_STYLE_PAINT);
        swatch->SetToolTip(tooltip_name);

        auto is_hovered = std::make_shared<bool>(false);

        swatch->Bind(wxEVT_PAINT, [swatch, color, display_index, is_hovered](wxPaintEvent&) {
            wxPaintDC dc(swatch);
            wxSize s = swatch->GetClientSize();
            wxColor c = color;
            wxString idx = display_index;
            int padding = *is_hovered ? 0 : swatch->FromDIP(2);

            dc.SetBackground(wxBrush(swatch->GetParent()->GetBackgroundColour()));
            dc.Clear();

            FilamentCardMixed::paint_clr_swatch(dc, s, c, idx, wxGetApp().dark_mode(), padding);
        });
        swatch->Bind(wxEVT_ENTER_WINDOW, [swatch, is_hovered](wxMouseEvent& e) {
            swatch->SetCursor(wxCursor(wxCURSOR_HAND));
            *is_hovered = true;
            swatch->Refresh(); e.Skip();
        });
        swatch->Bind(wxEVT_LEAVE_WINDOW, [swatch, is_hovered](wxMouseEvent& e) {
            *is_hovered = false;
            swatch->Refresh(); e.Skip();
        });

        // Clicking appends the filament index to the pattern input.
        int one_based = static_cast<int>(i + 1);
        swatch->Bind(wxEVT_LEFT_UP, [this, one_based](wxMouseEvent& e) {
            wxString token = (one_based <= 9)
                ? wxString::Format("%d", one_based)
                : wxString::Format("[%d]", one_based);
            m_pattern_input->SetValue(m_pattern_input->GetValue() + token);
            m_pattern_input->SetInsertionPointEnd();
            wxCommandEvent evt(wxEVT_TEXT);
            m_pattern_input->ProcessWindowEvent(evt);
            e.Skip();
        });

        parent->GetSizer()->Add(swatch, 0, wxALL, this->FromDIP(2));
    }
}

void MFDPatternSelectorAccordion::on_text_changed(wxCommandEvent&)
{
    update_title_preview();

    wxString val = m_pattern_input ? m_pattern_input->GetValue() : wxString();
    std::vector<int> parsed;
    wxString error_msg;

    if (val.Trim().IsEmpty()) {
        m_pattern_warning->SetLabel(_L("Pattern cannot be empty."));
        m_pattern_warning->Wrap(this->FromDIP(350));
        m_pattern_warning->Show();
        get_body_panel()->Layout();
        if (m_on_pattern_invalid) m_on_pattern_invalid();
        return;
    }

    if (parse_pattern(val, static_cast<int>(m_physical_filaments.size()), parsed, error_msg)) {
        m_pattern_warning->Hide();
        get_body_panel()->Layout();
        if (m_on_pattern_changed) m_on_pattern_changed(parsed);
    } else {
        m_pattern_warning->SetLabel(error_msg);
        m_pattern_warning->Wrap(this->FromDIP(350));
        m_pattern_warning->Show();
        get_body_panel()->Layout();
        // Do not call on_pattern_invalid here - keep the last valid preview.
    }
}

void MFDPatternSelectorAccordion::handle_backspace()
{
    if (!m_pattern_input)
        return;

    wxString val = m_pattern_input->GetValue();

    // Strip trailing separators first.
    while (!val.IsEmpty() && (val.Last() == ' ' || val.Last() == ','))
        val.RemoveLast();

    if (!val.IsEmpty()) {
        if (val.Last() == ']') {
            // Remove a bracketed token like [10]
            int open_pos = val.Find('[', true);
            if (open_pos != wxNOT_FOUND)
                val = val.Left(open_pos);
            else
                val.RemoveLast();
        } else {
            val.RemoveLast();
        }
    }

    // Strip any newly-exposed trailing separators.
    while (!val.IsEmpty() && (val.Last() == ' ' || val.Last() == ','))
        val.RemoveLast();

    m_pattern_input->SetValue(val);
    m_pattern_input->SetInsertionPointEnd();
    wxCommandEvent evt(wxEVT_TEXT);
    m_pattern_input->ProcessWindowEvent(evt);
}

void MFDPatternSelectorAccordion::update_title_preview()
{
    if (!m_title_preview_text)
        return;
    m_title_preview_text->Show(is_collapsed());
    if (is_collapsed()) {
        wxString val = m_pattern_input ? m_pattern_input->GetValue() : wxString();
        m_title_preview_text->SetLabel(val);
        m_title_preview_text->InvalidateBestSize();
    }
    if (get_body_panel() && get_body_panel()->GetParent())
        get_body_panel()->GetParent()->Layout();
}

void MFDPatternSelectorAccordion::on_collapsed_changed(bool /*collapsed*/)
{
    update_title_preview();
}

wxString MFDPatternSelectorAccordion::get_pattern_string() const
{
    return m_pattern_input ? m_pattern_input->GetValue() : wxString();
}

void MFDPatternSelectorAccordion::set_pattern_string(const wxString& pattern_str)
{
    if (m_pattern_input) {
        m_pattern_input->SetValue(pattern_str);
    }
}

void MFDPatternSelectorAccordion::trigger_initial_validation()
{
    if (m_pattern_input) {
        wxCommandEvent evt(wxEVT_TEXT);
        m_pattern_input->ProcessWindowEvent(evt);
    }
}

// static
bool MFDPatternSelectorAccordion::parse_pattern(
    const wxString& pattern_str,
    int             num_filaments,
    std::vector<int>& out_indices,
    wxString&         out_error_msg)
{
    out_indices.clear();
    std::string s = pattern_str.ToStdString();
    size_t i = 0;

    while (i < s.size()) {
        char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
            i++;
            continue;
        }
        if (c == '[') {
            // Bracketed multi-digit index: [10], [11], etc.
            i++;
            size_t start = i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
                i++;
            if (i == start || i >= s.size() || s[i] != ']') {
                out_error_msg = _L("Invalid bracket syntax. Expected [number]. Only digits are allowed inside brackets.");
                return false;
            }
            int val = std::stoi(s.substr(start, i - start));
            i++; // consume ']'
            if (val < 1 || val > num_filaments) {
                out_error_msg = wxString::Format(_L("Filament index %d is out of bounds (1-%d)."), val, num_filaments);
                return false;
            }
            out_indices.push_back(val);
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            int val = c - '0';
            if (val == 0) {
                out_error_msg = _L("Filament index cannot be 0.");
                return false;
            }
            if (val > num_filaments) {
                out_error_msg = wxString::Format(_L("Filament index %d is out of bounds (1-%d)."), val, num_filaments);
                return false;
            }
            out_indices.push_back(val);
            i++;
        } else {
            out_error_msg = wxString::Format(
                _L("Invalid character '%c' in pattern. Only digits, commas, spaces, and bracketed numbers (e.g. [10]) are allowed."), c);
            return false;
        }
    }

    if (out_indices.empty()) {
        out_error_msg = _L("Pattern cannot be empty.");
        return false;
    }
    return true;
}

} // namespace Slic3r::GUI


