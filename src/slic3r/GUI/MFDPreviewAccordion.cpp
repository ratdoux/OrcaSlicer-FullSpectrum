#include "MFDPreviewAccordion.hpp"

#include <wx/wx.h>
#include <wx/dcgraph.h>

#include "I18N.hpp"
#include "GUI_App.hpp"

namespace Slic3r::GUI {

MFDPreviewAccordion::MFDPreviewAccordion(wxWindow* parent)
    : Accordion(parent, _L("Preview"))
{
    build_ui();
}

void MFDPreviewAccordion::build_ui()
{
    wxPanel*    body   = get_body_panel();
    wxBoxSizer* sizer  = get_body_sizer();

    wxBoxSizer* row_sizer = new wxBoxSizer(wxHORIZONTAL);

    // Layer stack panel: draws one colored bar per layer entry.
    m_layers_panel = new wxPanel(body, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(120)), wxBORDER_NONE);
    m_layers_panel->SetMinSize(wxSize(-1, FromDIP(120)));
    m_layers_panel->SetBackgroundColour(body->GetBackgroundColour());
    m_layers_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_layers_panel->Bind(wxEVT_PAINT, &MFDPreviewAccordion::paint_layers_panel, this);

    // Solid color panel: shows the blended result color as a flat rectangle.
    m_color_panel = new wxPanel(body, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(120)), wxBORDER_NONE);
    m_color_panel->SetMinSize(wxSize(-1, FromDIP(120)));
    m_color_panel->SetBackgroundColour(body->GetBackgroundColour());
    m_color_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_color_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(m_color_panel);
        wxGCDC gcdc(dc);
        wxGraphicsContext* gc = gcdc.GetGraphicsContext();
        if (!gc) return;
        wxSize size = m_color_panel->GetSize();
        gc->SetBrush(wxBrush(m_color_panel->GetBackgroundColour()));
        gc->SetPen(wxPen(wxColour("#EBEBEB"), 1));
        gc->DrawRectangle(0, 0, size.x, size.y);
    });

    row_sizer->Add(m_layers_panel, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));
    row_sizer->Add(m_color_panel,  1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));
    sizer->Add(row_sizer, 0, wxEXPAND);

    // Header preview widgets: a mini layer strip and a small color swatch,
    // visible only when the accordion is collapsed.
    m_title_layers = new wxPanel(get_header_panel(), wxID_ANY, wxDefaultPosition, wxSize(FromDIP(144), FromDIP(18)), wxBORDER_NONE);
    m_title_layers->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_title_layers->Show(false);
    m_title_layers->Bind(wxEVT_PAINT, &MFDPreviewAccordion::paint_title_layers, this);
    add_header_control(m_title_layers);

    m_title_swatch = new wxPanel(get_header_panel(), wxID_ANY, wxDefaultPosition, wxSize(FromDIP(36), FromDIP(18)), wxBORDER_SIMPLE);
    m_title_swatch->SetBackgroundColour(*wxLIGHT_GREY);
    m_title_swatch->Show(false);
    add_header_control(m_title_swatch);
}

void MFDPreviewAccordion::update_preview(
    const std::vector<MFDPreviewLayerEntry>& layer_stack,
    const std::vector<wxColor>&              colors,
    const wxColor&                           mixed_color)
{
    m_layer_stack = layer_stack;
    m_colors      = colors;

    if (m_color_panel) {
        m_color_panel->SetBackgroundColour(mixed_color);
        m_color_panel->Refresh();
    }
    if (m_title_swatch) {
        m_title_swatch->SetBackgroundColour(mixed_color);
        m_title_swatch->Refresh();
    }
    if (m_layers_panel)
        m_layers_panel->Refresh();
    if (m_title_layers)
        m_title_layers->Refresh();
}

void MFDPreviewAccordion::clear_preview()
{
    m_layer_stack.clear();
    m_colors.clear();

    if (m_color_panel) {
        m_color_panel->SetBackgroundColour(*wxLIGHT_GREY);
        m_color_panel->Refresh();
    }
    if (m_title_swatch) {
        m_title_swatch->SetBackgroundColour(*wxLIGHT_GREY);
        m_title_swatch->Refresh();
    }
    if (m_layers_panel)
        m_layers_panel->Refresh();
    if (m_title_layers)
        m_title_layers->Refresh();
}

void MFDPreviewAccordion::on_collapsed_changed(bool collapsed)
{
    // Show the compact header previews only when the body is hidden,
    // giving the user a quick visual of the current mix at a glance.
    if (m_title_swatch)  m_title_swatch->Show(collapsed);
    if (m_title_layers)  m_title_layers->Show(collapsed);
    if (m_title_layers)  m_title_layers->Refresh();
}

void MFDPreviewAccordion::paint_layers_panel(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(m_layers_panel);
    wxGCDC gcdc(dc);
    wxGraphicsContext* gc = gcdc.GetGraphicsContext();
    if (!gc) return;

    wxSize size = m_layers_panel->GetSize();

    // Card background
    gc->SetBrush(wxBrush(m_layers_panel->GetBackgroundColour()));
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->DrawRectangle(0, 0, size.x, size.y);

    // Grey contrasting background
    gc->SetBrush(wxBrush(wxColour("#F0F0F0")));
    gc->SetPen(wxPen(wxColour("#E0E0E0"), 1));
    gc->DrawRoundedRectangle(0, 0, size.x, size.y, FromDIP(4));

    if (m_layer_stack.empty())
        return;

    double padding      = FromDIP(2);
    int    total_layers = static_cast<int>(m_layer_stack.size());
    double layer_h      = static_cast<double>(size.y - 2 * padding) / total_layers;
    double radius       = layer_h / 2.0;

    for (int i = 0; i < total_layers; ++i) {
        const auto& entry = m_layer_stack[i];
        if (entry.filament_index < 0 || entry.filament_index >= static_cast<int>(m_colors.size()))
            continue;

        wxColor col = m_colors[entry.filament_index];
        gc->SetBrush(wxBrush(col));
        gc->SetPen(*wxTRANSPARENT_PEN);

        double draw_w = (size.x - 2 * padding) * entry.scale;
        double draw_x = padding + ((size.x - 2 * padding) - draw_w) / 2.0;
        double draw_y = (size.y - padding) - (i + 1) * layer_h;

        gc->DrawRoundedRectangle(draw_x, draw_y, draw_w, layer_h, radius);
    }
}

void MFDPreviewAccordion::paint_title_layers(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(m_title_layers);
    wxGCDC gcdc(dc);
    wxGraphicsContext* gc = gcdc.GetGraphicsContext();
    if (!gc) return;

    wxSize size = m_title_layers->GetSize();

    // Card background
    gc->SetBrush(wxBrush(m_title_layers->GetBackgroundColour()));
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->DrawRectangle(0, 0, size.x, size.y);

    // Grey contrasting background
    gc->SetBrush(wxBrush(wxColour("#F0F0F0")));
    gc->SetPen(wxPen(wxColour("#E0E0E0"), 1));
    gc->DrawRoundedRectangle(0, 0, size.x, size.y, FromDIP(2));

    if (m_layer_stack.empty())
        return;

    double padding      = FromDIP(2);
    int    total_layers = static_cast<int>(m_layer_stack.size());
    double layer_w      = static_cast<double>(size.x - 2 * padding) / total_layers;
    double radius       = FromDIP(1);

    for (int i = 0; i < total_layers; ++i) {
        const auto& entry = m_layer_stack[i];
        if (entry.filament_index < 0 || entry.filament_index >= static_cast<int>(m_colors.size()))
            continue;

        wxColor col = m_colors[entry.filament_index];
        gc->SetBrush(wxBrush(col));
        gc->SetPen(*wxTRANSPARENT_PEN);

        double draw_h = (size.y - 2 * padding) * entry.scale;
        double draw_y = padding + ((size.y - 2 * padding) - draw_h) / 2.0;
        double draw_x = padding + i * layer_w;

        gc->DrawRoundedRectangle(draw_x, draw_y, layer_w, draw_h, radius);
    }
}

} // namespace Slic3r::GUI
