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
    MixedFilamentRatioPanel(wxWindow* parent, std::vector<double>& filament_weights, std::vector<wxColor>& filament_colors); 
    
    std::vector<double>& m_filament_weights; // 0...1 (e.g. 0.5 for 50%)
    std::vector<wxColor>& m_filament_colors; 

private:
    void on_paint(wxPaintEvent&);
    void on_left_down(wxMouseEvent&);
    void on_left_up(wxMouseEvent&);
    void on_motion(wxMouseEvent&);

    void update_from_mouse(int x, int y);
    void update_from_mouse_2(int x);
    void update_from_mouse_3(int x, int y);
    void update_from_mouse_4(int x, int y);

    void paint_2(wxDC& context, const wxSize& size, std::vector<wxColor>& colors, std::vector<double>& weights);
    void paint_3(wxDC& context, const wxSize& size, std::vector<wxColor>& colors, std::vector<double>& weights);
    void paint_4(wxDC& context, const wxSize& size, std::vector<wxColor>& colors, std::vector<double>& weights);

    std::tuple<MixedFilamentRatioPanel::TriPoint, MixedFilamentRatioPanel::TriPoint, MixedFilamentRatioPanel::TriPoint> triangle_vertices()
        const;



    bool m_dragging{false};

    wxBitmap             m_cached_background_3;
    wxBitmap             m_cached_background_4;

    wxSize               m_last_size;
    std::vector<wxColor> m_last_colors;
};


} // namespace Slic3r::GUI
#endif