#pragma once

// Shared declarations for color-match helpers used by Plater, the refactored
// mixed-filament UI, and MixedColorMatchPanel. Implementations live in
// MixedColorMatchHelpers.cpp.

#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Color.hpp"

#include <wx/wx.h>
#include <wx/bitmap.h>

#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

// ---- CIELAB color space types ----

struct CIELab {
    double L, a, b;
};

// ---- Pre-computed blend lookup table (stores CIELab results) ----

class BlendLUT {
public:
    BlendLUT() : m_n(0) {}
    explicit BlendLUT(size_t n);

    size_t size() const { return m_n; }
    bool   empty() const { return m_n < 2; }

    // Pre: a < b (callers always iterate a < b; polynomial mixing is not symmetric)
    const CIELab& get(size_t a, size_t b, int percent) const {
        assert(a < b);
        return m_pair[a][b - a][percent];
    }

private:
    size_t m_n;
    // m_pair[a][b-a][percent] for b >= a; symmetric access via get()
    std::vector<std::vector<std::vector<CIELab>>> m_pair;
    friend BlendLUT build_blend_lut(const std::vector<wxColour>& palette);
};

struct MixedColorMatchRecipeResult
{
    bool         cancelled     = false;
    bool         valid         = false;
    unsigned int component_a   = 1;
    unsigned int component_b   = 2;
    int          mix_b_percent = 50;
    std::string  manual_pattern;
    std::string  gradient_component_ids;
    std::string  gradient_component_weights;
    // Optional continuous apparent weights, aligned with the recipe components.
    // The integer recipe remains the printable layer cadence; these weights are
    // only used to encode a small surface-bias residual around that cadence.
    std::vector<double> apparent_component_weights;
    wxColour     preview_color = wxColour("#26A69A");
    double       delta_e       = std::numeric_limits<double>::infinity();
};

struct MixedColorMatchCreationResult
{
    bool         valid       = false;
    bool         created     = false;
    bool         used_surface_bias = false;
    unsigned int filament_id = 1;
    double       delta_e     = std::numeric_limits<double>::infinity();
};

struct AdaptiveColorMatchPreviewCycle
{
    unsigned int                    filament_id = 0;
    MixedFilamentDefinition         definition;
    std::vector<size_t>             target_indices;
};

struct AdaptiveColorMatchPreviewResult
{
    bool                                        valid = false;
    std::vector<unsigned int>                   target_filament_ids;
    std::vector<AdaptiveColorMatchPreviewCycle> mixed_cycles;
    size_t                                      direct_physical_target_count = 0;
};

enum class MixedColorMatchEncoding
{
    LayerRatio,
    SurfaceBias,
    PerimeterModulatedLayerSequence,
    AdaptiveLocalizedCycles
};

// ---- small pure helpers (defined here, used everywhere) ----

wxColour parse_mixed_color(const std::string &value);
wxString normalize_color_match_hex(const wxString &value);
bool     try_parse_color_match_hex(const wxString &value, wxColour &color_out);

std::vector<int> normalize_color_match_weights(const std::vector<int> &weights, size_t count);
std::vector<int> expand_color_match_recipe_weights(const MixedColorMatchRecipeResult &recipe, size_t num_physical);
std::string      summarize_color_match_recipe(const MixedColorMatchRecipeResult &recipe);
wxBitmap         make_color_match_swatch_bitmap(const wxColour &color, const wxSize &size);

std::vector<MixedColorMatchRecipeResult> build_color_match_presets(
    const std::vector<std::string> &physical_colors,
    int                             min_component_percent = 0);

double color_delta_e00(const wxColour &lhs, const wxColour &rhs);

CIELab sRGB_to_CIELab(const wxColour& c);

double  delta_e_lab(const CIELab& a, const CIELab& b);

BlendLUT build_blend_lut(const std::vector<wxColour>& palette);

/// Multi-color blend via sequential polynomial pigment mixing in
/// filament-ID-ascending order, then sRGB→CIELab.
CIELab blend_weighted_lab_accurate(const std::vector<wxColour>& palette,
                                    const std::vector<unsigned int>& ids,
                                    const std::vector<int>& weights);

MixedColorMatchRecipeResult build_best_color_match_recipe(
    const std::vector<std::string> &physical_colors,
    const wxColour                 &target_color,
    int                             min_component_percent,
    const std::vector<double>      &physical_tds,
    const std::vector<std::string> &physical_material_ids,
    MixedFilamentColorEngine        color_engine,
    bool                            use_td_prediction);

MixedFilamentDefinition mixed_filament_definition_from_color_match_recipe(
    const MixedColorMatchRecipeResult &recipe,
    size_t                             num_physical,
    MixedColorMatchEncoding            encoding = MixedColorMatchEncoding::LayerRatio,
    float                              reference_width_mm = 0.4f);

// Reuse an exact physical or virtual color when possible; otherwise create a
// custom mixed filament using the active FullSpectrum color engine.
MixedColorMatchCreationResult create_mixed_filament_color_match(
    const wxColour                 &target_color,
    const std::vector<std::string> &physical_colors,
    int                             min_component_percent = 15,
    size_t                          max_total_filaments = 16,
    MixedColorMatchEncoding         encoding = MixedColorMatchEncoding::LayerRatio);

// Dry-run the same sequential reuse/creation decisions used when adaptive OBJ
// colors are committed. This lets the import dialog show the unique physical
// cycles that will actually appear instead of one duplicate card per region.
AdaptiveColorMatchPreviewResult preview_adaptive_localized_color_matches(
    const std::vector<wxColour>    &target_colors,
    const std::vector<std::string> &physical_colors,
    int                             min_component_percent = 15,
    size_t                          max_total_filaments = 16);

// ---- display context helpers ----
MixedFilamentDisplayContext build_mixed_filament_display_context(
    const std::vector<std::string> &physical_colors);

struct MixedFilamentGradientPreview
{
    // Ordered physical filament ids (1-based) and their UI anchor positions.
    std::vector<unsigned int> component_ids;
    std::vector<double>       component_positions;

    // Uniformly spaced display samples evaluated with the selected color
    // engine. Keeping these samples uniform lets every GUI renderer share the
    // same compact preview without having to duplicate stop-curve logic.
    std::vector<wxColour> sampled_colors;
};

wxColour blend_mixed_filament_components(
    const std::vector<unsigned int>     &component_ids,
    const std::vector<int>              &component_percents,
    const MixedFilamentDisplayContext   &context);

MixedFilamentGradientPreview build_mixed_filament_gradient_preview(
    const std::vector<unsigned int>     &ordered_component_ids,
    const std::vector<double>           &component_stop_positions,
    const MixedFilamentDisplayContext   &context,
    size_t                               sample_count = 17);

MixedFilamentGradientPreview build_mixed_filament_gradient_preview(
    const MixedFilamentDefinition       &definition,
    const MixedFilamentDisplayContext   &context,
    size_t                               sample_count = 17);

// Build the colors that an adaptive localized cycle can actually expose. The
// requested source colors are projected through the same component-restricted
// KM/K-S solver used by the perimeter renderer; pair sweeps keep the displayed
// spectrum representative even when a cluster contains very few source colors.
std::vector<wxColour> build_adaptive_cycle_attainable_colors(
    const std::vector<unsigned int>     &component_ids,
    const std::vector<RGBA>             &requested_colors,
    const MixedFilamentDisplayContext   &context,
    size_t                               sample_count = 17);

wxColour compute_color_match_recipe_display_color(
    const MixedColorMatchRecipeResult  &recipe,
    const MixedFilamentDisplayContext  &context);

std::vector<int>          decode_color_match_gradient_weights(const std::string& value, size_t expected_components);
MixedColorMatchRecipeResult build_pair_color_match_candidate(const std::vector<wxColour>& palette,
                                                             unsigned int                 component_a,
                                                             unsigned int                 component_b,
                                                             int                          mix_b_percent,
                                                             int                          min_component_percent = 0);
MixedColorMatchRecipeResult build_multi_color_match_candidate(const std::vector<wxColour>&     palette,
                                                              const std::vector<unsigned int>& ids,
                                                              const std::vector<int>&          weights,
                                                              int                              min_component_percent = 0);
bool color_match_weights_within_range(const std::vector<int>& weights, int min_component_percent);
std::vector<unsigned int>   build_color_match_sequence(const std::vector<unsigned int>& ids, const std::vector<int>& weights);
wxColour                    blend_sequence_filament_mixer(const std::vector<wxColour>& palette, const std::vector<unsigned int>& sequence);

bool is_filament_compatible(const std::vector<unsigned int>& filament_ids);
bool is_filament_compatible(const MixedFilament& mf);
// Returns std::nullopt if all compatible (or insufficient data),
// otherwise the 1-based filament IDs of the first incompatible pair.
std::optional<std::pair<unsigned int, unsigned int>> find_incompatible_filament_pair(const std::vector<unsigned int>& filament_ids);

// Result of parsing a normalized cycle-mode manual pattern.
struct CyclePatternParseResult {
    std::string              invalid_token; // first token that failed strtoul
    unsigned int             invalid_id = 0; // first out-of-range 1-based ID (0 = none)
    std::vector<unsigned int> ids;           // valid 1-based filament IDs in parse order
    int                      total_tokens = 0;
};

// One-shot parse of a normalized cycle pattern: split→token→strtoul→validate.
CyclePatternParseResult parse_cycle_pattern(const std::string& normalized_pattern, int num_physical);

// Parse a normalized cycle pattern and produce a human-readable percentage summary.
// Format: "F1 60%+F2 40%". Returns empty string on invalid input.
std::string summarize_cycle_pattern_text(const std::string& normalized_pattern,
                                         const MixedFilament& entry,
                                         int num_physical);

}} // namespace Slic3r::GUI
