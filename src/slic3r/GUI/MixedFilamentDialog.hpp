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

    void build_ui();

    void update_tabs();
    void on_tab_hover_enter(wxMouseEvent& event, Tab tab);
    void on_tab_hover_leave(wxMouseEvent& event);

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
    ScalableButton* m_mix_btn{nullptr};
    ScalableButton* m_pattern_btn{nullptr};

    // Content

    // Footer
};

} // namespace Slic3r::GUI
#endif