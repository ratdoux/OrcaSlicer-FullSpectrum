#ifndef slic3r_GUI_SliceModePopup_hpp_
#define slic3r_GUI_SliceModePopup_hpp_

#include <wx/popupwin.h>
#include <wx/timer.h>

#include <vector>

namespace Slic3r { namespace GUI {

// Requirement 7.1 hover popup on the slice button: choose between the standard
// and the custom filament grouping mode (Figma node 27526:61473).
// Shown with Show()/Hide() plus a polling timer instead of Popup()/Dismiss(),
// so the underlying buttons keep receiving mouse clicks.
//
// Deliberately derived from plain wxPopupWindow, NOT from wxPopupTransientWindow
// (nor Widgets/PopupWindow which wraps it): under wxGTK wxPopupTransientWindow
// overrides Show() itself to call gtk_grab_add() + gdk_device_grab() (see
// wxWidgets src/common/popupcmn.cpp, wxPopupTransientWindow::Show). While that
// grab is active GTK routes all pointer events of the whole application to the
// popup and gtk_popup_button_press() swallows clicks outside it, which made the
// slice button unclickable on Linux whenever this popup was visible. Plain
// wxPopupWindow::Show() takes no grab, so the button keeps working.
class SliceModePopup : public wxPopupWindow
{
public:
    explicit SliceModePopup(wxWindow *parent);

    // Shows the popup below |align_to| with right edges aligned. The popup stays
    // open while the mouse is over it or over any window in |anchors|.
    void ShowFor(const std::vector<wxWindow*> &anchors, wxWindow *align_to);

    void HidePopup();

    ~SliceModePopup();

private:
    void on_paint(wxPaintEvent &evt);
    void on_mouse_move(wxMouseEvent &evt);
    void on_left_up(wxMouseEvent &evt);
    void on_timer(wxTimerEvent &evt);
    void update_metrics();
#ifdef __WXGTK__
    // The popup is an override-redirect window that stays on top of everything,
    // so hide it when the application loses activation (mirrors Widgets/PopupWindow).
    void on_top_window_activate(wxActivateEvent &evt);
#endif

    std::vector<wxWindow*> m_anchors;
    wxTimer                m_timer;
    wxRect                 m_std_rect;              // "Standard Mode" row, client coords
    wxRect                 m_custom_rect;           // "Custom Mode" row, client coords
    int                    m_hovered_row { -1 };    // 0 = standard, 1 = custom
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_SliceModePopup_hpp_
