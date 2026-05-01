#ifndef slic3r_GUI_SidebarFilamentMenu_hpp_
#define slic3r_GUI_SidebarFilamentMenu_hpp_

#include <wx/wx.h>
#include <functional>
#include <vector>
#include <string>
#include <map>
#include "Widgets/FilamentCard.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/StaticBox.hpp"


namespace Slic3r::GUI {

class SidebarFilamentMenu : public ::wxPanel
{
public:
    enum class ActionType { AddFilament, DeleteFilament, SyncAMS, Settings, FlushingVolumes, CollapseToggle };

    SidebarFilamentMenu(wxWindow* parent, const wxColour& title_bg);

    void on_filaments_change(size_t physical_count);

    // Intent Callbacks
    void set_on_action(ActionType type, std::function<void()> cb)
    {
        m_callbacks[type] = std::move(cb); 
    }
    void set_on_edit_physical(std::function<void(int)> cb)
    {
         m_on_edit_physical = std::move(cb);
    }
    wxPoint get_edit_btn_client_position(int index);
    bool    switch_to_tab(int index);

    // State Setters
    void update_title(const wxString& label, const std::string& icon_name);
    
    int m_physical_count() const { return static_cast<int>(m_physical_cards.size()); }
    void update_physical_states();

    void msw_rescale();
    void sys_color_changed();
    void toggle_collapse(bool only_open);
    void show_SEMM_buttons(bool is_single_extruder_multi_material);
    void show_AMS_button(bool show);

private:
    void build_ui(const wxColour& title_bg);

    std::vector<FilamentCardPhysical*> m_physical_cards;

    StaticBox* m_title_panel{nullptr};
    wxPanel*   m_content_panel{nullptr};
    wxPanel*   m_physical_panel{nullptr};
    wxPanel*   m_mixed_panel{nullptr};

    wxBoxSizer*  m_main_sizer{nullptr};
    wxBoxSizer*  m_title_sizer{nullptr};
    wxBoxSizer*  m_content_sizer{nullptr};
    wxGridSizer* m_physical_sizer{nullptr};
    wxGridSizer* m_mixed_sizer{nullptr};

    // Title bar elements
    ScalableButton* m_btn_icon{nullptr};
    Label*          m_lbl_title{nullptr};
    ScalableButton* m_btn_add{nullptr};
    ScalableButton* m_btn_del{nullptr};
    ScalableButton* m_btn_ams{nullptr};
    ScalableButton* m_btn_settings{nullptr};
    Button*         m_btn_flushing{nullptr};

    std::map<ActionType, std::function<void()>> m_callbacks;
    std::function<void(int)>                    m_on_edit_physical; // Callback for edit button in physical filament card

};

} // namespace Slic3r::GUI

#endif
