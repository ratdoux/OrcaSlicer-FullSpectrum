#ifndef slic3r_GUI_MixedFilamentDialog_hpp_
#define slic3r_GUI_MixedFilamentDialog_hpp_

// MixedFilamentDialog: Main dialog for creating and editing Mixed Filament presets.
//
// Architecture overview
// ---------------------
// The dialog is structured around four responsibilities:
//   1. State ownership:   m_selected_filaments, m_selected_filaments_weights,
//                         m_selected_filaments_colors, m_min_weight_ratio, etc.
//   2. Tab management:    Three tabs (Mix / Pattern / Gradient) each show a
//                         different combination of accordion sections.
//   3. Cross-section coordination: The dialog is the single place that wires
//                         accordion callbacks together (e.g. ratio change ->
//                         update material panel -> sync color picker -> update preview).
//   4. Data output:       Will expose get_result() or similar once the footer
//                         is fully implemented.
//
// Why accordion sub-classes instead of a monolithic dialog?
//   The original MixedFilamentDialog had >250 private member variables covering
//   every pixel of every collapsible panel, plus build_*_ui / setup_collapsible_section
//   helper methods that cross-referenced those variables in hard-to-follow ways.
//   The accordion sub-classes (MFDColorPickerAccordion, MFDMaterialAccordion, etc.)
//   each own the UI pointers for their section. The dialog only needs a single typed
//   pointer per section and calls a narrow set of push-from-dialog update methods.
//
// Unidirectional data flow:
//   Accordion raises callback  -->  dialog adjusts canonical state
//                              -->  dialog calls update_*() on affected accordions
//   This keeps state mutations in one place and avoids circular updates.

#include <memory>
#include <functional>
#include <wx/wx.h>
#include <wx/scrolwin.h>

#include "Widgets/Label.hpp"
#include "Widgets/Button.hpp"
#include "GUI_Utils.hpp"
#include "MFDPreviewAccordion.hpp"

namespace Slic3r::GUI {

class MFDColorPickerAccordion;
class MFDPatternSelectorAccordion;
class MFDMaterialAccordion;
class MFDRatioAccordion;
class MFDRecommendationsAccordion;
class MFDPreviewAccordion;
class MFDGradientAccordion;

class MixedFilamentDialog : public DPIDialog
{
public:
    enum class Action { Add, Edit };
    enum class Tab { Mix, Pattern, Gradient };
    enum class MixMethod { ManualRatio, ByColor };

    MixedFilamentDialog(
        wxWindow* parent,
        Action    action,
        std::vector<std::pair<std::string, std::string>>& physical_filaments);

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    // -----------------------------------------------------------------------
    // Core state (single source of truth for all accordion sections)
    // -----------------------------------------------------------------------
    Action    m_action{Action::Add};
    Tab       m_current_tab{Tab::Mix};
    MixMethod m_mix_method{MixMethod::ManualRatio};

    // Filament blend weights: values in [0, 1], sum to 1.0.
    double m_min_weight_ratio{0.15};

    int       max_filament{4};
    const int min_filament{2};

    int m_width_fixed;
    int m_height_start;
    int m_height_min;
    int m_clr_swatch_size; // size in physical pixels for combobox color swatches

    // Physical filament catalog (name, hex color) — not owned by this dialog.
    std::vector<std::pair<std::string, std::string>>& m_physical_filaments;

    // Currently selected blend of physical filaments.
    std::vector<int>     m_selected_filaments;
    std::vector<double>  m_selected_filaments_weights;
    std::vector<wxColor> m_selected_filaments_colors;

    // Gradient state
    std::vector<double>  m_gradient_positions;
    double               m_gradient_min_ratio{0.10};

    // -----------------------------------------------------------------------
    // UI construction
    // -----------------------------------------------------------------------
    void build_ui(wxWindow* parent);
    void build_mix_method_ui(wxPanel* parent, wxBoxSizer* parent_sizer);

    // -----------------------------------------------------------------------
    // Tab management
    // -----------------------------------------------------------------------
    void update_tabs();

    wxColour m_orca_colour = wxColour(ColorRGBA::ORCA().r_uchar(), ColorRGBA::ORCA().g_uchar(), ColorRGBA::ORCA().b_uchar());
    wxColour getTabBorderColor(bool is_selected, bool is_hovered) const;
    wxColour getTabBackgroundColor(bool is_selected, bool is_hovered) const;
    wxColour getTabTextColor(bool is_selected, bool is_hovered) const;
    void     paintTabBtn(wxPanel* panel, bool round_left, bool round_right,
                         double radius, wxString label, wxString icon_name,
                         bool is_selected, bool is_hovered);

    bool m_mix_tab_hovered     = false;
    bool m_pattern_tab_hovered = false;
    bool m_gradient_tab_hovered= false;

    // -----------------------------------------------------------------------
    // Material slot management
    // (state changes flow through these methods, then propagate to accordions)
    // -----------------------------------------------------------------------
    void add_material_slot(int selected_filament_index = -1);
    void remove_last_material_slot();
    void on_filament_selection_changed(size_t slot, int new_phys_idx);
    int  find_first_free_filament() const;

    // Recompute and push updated weights/colors/labels to affected accordions.
    void update_material_state();

    // -----------------------------------------------------------------------
    // Min-weight clamping
    // Called when the ratio accordion signals a min-weight change.
    // -----------------------------------------------------------------------
    void apply_min_weight_clamping();

    // -----------------------------------------------------------------------
    // Color picker <-> ratio synchronization
    // -----------------------------------------------------------------------
    // Sync the HSL picker to the current weighted blend color.
    void sync_color_picker_to_mix();
    // Called when the color picker fires a color-changed event.
    void on_color_picker_changed(const wxColour& color, bool is_dragging);

    // Guard to prevent the sync from looping back when we programmatically
    // set the picker from a ratio change (or vice versa).
    bool m_syncing_from_color_picker{false};

    // -----------------------------------------------------------------------
    // Preview update
    // -----------------------------------------------------------------------
    void update_preview();
    void update_preview(const std::vector<int>& filaments,
                        const std::vector<double>& weights);

    // -----------------------------------------------------------------------
    // Mix preset helpers (used by color-picker matching)
    // -----------------------------------------------------------------------
    struct MixPreset {
        std::vector<int>    filament_indices;
        std::vector<double> weights;
        wxColor             mixed_color;
    };
    std::vector<MixPreset> m_mix_presets;
    void             generate_mix_presets();
    static wxColor   compute_mixed_color(
        const std::vector<std::pair<std::string, std::string>>& filaments,
        const std::vector<int>&    indices,
        const std::vector<double>& weights);
    const MixPreset* find_closest_mix(const wxColour& target) const;
    void             update_color_match(const wxColour& selected_color,
                                        bool update_active_mix = false);

    // Apply a recommendation preset (adjusts filament slots + weights).
    void set_active_mix(const std::vector<int>& physical_filaments,
                        const std::vector<double>& weights);

    // -----------------------------------------------------------------------
    // Utility helpers
    // -----------------------------------------------------------------------
    std::vector<double> get_default_weights(int filament_count);
    std::vector<wxColor> get_selected_filaments_colors(
        const std::vector<int>& filament_indices) const;
    std::vector<wxColor> get_colors_from_indices(const std::vector<int>& indices) const;

    // Layer-stack computation for the preview visualization.
    // Struct definition removed, now using MFDPreviewLayerEntry from MFDPreviewAccordion.hpp
    static std::vector<MFDPreviewLayerEntry> compute_layer_stack(
        const std::vector<double>& weights, int total_layers = 20);
    std::vector<MFDPreviewLayerEntry> compute_pattern_layer_stack(
        const std::vector<int>& pattern_indices, int total_layers = 20);

    // Fallback size constraint (prevents horizontal resizing).
    void on_sizing(wxSizeEvent& event);

    void on_ok();
    void on_cancel();

    // -----------------------------------------------------------------------
    // Top-level UI panels
    // -----------------------------------------------------------------------
    wxPanel*          m_title_panel{nullptr};
    wxScrolledWindow* m_content_panel{nullptr};
    wxPanel*          m_footer_panel{nullptr};
    wxPanel*          m_list_preview_panel{nullptr};
    Button*           m_btn_ok{nullptr};
    Button*           m_btn_cancel{nullptr};
    bool              m_is_list_preview_hovered{false};

    wxBoxSizer* m_main_sizer{nullptr};
    wxBoxSizer* m_title_sizer{nullptr};
    wxBoxSizer* m_content_sizer{nullptr};
    wxBoxSizer* m_footer_sizer{nullptr};

    // Tab buttons
    wxPanel* m_mix_tab_btn{nullptr};
    wxPanel* m_pattern_tab_btn{nullptr};
    wxPanel* m_gradient_tab_btn{nullptr};

    // Mix-method radio buttons (Manual Ratio / By Color) — lives outside any accordion
    // because it controls which accordions are visible in the Mix tab.
    wxPanel*       m_mix_method_panel{nullptr};
    wxBoxSizer*    m_mix_method_sizer{nullptr};
    wxRadioButton* m_method_manual_radio{nullptr};
    wxRadioButton* m_method_by_color_radio{nullptr};

    // -----------------------------------------------------------------------
    // Accordion section pointers (each owns its own UI sub-tree)
    // -----------------------------------------------------------------------
    MFDColorPickerAccordion*      m_color_picker_accordion{nullptr};
    MFDPatternSelectorAccordion*  m_pattern_selector_accordion{nullptr};
    MFDMaterialAccordion*         m_material_accordion{nullptr};
    MFDRatioAccordion*            m_ratio_accordion{nullptr};
    MFDRecommendationsAccordion*  m_recommendations_accordion{nullptr};
    MFDPreviewAccordion*          m_preview_accordion{nullptr};
    MFDGradientAccordion*         m_gradient_accordion{nullptr};

    // Layer-stack data cached between preview updates
    std::vector<MFDPreviewLayerEntry> m_preview_layer_stack;
    std::vector<wxColor>         m_preview_colors;

    // Color-match deviation (kept on the dialog for use by the color picker accordion)
    double m_current_deviation{-1.0};
    double m_warning_deviation_threshold{65.0};
    double m_max_deviation{441.673}; // sqrt(3 * 255^2)
};

} // namespace Slic3r::GUI

#endif // slic3r_GUI_MixedFilamentDialog_hpp_
