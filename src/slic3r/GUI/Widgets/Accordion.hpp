#ifndef slic3r_GUI_Widgets_Accordion_hpp_
#define slic3r_GUI_Widgets_Accordion_hpp_

// Accordion: A generic, reusable collapsible section widget.
//
// Why a standalone widget?
// --------------------------------------------------
// Previously, all collapsible-section behavior was handled by a single
// monolithic helper method (MixedFilamentDialog::setup_collapsible_section)
// that had to be called with a pile of raw pointer arguments for every
// section. This made each section hard to follow and impossible to reuse
// outside MixedFilamentDialog.
//
// Accordion encapsulates the toggle logic, chevron animation, hover states,
// and card-style painting in one place. Any window in the application can
// now host a collapsible section with a single instantiation.
//
// Design contract:
//  - Subclasses populate get_body_panel() with their own child widgets.
//  - "Action controls" (e.g. add/delete buttons) must be registered via
//    add_header_control(..., is_action_control=true) so that clicks on them
//    do NOT accidentally collapse/expand the section.
//  - Callers are notified of toggle events via set_on_toggle().

#include <wx/wx.h>
#include <wx/statbmp.h>
#include <functional>
#include <vector>

#include "Label.hpp"
#include "../GUI_Utils.hpp"

namespace Slic3r::GUI {

class Accordion : public wxPanel
{
public:
    Accordion(wxWindow* parent, const wxString& title, bool initially_collapsed = false);
    ~Accordion() override = default;

    bool Layout() override;

    // Returns the body panel where subclasses (and callers) should place content.
    wxPanel*    get_body_panel() const { return m_body_panel; }
    wxBoxSizer* get_body_sizer() const { return m_body_sizer; }

    // Returns the header panel where subclasses (and callers) should place header controls.
    wxPanel*    get_header_panel() const { return m_header_panel; }

    void collapse();
    void expand();
    void toggle();
    bool is_collapsed() const { return m_is_collapsed; }

    // Called after every collapse/expand with the new collapsed state.
    void set_on_toggle(std::function<void(bool collapsed)> cb) { m_on_toggle = std::move(cb); }

    void     set_title(const wxString& title);
    wxString get_title() const;

    // Add a widget to the right side of the header bar.
    // If is_action_control is true, clicks on this widget will NOT trigger
    // the collapse/expand toggle, preventing accidental section toggling.
    void add_header_control(wxWindow* ctrl, bool is_action_control = false);

protected:
    // Called when collapse state changes. Subclasses can override for custom animations.
    virtual void on_collapsed_changed(bool collapsed);

    // Bind click/hover events recursively to all non-action children of header.
    void bind_header_events(wxWindow* win);

private:
    void build_ui(const wxString& title);
    void apply_collapsed_state();
    void update_header_visual();

    void on_header_click(wxMouseEvent& event);
    void on_header_enter(wxMouseEvent& event);
    void on_header_leave(wxMouseEvent& event);
    void on_paint(wxPaintEvent& event);



    wxPanel*        m_header_panel{nullptr};
    wxBoxSizer*     m_header_sizer{nullptr};
    wxStaticText*   m_title_text{nullptr};
    wxStaticBitmap* m_chevron_bmp{nullptr};
    wxPanel*        m_body_panel{nullptr};
    wxBoxSizer*     m_body_sizer{nullptr};

    bool m_is_collapsed{false};
    bool m_is_hovered{false};

    wxBitmap m_chevron_normal;
    wxBitmap m_chevron_rotated;

    // Controls added to the header that should NOT trigger collapse on click.
    std::vector<wxWindow*> m_action_controls;

    std::function<void(bool)> m_on_toggle;
};

} // namespace Slic3r::GUI

#endif // slic3r_GUI_Widgets_Accordion_hpp_
