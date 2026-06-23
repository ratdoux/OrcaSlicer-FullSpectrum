#ifndef slic3r_GUI_MFDColorPickerAccordion_hpp_
#define slic3r_GUI_MFDColorPickerAccordion_hpp_

// MFDColorPickerAccordion: Collapsible color picker section for the Mixed Filament Dialog.
//
// Responsibility:
//   Hosts the HSLColorPicker wheel and a match-status row showing:
//     - The nearest achievable hex color for the current filament set
//     - A swatch displaying that color
//     - A hover-animated match percentage / warning indicator
//   Also maintains compact summary widgets in the accordion header (selected hex
//   color + optional warning icon) that are visible when the section is collapsed.
//
// Why a separate class?
//   All this logic was previously scattered across build_color_picker_ui(),
//   update_color_match(), refresh_color_picker_title_preview(), and paint_matched_color()
//   inside MixedFilamentDialog, making each of those functions reference a large number
//   of member variables from the dialog. Encapsulating it here gives the color picker
//   its own clear lifecycle and painting responsibility.
//
// Data flow:
//   User drags HSL picker  -->  on_color_changed callback  -->  dialog updates state
//   Dialog calls update_match_status() / sync_color()  -->  display refreshes
//
// Sync guard (m_syncing):
//   When the dialog programmatically sets the color via sync_color() (e.g. after
//   the user changes a ratio), m_syncing prevents the resulting color-change event
//   from looping back and triggering another ratio update.

#include "Widgets/Accordion.hpp"
#include "Widgets/HSLColorPicker.hpp"
#include <functional>

namespace Slic3r::GUI {

class MFDColorPickerAccordion : public Accordion
{
public:
    explicit MFDColorPickerAccordion(wxWindow* parent);
    ~MFDColorPickerAccordion() override = default;

    // Called whenever the user moves the color picker (is_dragging=true while dragging).
    void set_on_color_changed(std::function<void(const wxColour&, bool is_dragging)> cb)
    {
        m_on_color_changed = std::move(cb);
    }

    // Programmatically move the picker to 'color' without firing on_color_changed.
    // Used when the ratio or material selection changes and the picker must follow.
    void sync_color(const wxColour& color);

    // Update the match-status row with the nearest achievable color and its deviation.
    void update_match_status(
        const wxColour& selected_color,
        const wxColour& matched_color,
        double          deviation,
        double          warning_threshold,
        double          max_deviation);

    // Reset the match row to an empty/unknown state.
    void clear_match();

protected:
    // Refreshes the header summary (hex label, swatch, warning icon).
    void on_collapsed_changed(bool collapsed) override;

private:
    void build_ui();
    void paint_matched_color_preview(wxPaintEvent& event);

    HSLColorPicker* m_hsl_color_picker{nullptr};

    // Match-status row
    wxPanel*        m_match_section_panel{nullptr};
    wxTextCtrl*     m_matched_hex_display{nullptr};
    wxPanel*        m_matched_color_preview{nullptr};

    // Header summary (visible when collapsed)
    wxStaticText*   m_title_selected_hex{nullptr};
    wxPanel*        m_title_selected_preview{nullptr};
    wxStaticText*   m_title_warning{nullptr};

    bool   m_match_section_hovered{false};
    double m_current_deviation{-1.0};
    double m_warning_deviation_threshold{65.0};
    double m_max_deviation{441.673}; // sqrt(3 * 255^2)

    std::function<void(const wxColour&, bool)> m_on_color_changed;

    // Prevents circular updates when syncing color from outside.
    bool m_syncing{false};
};

} // namespace Slic3r::GUI

#endif // slic3r_GUI_MFDColorPickerAccordion_hpp_
