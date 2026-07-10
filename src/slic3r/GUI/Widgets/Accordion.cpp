#include "Accordion.hpp"

#include <wx/wx.h>
#include <wx/dcgraph.h>

#include "Slic3r/GUI/GUI.hpp"
#include "Slic3r/GUI/GUI_App.hpp"
#include "Slic3r/GUI/GUI_Factories.hpp"
#include "Slic3r/GUI/MFDTheme.hpp"

namespace Slic3r::GUI {

Accordion::Accordion(wxWindow* parent, const wxString& title, bool initially_collapsed)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    , m_is_collapsed(initially_collapsed)
{
    build_ui(title);
}

void Accordion::build_ui(const wxString& title)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &Accordion::on_paint, this);

    SetBackgroundColour(MFDTheme::card_background());

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);

    // --- Header panel ---
    m_header_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_header_panel->SetBackgroundColour(MFDTheme::card_background());
    m_header_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_header_panel->SetSizer(m_header_sizer);

    m_title_text = new wxStaticText(m_header_panel, wxID_ANY, title,
        wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    m_title_text->SetForegroundColour(MFDTheme::primary_text());
    m_title_text->SetFont(::Label::Head_14);

    // Get un-truncated best width with correct font applied first
    int best_width = m_title_text->GetBestSize().GetWidth();
    m_title_text->SetSize(wxSize(best_width, -1));
    m_title_text->SetMaxSize(wxSize(best_width, -1));
    m_title_text->SetMinSize(wxSize(1, -1));

    // Load both chevron states upfront so we only allocate bitmaps once.
    wxBitmap normal_bmp = ScalableBitmap(m_header_panel, "drop_down", 20).bmp();
    m_chevron_normal  = normal_bmp;
    m_chevron_rotated = wxBitmap(normal_bmp.ConvertToImage().Rotate180());

    m_chevron_bmp = new wxStaticBitmap(m_header_panel, wxID_ANY, m_chevron_normal);

    // Title + chevron together in an inner sizer so they shrink together 
    wxBoxSizer* title_chevron_sizer = new wxBoxSizer(wxHORIZONTAL);
    title_chevron_sizer->Add(m_title_text, 1, wxALIGN_CENTER_VERTICAL);
    title_chevron_sizer->Add(m_chevron_bmp, 0, wxRESERVE_SPACE_EVEN_IF_HIDDEN | wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));

    m_header_sizer->Add(title_chevron_sizer, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    // --- Body panel ---
    m_body_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_body_panel->SetBackgroundColour(MFDTheme::card_background());
    m_body_sizer = new wxBoxSizer(wxVERTICAL);
    m_body_panel->SetSizer(m_body_sizer);

    main_sizer->Add(m_header_panel, 0, wxEXPAND | wxALL, FromDIP(8));
    main_sizer->Add(m_body_panel,   0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    SetSizer(main_sizer);

    // Bind events to the header panel and all its existing children.
    // add_header_control() will bind new controls as they are added later.
    m_header_panel->Bind(wxEVT_LEFT_UP,       &Accordion::on_header_click, this);
    m_header_panel->Bind(wxEVT_ENTER_WINDOW,  &Accordion::on_header_enter, this);
    m_header_panel->Bind(wxEVT_LEAVE_WINDOW,  &Accordion::on_header_leave, this);
    m_header_panel->SetCursor(wxCursor(wxCURSOR_HAND));

    bind_header_events(m_title_text);
    bind_header_events(m_chevron_bmp);

    apply_collapsed_state();
    update_header_visual();
}

void Accordion::add_header_control(wxWindow* ctrl, bool is_action_control)
{
    if (!ctrl)
        return;

    m_header_sizer->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));

    if (is_action_control) {
        // Action controls are intentionally excluded from toggle binding so that
        // clicking Add/Delete buttons doesn't accidentally collapse the section.
        m_action_controls.push_back(ctrl);
    } else {
        bind_header_events(ctrl);
    }

    m_header_panel->Layout();
}

void Accordion::set_title(const wxString& title)
{
    if (m_title_text) {
        m_title_text->SetMinSize(wxDefaultSize);
        m_title_text->SetMaxSize(wxDefaultSize);
        m_title_text->SetLabelText(title);
        m_title_text->InvalidateBestSize();

        int best_width = m_title_text->GetBestSize().GetWidth();
        m_title_text->SetSize(wxSize(best_width, -1));
        m_title_text->SetMaxSize(wxSize(best_width, -1));
        m_title_text->SetMinSize(wxSize(1, -1));
        if (m_header_panel) {
            m_header_panel->Layout();
        }
    }
}

wxString Accordion::get_title() const
{
    return m_title_text ? m_title_text->GetLabel() : wxString();
}

void Accordion::collapse()
{
    if (m_is_collapsed)
        return;
    m_is_collapsed = true;

    apply_collapsed_state();
    update_header_visual();

    if (m_on_toggle)
        m_on_toggle(true);
}

void Accordion::expand()
{
    if (!m_is_collapsed)
        return;
    m_is_collapsed = false;

    apply_collapsed_state();
    update_header_visual();

    if (m_on_toggle)
        m_on_toggle(false);
}

void Accordion::toggle()
{
    if (m_is_collapsed)
        expand();
    else
        collapse();
}

void Accordion::apply_collapsed_state()
{
    if (m_body_panel)
        m_body_panel->Show(!m_is_collapsed);

    on_collapsed_changed(m_is_collapsed);

    // Force parent scrolled window to recalculate virtual size after show/hide.
    wxWindow* parent = GetParent();
    while (parent) {
        if (auto* scrolled = dynamic_cast<wxScrolledWindow*>(parent)) {
            scrolled->FitInside();
            break;
        }
        parent = parent->GetParent();
    }

    Layout();
    if (GetParent())
        GetParent()->Layout();
}

void Accordion::update_header_visual()
{
    if (!m_chevron_bmp || !m_title_text)
        return;

    if (m_is_collapsed) {
        m_chevron_bmp->SetBitmap(m_chevron_rotated);
        m_chevron_bmp->Show(true);
        m_title_text->SetForegroundColour(MFDTheme::primary_text());
    } else {
        m_chevron_bmp->SetBitmap(m_chevron_normal);
        m_chevron_bmp->Show(m_is_hovered);
        m_title_text->SetForegroundColour(MFDTheme::primary_text());
    }

    m_title_text->Refresh();
    if (m_header_panel)
        m_header_panel->Layout();
}

void Accordion::on_collapsed_changed(bool /*collapsed*/)
{
    // Subclasses override this to update header summary previews when collapsed.
}

void Accordion::on_header_click(wxMouseEvent& /*event*/)
{
    // Ignore clicks that land on action controls (e.g. add/delete buttons)
    // to prevent accidentally collapsing the section when operating those.
    wxPoint screen_pos = wxGetMousePosition();
    for (wxWindow* action_ctrl : m_action_controls) {
        if (action_ctrl && action_ctrl->IsShown()) {
            if (action_ctrl->GetScreenRect().Contains(screen_pos))
                return;
        }
    }
    toggle();
}

void Accordion::on_header_enter(wxMouseEvent& event)
{
    m_is_hovered = true;
    update_header_visual();
    event.Skip();
}

void Accordion::on_header_leave(wxMouseEvent& event)
{
    // Only mark as un-hovered when the mouse truly leaves the header rect.
    // Child windows fire their own leave events even when moving to a sibling
    // inside the same header, which would cause flickering without this guard.
    wxPoint pos  = wxGetMousePosition();
    wxRect  rect = m_header_panel->GetScreenRect();
    if (!rect.Contains(pos)) {
        m_is_hovered = false;
        update_header_visual();
    }
    event.Skip();
}

void Accordion::on_paint(wxPaintEvent& /*event*/)
{
    wxAutoBufferedPaintDC dc(this);
    wxSize size = GetSize();

    // Draw parent background color first so rounded corners are clean.
    wxColour parent_bg = GetParent() ? GetParent()->GetBackgroundColour() : GetBackgroundColour();
    dc.SetBrush(wxBrush(parent_bg));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(0, 0, size.x, size.y);

    // White card with subtle rounded border.
    wxColour card_bg = MFDTheme::card_background();
    dc.SetBrush(wxBrush(card_bg));
    dc.SetPen(wxPen(MFDTheme::card_border(), 1));
    dc.DrawRoundedRectangle(0, 0, size.x, size.y, 4);
}

void Accordion::bind_header_events(wxWindow* win)
{
    if (!win)
        return;

    win->Bind(wxEVT_LEFT_UP,      &Accordion::on_header_click, this);
    win->Bind(wxEVT_ENTER_WINDOW, &Accordion::on_header_enter, this);
    win->Bind(wxEVT_LEAVE_WINDOW, &Accordion::on_header_leave, this);
    win->SetCursor(wxCursor(wxCURSOR_HAND));
}

bool Accordion::Layout()
{
    bool res = wxPanel::Layout();
    if (m_header_panel)
        m_header_panel->Layout();
    if (m_body_panel)
        m_body_panel->Layout();
    return res;
}

} // namespace Slic3r::GUI
