#ifndef slic3r_GUI_MFDRecommendationsAccordion_hpp_
#define slic3r_GUI_MFDRecommendationsAccordion_hpp_

// MFDRecommendationsAccordion: Collapsible "Mixing Recommendations" panel.
//
// Responsibility:
//   Displays a grid of color tiles representing common mix presets
//   (2-way 50/50, 2-way 66/34, 3-way equal, etc.) computed from the
//   available physical filaments. Clicking a tile fires on_preset_selected.
//
// Why a separate class?
//   The recommendation tile layout depended on fill_recommendations() being
//   called on the dialog, which directly manipulated dialog-level state
//   (m_min_weight_slider, m_min_weight_value_input) when a tile was clicked.
//   Extracting it here removes that direct coupling: the class now raises a
//   callback that the dialog handles, keeping state changes in one place.
//
// Data flow:
//   Physical filaments passed at construction time (read-only reference).
//   on_preset_selected callback -> dialog adjusts its state.

#include "Widgets/Accordion.hpp"
#include <functional>
#include <vector>
#include <string>

namespace Slic3r::GUI {

class MFDRecommendationsAccordion : public Accordion
{
public:
    enum class Mode { Mix, Gradient };

    MFDRecommendationsAccordion(
        wxWindow*                                                    parent,
        const std::vector<std::pair<std::string, std::string>>&      physical_filaments);
    ~MFDRecommendationsAccordion() override = default;

    void set_mode(Mode mode);

    // Called when the user clicks a recommendation tile.
    // Provides the physical filament indices and weights for the chosen preset.
    void set_on_preset_selected(
        std::function<void(const std::vector<int>&, const std::vector<double>&)> cb)
    {
        m_on_preset_selected = std::move(cb);
    }

    void update_recommendations(
        const std::vector<std::pair<std::string, std::string>>& physical_filaments, 
        double current_min_weight_ratio);

private:
    void build_ui();
    void fill_recommendations(wxPanel* container, wxBoxSizer* container_sizer);

    wxPanel* create_mix_tile(
        wxPanel*                    parent,
        const wxColor&              color,
        const wxString&             tooltip,
        const std::vector<int>&     physical_indices,
        const std::vector<double>&  weights);

    wxPanel* create_gradient_tile(
        wxPanel*                    parent,
        const std::vector<int>&     physical_indices);

    wxString format_tooltip(
        const std::vector<int>&    phys_indices,
        const std::vector<double>& weights,
        const wxColor&             mixed_color) const;

    wxString format_gradient_tooltip(const std::vector<int>& phys_indices) const;

    wxColor get_mixed_color(
        const std::vector<int>&    phys_indices,
        const std::vector<double>& weights) const;

    static bool is_color_in_between(const wxColor& c1, const wxColor& c2, const wxColor& c3, double max_dev = 60.0);

    const std::vector<std::pair<std::string, std::string>>& m_physical_filaments;

    std::function<void(const std::vector<int>&, const std::vector<double>&)> m_on_preset_selected;

    Mode     m_mode{Mode::Mix};
    wxPanel* m_mix_panel{nullptr};
    wxPanel* m_gradient_panel{nullptr};
};

} // namespace Slic3r::GUI

#endif // slic3r_GUI_MFDRecommendationsAccordion_hpp_
