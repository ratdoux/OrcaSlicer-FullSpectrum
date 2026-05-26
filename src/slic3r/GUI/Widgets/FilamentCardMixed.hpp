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

struct FilamentCardMixedData
{
    MixedFilamentDefinition* definition;

    // same order as components in definition, used for gradient preview and tooltip generation
    std::vector<std::string> physical_component_colors = {};

    wxString tooltip = wxString();

    FilamentCardMixedData(MixedFilamentDefinition* definition) : definition(definition) {}
};

class FilamentCardMixed : public wxPanel
{
public:
    FilamentCardMixed(wxWindow* parent, MixedFilamentDefinition* definition);

    void set_on_box_edit_callback(std::function<void()> callback) 
    { 
        m_on_box_edit = std::move(callback); 
    }

    void update_state(MixedFilamentDefinition* definition);

    // TODO refactor other paint functions to take in DC too (->Backgroundcolor!)
    static void paint_clr_swatch(
        wxDC&           context, 
        const wxSize&   size, 
        wxColor&        color, 
        wxString&       index_text, 
        bool            is_dark
    );

    static void paint_box_mix(
        wxPanel&               panel,
        wxPaintEvent&          event,
        std::vector<int>&      percentages,
        std::vector<wxColor>&  colors,
        std::vector<wxString>& index_texts,
        bool                   is_dark,
        bool                   is_hovered,
        wxSize&                swatch_size
    );

    static void paint_box_pattern(
        wxPanel&               panel,
        wxPaintEvent&          event,
        std::vector<wxColor>&  colors,
        std::vector<wxString>& index_texts,
        bool                   is_dark,
        bool                   is_hovered,
        wxSize&                swatch_size);

private:
    FilamentCardMixedData m_data;

    std::function<void()> m_on_box_edit;

    void build_ui();

    wxBoxSizer*     m_main_sizer{nullptr};

    wxPanel*        m_clr_swatch_panel{nullptr};
    wxPanel*        m_box_panel{nullptr};
    ScalableButton* m_filament_edit_btn{nullptr};

    bool            m_is_box_panel_hovered = false;

};

} // namespace Slic3r::GUI

#endif
