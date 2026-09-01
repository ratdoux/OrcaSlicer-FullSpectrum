#ifndef slic3r_GUI_FilamentGroupDialog_hpp_
#define slic3r_GUI_FilamentGroupDialog_hpp_

#include "GUI_Utils.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <string>
#include <vector>

class Button;
class StaticBox;
class wxFlexGridSizer;
class wxScrolledWindow;

namespace Slic3r { namespace GUI {

// Requirement 7.1 custom filament grouping dialog (Figma node 27673:62092):
// drag filament chips between the standard and the high flow nozzle group, or
// swap both groups at once with the round button in the middle. Only filaments
// actually used by objects in the scene are listed.
// OK writes the mapping through FlowType::apply_custom_mapping() and returns
// wxID_OK so the caller can continue slicing; Cancel changes nothing.
class FilamentGroupDialog : public DPIDialog
{
public:
    explicit FilamentGroupDialog(wxWindow *parent);

    // One FLOW_MODE_* entry per filament (all filaments, in filament order --
    // entries of unused filaments are left untouched).
    const std::vector<FilamentVolumeType> &mapping() const { return m_mapping; }

    // Called by the drop targets; |filament_idx| is the 0-based filament index.
    void move_filament(size_t filament_idx, bool to_high_flow);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    struct FilamentInfo {
        size_t      filament_idx; // 0-based index into the sidebar filament list
        wxColour    color;
        wxString    label;        // material name shown under the chip
        std::string type_raw;     // filament_type for compatibility checks
        std::string preset_name;  // preset display name, extra compatibility hint
    };

    void load_filaments();
    void swap_groups();
    void rebuild_chips();
    void update_warnings();

    std::vector<FilamentInfo>      m_filaments; // only the filaments used by the model
    std::vector<FilamentVolumeType> m_mapping;   // full-length, indexed by filament_idx

    StaticBox       *m_std_box   { nullptr };
    StaticBox       *m_high_box  { nullptr };
    wxScrolledWindow *m_std_scroll  { nullptr };
    wxScrolledWindow *m_high_scroll { nullptr };
    wxFlexGridSizer *m_std_grid  { nullptr };
    wxFlexGridSizer *m_high_grid { nullptr };
    wxBoxSizer      *m_warning_sizer { nullptr };
    Button          *m_confirm_button { nullptr };
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_FilamentGroupDialog_hpp_
