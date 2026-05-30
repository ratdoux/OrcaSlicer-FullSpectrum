#ifndef slic3r_GUI_MixedFilamentDialog_hpp_
#define slic3r_GUI_MixedFilamentDialog_hpp_

#include <memory> 
#include <wx/wx.h>

#include "Widgets/Label.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"
#include "GUI_Utils.hpp"

namespace Slic3r::GUI {


class MixedFilamentDialog : public DPIDialog
{
public:
    enum class Action { Add, Edit };
    enum class Tab { Mix, Pattern };
 
    MixedFilamentDialog(wxWindow* parent, Action action, std::vector<std::pair<std::string, std::string>>& physical_filaments);
   
 


protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    Action m_action{Action::Add};
    Tab    m_current_tab{Tab::Mix};

    int max_filament = 4; // mix 2-4 physical filaments
    const int min_filament = 2;

    int m_width_fixed;
    int m_height_start;
    int m_height_min;
    int m_clr_swatch_size;
    std::vector<std::pair<std::string, std::string>>& m_physical_filaments;

    std::vector<int>    m_selected_filaments;
    std::vector<double>  m_selected_filaments_weights; // 0...1 (e.g. 0.5 for 50%)
    std::vector<wxColor> m_selected_filaments_colors;

    void build_ui(wxWindow* parent);

    wxColour getTabBorderColor(bool is_selected, bool is_hovered) const;
    wxColour getTabBackgroundColor(bool is_selected, bool is_hovered) const;
    wxColour getTabTextColor(bool is_selected, bool is_hovered) const;
    void     paintTabBtn(
        wxPanel* panel,
        bool     round_left,
        bool     round_right,
        double   radius,
        wxString label,
        wxString icon_name ,
        bool     is_selected,
        bool     is_hovered);
    void update_tabs();
    bool m_mix_tab_hovered = false;
    bool m_pattern_tab_hovered = false;

    // fallback, when MinSize/MaxSize constraints are not sufficient, 
    // restrict to only vertical resizing with a minimum height
    void on_sizing(wxSizeEvent& event);

    // content is filled by m_physical_filaments
    void add_material_combobox(wxPanel* parent, wxBoxSizer* sizer);
    void remove_material_combobox();
    void on_selected_filaments_changed(int index);
    int  find_first_free_filament() const;
    void refresh_material_combobox_items();

    std::vector<double> get_default_weights(int filament_count);
    std::vector<wxColor> get_selected_filaments_colors(const std::vector<int>& filament_indices) const;


    // UI 
    wxPanel* m_title_panel{nullptr};
    wxPanel* m_content_panel{nullptr};
    wxPanel* m_footer_panel{nullptr};

    wxBoxSizer* m_main_sizer{nullptr};
    wxBoxSizer* m_title_sizer{nullptr};
    wxBoxSizer* m_content_sizer{nullptr};
    wxBoxSizer* m_footer_sizer{nullptr};

    // Title
    wxPanel*        m_mix_tab_btn;
    wxPanel*        m_pattern_tab_btn;

    // Content
    wxPanel*                m_material_panel;
    wxPanel*                m_material_title_panel;
    wxStaticText*           m_material_title_text;
    ScalableButton*         m_add_material_btn;
    ScalableButton*         m_delete_material_btn;
    wxPanel*                m_material_combobox_panel;
    std::vector<ComboBox*>  m_material_comboboxes;

    wxBoxSizer* m_material_sizer;
    wxBoxSizer* m_material_title_sizer;
    wxBoxSizer* m_material_combobox_sizer;

    wxPanel*    m_mix_ratio_panel;
    wxBoxSizer* m_mix_ratio_sizer;

    // Footer
    
};


} // namespace Slic3r::GUI
#endif