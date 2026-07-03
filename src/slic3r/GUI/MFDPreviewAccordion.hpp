#ifndef slic3r_GUI_MFDPreviewAccordion_hpp_
#define slic3r_GUI_MFDPreviewAccordion_hpp_

// MFDPreviewAccordion: Collapsible preview panel for the Mixed Filament Dialog.
//
// Responsibility:
//   Renders two side-by-side views driven by data pushed from MixedFilamentDialog:
//     - Layer stack panel: a stack of colored bars showing which filament prints on each layer.
//     - Solid color panel: the computed blended color of the mix.
//
// Why a separate class?
//   The preview update logic was previously tangled inside MixedFilamentDialog with
//   direct member references. Isolating it here keeps the paint code in one place and
//   lets MixedFilamentDialog simply call update_preview() or clear_preview() without
//   having to know anything about the internal panel structure.
//
// Data flow (unidirectional push):
//   MixedFilamentDialog computes layer stack & colors  -->  update_preview()
//   No callbacks: the preview is read-only and never drives state changes.

#include "Widgets/Accordion.hpp"
#include <vector>

namespace Slic3r::GUI {

// A single entry in the layer-stack representation of a mixed filament print.
// filament_index: 0-based index into the colors array passed to update_preview().
// scale: width/height fraction (0..1) for the visual bar; always 1.0 in current usage.
struct MFDPreviewLayerEntry {
    int    filament_index;
    double scale;
};

class MFDPreviewAccordion : public Accordion
{
public:
    enum class PreviewMode { Mix, Pattern, Gradient };

    MFDPreviewAccordion(wxWindow* parent);
    ~MFDPreviewAccordion() override = default;

    void update_preview_mix(
        const std::vector<double>&               weights,
        const std::vector<wxColor>&              colors,
        const wxColor&                           mixed_color);

    void update_preview_pattern(
        const std::vector<int>&                  pattern_indices,
        const std::vector<wxColor>&              colors,
        const wxColor&                           mixed_color);

    void update_preview_gradient(
        const std::vector<wxColor>&              colors,
        const std::vector<double>&               positions);

    void set_preview_mode(PreviewMode mode);

    // Clear preview to an empty state (no layer stack, grey placeholder).
    void clear_preview();

protected:
    // Updates the mini-preview thumbnails shown in the collapsed header.
    void on_collapsed_changed(bool collapsed) override;

private:
    void build_ui();
    void paint_layers_panel(wxPaintEvent& event);
    void paint_title_layers(wxPaintEvent& event);
    void paint_color_panel(wxPaintEvent& event);
    void paint_title_swatch(wxPaintEvent& event);

    struct GradientSample {
        wxColor color_a;
        wxColor color_b;
        double  weight_b{0.0};
        int     index_a{-1};
        int     index_b{-1};
    };
    GradientSample sample_gradient(double t) const;

    static std::vector<MFDPreviewLayerEntry> compute_layer_stack(const std::vector<double>& weights, int total_layers = 20);
    static std::vector<MFDPreviewLayerEntry> compute_pattern_layer_stack(const std::vector<int>& pattern_indices, int total_layers = 20);

    void draw_layer_stack_common(wxGraphicsContext* gc, const wxSize& size, bool vertical, double padding, double corner_radius);
    void draw_gradient_common(wxGraphicsContext* gc, const wxSize& size, bool vertical);

    wxPanel* m_layers_panel{nullptr};      // Full-height layer stack view
    wxPanel* m_color_panel{nullptr};       // Solid blended-color swatch
    wxPanel* m_title_layers{nullptr};      // Compact layer preview shown in header
    wxPanel* m_title_swatch{nullptr};      // Compact color swatch shown in header

    PreviewMode                       m_preview_mode{PreviewMode::Mix};
    std::vector<MFDPreviewLayerEntry> m_layer_stack;
    std::vector<wxColor>              m_colors;
    std::vector<double>               m_gradient_positions;
};

} // namespace Slic3r::GUI

#endif // slic3r_GUI_MFDPreviewAccordion_hpp_
