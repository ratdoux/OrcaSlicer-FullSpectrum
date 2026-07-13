#ifndef slic3r_GUI_FilamentCardMixed_hpp_
#define slic3r_GUI_FilamentCardMixed_hpp_

#include <wx/wx.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <functional>
#include <string>
#include <algorithm>

#include "libslic3r/MixedFilament.hpp"
#include "Button.hpp"


namespace Slic3r::GUI {

class FilamentCardMixed : public wxPanel
{
public:
    FilamentCardMixed(wxWindow* parent, MixedFilamentDefinition* definition, std::vector<std::pair<std::string, std::string>>& physical_filaments);

    void set_on_box_edit_callback(std::function<void(bool edit_by_color)> callback) 
    { 
        m_on_box_edit = std::move(callback); 
    }

    void set_on_right_click_callback(std::function<void(const wxPoint& screen_pos)> callback)
    {
        m_on_right_click = std::move(callback);
    }

    void set_on_edit_btn_callback(std::function<void(wxWindow* anchor)> callback)
    {
        m_on_edit_btn = std::move(callback);
    }

    void set_dialog_open(bool open)
    {
        m_is_dialog_open = open;
        if (m_box_panel)
            m_box_panel->Refresh();
    }

    void update_state(MixedFilamentDefinition* definition, bool refresh);

    static void paint_clr_swatch(
        wxDC&           context, 
        const wxSize&   size, 
        const wxColor&  color, 
        const wxString& index_text, 
        bool            is_dark,
        int             padding = 0
    );

    static void paint_clr_swatch_gradient(
        wxDC&                       context,
        const wxSize&               size,
        const std::vector<wxColor>& colors,
        const wxString&             text,
        bool                        is_dark,
        int                         padding = 0
    );

    static void paint_box_mix(
        wxDC&                       context, 
        const wxSize&               size, 
        const wxColor&              background_color,
        const std::vector<unsigned int>& indices,
        const std::vector<int>&     percentages,
        const std::vector<wxColor>& colors,
        bool                        is_dark,
        bool                        is_hovered,
        const wxSize&               swatch_size
    );

    static void paint_box_pattern(
        wxDC&                       context, 
        const wxSize&               size, 
        const wxColor&              background_color,
        const std::vector<unsigned int>& indices,
        const std::vector<wxColor>& colors,
        bool                        is_dark,
        bool                        is_hovered,
        const wxSize&               swatch_size);

    static void paint_box_gradient(
        wxDC&                       context,
        const wxSize&               size,
        const wxColor&              background_color,
        const std::vector<wxColor>& colors,
        const std::vector<double>&  component_positions,
        const std::vector<unsigned int>& indices,
        bool                        is_dark,
        bool                        is_hovered,
        const wxSize&               swatch_size);

private:
    MixedFilamentDefinition* m_definition;
    std::vector<std::pair<std::string, std::string>>& m_physical_filaments;
    wxString m_tooltip = wxString();

    std::vector<unsigned int>   m_physical_filaments_indices;     // 1-based, calculated in update_state()
    std::vector<wxColor>        m_physical_filaments_colors;      // calculated in update_state() using get_physical_filaments_colors()
    std::vector<int>            m_physical_filaments_percentages; // calculated in update_state() 
    std::vector<wxColor>        m_gradient_preview_colors;
    std::vector<double>         m_gradient_component_positions;
    std::vector<unsigned int>   m_gradient_component_ids;

    std::function<void(bool edit_by_color)> m_on_box_edit;
    std::function<void(const wxPoint& screen_pos)> m_on_right_click;
    std::function<void(wxWindow* anchor)> m_on_edit_btn;
    bool m_is_dialog_open = false;

    void build_ui();
    std::vector<wxColor> get_physical_filaments_colors(const std::vector<unsigned int>& filament_indices) const;
    wxString display_id_text() const;
    wxSize color_swatch_size_for_text(const wxString& text) const;
    void update_color_swatch_size();

    wxBoxSizer*     m_main_sizer{nullptr};

    wxPanel*        m_clr_swatch_panel{nullptr};
    wxPanel*        m_box_panel{nullptr};
    ScalableButton* m_filament_edit_btn{nullptr};

    bool            m_is_box_panel_hovered = false;

};

} // namespace Slic3r::GUI

#endif
