#include "FilamentCardMixed.hpp"

#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Factories.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "libslic3r/MixedFilament.hpp"

#include <wx/wx.h>
#include <string>
#include <cctype> 

//TODO: implement card rendering, mirroring current mixed filament list
// use gemini overview

namespace Slic3r::GUI {

FilamentCardMixed::FilamentCardMixed(wxWindow* parent, MixedFilamentDefinition* definition, std::vector<std::pair<std::string, std::string>>& physical_filaments)
    : ::wxPanel(parent, wxID_ANY)
    , m_definition{definition}
    , m_physical_filaments{physical_filaments}
{
    SetBackgroundColour(*wxWHITE);
    update_state(definition, false);

    build_ui();
}

void FilamentCardMixed::build_ui()
{
    // TODO check for dark_mode -> use smth like     const wxColour mc_bg     = StateColor::darkModeColorFor(*wxWHITE);
    int swatch_size = FromDIP(20);
    int content_heigth = FromDIP(30);
    

    m_main_sizer = new wxBoxSizer(wxHORIZONTAL);


    m_clr_swatch_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(swatch_size, swatch_size));
    m_clr_swatch_panel->SetMinSize(wxSize(swatch_size, swatch_size));
    // paint
    m_clr_swatch_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_clr_swatch_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent& event) {
        if (!m_definition) {
            wxPaintDC context(m_clr_swatch_panel);
            return;
        }


        // TODO implement function in MxiedFilamentPresentation
        auto HexStringToWxColor = [](const std::string& hexStr) -> wxColor {
            if (hexStr.empty()) {
                return wxColor(0, 0, 0);
            }

            wxString hex(hexStr);
            if (hex[0] == '#') {
                hex = hex.Mid(1);
            }

            if (hex.Length() != 6) {
                return wxColor(0, 0, 0);
            }

            long red, green, blue;
            if (!hex.Mid(0, 2).ToLong(&red, 16) || 
                !hex.Mid(2, 2).ToLong(&green, 16) || 
                !hex.Mid(4, 2).ToLong(&blue, 16))
            {
                return wxColor(0, 0, 0);
            }

            // Create and return the wxColor
            return wxColor(red, green, blue);
        };

        
        wxPaintDC    context(m_clr_swatch_panel);
        const wxSize size = m_clr_swatch_panel->GetClientSize();

        //TODO get color and text from data
        paint_clr_swatch(
            context,
            size, 
            HexStringToWxColor("AABBCC"), // m_definition->presentation.display_color), 
            wxString(std::to_string(m_definition->identity.stable_id)),
            wxGetApp().dark_mode()
        );
    });
    

    m_box_panel = new wxPanel(this);
    m_box_panel->SetMinSize(wxSize(-1, content_heigth));

    // click
    m_box_panel->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
        if (m_on_box_edit) 
            m_on_box_edit();
        
        event.Skip();
    });

    // hover
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

    // paint
    m_box_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_box_panel->Bind(wxEVT_PAINT, [this, swatch_size](wxPaintEvent& event) {
        if (!m_definition) {
            wxPaintDC context(m_box_panel);
            return;
        }
        /*

        // TODO get colors from data
        std::vector<wxColor> colors;
        for (int i = 0; i < m_data.definition->recipe.blend.components.size(); i++) {
            switch (i) {
                case 0: colors.push_back(wxColor("RED")); break;
                case 1: colors.push_back(wxColor("GREEN")); break;
                case 2: colors.push_back(wxColor("BLUE")); break;
                default: colors.push_back(wxColor("GRAY")); break; // fallback color for more than 3 components
            }
        }

        // TODO get actual index texts
        std::vector<wxString> index_texts;
        for (int i = 0; i < m_data.definition->recipe.blend.components.size(); i++) {
            index_texts.push_back(wxString(std::to_string(i + 1)));
        }*/

        // TODO actual logic
        wxPaintDC context(m_box_panel);

        const wxSize size               = m_box_panel->GetClientSize();
        const wxColor background_color  = GetBackgroundColour();

        if (m_definition->recipe.kind == MixedFilamentRecipeKind::WeightedBlend) 
        {
            paint_box_mix(
                context, size, background_color,
                m_physical_filaments_indices,
                m_physical_filaments_percentages,
                m_physical_filaments_colors,    
                wxGetApp().dark_mode(), m_is_box_panel_hovered, wxSize(swatch_size, swatch_size)
            );
        } 
        else if (m_definition->recipe.kind == MixedFilamentRecipeKind::ManualPattern) 
        {
            paint_box_pattern(
                context, size, background_color,
                m_physical_filaments_indices, 
                m_physical_filaments_colors,
                wxGetApp().dark_mode(), m_is_box_panel_hovered, wxSize(swatch_size, swatch_size)
            );
        }


    });

    m_filament_edit_btn = new ScalableButton(this, wxID_ANY, "menu_filament");
    m_filament_edit_btn->SetToolTip(_L("Click to edit preset"));
    // bind edit event

    // Sizing

    SetSizer(m_main_sizer);
    m_main_sizer->Add(m_clr_swatch_panel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_main_sizer->Add(m_box_panel, 1, wxEXPAND | wxALL, FromDIP(2));
    m_main_sizer->Add(m_filament_edit_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
}

std::vector<wxColor> FilamentCardMixed::get_physical_filaments_colors(const std::vector<unsigned int>& filament_indices) const
{
    std::vector<wxColor> colors;
    colors.reserve(filament_indices.size());

    for (auto physical_filament_index : filament_indices) {
        if (physical_filament_index < 0 || physical_filament_index >= (int) m_physical_filaments.size()) {
            colors.push_back(wxColor(*wxBLACK));
        } else {
            const auto& [color_hex, name] = m_physical_filaments[physical_filament_index - 1];
            colors.push_back(wxColor(color_hex));
        }
    }

    return colors;
}

// static
// paint background, text and optional border of color swatch
void FilamentCardMixed::paint_clr_swatch(wxDC& context, const wxSize& size, wxColor& color, wxString& text, bool is_dark, int padding)
{
    // Draw the swatch box (optionally inset by padding)
    int x = padding;
    int y = padding;
    int w = size.x - 2 * padding;
    int h = size.y - 2 * padding;

    if (w <= 0 || h <= 0) return;

    // Draw the swatch background
    context.SetPen(*wxTRANSPARENT_PEN);
    context.SetBrush(wxBrush(color));
    context.DrawRectangle(x, y, w, h);

    // optional border
    if (is_dark && color.Red() < 45 && color.Green() < 45 && color.Blue() < 45)
    {
        context.SetPen(wxPen(wxColour(130, 130, 128), 1)); // grey border for very dark colors
        context.SetBrush(*wxTRANSPARENT_BRUSH);
        context.DrawRectangle(x, y, w, h);
    } 
    else if (!is_dark && color.Red() > 224 && color.Green() > 224 && color.Blue() > 224)
    {
        context.SetPen(wxPen(wxColour(207, 207, 207), 1)); // light grey border for very light colors
        context.SetBrush(*wxTRANSPARENT_BRUSH);
        context.DrawRectangle(x, y, w, h);
    }

    // text
    context.SetFont(::Label::Body_14);
    context.SetTextForeground(color.GetLuminance() > 0.5 ? wxColour(50, 58, 61) : *wxWHITE);

    const wxSize text_size = context.GetTextExtent(text);
    const wxPoint text_baseline_start_pos(x + (w - text_size.x) / 2, y + (h - text_size.y) / 2);
    context.DrawText(text, text_baseline_start_pos.x, text_baseline_start_pos.y);
}

// static
void FilamentCardMixed::paint_box_mix(
    wxDC& context, 
    const wxSize& size, 
    const wxColor& background_color,
    std::vector<unsigned int> indices,
    std::vector<int>&      percentages,
    std::vector<wxColor>&  colors, 
    bool is_dark, 
    bool is_hovered,
    wxSize& swatch_size)
{
    // background
    context.SetBrush(wxBrush(background_color));
    context.SetPen(*wxTRANSPARENT_PEN);
    context.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());

    if (colors.size() != percentages.size()) {
        return;
    }

    const int    physical_count             = percentages.size();
    const int    padding                    = (size.y - swatch_size.y) / 2;
    const int    available_width            = size.x - 2 * padding; // account for padding on left and right of content panel
    const int    paintable_width            = available_width - ((physical_count - 1) * padding); // account for padding between swatches
    const int    swatch_width_min           = std::min(swatch_size.x, int(std::round(0.2 * paintable_width)));
    const int    paintable_width_after_min  = paintable_width - (physical_count * swatch_width_min);
    
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
        if (is_dark && colors[i].Red() < 45 && colors[i].Green() < 45 && colors[i].Blue() < 45)
        {
            context.SetPen(wxPen(wxColour(130, 130, 128), 1)); // grey border for very dark colors
            context.SetBrush(*wxTRANSPARENT_BRUSH);
            context.DrawRectangle(swatch_start_x_positions[i], padding, swatch_widths[i], swatch_size.y);
        } 
        else if (!is_dark && colors[i].Red() > 224 && colors[i].Green() > 224 && colors[i].Blue() > 224)
        {
            context.SetPen(wxPen(wxColour(207, 207, 207), 1)); // light grey border for very light colors
            context.SetBrush(*wxTRANSPARENT_BRUSH);
            context.DrawRectangle(swatch_start_x_positions[i], padding, swatch_widths[i], swatch_size.y);
        }

        // text
        context.SetFont(::Label::Body_14);
        context.SetTextForeground(colors[i].GetLuminance() > 0.5 ? wxColour(50, 58, 61) : *wxWHITE);

        const wxString text = is_hovered 
            ? wxString(std::to_string(percentages[i]) + "%") 
            : wxString(std::to_string(indices[i]));
        const wxSize text_size = context.GetTextExtent(text);
        const wxPoint text_baseline_start_pos(
            swatch_start_x_positions[i] + (swatch_widths[i] - text_size.x) / 2, 
            padding + (swatch_size.y - text_size.y) / 2
        );
        context.DrawText(text, text_baseline_start_pos.x, text_baseline_start_pos.y);
    }

    // border (draw last, so its on top)
    const int border_width = 1;
    const wxColor border_color = is_hovered
        ? wxColor(ColorRGB::ORCA().r_uchar(), ColorRGB::ORCA().g_uchar(), ColorRGB::ORCA().b_uchar(), 1) 
        : wxColor("#CECECE");

    context.SetBrush(*wxTRANSPARENT_BRUSH);
    context.SetPen(wxPen(border_color, border_width));
    context.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());
}

// static
void FilamentCardMixed::paint_box_pattern(
    wxDC& context, 
    const wxSize& size, 
    const wxColor& background_color,
    std::vector<unsigned int> indices,
    std::vector<wxColor>&   colors,
    bool                    is_dark,
    bool                    is_hovered,
    wxSize&                 swatch_size)
{
    // background
    context.SetBrush(wxBrush(background_color));
    context.SetPen(*wxTRANSPARENT_PEN);
    context.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());

    if (colors.size() != indices.size()) {
        return;
    }

    const int pattern_count          = colors.size();
    const int padding                = (size.y - swatch_size.y) / 2;
    const int available_swatch_width = size.x - padding; // account for padding on left of content panel
    const int swatch_width           = std::min(swatch_size.x, int(std::round(available_swatch_width / 4.5))); // shrink, so that atleast 4 swatches visible

    // calculate starting x position for each swatch, accounting for padding between swatches
    std::vector<int> swatch_start_x_positions;
    int max_paintable_swatch_count = 0;
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

        const wxString text = wxString(std::to_string(indices[i]));
        const wxSize  text_size = context.GetTextExtent(text);
        const wxPoint text_baseline_start_pos(swatch_start_x_positions[i] + (swatch_width - text_size.x) / 2,
                                              padding + (swatch_size.y - text_size.y) / 2);
        context.DrawText(text, text_baseline_start_pos.x, text_baseline_start_pos.y);
    }

    // fade (when not all swatches fit)
    int all_swatch_width = padding + swatch_width * pattern_count; 
    if (all_swatch_width > size.x)
    {
        std::unique_ptr<wxGraphicsContext> graphics_context(wxGraphicsContext::CreateFromUnknownDC(context));

        if (graphics_context) {
            const int fade_width = std::min(30, size.x / 3); 

            wxColour base_color        = background_color;
            wxColour transparent_color = wxColour(base_color.Red(), base_color.Green(), base_color.Blue(), 0);

            wxGraphicsBrush gradient_brush = graphics_context->CreateLinearGradientBrush(
                size.x - fade_width, 0,
                size.x, 0, 
                transparent_color, 
                base_color
            );

            graphics_context->SetBrush(gradient_brush);
            graphics_context->SetPen(*wxTRANSPARENT_PEN);
            graphics_context->DrawRectangle(
                size.x - fade_width, 
                0, 
                fade_width, 
                size.y
            );
        }
    }

    // border (draw last, so its on top)
    const int border_width = 1;
    const wxColor border_color = is_hovered
        ? wxColor(ColorRGB::ORCA().r_uchar(), ColorRGB::ORCA().g_uchar(), ColorRGB::ORCA().b_uchar(), 1) 
        : wxColor("#CECECE");

    context.SetBrush(*wxTRANSPARENT_BRUSH);
    context.SetPen(wxPen(border_color, border_width));
    context.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());
}


void FilamentCardMixed::update_state(MixedFilamentDefinition* definition, bool refresh)
{
    // TODO update color swatch and content panel based on data

    if (definition->recipe.kind == MixedFilamentRecipeKind::WeightedBlend) {
        m_physical_filaments_indices     = definition->recipe.blend.component_ids(definition->recipe.blend.components.size());
        m_physical_filaments_colors      = get_physical_filaments_colors(m_physical_filaments_indices);
        m_physical_filaments_percentages = definition->recipe.blend.component_percents();
    } else {
        std::vector<unsigned int> main_pattern_indices;
        for (const auto& group : definition->recipe.manual_pattern->groups[0]) {
            main_pattern_indices.push_back(group.id);
        }

        m_physical_filaments_indices     = main_pattern_indices;
        m_physical_filaments_colors      = get_physical_filaments_colors(m_physical_filaments_indices);
        m_physical_filaments_percentages = {};
    }

    m_definition = definition;

    if (refresh)
        Refresh();

}

void FilamentCardMixed::paint_box_gradient(
    wxDC&                       context,
    const wxSize&               size,
    const wxColor&              background_color,
    const std::vector<wxColor>& colors,
    const std::vector<double>&  positions,
    const std::vector<unsigned int>& indices,
    bool                        is_dark,
    bool                        is_hovered,
    wxSize&                     swatch_size)
{
    // background
    context.SetBrush(wxBrush(background_color));
    context.SetPen(*wxTRANSPARENT_PEN);
    context.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());

    if (colors.empty() || positions.empty() || colors.size() != indices.size() || positions.size() != (2 * colors.size() - 1)) {
        return;
    }

    const int count = static_cast<int>(colors.size());
    const int padding = (size.y - swatch_size.y) / 2;
    const double w = std::max(1.0, (double)size.x - 2.0 * padding);
    const double h = swatch_size.y;
    const double sx = padding;
    const double sy = padding;

    // Use wxGraphicsContext for smooth gradient drawing
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::CreateFromUnknownDC(context));
    if (!gc) return;

    // Draw the gradient bar
    for (int i = 0; i < count - 1; ++i) {
        double p_start = positions[2 * i];
        double p_mid   = positions[2 * i + 1];
        double p_end   = positions[2 * i + 2];

        double x_start = sx + p_start * w;
        double x_mid   = sx + p_mid * w;
        double x_end   = sx + p_end * w;

        wxColor c_start = colors[i];
        wxColor c_end   = colors[i + 1];
        wxColor c_mid(
            (c_start.Red() + c_end.Red()) / 2,
            (c_start.Green() + c_end.Green()) / 2,
            (c_start.Blue() + c_end.Blue()) / 2
        );

        if (x_mid > x_start) {
            wxGraphicsBrush gb1 = gc->CreateLinearGradientBrush(x_start, sy, x_mid, sy, c_start, c_mid);
            gc->SetBrush(gb1);
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->DrawRectangle(x_start, sy, x_mid - x_start + 0.5, h);
        }
        if (x_end > x_mid) {
            wxGraphicsBrush gb2 = gc->CreateLinearGradientBrush(x_mid, sy, x_end, sy, c_mid, c_end);
            gc->SetBrush(gb2);
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->DrawRectangle(x_mid, sy, x_end - x_mid, h);
        }
    }

    // Now draw the filament numbers over their positions.
    std::vector<double> x_pos(count);
    std::vector<wxString> labels(count);
    std::vector<double> text_widths(count);
    double min_gap = wxWindow::FromDIP(4, context.GetWindow()); // minimal gap between text labels
    double side_padding = wxWindow::FromDIP(4, context.GetWindow());

    context.SetFont(::Label::Body_14);

    for (int i = 0; i < count; ++i) {
        x_pos[i] = sx + positions[2 * i] * w;
        labels[i] = wxString::Format("%u", indices[i]);
        wxSize text_size = context.GetTextExtent(labels[i]);
        text_widths[i] = text_size.x;
    }

    std::vector<double> left_bounds(count);
    std::vector<double> right_bounds(count);

    // Initial positioning: center each label at its filament's X coordinate
    for (int i = 0; i < count; ++i) {
        left_bounds[i] = x_pos[i] - text_widths[i] / 2.0;
        right_bounds[i] = left_bounds[i] + text_widths[i];
    }

    // Clamp to boundaries first (including side padding)
    left_bounds[0] = std::max(sx + side_padding, left_bounds[0]);
    right_bounds[0] = left_bounds[0] + text_widths[0];

    right_bounds[count - 1] = std::min(sx + w - side_padding, right_bounds[count - 1]);
    left_bounds[count - 1] = right_bounds[count - 1] - text_widths[count - 1];

    // Forward pass: push overlapping labels rightwards
    for (int i = 1; i < count; ++i) {
        if (left_bounds[i] < right_bounds[i - 1] + min_gap) {
            left_bounds[i] = right_bounds[i - 1] + min_gap;
            right_bounds[i] = left_bounds[i] + text_widths[i];
        }
    }

    // Backward pass: push overlapping labels leftwards
    if (right_bounds[count - 1] > sx + w - side_padding) {
        right_bounds[count - 1] = sx + w - side_padding;
        left_bounds[count - 1] = right_bounds[count - 1] - text_widths[count - 1];
    }
    for (int i = count - 2; i >= 0; --i) {
        if (right_bounds[i] > left_bounds[i + 1] - min_gap) {
            right_bounds[i] = left_bounds[i + 1] - min_gap;
            left_bounds[i] = right_bounds[i] - text_widths[i];
        }
    }

    // Double check boundary violation after backward pass
    if (left_bounds[0] < sx + side_padding) {
        left_bounds[0] = sx + side_padding;
        right_bounds[0] = left_bounds[0] + text_widths[0];
        for (int i = 1; i < count; ++i) {
            if (left_bounds[i] < right_bounds[i - 1] + min_gap) {
                left_bounds[i] = right_bounds[i - 1] + min_gap;
                right_bounds[i] = left_bounds[i] + text_widths[i];
            }
        }
    }

    // Draw the text
    for (int i = 0; i < count; ++i) {
        wxColor fill_color = colors[i];
        wxColor text_color = fill_color.GetLuminance() > 0.5 ? wxColour(50, 58, 61) : *wxWHITE;
        
        context.SetTextForeground(text_color);
        wxSize text_size = context.GetTextExtent(labels[i]);
        int text_y = sy + (h - text_size.y) / 2;
        context.DrawText(labels[i], (int)left_bounds[i], text_y);
    }

    // border (draw last, so its on top)
    const int border_width = 1;
    const wxColor border_color = is_hovered
        ? wxColor(ColorRGB::ORCA().r_uchar(), ColorRGB::ORCA().g_uchar(), ColorRGB::ORCA().b_uchar(), 1) 
        : wxColor("#CECECE");

    context.SetBrush(*wxTRANSPARENT_BRUSH);
    context.SetPen(wxPen(border_color, border_width));
    context.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());
}

} // namespace Slic3r::GUI
