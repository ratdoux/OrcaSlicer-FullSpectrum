#ifndef slic3r_GUI_MFDRatioAccordion_hpp_
#define slic3r_GUI_MFDRatioAccordion_hpp_

// MFDRatioAccordion: Collapsible "Select Ratio" panel for the Mixed Filament Dialog.
//
// Responsibility:
//   Provides the fully interactive ratio-picker widget for 2-, 3-, and 4-filament
//   blends, together with the Min Weight Ratio slider/input that constrains the
//   allowable blend extremes. This class fully replaces the old
//   MixedFilamentRatioPanel standalone widget.
//
// Why inlined (not a wrapper around MixedFilamentRatioPanel)?
//   MixedFilamentRatioPanel was a self-contained wxPanel subclass with no concept
//   of a collapsible header or Min-Weight controls. Adding accordion behavior on
//   top of it would have required awkward bridging. By integrating the ratio-picker
//   painting and interaction logic directly inside this accordion subclass we:
//     a) Eliminate an extra class that had no value outside this dialog.
//     b) Keep all ratio-related state (drawing cache, dragging, min_weight controls)
//        in one well-named class.
//     c) Avoid duplicate include paths between Widgets/ and GUI/.
//
// Barycentric math / TriPoint:
//   TriPoint is the same simple 2D point used for barycentric coordinate
//   operations on the triangle (3-filament) and quad (4-filament) shapes.
//   The static helpers (barycentric, project_to_segment, project_to_triangle)
//   mirror those of the old panel and are kept static for testability.
//
// Data flow:
//   User drags within the canvas  -->  on_weights_changed callback
//   Dialog calls update_filaments()  -->  panel repaints
//   Dialog calls get/set_min_weight_ratio()  -->  overlay updates

#include "Widgets/Accordion.hpp"
#include <functional>
#include <vector>
#include <tuple>

namespace Slic3r::GUI {

class MFDRatioAccordion : public Accordion
{
public:
    MFDRatioAccordion(wxWindow* parent,
                      std::vector<double>&  filament_weights,
                      std::vector<wxColor>& filament_colors,
                      double&               min_weight_ratio);
    ~MFDRatioAccordion() override = default;

    // Fired whenever the user drags the handle and the weights change.
    void set_on_weights_changed(std::function<void()> cb) { m_on_weights_changed = std::move(cb); }

    // Called when the number of filaments or their colors change.
    void update_sizing();

    double get_min_weight_ratio() const { return m_min_weight_ratio; }
    void   set_min_weight_ratio(double v) { m_min_weight_ratio = v; }

    // ----------------------------------------------------------------
    // Clamp helpers (static so they can be called from the dialog too)
    // ----------------------------------------------------------------
    static void clamp_weights_2(std::vector<double>& weights, double min_weight_ratio);
    static void clamp_weights_3(std::vector<double>& weights, double min_weight_ratio);
    static void clamp_weights_4(std::vector<double>& weights, double min_weight_ratio);

private:
    void build_ui();
    void build_ratio_canvas();
    void build_min_weight_row();

    // --- Mouse events on the ratio canvas ---
    void on_canvas_paint(wxPaintEvent& event);
    void on_canvas_left_down(wxMouseEvent& event);
    void on_canvas_left_up(wxMouseEvent& event);
    void on_canvas_motion(wxMouseEvent& event);
    void update_weights_from_mouse(int x, int y);
    void update_weights_from_mouse_2(int x);
    void update_weights_from_mouse_3(int x, int y);
    void update_weights_from_mouse_4(int x, int y);

    // --- Painting ---
    void paint_2(wxGraphicsContext& gc, const wxSize& size);
    void paint_2_handle(wxGraphicsContext& gc, const wxSize& size);
    void paint_3(wxGraphicsContext& gc, const wxSize& size);
    void paint_3_handle(wxGraphicsContext& gc, const wxSize& size);
    void paint_4(wxGraphicsContext& gc, const wxSize& size);
    void paint_4_handle(wxGraphicsContext& gc, const wxSize& size);
    void paint_min_ratio_overlay(wxGraphicsContext& gc, const wxSize& size);

    // --- Geometry helpers ---
    std::tuple<struct TriPoint, struct TriPoint, struct TriPoint> triangle_vertices() const;
    std::tuple<struct TriPoint, struct TriPoint, struct TriPoint, struct TriPoint> quad_vertices() const;

    // --- Color helpers ---
    wxColour get_contrast_border_color(const wxColour& bg) const;
    wxColour get_border_color()     const;
    wxColour get_background_color() const;
    double   get_margin()           const { return FromDIP(12); }
    double   get_border_width()     const { return FromDIP(2); }
    double   get_handle_radius()    const { return m_dragging ? FromDIP(10) : FromDIP(6); }

    // --- References to dialog-owned state ---
    std::vector<double>&  m_filament_weights;
    std::vector<wxColor>& m_filament_colors;
    double&               m_min_weight_ratio;

    // --- Canvas ---
    wxPanel* m_canvas{nullptr};

    // --- Min weight controls ---
    wxPanel*        m_min_weight_panel{nullptr};
    wxSlider*       m_min_weight_slider{nullptr};
    wxTextCtrl*     m_min_weight_value_input{nullptr};

    // --- Drag state ---
    bool m_dragging{false};

    // --- Bitmap caches (avoid recomputing barycentric gradient every frame) ---
    wxBitmap            m_cached_background_3;
    wxBitmap            m_cached_background_4;
    wxBitmap            m_cached_min_weight_ratio_overlay;
    std::vector<wxColor> m_last_colors;
    double              m_last_min_weight_ratio{-1.0};
    int                 m_last_count{-1};

    std::function<void()> m_on_weights_changed;
};

// Simple 2D point struct used for barycentric coordinate calculations.
struct TriPoint {
    double x, y;

    // Signed area of triangle (a, b, c) * 2  -- used for barycentric coords.
    static double signed_area2(TriPoint a, TriPoint b, TriPoint c);
    // Barycentric weights of point p inside triangle (v0, v1, v2).
    static void   barycentric(TriPoint p, TriPoint v0, TriPoint v1, TriPoint v2,
                               double& w0, double& w1, double& w2);
    static double    dot(TriPoint a, TriPoint b);
    static TriPoint  project_to_segment(TriPoint p, TriPoint a, TriPoint b);
    static TriPoint  project_to_triangle(TriPoint p, TriPoint a, TriPoint b, TriPoint c);
};

} // namespace Slic3r::GUI

#endif // slic3r_GUI_MFDRatioAccordion_hpp_
