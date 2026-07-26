#ifndef slic3r_GUI_SidebarFilamentMenu_hpp_
#define slic3r_GUI_SidebarFilamentMenu_hpp_

#include <wx/wx.h>
#include <functional>
#include <vector>
#include <string>
#include <map>
#include "Widgets/FilamentCard.hpp"
#include "Widgets/FilamentCardMixed.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/StaticBox.hpp"
#include "libslic3r/MixedFilament.hpp"


namespace Slic3r::GUI {

class SidebarFilamentMenu : public ::wxPanel
{
public:
    enum class ActionType { AddFilament, DeleteFilament, SyncAMS, Settings, FlushingVolumes, CollapseToggle };

    SidebarFilamentMenu(wxWindow* parent, const wxColour& title_bg);

    void on_filaments_change(size_t physical_count, std::vector<MixedFilamentDefinition>& mixed_filaments);

    // Intent Callbacks
    void set_on_action(ActionType type, std::function<void()> cb)
    {
        m_callbacks[type] = std::move(cb);
    }

    // State Setters
    void update_title(const wxString& label, const std::string& icon_name);
    void toggle_collapse(bool only_open);
    void show_SEMM_buttons(bool is_single_extruder_multi_material);
    void show_AMS_button(bool show);

    // Physical specific
    int m_physical_count() const { return static_cast<int>(m_physical_cards.size()); }
    void update_physical_states();
    bool switch_to_tab(int index);
    wxPoint get_edit_btn_client_position(int index);
    void set_on_edit_physical(std::function<void(int)> cb)
    {
        m_on_edit_physical = std::move(cb);
    }
    void set_on_right_click_physical(std::function<void(int, const wxPoint&)> cb)
    {
        m_on_right_click_physical = std::move(cb);
    }
    void set_on_edit_mixed(std::function<void(int, bool)> cb)
    {
        m_on_edit_mixed = std::move(cb);
    }
    void set_on_delete_mixed(std::function<void(int)> cb)
    {
        m_on_delete_mixed = std::move(cb);
    }
    void set_on_delete_image_map(std::function<void(int)> cb)
    {
        m_on_delete_image_map = std::move(cb);
    }
    void set_get_physical_filaments(std::function<std::vector<std::pair<std::string, std::string>>()> cb)
    {
        m_get_physical_filaments = std::move(cb);
    }
    const std::vector<std::pair<std::string, std::string>>& get_physical_filaments() const { return m_physical_filaments; }

    // Mixed specific
    int m_mixed_count() const { return static_cast<int>(m_mixed_cards.size()); }
    void update_mixed_states(std::vector<MixedFilamentDefinition>& mixed_filaments);
    void refresh_mixed_color_previews();
    void refresh_image_map_entries();
    void edit_mixed_filament(int index, bool edit_by_color);
    void delete_mixed_filament(int index);
    void show_mixed_filament_menu(int index, const wxPoint& screen_pos, wxWindow* anchor);

    // UI 
    void msw_rescale();
    void sys_color_changed();

private:
    const int m_scrollbar_threshold = FromDIP(120); // height in pixels after which scrollbar appears

    std::vector<MixedFilamentDefinition> m_mixed_filaments{};
    std::vector<FilamentCardPhysical*>   m_physical_cards;
    std::vector<FilamentCardMixed*>      m_mixed_cards;
    std::vector<size_t>                  m_mixed_definition_indices;
    std::vector<FilamentCardImageMap*>   m_image_map_cards;
    std::vector<unsigned int>            m_image_map_adaptive_filament_ids;
    unsigned int                         m_selected_adaptive_filament_id{0};
    size_t                               m_image_map_entries_signature{0};
    bool                                 m_image_map_entries_signature_valid{false};
    
    void build_ui(const wxColour& title_bg);

    void on_physical_change(size_t physical_count);
    void on_mixed_change(std::vector<MixedFilamentDefinition>& mixed_filaments);
    void rebuild_mixed_cards(const std::vector<MixedFilamentDefinition>& mixed_filaments);
    void begin_mixed_card_callback();
    void end_mixed_card_callback();
    void schedule_pending_mixed_rebuild();
    void set_adaptive_cycle_highlight(unsigned int filament_id);

    int                                  m_mixed_card_callback_depth{0};
    bool                                 m_mixed_rebuild_pending{false};
    bool                                 m_mixed_rebuild_scheduled{false};
    std::vector<MixedFilamentDefinition> m_pending_mixed_filaments;
    
    std::function<std::vector<std::pair<std::string, std::string>>()>   m_get_physical_filaments;
    void                                        update_physical_filaments();
    // filament <name, color>
    std::vector<std::pair<std::string, std::string>> m_physical_filaments;

    std::map<ActionType, std::function<void()>> m_callbacks;
    std::function<void(int)>                    m_on_edit_physical; // Callback for edit button in physical filament card
    std::function<void(int, const wxPoint&)>    m_on_right_click_physical; // Callback for right-click in physical filament card
    std::function<void(int, bool)>              m_on_edit_mixed;
    std::function<void(int)>                    m_on_delete_mixed;
    std::function<void(int)>                    m_on_delete_image_map;

    // Drag handling for physical and mixed panels
    struct DragState
    {
        bool is_dragging   = false;
        int  drag_start_y  = 0;
        int  panel_start_height = 0;
        int  max_content_height = 0; 
        std::chrono::steady_clock::time_point last_layout_time;
    };

    void start_drag(const wxMouseEvent& event, DragState* drag_state, wxPanel* grap_panel, wxScrolledWindow* panel);
    void on_drag(const wxMouseEvent& event, DragState* drag_state, wxScrolledWindow* panel, wxPanel* parent_panel, wxPanel* grab_panel);
    void end_drag(const wxMouseEvent& event, DragState* drag_state, wxPanel* grap_panel);
    void update_grab_panel_visibility(wxScrolledWindow* panel, wxPanel* parent_panel, wxPanel* grab_panel, int max_content_height);
    void set_grab_panel_line_thickness(wxPanel* grab_panel, int thickness);
    
    int m_physical_grab_line_thickness = 1;
    int m_mixed_grab_line_thickness = 1;

    DragState* m_physical_drag_state = new DragState();
    DragState* m_mixed_drag_state = new DragState();

    // Main panels and sizers
    StaticBox*          m_title_panel{nullptr};
    wxPanel*            m_content_panel{nullptr};
    wxPanel*            m_physical_title_panel{nullptr};
    wxScrolledWindow*   m_physical_panel{nullptr};
    wxPanel*            m_physical_grab_panel{nullptr}; 
    ScalableButton*     m_btn_mixed_add_big{nullptr}; // only shown when no mixed filaments are present
    wxPanel*            m_mixed_title_panel{nullptr};
    wxScrolledWindow*   m_mixed_panel{nullptr};
    wxPanel*            m_mixed_grab_panel{nullptr}; 

    wxBoxSizer*         m_main_sizer{nullptr};
    wxBoxSizer*         m_title_sizer{nullptr};
    wxBoxSizer*         m_content_sizer{nullptr};
    wxBoxSizer*         m_physical_title_sizer{nullptr};
    wxGridSizer*        m_physical_sizer{nullptr};
    wxBoxSizer*         m_mixed_title_sizer{nullptr};
    wxBoxSizer*         m_image_map_sizer{nullptr};
    wxGridSizer*        m_mixed_sizer{nullptr};

    // Title bar elements
    ScalableButton* m_btn_icon{nullptr};
    Label*          m_lbl_title{nullptr};
    ScalableButton* m_btn_settings{nullptr};
    Button*         m_btn_flushing{nullptr};

    // Physical Title panel elements
    wxStaticText*   m_lbl_physical_title{nullptr};
    wxStaticText*   m_lbl_physical_counter{nullptr};
    wxPanel*        m_physical_divider{nullptr};
    ScalableButton* m_btn_physical_del{nullptr};
    ScalableButton* m_btn_physical_add{nullptr};
    ScalableButton* m_btn_ams{nullptr};

    // Mixed Title panel elements
    wxStaticText*   m_lbl_mixed_title{nullptr};
    wxStaticText*   m_lbl_mixed_counter{nullptr};
    wxPanel*        m_mixed_divider{nullptr};
    wxChoice*       m_choice_mixed_color_engine{nullptr};
    wxCheckBox*     m_check_mixed_use_td{nullptr};
    Button*         m_btn_mixed_manage{nullptr};
    ScalableButton* m_btn_mixed_del{nullptr};
    ScalableButton* m_btn_mixed_add{nullptr};

    wxSizerItem*    m_mixed_list_bottom_spacer{nullptr};
};

} // namespace Slic3r::GUI

#endif
