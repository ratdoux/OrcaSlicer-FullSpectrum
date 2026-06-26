#ifndef slic3r_GUI_MFDMaterialAccordion_hpp_
#define slic3r_GUI_MFDMaterialAccordion_hpp_

// MFDMaterialAccordion: Collapsible material-selection panel for the Mixed Filament Dialog.
//
// Responsibility:
//   Hosts one ComboBox per active physical filament (minimum 2, maximum 4)
//   and their associated weight-percentage labels. Also provides Add/Delete
//   buttons in the accordion header so the user can change the number of mixed
//   filaments without collapsing the section.
//
//   Renders a compact collapsed header summary showing color swatches + percentages.
//
// Why a separate class?
//   Previously, the material comboboxes were dynamically added and removed by
//   methods scattered across MixedFilamentDialog (add_material_combobox,
//   remove_material_combobox, refresh_material_combobox_items,
//   refresh_material_weight_labels, update_material_buttons_visibility).
//   Each of those methods touched 8+ dialog-level member variables.
//   Consolidating them here means any future maintainer has a single
//   self-contained place to understand material-selection UI.
//
// Data flow:
//   - The dialog owns m_selected_filaments, m_selected_filaments_weights, etc.
//   - MFDMaterialAccordion owns the ComboBoxes and drives changes via callbacks:
//       on_filament_added   -> dialog adjusts weights/colors, calls update_from_dialog()
//       on_filament_removed -> dialog adjusts weights/colors, calls update_from_dialog()
//       on_filament_changed -> dialog adjusts weights/colors, calls update_from_dialog()
//   - update_from_dialog() is how the dialog pushes new state back to the UI.

#include "Widgets/Accordion.hpp"
#include "Widgets/ComboBox.hpp"
#include <functional>
#include <vector>
#include <string>

class ScalableButton;

namespace Slic3r::GUI {

class MFDMaterialAccordion : public Accordion
{
public:
    // min/max_filament: the minimum/maximum number of physical filaments that can be blended.
    MFDMaterialAccordion(
        wxWindow*                                                   parent,
        const std::vector<std::pair<std::string, std::string>>&     physical_filaments,
        int clr_swatch_size_dip,
        int min_filament,
        int max_filament);
    ~MFDMaterialAccordion() override = default;

    // --- Callbacks (set before construction is complete) ---
    // Called when the user requests an additional filament slot.
    void set_on_add_filament(std::function<void()> cb) { m_on_add_filament = std::move(cb); }
    // Called when the user requests removal of the last filament slot.
    void set_on_remove_filament(std::function<void()> cb) { m_on_remove_filament = std::move(cb); }
    // Called when the user changes a combobox selection. Provides the (0-based) slot index
    // and the newly selected physical filament index.
    void set_on_filament_changed(std::function<void(size_t slot, int new_phys_idx)> cb)
    {
        m_on_filament_changed = std::move(cb);
    }

    // --- Push-from-dialog update methods ---

    // Add a new combobox row pre-set to selected_filament_index.
    // Returns false if the maximum count has been reached.
    bool add_combobox_row(int selected_filament_index = -1);

    // Remove the last combobox row.
    // Returns false if already at minimum count.
    bool remove_last_combobox_row();

    // Refresh combobox item text to reflect which filaments are "already selected" (swap labels).
    void refresh_combobox_items(const std::vector<int>& selected_filaments);

    // Update the percentage labels next to each combobox.
    void refresh_weight_labels(const std::vector<double>& weights);

    // Update the header summary to reflect the currently selected filaments + weights.
    void update_title_preview(
        const std::vector<int>&    selected_filaments,
        const std::vector<double>& weights);

    // Show/hide add and delete buttons based on current tab/method context.
    // can_add_or_remove: true when in ManualRatio mix or Gradient tab.
    void update_button_visibility(bool can_add_or_remove);

    // Sync combobox selection to a specific physical filament index for a given slot.
    void set_combobox_selection(size_t slot, int phys_idx);

    // Returns current number of combobox rows.
    int get_combobox_count() const { return static_cast<int>(m_comboboxes.size()); }

protected:
    void on_collapsed_changed(bool collapsed) override;

private:
    void build_ui();
    void update_header_layout();

    const std::vector<std::pair<std::string, std::string>>&  m_physical_filaments;
    int m_clr_swatch_size_dip;
    int m_min_filament;
    int m_max_filament;
    bool m_can_add_or_remove{true};

    // Material combobox panel (body content)
    wxPanel*    m_combobox_panel{nullptr};
    wxBoxSizer* m_combobox_sizer{nullptr};

    // Dynamic rows of (combobox, weight_label) — indexed by slot
    std::vector<ComboBox*>       m_comboboxes;
    std::vector<wxStaticText*>   m_weight_labels;

    // Action buttons in the header (marked as action controls so they
    // don't accidentally trigger section collapse).
    ScalableButton* m_add_btn{nullptr};
    ScalableButton* m_delete_btn{nullptr};

    // Header summary (visible when collapsed)
    wxPanel*              m_title_preview_panel{nullptr};
    std::vector<wxPanel*> m_title_swatches;
    std::vector<wxStaticText*> m_title_percent_texts;
    std::vector<int>      m_last_preview_filaments;

    std::function<void()>                       m_on_add_filament;
    std::function<void()>                       m_on_remove_filament;
    std::function<void(size_t, int)>            m_on_filament_changed;
};

} // namespace Slic3r::GUI

#endif // slic3r_GUI_MFDMaterialAccordion_hpp_
