#ifndef slic3r_GUI_MixedFilamentDialog_hpp_
#define slic3r_GUI_MixedFilamentDialog_hpp_

#include <memory> 
#include <wx/wx.h>

#include "Widgets/Label.hpp"
#include "Widgets/Button.hpp"
#include "GUI_Utils.hpp"

namespace Slic3r::GUI {


class MixedFilamentDialog : public DPIDialog
{
public:
    enum class Action { Add, Edit };
    enum class Tab { Mix, Pattern };
 
    MixedFilamentDialog(wxWindow* parent, Action action, std::vector<std::string>& physical_colors);

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    Action m_action{Action::Add};
    Tab    m_current_tab{Tab::Mix};

    int m_width_fixed;
    int m_height_start;
    int m_height_min;
    std::vector<std::string>& m_physical_colors;

    void build_ui(wxWindow* parent);

    wxColour getTabBorderColor(bool is_selected, bool is_hovered) const;
    void     paintRoundedPanel(wxPanel* panel,
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

    // Footer
};

} // namespace Slic3r::GUI
#endif