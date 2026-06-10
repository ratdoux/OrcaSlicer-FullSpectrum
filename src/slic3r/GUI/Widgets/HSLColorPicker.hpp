#ifndef slic3r_GUI_HSLColorPicker_hpp_
#define slic3r_GUI_HSLColorPicker_hpp_

#include <functional>
#include <wx/wx.h>

#include "Label.hpp"
#include "Slic3r/GUI/GUI.hpp"
#include "Slic3r/GUI/GUI_App.hpp"
#include "Slic3r/GUI/GUI_Utils.hpp"

namespace Slic3r::GUI {

/// Affinity/Photoshop-style HSL color wheel with hex input and color preview.
///
/// Layout:
///   ┌──────────────────────────────┐
///   │     Outer Hue Ring           │
///   │   ┌──────────────────┐       │
///   │   │  Inner S/L       │       │
///   │   │  Square          │
///   │   └──────────────────┘       │
///   └──────────────────────────────┘
///   ┌─────────────┬────────────────┐
///   │  #RRGGBB    │  [color swatch]│
///   └─────────────┴────────────────┘
///
/// The square is inscribed inside the hue ring, matching Photoshop behavior.
class HSLColorPicker : public wxPanel
{
public:
    HSLColorPicker(wxWindow*                            parent,
                   const wxColour&                      initial_color   = *wxRED,
                   std::function<void(const wxColour&, bool)> on_color_changed = nullptr);

    wxColour GetColor() const;
    void     SetColor(const wxColour& color);

    // ── HSL ↔ RGB helpers ──────────────
    static wxColour hsl_to_rgb(double h, double s, double l);
    static void     rgb_to_hsl(const wxColour& c, double& h, double& s, double& l);
    static wxColour hex_to_colour(const wxString& hex);
    static wxString colour_to_hex(const wxColour& c);

private:
    // ── HSL state ────────────────────────────────────────────────────
    double m_hue{0.0};        // 0..360
    double m_saturation{1.0}; // 0..1
    double m_lightness{0.5};  // 0..1

    // ── Drag tracking ────────────────────────────────────────────────
    enum class DragTarget { None, HueRing, Square };
    DragTarget m_drag_target{DragTarget::None};
    bool       m_dragging{false};

    // ── Cached bitmaps ───────────────────────────────────────────────
    wxBitmap m_cached_hue_ring;
    wxBitmap m_cached_square;
    double   m_cached_hue_for_square{-1.0};
    wxSize   m_cached_hue_ring_size;
    wxSize   m_cached_square_size;

    // ── UI sub-elements ──────────────────────────────────────────────
    wxPanel*    m_wheel_panel{nullptr};   // custom-painted wheel area
    wxTextCtrl* m_hex_input{nullptr};
    wxPanel*    m_color_preview{nullptr};

    // ── Callback ─────────────────────────────────────────────────────
    std::function<void(const wxColour&, bool)> m_on_color_changed;

    // ── Style helpers  ─────────────
    double   get_ring_width() const  { return m_wheel_panel->FromDIP(20); }
    double   get_hue_handle_radius() const { return (m_dragging && m_drag_target == DragTarget::HueRing) ? m_wheel_panel->FromDIP(8) : m_wheel_panel->FromDIP(5); }
    double   get_square_handle_radius() const { return (m_dragging && m_drag_target == DragTarget::Square) ? m_wheel_panel->FromDIP(8) : m_wheel_panel->FromDIP(5); }
    double   get_border_width() const  { return m_wheel_panel->FromDIP(2); }
    wxColor  get_border_color() const  { return StateColor::darkModeColorFor(wxColour("#BDBDBD")); }
    wxColour get_contrast_border_color(const wxColour& bg_color) const;

    // ── Geometry helpers ─────────────────────────────────────────────
    struct WheelGeometry
    {
        double cx, cy;         // center of the wheel
        double outer_radius;   // outer edge of hue ring
        double inner_radius;   // inner edge of hue ring (= outer - ring_width)
    };
    WheelGeometry get_wheel_geometry() const;

    // ── Painting ─────────────────────────────────────────────────────
    void on_wheel_paint(wxPaintEvent&);
    void paint_hue_ring(wxGraphicsContext& gc, const WheelGeometry& geom, const wxSize& size);
    void paint_square(wxGraphicsContext& gc, const WheelGeometry& geom, const wxSize& size);
    void paint_hue_handle(wxGraphicsContext& gc, const WheelGeometry& geom);
    void paint_square_handle(wxGraphicsContext& gc, const WheelGeometry& geom);

    // ── Mouse interaction ────────────────────────────────────────────
    void on_left_down(wxMouseEvent&);
    void on_left_up(wxMouseEvent&);
    void on_motion(wxMouseEvent&);

    DragTarget hit_test(int x, int y) const;
    void       update_hue_from_mouse(int x, int y);
    void       update_sl_from_mouse(int x, int y);

    // ── Internal sync ────────────────────────────────────────────────
    void update_from_hsl();
    void update_hex_input();
    void sync_preview_color();
    void on_hex_input_changed(wxCommandEvent&);
    void on_hex_input_focus_lost(wxFocusEvent&);

    bool m_updating_hex{false}; // guard against circular updates
};

} // namespace Slic3r::GUI
#endif
