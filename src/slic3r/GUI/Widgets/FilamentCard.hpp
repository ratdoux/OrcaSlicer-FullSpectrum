#ifndef slic3r_GUI_FilamentCard_hpp_
#define slic3r_GUI_FilamentCard_hpp_

#include <wx/panel.h>
#include <wx/sizer.h>
#include <functional>
#include <string>

#include "slic3r/GUI/PresetComboBoxes.hpp"

namespace Slic3r::GUI {

class FilamentCardPhysical : public wxPanel
{
public:
    FilamentCardPhysical(wxWindow* parent, const int index);

    void set_on_edit_callback(std::function<void(int index, wxWindow* anchor)> callback) 
    { 
        m_on_edit = std::move(callback);
    }
    wxPoint get_edit_btn_client_position();
    void    update_state();

    Slic3r::GUI::PlaterPresetComboBox*  m_filament_combo_box {nullptr};

private:
    int m_index{-1};

    wxBoxSizer*                         m_sizer{nullptr};
    ScalableButton*                     m_filament_edit_btn {nullptr};

    std::function<void(int, wxWindow*)> m_on_edit;

    void build_ui();
};

} // namespace Slic3r::GUI

#endif
