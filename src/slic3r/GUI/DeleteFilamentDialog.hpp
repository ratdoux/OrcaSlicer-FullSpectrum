#ifndef slic3r_GUI_DeleteFilamentDialog_hpp_
#define slic3r_GUI_DeleteFilamentDialog_hpp_

#include "GUI_Utils.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include <vector>

namespace Slic3r { namespace GUI {

struct FilamentChoiceItem {
    wxString name;
    wxBitmap bitmap;
    size_t   filament_id{0}; // 0-based virtual ID
};

class DeleteFilamentDialog : public DPIDialog
{
public:
    DeleteFilamentDialog(
        wxWindow* parent,
        const wxString& title,
        const wxString& message,
        bool has_color_or_painting,
        const std::vector<FilamentChoiceItem>& target_choices,
        int default_selection = 0);

    ~DeleteFilamentDialog() override;

    int get_target_filament_id() const;

    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    bool m_has_color_or_painting{false};
    ComboBox* m_target_combo{nullptr};
    std::vector<FilamentChoiceItem> m_target_choices;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_DeleteFilamentDialog_hpp_
