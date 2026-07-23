#include "FilamentCardMixed.hpp"

#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Factories.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/MixedFilamentBadge.hpp"
#include "slic3r/GUI/MixedColorMatchHelpers.hpp"
#include "libslic3r/MixedFilament.hpp"

#include <wx/wx.h>
#include <wx/graphics.h>
#include <string>

namespace Slic3r::GUI {

namespace {

bool is_gradient_definition(const MixedFilamentDefinition* definition)
{
    return definition != nullptr && definition->recipe.kind == MixedFilamentRecipeKind::WeightedBlend &&
           definition->behavior.gradient.enabled && definition->recipe.blend.components.size() >= 2;
}

} // namespace

FilamentCardMixed::FilamentCardMixed(wxWindow*                                         parent,
                                     MixedFilamentDefinition*                          definition,
                                     std::vector<std::pair<std::string, std::string>>& physical_filaments)
    : ::wxPanel(parent, wxID_ANY), m_definition{definition}, m_physical_filaments{physical_filaments}
{
    SetBackgroundColour(parent ? parent->GetBackgroundColour() : StateColor::darkModeColorFor(*wxWHITE));
    update_state(definition, false);

    build_ui();
}

FilamentCardImageMap::FilamentCardImageMap(wxWindow*             parent,
                                           const wxString&       object_name,
                                           std::vector<wxColour> spectrum_colors,
                                           bool                  show_delete,
                                           const wxString&       spectrum_tooltip,
                                           std::vector<ComponentFilament> component_filaments)
    : wxPanel(parent, wxID_ANY)
    , m_object_name(object_name)
    , m_spectrum_tooltip(spectrum_tooltip)
    , m_spectrum_colors(std::move(spectrum_colors))
    , m_component_filaments(std::move(component_filaments))
    , m_show_delete(show_delete)
{
    SetBackgroundColour(parent ? parent->GetBackgroundColour() : StateColor::darkModeColorFor(*wxWHITE));
    build_ui();
}

void FilamentCardImageMap::build_ui()
{
    auto* sizer = new wxBoxSizer(wxHORIZONTAL);

    for (const ComponentFilament& component : m_component_filaments) {
        const int swatch_size = FromDIP(24);
        auto*     swatch      = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(swatch_size, swatch_size));
        swatch->SetMinSize(wxSize(swatch_size, swatch_size));
        swatch->SetBackgroundStyle(wxBG_STYLE_PAINT);
        swatch->SetToolTip(wxString::Format(_L("Physical filament %u"), component.first));
        swatch->Bind(wxEVT_PAINT, [swatch, component](wxPaintEvent&) {
            wxPaintDC dc(swatch);
            dc.SetBackground(wxBrush(swatch->GetParent()->GetBackgroundColour()));
            dc.Clear();
            FilamentCardMixed::paint_clr_swatch(dc, swatch->GetClientSize(), component.second,
                                                wxString::Format("%u", component.first), wxGetApp().dark_mode(), 1);
        });
        sizer->Add(swatch, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(3));
    }

    m_spectrum_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(30)));
    m_spectrum_panel->SetMinSize(wxSize(-1, FromDIP(30)));
    m_spectrum_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_spectrum_panel->SetToolTip(
        m_spectrum_tooltip.empty() ? _L("Continuous source colors for this object's perimeter-modulated image map") : m_spectrum_tooltip);
    m_spectrum_panel->Bind(wxEVT_PAINT, &FilamentCardImageMap::paint_spectrum, this);
    m_spectrum_panel->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
        if (m_on_select)
            m_on_select();
        event.Skip();
    });

    sizer->Add(m_spectrum_panel, 1, wxEXPAND | wxALL, FromDIP(2));
    if (m_show_delete) {
        m_delete_btn = new ScalableButton(this, wxID_ANY, "delete_filament");
        m_delete_btn->SetToolTip(_L("Remove image colors from this object"));
        m_delete_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (m_on_delete)
                m_on_delete();
        });
        sizer->Add(m_delete_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
    }
    SetSizer(sizer);
}

void FilamentCardImageMap::set_on_select_callback(std::function<void()> callback)
{
    m_on_select = std::move(callback);
    if (m_spectrum_panel != nullptr)
        m_spectrum_panel->SetCursor(m_on_select ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
}

void FilamentCardImageMap::set_selected(bool selected)
{
    if (m_selected == selected)
        return;
    m_selected = selected;
    if (m_spectrum_panel != nullptr)
        m_spectrum_panel->Refresh();
}

void FilamentCardImageMap::paint_spectrum(wxPaintEvent&)
{
    wxPaintDC    dc(m_spectrum_panel);
    const wxSize size = m_spectrum_panel->GetClientSize();
    if (size.x <= 0 || size.y <= 0)
        return;

    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(GetBackgroundColour()));
    dc.DrawRectangle(0, 0, size.x, size.y);

    const std::vector<wxColour>        fallback{StateColor::darkModeColorFor(wxColour("#808080"))};
    const std::vector<wxColour>&       spectrum = m_spectrum_colors.empty() ? fallback : m_spectrum_colors;
    std::unique_ptr<wxGraphicsContext> graphics(wxGraphicsContext::CreateFromUnknownDC(dc));
    if (graphics) {
        const double width = std::max(1.0, double(size.x - 2));
        if (spectrum.size() == 1) {
            graphics->SetBrush(wxBrush(spectrum.front()));
            graphics->SetPen(*wxTRANSPARENT_PEN);
            graphics->DrawRectangle(1.0, 1.0, width, std::max(1, size.y - 2));
        } else {
            for (size_t index = 0; index + 1 < spectrum.size(); ++index) {
                const double x0 = 1.0 + width * double(index) / double(spectrum.size() - 1);
                const double x1 = 1.0 + width * double(index + 1) / double(spectrum.size() - 1);
                graphics->SetBrush(graphics->CreateLinearGradientBrush(x0, 1.0, x1, 1.0, spectrum[index], spectrum[index + 1]));
                graphics->SetPen(*wxTRANSPARENT_PEN);
                graphics->DrawRectangle(x0, 1.0, x1 - x0 + 0.5, std::max(1, size.y - 2));
            }
        }
    }

    const wxColour selected_color(ColorRGB::ORCA().r_uchar(), ColorRGB::ORCA().g_uchar(), ColorRGB::ORCA().b_uchar());
    dc.SetPen(wxPen(m_selected ? selected_color : StateColor::darkModeColorFor(wxColour("#CECECE")), m_selected ? 3 : 1));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    const int border_inset = m_selected ? 1 : 0;
    dc.DrawRectangle(border_inset, border_inset, size.x - 2 * border_inset, size.y - 2 * border_inset);

    wxString label = m_object_name.empty() ? _L("Image map") : m_object_name;
    dc.SetFont(::Label::Body_14.Bold());
    const int available_width = std::max(0, size.x - FromDIP(16));
    while (label.length() > 1 && dc.GetTextExtent(label + wxString::FromUTF8("\xE2\x80\xA6")).x > available_width)
        label.RemoveLast();
    if (label != m_object_name && !label.empty())
        label += wxString::FromUTF8("\xE2\x80\xA6");

    const wxSize  text_size = dc.GetTextExtent(label);
    const wxPoint text_position((size.x - text_size.x) / 2, (size.y - text_size.y) / 2);
    dc.SetTextForeground(wxColour(0, 0, 0, 180));
    dc.DrawText(label, text_position.x + 1, text_position.y + 1);
    dc.SetTextForeground(*wxWHITE);
    dc.DrawText(label, text_position);
}

void FilamentCardMixed::build_ui()
{
    const int    swatch_size         = FromDIP(20);
    const wxSize display_swatch_size = color_swatch_size_for_text(display_id_text());
    const int    content_height      = FromDIP(30);

    m_main_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_clr_swatch_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, display_swatch_size);
    m_clr_swatch_panel->SetMinSize(display_swatch_size);
    m_clr_swatch_panel->SetBackgroundColour(GetBackgroundColour());
    m_clr_swatch_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_clr_swatch_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent& event) {
        if (!m_definition) {
            wxPaintDC context(m_clr_swatch_panel);
            return;
        }

        auto hex_string_to_wx_color = [](const std::string& hex) -> wxColor {
            if (hex.empty()) {
                return wxColor(0, 0, 0);
            }

            wxString normalized(hex);
            if (normalized[0] == '#') {
                normalized = normalized.Mid(1);
            }

            if (normalized.Length() != 6) {
                return wxColor(0, 0, 0);
            }

            long red, green, blue;
            if (!normalized.Mid(0, 2).ToLong(&red, 16) || !normalized.Mid(2, 2).ToLong(&green, 16) ||
                !normalized.Mid(4, 2).ToLong(&blue, 16)) {
                return wxColor(0, 0, 0);
            }

            return wxColor(red, green, blue);
        };

        wxPaintDC    context(m_clr_swatch_panel);
        const wxSize size = m_clr_swatch_panel->GetClientSize();

        if (is_gradient_definition(m_definition)) {
            paint_clr_swatch_gradient(context, size, m_gradient_preview_colors, display_id_text(), wxGetApp().dark_mode());
        } else {
            paint_clr_swatch(context, size, hex_string_to_wx_color(m_definition->presentation.display_color), display_id_text(),
                             wxGetApp().dark_mode());
        }
    });

    m_clr_swatch_panel->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
        if (m_on_box_edit)
            m_on_box_edit(true); // true means edit on Mix by color tab
        event.Skip();
    });

    m_clr_swatch_panel->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& event) {
        SetCursor(wxCursor(wxCURSOR_HAND));
        event.Skip();
    });
    m_clr_swatch_panel->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
        SetCursor(wxCursor(wxNullCursor));
        event.Skip();
    });

    m_box_panel = new wxPanel(this);
    m_box_panel->SetMinSize(wxSize(-1, content_height));
    m_box_panel->SetBackgroundColour(GetBackgroundColour());

    m_box_panel->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
        if (m_on_box_edit)
            m_on_box_edit(false); // false means edit on default tab

        event.Skip();
    });

    m_box_panel->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& event) {
        m_is_box_panel_hovered = true;
        SetCursor(wxCursor(wxCURSOR_HAND));
        m_box_panel->Refresh();
        event.Skip();
    });
    m_box_panel->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
        m_is_box_panel_hovered = false;
        SetCursor(wxCursor(wxNullCursor));
        m_box_panel->Refresh();
        event.Skip();
    });

    m_box_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_box_panel->Bind(wxEVT_PAINT, [this, swatch_size](wxPaintEvent& event) {
        if (!m_definition) {
            wxPaintDC context(m_box_panel);
            return;
        }

        wxPaintDC context(m_box_panel);

        const wxSize  size             = m_box_panel->GetClientSize();
        const wxColor background_color = GetBackgroundColour();

        const bool highlight = m_is_box_panel_hovered || m_is_dialog_open;

        if (is_gradient_definition(m_definition)) {
            paint_box_gradient(context, size, background_color, m_gradient_preview_colors, m_gradient_component_positions,
                               m_gradient_component_ids, wxGetApp().dark_mode(), highlight, wxSize(swatch_size, swatch_size));
        } else if (m_definition->recipe.kind == MixedFilamentRecipeKind::WeightedBlend) {
            paint_box_mix(context, size, background_color, m_physical_filaments_indices, m_physical_filaments_percentages,
                          m_physical_filaments_colors, wxGetApp().dark_mode(), highlight, wxSize(swatch_size, swatch_size));
        } else if (m_definition->recipe.kind == MixedFilamentRecipeKind::ManualPattern) {
            paint_box_pattern(context, size, background_color, m_physical_filaments_indices, m_physical_filaments_colors,
                              wxGetApp().dark_mode(), highlight, wxSize(swatch_size, swatch_size));
        }
    });

    m_filament_edit_btn = new ScalableButton(this, wxID_ANY, "menu_filament");
    m_filament_edit_btn->SetToolTip(_L("Click to edit preset"));
    m_filament_edit_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_on_edit_btn)
            m_on_edit_btn(m_filament_edit_btn);
    });

    auto right_click_handler = [this](wxMouseEvent& event) {
        if (m_on_right_click) {
            wxPoint screen_pos = event.GetEventObject() ?
                                     static_cast<wxWindow*>(event.GetEventObject())->ClientToScreen(event.GetPosition()) :
                                     wxGetMousePosition();
            m_on_right_click(screen_pos);
        }
        event.Skip();
    };

    Bind(wxEVT_RIGHT_DOWN, right_click_handler);
    m_clr_swatch_panel->Bind(wxEVT_RIGHT_DOWN, right_click_handler);
    m_box_panel->Bind(wxEVT_RIGHT_DOWN, right_click_handler);
    m_filament_edit_btn->Bind(wxEVT_RIGHT_DOWN, right_click_handler);

    SetSizer(m_main_sizer);
    m_main_sizer->Add(m_clr_swatch_panel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_main_sizer->Add(m_box_panel, 1, wxEXPAND | wxALL, FromDIP(2));
    m_main_sizer->Add(m_filament_edit_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
}

wxString FilamentCardMixed::display_id_text() const
{
    return m_definition ? wxString(std::to_string(m_definition->identity.stable_id)) : wxString();
}

wxSize FilamentCardMixed::color_swatch_size_for_text(const wxString& text) const
{
    const int height = FromDIP(20);
    if (text.Length() <= 2)
        return wxSize(height, height);

    const int width = std::max(height, FromDIP(10 + 8 * static_cast<int>(text.Length())));
    return wxSize(width, height);
}

void FilamentCardMixed::update_color_swatch_size()
{
    if (!m_clr_swatch_panel)
        return;

    const wxSize swatch_size = color_swatch_size_for_text(display_id_text());
    m_clr_swatch_panel->SetMinSize(swatch_size);
    m_clr_swatch_panel->SetSize(swatch_size);
    m_clr_swatch_panel->Refresh();

    if (m_main_sizer)
        m_main_sizer->Layout();
    Layout();
    if (GetParent())
        GetParent()->Layout();
}

std::vector<wxColor> FilamentCardMixed::get_physical_filaments_colors(const std::vector<unsigned int>& filament_indices) const
{
    std::vector<wxColor> colors;
    colors.reserve(filament_indices.size());

    for (auto physical_filament_index : filament_indices) {
        if (physical_filament_index < 1 || physical_filament_index > m_physical_filaments.size()) {
            colors.push_back(wxColor(*wxBLACK));
        } else {
            const auto& color_hex = m_physical_filaments[physical_filament_index - 1].first;
            colors.push_back(wxColor(color_hex));
        }
    }

    return colors;
}

// static
// paint background, text and optional border of color swatch
void FilamentCardMixed::paint_clr_swatch(
    wxDC& context, const wxSize& size, const wxColor& color, const wxString& text, bool is_dark, int padding)
{
    // Draw the swatch box (optionally inset by padding)
    int x = padding;
    int y = padding;
    int w = size.x - 2 * padding;
    int h = size.y - 2 * padding;

    if (w <= 0 || h <= 0)
        return;

    // Draw the swatch background
    context.SetPen(*wxTRANSPARENT_PEN);
    context.SetBrush(wxBrush(color));
    context.DrawRectangle(x, y, w, h);

    // optional border
    if (is_dark && color.Red() < 45 && color.Green() < 45 && color.Blue() < 45) {
        context.SetPen(wxPen(wxColour(130, 130, 128), 1)); // grey border for very dark colors
        context.SetBrush(*wxTRANSPARENT_BRUSH);
        context.DrawRectangle(x, y, w, h);
    } else if (!is_dark && color.Red() > 224 && color.Green() > 224 && color.Blue() > 224) {
        context.SetPen(wxPen(wxColour(207, 207, 207), 1)); // light grey border for very light colors
        context.SetBrush(*wxTRANSPARENT_BRUSH);
        context.DrawRectangle(x, y, w, h);
    }

    // text
    context.SetFont(::Label::Body_14);
    context.SetTextForeground(color.GetLuminance() > 0.5 ? wxColour(50, 58, 61) : *wxWHITE);

    const wxSize  text_size = context.GetTextExtent(text);
    const wxPoint text_baseline_start_pos(x + (w - text_size.x) / 2, y + (h - text_size.y) / 2);
    context.DrawText(text, text_baseline_start_pos.x, text_baseline_start_pos.y);
}

void FilamentCardMixed::paint_clr_swatch_gradient(
    wxDC& context, const wxSize& size, const std::vector<wxColor>& colors, const wxString& text, bool is_dark, int padding)
{
    int x = padding;
    int y = padding;
    int w = size.x - 2 * padding;
    int h = size.y - 2 * padding;

    if (w <= 0 || h <= 0 || colors.empty())
        return;

    wxWindow*     window     = context.GetWindow();
    const wxColor background = window ? window->GetBackgroundColour() : StateColor::darkModeColorFor(*wxWHITE);
    context.SetPen(*wxTRANSPARENT_PEN);
    context.SetBrush(wxBrush(background));
    context.DrawRectangle(0, 0, size.x, size.y);

    wxUnusedVar(is_dark);
    const wxColor border_color(ColorRGB::ORCA().r_uchar(), ColorRGB::ORCA().g_uchar(), ColorRGB::ORCA().b_uchar());
    wxBitmap*     gradient_bitmap = get_color_block_bitmap_cached(colors, true, w, h, text, wxColour(130, 130, 128), {}, true);
    if (gradient_bitmap != nullptr)
        context.DrawBitmap(*gradient_bitmap, x, y, true);

    context.SetPen(wxPen(border_color, 1));
    context.SetBrush(*wxTRANSPARENT_BRUSH);
    context.DrawRectangle(x, y, w, h);
}

// static
void FilamentCardMixed::paint_box_mix(wxDC&                            context,
                                      const wxSize&                    size,
                                      const wxColor&                   background_color,
                                      const std::vector<unsigned int>& indices,
                                      const std::vector<int>&          percentages,
                                      const std::vector<wxColor>&      colors,
                                      bool                             is_dark,
                                      bool                             is_hovered,
                                      const wxSize&                    swatch_size)
{
    // background
    context.SetBrush(wxBrush(background_color));
    context.SetPen(*wxTRANSPARENT_PEN);
    context.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());

    if (colors.empty() || colors.size() != percentages.size()) {
        return;
    }

    const int physical_count  = percentages.size();
    const int padding         = (size.y - swatch_size.y) / 2;
    const int available_width = size.x - 2 * padding; // account for padding on left and right of content panel
    const int paintable_width = available_width - ((physical_count - 1) * padding); // account for padding between swatches
    if (paintable_width <= 0)
        return;
    const int swatch_width_min          = std::min(swatch_size.x, int(std::round(0.2 * paintable_width)));
    const int paintable_width_after_min = paintable_width - (physical_count * swatch_width_min);

    const int theoretical_percentage_of_width_min = 100 * (double(swatch_width_min) / double(paintable_width));

    // calculate vector of swatch widths, proportional to component percentages, but respecting swatch_width_min
    int theoretical_total_percentage_left = 0;
    for (int percentage : percentages) {
        if (percentage > theoretical_percentage_of_width_min)
            theoretical_total_percentage_left += (percentage - theoretical_percentage_of_width_min);
    }

    std::vector<int> swatch_widths;
    int              total_width_used = 0;
    for (int i = 0; i < physical_count; i++) {
        int width = swatch_width_min;

        // Only swatches above the threshold get a slice of the leftover pixels
        if (percentages[i] > theoretical_percentage_of_width_min) {
            double percentage_of_rest_of_width = percentages[i] - theoretical_percentage_of_width_min;
            width += std::round((percentage_of_rest_of_width * paintable_width_after_min) / theoretical_total_percentage_left);
        }

        // Safety: Ensure the last swatch fills the remaining space perfectly (handles rounding errors)
        if (i == physical_count - 1) {
            width = paintable_width - total_width_used;
        }

        swatch_widths.push_back(width);
        total_width_used += width;
    }

    // calculate starting x position for each swatch, accounting for padding between swatches
    std::vector<int> swatch_start_x_positions;
    for (int i = 0; i < physical_count; i++) {
        int start_x = padding; // initial padding on the left of the content panel
        for (int j = 0; j < i; j++) {
            start_x += swatch_widths[j] + padding; // add width of previous swatches and padding between them
        }
        swatch_start_x_positions.push_back(start_x);
    }

    // paint swatches
    for (int i = 0; i < physical_count; i++) {
        // background
        context.SetBrush(wxBrush(colors[i]));
        context.SetPen(*wxTRANSPARENT_PEN);
        context.DrawRectangle(swatch_start_x_positions[i], padding, swatch_widths[i], swatch_size.y);

        // optional border for very dark or very light colors
        if (is_dark && colors[i].Red() < 45 && colors[i].Green() < 45 && colors[i].Blue() < 45) {
            context.SetPen(wxPen(wxColour(130, 130, 128), 1)); // grey border for very dark colors
            context.SetBrush(*wxTRANSPARENT_BRUSH);
            context.DrawRectangle(swatch_start_x_positions[i], padding, swatch_widths[i], swatch_size.y);
        } else if (!is_dark && colors[i].Red() > 224 && colors[i].Green() > 224 && colors[i].Blue() > 224) {
            context.SetPen(wxPen(wxColour(207, 207, 207), 1)); // light grey border for very light colors
            context.SetBrush(*wxTRANSPARENT_BRUSH);
            context.DrawRectangle(swatch_start_x_positions[i], padding, swatch_widths[i], swatch_size.y);
        }

        // text
        context.SetFont(::Label::Body_14);
        context.SetTextForeground(colors[i].GetLuminance() > 0.5 ? wxColour(50, 58, 61) : *wxWHITE);

        const wxString text      = is_hovered ? wxString(std::to_string(percentages[i]) + "%") : wxString(std::to_string(indices[i]));
        const wxSize   text_size = context.GetTextExtent(text);
        const wxPoint  text_baseline_start_pos(swatch_start_x_positions[i] + (swatch_widths[i] - text_size.x) / 2,
                                               padding + (swatch_size.y - text_size.y) / 2);
        context.DrawText(text, text_baseline_start_pos.x, text_baseline_start_pos.y);
    }

    // border (draw last, so its on top)
    const int     border_width = 1;
    const wxColor border_color = is_hovered ?
                                     wxColor(ColorRGB::ORCA().r_uchar(), ColorRGB::ORCA().g_uchar(), ColorRGB::ORCA().b_uchar(), 1) :
                                     StateColor::darkModeColorFor(wxColour("#CECECE"));

    context.SetBrush(*wxTRANSPARENT_BRUSH);
    context.SetPen(wxPen(border_color, border_width));
    context.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());
}

// static
void FilamentCardMixed::paint_box_pattern(wxDC&                            context,
                                          const wxSize&                    size,
                                          const wxColor&                   background_color,
                                          const std::vector<unsigned int>& indices,
                                          const std::vector<wxColor>&      colors,
                                          bool                             is_dark,
                                          bool                             is_hovered,
                                          const wxSize&                    swatch_size)
{
    // background
    context.SetBrush(wxBrush(background_color));
    context.SetPen(*wxTRANSPARENT_PEN);
    context.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());

    if (colors.empty() || colors.size() != indices.size()) {
        return;
    }

    const int pattern_count          = colors.size();
    const int padding                = (size.y - swatch_size.y) / 2;
    const int available_swatch_width = size.x - padding; // account for padding on left of content panel
    const int swatch_width           = std::min(swatch_size.x,
                                                int(std::round(available_swatch_width / 4.5))); // shrink, so that atleast 4 swatches visible

    // calculate starting x position for each swatch, accounting for padding between swatches
    std::vector<int> swatch_start_x_positions;
    int              max_paintable_swatch_count = 0;
    for (int i = 0; i < pattern_count; i++) {
        int start_x = padding;
        for (int j = 0; j < i; j++) {
            start_x += swatch_width; // add width of previous swatches
        }
        swatch_start_x_positions.push_back(start_x);

        if (start_x <= size.x) {
            max_paintable_swatch_count++; // calculate how many swatches fit
        }
    }

    // paint swatches
    for (int i = 0; i < max_paintable_swatch_count; i++) {
        // background
        context.SetBrush(wxBrush(colors[i]));
        context.SetPen(*wxTRANSPARENT_PEN);
        context.DrawRectangle(swatch_start_x_positions[i], padding, swatch_width, swatch_size.y);

        // optional border for very dark or very light colors
        if (is_dark && colors[i].Red() < 45 && colors[i].Green() < 45 && colors[i].Blue() < 45) {
            context.SetPen(wxPen(wxColour(130, 130, 128), 1)); // grey border for very dark colors
            context.SetBrush(*wxTRANSPARENT_BRUSH);
            context.DrawRectangle(swatch_start_x_positions[i], padding, swatch_width, swatch_size.y);
        } else if (!is_dark && colors[i].Red() > 224 && colors[i].Green() > 224 && colors[i].Blue() > 224) {
            context.SetPen(wxPen(wxColour(207, 207, 207), 1)); // light grey border for very light colors
            context.SetBrush(*wxTRANSPARENT_BRUSH);
            context.DrawRectangle(swatch_start_x_positions[i], padding, swatch_width, swatch_size.y);
        }

        // text
        context.SetFont(::Label::Body_14);
        context.SetTextForeground(colors[i].GetLuminance() > 0.5 ? wxColour(50, 58, 61) : *wxWHITE);

        const wxString text      = wxString(std::to_string(indices[i]));
        const wxSize   text_size = context.GetTextExtent(text);
        const wxPoint  text_baseline_start_pos(swatch_start_x_positions[i] + (swatch_width - text_size.x) / 2,
                                               padding + (swatch_size.y - text_size.y) / 2);
        context.DrawText(text, text_baseline_start_pos.x, text_baseline_start_pos.y);
    }

    // fade (when not all swatches fit)
    int all_swatch_width = padding + swatch_width * pattern_count;
    if (all_swatch_width > size.x) {
        std::unique_ptr<wxGraphicsContext> graphics_context(wxGraphicsContext::CreateFromUnknownDC(context));

        if (graphics_context) {
            const int fade_width = std::min(30, size.x / 3);

            wxColour base_color        = background_color;
            wxColour transparent_color = wxColour(base_color.Red(), base_color.Green(), base_color.Blue(), 0);

            wxGraphicsBrush gradient_brush = graphics_context->CreateLinearGradientBrush(size.x - fade_width, 0, size.x, 0,
                                                                                         transparent_color, base_color);

            graphics_context->SetBrush(gradient_brush);
            graphics_context->SetPen(*wxTRANSPARENT_PEN);
            graphics_context->DrawRectangle(size.x - fade_width, 0, fade_width, size.y);
        }
    }

    // border (draw last, so its on top)
    const int     border_width = 1;
    const wxColor border_color = is_hovered ?
                                     wxColor(ColorRGB::ORCA().r_uchar(), ColorRGB::ORCA().g_uchar(), ColorRGB::ORCA().b_uchar(), 1) :
                                     StateColor::darkModeColorFor(wxColour("#CECECE"));

    context.SetBrush(*wxTRANSPARENT_BRUSH);
    context.SetPen(wxPen(border_color, border_width));
    context.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());
}

void FilamentCardMixed::update_state(MixedFilamentDefinition* definition, bool refresh)
{
    m_gradient_preview_colors.clear();
    m_gradient_component_positions.clear();
    m_gradient_component_ids.clear();

    if (definition == nullptr) {
        m_definition = nullptr;
        m_physical_filaments_indices.clear();
        m_physical_filaments_colors.clear();
        m_physical_filaments_percentages.clear();
        update_color_swatch_size();
        if (refresh)
            Refresh();
        return;
    }

    if (definition->recipe.kind == MixedFilamentRecipeKind::WeightedBlend) {
        m_physical_filaments_indices     = definition->recipe.blend.component_ids();
        m_physical_filaments_colors      = get_physical_filaments_colors(m_physical_filaments_indices);
        m_physical_filaments_percentages = definition->recipe.blend.component_percents();
    } else if (definition->recipe.manual_pattern && !definition->recipe.manual_pattern->groups.empty()) {
        std::vector<unsigned int> main_pattern_indices;
        for (const auto& group : definition->recipe.manual_pattern->groups[0]) {
            main_pattern_indices.push_back(group.id);
        }

        m_physical_filaments_indices     = main_pattern_indices;
        m_physical_filaments_colors      = get_physical_filaments_colors(m_physical_filaments_indices);
        m_physical_filaments_percentages = {};
    } else {
        m_physical_filaments_indices.clear();
        m_physical_filaments_colors.clear();
        m_physical_filaments_percentages.clear();
    }

    m_definition = definition;

    if (is_gradient_definition(definition)) {
        std::vector<std::string> physical_colors;
        physical_colors.reserve(m_physical_filaments.size());
        for (const auto& filament : m_physical_filaments)
            physical_colors.emplace_back(filament.first);
        const MixedFilamentGradientPreview preview = build_mixed_filament_gradient_preview(*definition, build_mixed_filament_display_context(
                                                                                                            physical_colors));
        m_gradient_preview_colors      = preview.sampled_colors;
        m_gradient_component_positions = preview.component_positions;
        m_gradient_component_ids       = preview.component_ids;
    }

    update_color_swatch_size();

    if (refresh)
        Refresh();
}

void FilamentCardMixed::paint_box_gradient(wxDC&                            context,
                                           const wxSize&                    size,
                                           const wxColor&                   background_color,
                                           const std::vector<wxColor>&      colors,
                                           const std::vector<double>&       component_positions,
                                           const std::vector<unsigned int>& indices,
                                           bool                             is_dark,
                                           bool                             is_hovered,
                                           const wxSize&                    swatch_size)
{
    wxUnusedVar(is_dark);

    // background
    context.SetBrush(wxBrush(background_color));
    context.SetPen(*wxTRANSPARENT_PEN);
    context.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());

    if (colors.size() < 2 || indices.size() < 2 || component_positions.size() != (2 * indices.size() - 1)) {
        return;
    }

    const int    count   = static_cast<int>(indices.size());
    const int    padding = (size.y - swatch_size.y) / 2;
    const double w       = std::max(1.0, (double) size.x - 2.0 * padding);
    const double h       = swatch_size.y;
    const double sx      = padding;
    const double sy      = padding;

    // Use wxGraphicsContext for smooth gradient drawing
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::CreateFromUnknownDC(context));
    if (!gc)
        return;

    // Draw the uniformly sampled, engine-aware preview. The samples already
    // encode the user stop curve, including asymmetric midpoint placement.
    const int sample_segments = int(colors.size()) - 1;
    for (int index = 0; index < sample_segments; ++index) {
        const double    x_start = sx + w * double(index) / double(sample_segments);
        const double    x_end   = sx + w * double(index + 1) / double(sample_segments);
        wxGraphicsBrush brush   = gc->CreateLinearGradientBrush(x_start, sy, x_end, sy, colors[size_t(index)], colors[size_t(index + 1)]);
        gc->SetBrush(brush);
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->DrawRectangle(x_start, sy, x_end - x_start + 0.5, h);
    }

    // Now draw the filament numbers over their positions.
    std::vector<double>   x_pos(count);
    std::vector<wxString> labels(count);
    std::vector<double>   text_widths(count);
    double                min_gap      = wxWindow::FromDIP(4, context.GetWindow()); // minimal gap between text labels
    double                side_padding = wxWindow::FromDIP(4, context.GetWindow());

    context.SetFont(::Label::Body_14);

    for (int i = 0; i < count; ++i) {
        x_pos[i]         = sx + component_positions[2 * i] * w;
        labels[i]        = wxString::Format("%u", indices[i]);
        wxSize text_size = context.GetTextExtent(labels[i]);
        text_widths[i]   = text_size.x;
    }

    std::vector<double> left_bounds(count);
    std::vector<double> right_bounds(count);

    // Initial positioning: center each label at its filament's X coordinate
    for (int i = 0; i < count; ++i) {
        left_bounds[i]  = x_pos[i] - text_widths[i] / 2.0;
        right_bounds[i] = left_bounds[i] + text_widths[i];
    }

    // Clamp to boundaries first (including side padding)
    left_bounds[0]  = std::max(sx + side_padding, left_bounds[0]);
    right_bounds[0] = left_bounds[0] + text_widths[0];

    right_bounds[count - 1] = std::min(sx + w - side_padding, right_bounds[count - 1]);
    left_bounds[count - 1]  = right_bounds[count - 1] - text_widths[count - 1];

    // Forward pass: push overlapping labels rightwards
    for (int i = 1; i < count; ++i) {
        if (left_bounds[i] < right_bounds[i - 1] + min_gap) {
            left_bounds[i]  = right_bounds[i - 1] + min_gap;
            right_bounds[i] = left_bounds[i] + text_widths[i];
        }
    }

    // Backward pass: push overlapping labels leftwards
    if (right_bounds[count - 1] > sx + w - side_padding) {
        right_bounds[count - 1] = sx + w - side_padding;
        left_bounds[count - 1]  = right_bounds[count - 1] - text_widths[count - 1];
    }
    for (int i = count - 2; i >= 0; --i) {
        if (right_bounds[i] > left_bounds[i + 1] - min_gap) {
            right_bounds[i] = left_bounds[i + 1] - min_gap;
            left_bounds[i]  = right_bounds[i] - text_widths[i];
        }
    }

    // Double check boundary violation after backward pass
    if (left_bounds[0] < sx + side_padding) {
        left_bounds[0]  = sx + side_padding;
        right_bounds[0] = left_bounds[0] + text_widths[0];
        for (int i = 1; i < count; ++i) {
            if (left_bounds[i] < right_bounds[i - 1] + min_gap) {
                left_bounds[i]  = right_bounds[i - 1] + min_gap;
                right_bounds[i] = left_bounds[i] + text_widths[i];
            }
        }
    }

    // Draw the text
    for (int i = 0; i < count; ++i) {
        wxColor fill_color = interpolate_color(colors, component_positions[2 * i]);
        wxColor text_color = fill_color.GetLuminance() > 0.5 ? wxColour(50, 58, 61) : *wxWHITE;

        context.SetTextForeground(text_color);
        wxSize text_size = context.GetTextExtent(labels[i]);
        int    text_y    = sy + (h - text_size.y) / 2;
        context.DrawText(labels[i], (int) left_bounds[i], text_y);
    }

    // border (draw last, so its on top)
    const int     border_width = 1;
    const wxColor border_color = is_hovered ?
                                     wxColor(ColorRGB::ORCA().r_uchar(), ColorRGB::ORCA().g_uchar(), ColorRGB::ORCA().b_uchar(), 1) :
                                     StateColor::darkModeColorFor(wxColour("#CECECE"));

    context.SetBrush(*wxTRANSPARENT_BRUSH);
    context.SetPen(wxPen(border_color, border_width));
    context.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());
}

} // namespace Slic3r::GUI
