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

    void set_on_box_edit_callback(std::function<void()> callback) 
    { 
        m_on_box_edit = std::move(callback); 
    }

    void update_state(MixedFilamentDefinition* definition, bool refresh);

    // TODO refactor other paint functions to take in DC too (->Backgroundcolor!)
    static void paint_clr_swatch(
        wxDC&           context, 
        const wxSize&   size, 
        wxColor&        color, 
        wxString&       index_text, 
        bool            is_dark,
        int             padding = 0
    );

    static void paint_box_mix(
        wxDC&                       context, 
        const wxSize&               size, 
        const wxColor&              background_color,
        std::vector<unsigned int>   indices,
        std::vector<int>&           percentages,
        std::vector<wxColor>&       colors,
        bool                        is_dark,
        bool                        is_hovered,
        wxSize&                     swatch_size
    );

    static void paint_box_pattern(
        wxDC&                       context, 
        const wxSize&               size, 
        const wxColor&              background_color,
        std::vector<unsigned int>   indices,
        std::vector<wxColor>&       colors,
        bool                        is_dark,
        bool                        is_hovered,
        wxSize&                     swatch_size);

    static void paint_box_gradient(
        wxDC&                       context,
        const wxSize&               size,
        const wxColor&              background_color,
        const std::vector<wxColor>& colors,
        const std::vector<double>&  positions,
        const std::vector<unsigned int>& indices,
        bool                        is_dark,
        bool                        is_hovered,
        wxSize&                     swatch_size);

private:
    MixedFilamentDefinition* m_definition;
    std::vector<std::pair<std::string, std::string>>& m_physical_filaments;
    wxString m_tooltip = wxString();

    std::vector<unsigned int>   m_physical_filaments_indices;     // 1-based, calculated in update_state()
    std::vector<wxColor>        m_physical_filaments_colors;      // calculated in update_state() using get_physical_filaments_colors()
    std::vector<int>            m_physical_filaments_percentages; // calculated in update_state() 

    std::function<void()> m_on_box_edit;

    void build_ui();
    std::vector<wxColor> get_physical_filaments_colors(const std::vector<unsigned int>& filament_indices) const;

    wxBoxSizer*     m_main_sizer{nullptr};

    wxPanel*        m_clr_swatch_panel{nullptr};
    wxPanel*        m_box_panel{nullptr};
    ScalableButton* m_filament_edit_btn{nullptr};

    bool            m_is_box_panel_hovered = false;

};

} // namespace Slic3r::GUI

#endif
