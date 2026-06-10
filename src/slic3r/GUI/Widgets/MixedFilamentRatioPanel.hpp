#ifndef slic3r_GUI_MixedFilamentRatioPanel_hpp_
#define slic3r_GUI_MixedFilamentRatioPanel_hpp_

#include <memory>
#include <wx/wx.h>

#include "Label.hpp"
#include "Button.hpp"
#include "ComboBox.hpp"
#include "Slic3r/GUI/GUI_Utils.hpp"

namespace Slic3r::GUI {

class MixedFilamentRatioPanel : public wxPanel
{
public:
    struct TriPoint
    {
        double x;
        double y;

        static double signed_area2(TriPoint a, TriPoint b, TriPoint c);

        static void barycentric(TriPoint p, TriPoint v0, TriPoint v1, TriPoint v2, double& w0, double& w1, double& w2);

        static double   dot(TriPoint a, TriPoint b);
        static TriPoint project_to_segment(TriPoint p, TriPoint a, TriPoint b);
        static TriPoint project_to_triangle(TriPoint p, TriPoint a, TriPoint b, TriPoint c);
    };

public:
    MixedFilamentRatioPanel(wxWindow*             parent,
                            std::vector<double>&  filament_weights,
                            std::vector<wxColor>& filament_colors,
                            double&               min_weight_ratio,
                            std::function<void()> on_weights_changed_callback); 
    
    std::vector<double>& m_filament_weights; // 0...1 (e.g. 0.5 for 50%)
    std::vector<wxColor>& m_filament_colors; 
    double& m_min_weight_ratio;

    void update_sizing();

    static void clamp_weights_2(std::vector<double>& weights, double min_weight_ratio);
    static void clamp_weights_3(std::vector<double>& weights, double min_weight_ratio);
    static void clamp_weights_4(std::vector<double>& weights, double min_weight_ratio);

private:

    std::function<void()> m_on_weights_changed_callback;

    // Shared UI Style Variables
    // margin is respected in paint to ensure the handle doesnt get cut off
    double   get_margin() const { return FromDIP(14); } // Enough to fit the 10px dragging handle + 2px border
    double   get_border_width() const { return FromDIP(2); }
    double   get_handle_radius() const { return m_dragging ? FromDIP(10) : FromDIP(6); }
    wxColor  get_border_color() const { return StateColor::darkModeColorFor(wxColour("#BDBDBD")); }
    wxColor  get_background_color() const { return StateColor::darkModeColorFor(*wxWHITE); }
    wxColour get_contrast_border_color(const wxColour& bg_color) const;

    void on_paint(wxPaintEvent&);
    void on_left_down(wxMouseEvent&);
    void on_left_up(wxMouseEvent&);
    void on_motion(wxMouseEvent&);

    void update_weights_from_mouse(int x, int y);
    
    void update_weights_from_mouse_2(int x);
    void paint_2(wxGraphicsContext& context, const wxSize& size, std::vector<wxColor>& colors, std::vector<double>& weights);
    void paint_2_handle(wxGraphicsContext& gc, const wxSize& size, const std::vector<wxColor>& colors, const std::vector<double>& weights);

    void update_weights_from_mouse_3(int x, int y);
    void paint_3(wxGraphicsContext& context, const wxSize& size, std::vector<wxColor>& colors, std::vector<double>& weights);
    void paint_3_handle(wxGraphicsContext& gc, const wxSize& size, const std::vector<wxColor>& colors, const std::vector<double>& weights);

    void update_weights_from_mouse_4(int x, int y);
    void paint_4(wxGraphicsContext& context, const wxSize& size, std::vector<wxColor>& colors, std::vector<double>& weights);
    void paint_4_handle(wxGraphicsContext& gc, const wxSize& size, const std::vector<wxColor>& colors, const std::vector<double>& weights);

    void paint_min_ratio_overlay(wxGraphicsContext& context, const wxSize& size);

    std::tuple<MixedFilamentRatioPanel::TriPoint, MixedFilamentRatioPanel::TriPoint, MixedFilamentRatioPanel::TriPoint> triangle_vertices()
        const;
    std::tuple<MixedFilamentRatioPanel::TriPoint, MixedFilamentRatioPanel::TriPoint, MixedFilamentRatioPanel::TriPoint, MixedFilamentRatioPanel::TriPoint> quad_vertices() const;


    bool m_dragging{false};

    std::vector<wxColor> m_last_colors;
    wxBitmap             m_cached_background_3;
    wxBitmap             m_cached_background_4;

    int                  m_last_count{0};
    double               m_last_min_weight_ratio{0.0};
    wxBitmap             m_cached_min_weight_ratio_overlay;
};


} // namespace Slic3r::GUI
#endif