#ifndef slic3r_GUI_MFDGradientAccordion_hpp_
#define slic3r_GUI_MFDGradientAccordion_hpp_

#include "Widgets/Accordion.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "Widgets/ComboBox.hpp"
#include <functional>
#include <vector>
#include <string>
#include <utility>

namespace Slic3r::GUI {

class MFDGradientAccordion : public Accordion
{
public:
    MFDGradientAccordion(
        wxWindow* parent,
        std::vector<int>& selected_filaments,
        std::vector<wxColor>& filament_colors,
        std::vector<double>& gradient_positions,
        double& min_ratio,
        const std::vector<std::pair<std::string, std::string>>& physical_filaments
    );
    ~MFDGradientAccordion() override = default;

    void set_on_changed(std::function<void()> cb) { m_on_changed = std::move(cb); }
    void set_on_filament_changed(std::function<void(size_t, int)> cb) { m_on_filament_changed = std::move(cb); }

    void update_sizing();
    void sync_data();
    void reset_to_defaults();

private:
    void build_ui();
    void build_canvas();
    void build_edit_row();
    void build_min_ratio_row();

    void on_canvas_paint(wxPaintEvent& event);
    void on_canvas_left_down(wxMouseEvent& event);
    void on_canvas_left_up(wxMouseEvent& event);
    void on_canvas_motion(wxMouseEvent& event);
    void update_positions_from_mouse(int x);

    void sync_edit_panel();
    void update_percentage_input();
    void apply_text_input_change();
    void refresh_combobox_items();

    void clamp_all_stops();
    void reset_points_to_defaults(int count);

    // Color helpers
    wxColour get_contrast_border_color(const wxColour& bg) const;
    wxColour get_border_color()     const;
    wxColour get_background_color() const;
    double   get_margin()           const { return FromDIP(12); }
    double   get_border_width()     const { return FromDIP(2); }
    double   get_handle_radius(bool grabbed = false) const { return grabbed ? FromDIP(10) : FromDIP(7); }

    // References to dialog-owned state
    std::vector<int>&       m_selected_filaments;
    std::vector<wxColor>&   m_filament_colors;
    std::vector<double>&    m_gradient_positions;
    double&                 m_min_ratio;
    const std::vector<std::pair<std::string, std::string>>& m_physical_filaments;
    MixedFilamentDisplayContext                              m_display_context;

    // UI Controls
    wxPanel*        m_canvas{nullptr};
    wxPanel*        m_edit_panel{nullptr};
    ComboBox*       m_filament_combo{nullptr};
    wxStaticText*   m_pos_label{nullptr};
    wxTextCtrl*     m_pos_input{nullptr};
    wxStaticText*   m_pct_label{nullptr};

    wxPanel*        m_min_ratio_panel{nullptr};
    wxSlider*       m_min_ratio_slider{nullptr};
    wxTextCtrl*     m_min_ratio_value_input{nullptr};

    // Drag and selection state
    bool m_dragging{false};
    int  m_selected_stop_index{0}; // Index of the selected handle (0 to 2N-2)
    int  m_last_count{-1};

    // Callbacks
    std::function<void()> m_on_changed;
    std::function<void(size_t, int)> m_on_filament_changed;
};

} // namespace Slic3r::GUI

#endif // slic3r_GUI_MFDGradientAccordion_hpp_
