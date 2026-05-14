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

private:
    FilamentCardMixedData m_data;

    std::function<void()> m_on_box_edit;

    void build_ui();

    wxBoxSizer*     m_main_sizer{nullptr};

    wxPanel*        m_clr_swatch_panel{nullptr};
    wxPanel*        m_box_panel{nullptr};
    ScalableButton* m_filament_edit_btn{nullptr};

    bool            m_is_box_panel_hovered = false;

    void paint_clr_swatch(
        wxPaintEvent& event, 
        wxColor color, 
        wxString index_text, 
        bool is_dark
    );
    void paint_box_mix(
        wxPaintEvent&         event,
        std::vector<int>      percentages,
        std::vector<wxColor>  colors,
        std::vector<wxString> index_texts,
        bool                  is_dark,
        wxSize                swatch_size
    );

    
    void paint_box_pattern(
        wxPaintEvent&         event,
        std::vector<wxColor>  colors,
        std::vector<wxString> index_texts,
        bool                  is_dark,
        wxSize                swatch_size);
};

} // namespace Slic3r::GUI

#endif
