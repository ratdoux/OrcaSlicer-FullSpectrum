#pragma once

// Shared declarations for color-match helpers used by Plater, the refactored
// mixed-filament UI, and MixedColorMatchPanel. Implementations live in
// MixedColorMatchHelpers.cpp.

#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/ImageMap/VolumeData.hpp"
#include "libslic3r/libslic3r.h"

#include <wx/wx.h>
#include <wx/bitmap.h>

#include <limits>
#include <optional>
#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include <memory>

namespace Slic3r { class Print; }

namespace Slic3r { namespace GUI {

// ---- Shared constants ----

// Canonical preset name for the Full Spectrum (formerly "CMYW") recommended-mode
// palette. Resolved at call time against the current printer's nozzle_diameter:
// if a matching Full Spectrum preset exists for that nozzle, it is returned;
// otherwise falls back to the canonical 0.4 variant (the only SKU shipped today).
// Future-proof: shipping a new nozzle variant (e.g. 0.6/0.8) just requires adding
// its preset JSON under resources/profiles/Snapmaker/filament/ -- no code change.
//
// Thread-safety convention: same as the other preset_bundle readers in this file
// (build_mixed_filament_display_context, extract_model_colors, etc.) -- caller
// MUST be on the UI thread. No worker-thread call site exists today; the worker
// lambda in launch_background_match captures values by copy precisely to keep
// off preset_bundle (see load_full_spectrum_colors's safety note).
std::string full_spectrum_preset_name();

// True iff a Full Spectrum preset exists for the printer's *current* nozzle
// diameter (no 0.4 fallback, unlike full_spectrum_preset_name). UI-thread only
// (reads preset_bundle, same convention as above). Used by the batch-match guard
// to gate Recommended mode on preset availability rather than a hard-coded 0.4.
bool full_spectrum_preset_exists_for_current_nozzle();

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

struct NormalColorMatchPlanEntry
{
    unsigned int filament_id       = 0;
    wxColour     target_color;
    wxColour     predicted_color;
    double       delta_e           = std::numeric_limits<double>::infinity();
    bool         newly_created     = false;
    bool         used_surface_bias = false;
    std::string  recipe_summary;
};

struct NormalColorMatchPlannedFilament
{
    unsigned int            filament_id = 0;
    MixedFilamentDefinition definition;
    std::string             recipe_summary;
};

// Immutable dry-run of the normal OBJ image-map mapping. The mixed-filament
// manager is copied while this plan is built, so opening or editing the import
// prompt never mutates the project. Committing applies these exact definitions
// instead of solving the colors a second time.
struct NormalColorMatchPlan
{
    bool                                         valid = false;
    std::vector<std::string>                     physical_colors;
    std::vector<uint64_t>                        base_visible_stable_ids;
    std::vector<NormalColorMatchPlanEntry>       entries;
    std::vector<NormalColorMatchPlannedFilament> new_filaments;
};

struct NormalColorMatchPlanCommitResult
{
    bool        valid   = false;
    bool        created = false;
    std::string error;
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

MixedColorMatchRecipeResult build_best_color_match_recipe(const std::vector<std::string>& physical_colors,
                                                          const wxColour&                 target_color,
                                                          int                             min_component_percent,
                                                          const std::vector<double>&      physical_tds,
                                                          const std::vector<std::string>& physical_material_ids,
                                                          MixedFilamentColorEngine        color_engine,
                                                          bool                            use_td_prediction,
                                                          int                             max_component_percent = 100,
                                                          bool                            check_compatible       = true);

MixedColorMatchRecipeResult build_best_color_match_recipe(const std::vector<std::string>& physical_colors,
                                                          const wxColour&                 target_color,
                                                          int                             min_component_percent = 0,
                                                          int                             max_component_percent = 100,
                                                          bool                            check_compatible       = true);

MixedFilamentDefinition mixed_filament_definition_from_color_match_recipe(
    const MixedColorMatchRecipeResult& recipe,
    size_t                             num_physical,
    MixedColorMatchEncoding            encoding                = MixedColorMatchEncoding::LayerRatio,
    float                              reference_width_mm      = 0.4f,
    const std::vector<unsigned int>&   perimeter_component_ids = {});

// Reuse an exact physical or virtual color when possible; otherwise create a
// custom mixed filament using the active FullSpectrum color engine.
MixedColorMatchCreationResult create_mixed_filament_color_match(const wxColour&                 target_color,
                                                                const std::vector<std::string>& physical_colors,
                                                                int                             min_component_percent = 15,
                                                                size_t                  max_total_filaments = MAXIMUM_FILAMENT_NUMBER,
                                                                MixedColorMatchEncoding encoding = MixedColorMatchEncoding::LayerRatio,
                                                                const std::vector<unsigned int>& perimeter_component_ids = {},
                                                                ImageMap::ColorMixModel color_mix_model =
                                                                    ImageMap::ColorMixModel::FullSpectrumKmKs);

// Adaptive image maps are rendered only on the visible shell. Configure the
// active project even when every palette entry resolves directly to a physical
// filament and no virtual mixed-filament definition needs to be created.
void configure_adaptive_perimeter_color_match_mode();

NormalColorMatchPlan preview_normal_color_matches(const std::vector<wxColour>&    target_colors,
                                                  const std::vector<std::string>& physical_colors,
                                                  int                             min_component_percent = 15,
                                                  size_t                          max_total_filaments   = MAXIMUM_FILAMENT_NUMBER);

NormalColorMatchPlan build_normal_color_match_plan(const std::vector<wxColour>&       target_colors,
                                                   const std::vector<std::string>&    physical_colors,
                                                   int                                min_component_percent,
                                                   size_t                             max_total_filaments,
                                                   const MixedFilamentDisplayContext& context,
                                                   const MixedFilamentManager&        base_manager);

NormalColorMatchPlanCommitResult apply_normal_color_match_plan(
    const NormalColorMatchPlan       &plan,
    MixedFilamentManager             &manager,
    const MixedFilamentDisplayContext& context);

NormalColorMatchPlanCommitResult commit_normal_color_match_plan(const NormalColorMatchPlan& plan);

// Dry-run the same sequential reuse/creation decisions used when adaptive OBJ
// colors are committed. This lets the import dialog show the unique physical
// cycles that will actually appear instead of one duplicate card per region.
AdaptiveColorMatchPreviewResult preview_adaptive_localized_color_matches(
    const std::vector<wxColour>    &target_colors,
    const std::vector<std::string> &physical_colors,
    int                             min_component_percent = 15,
    size_t                          max_total_filaments = MAXIMUM_FILAMENT_NUMBER,
    ImageMap::ColorMixModel         color_mix_model = ImageMap::ColorMixModel::FullSpectrumKmKs);

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
    size_t                               sample_count = 17,
    ImageMap::ColorMixModel              color_mix_model = ImageMap::ColorMixModel::FullSpectrumKmKs);

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

// ---- Batch Match Mapping ----

/// Represents one color extracted from a multi-color model.
struct ModelColorEntry
{
    unsigned int              color_index;    // 1-based
    wxColour                  color;          // parsed wxColour for GUI operations
    std::string               hex_value;      // "#RRGGBB"
    std::vector<unsigned int> extruder_ids;   // extruder IDs (1-based) that use this color
};

/// One model color → mixed filament recipe mapping.
struct ColorMappingEntry
{
    unsigned int                  model_color_index;
    wxColour                      source_color;
    MixedColorMatchRecipeResult   recipe;
    unsigned int                  target_filament_id = 0;
    wxColour                      matched_color;
    double                        delta_e          = std::numeric_limits<double>::infinity();
    bool                          is_pure_recipe   = false;
    double                        pure_delta_e     = std::numeric_limits<double>::infinity();
    std::vector<unsigned int>     merged_model_indices;
    std::vector<unsigned int>     source_extruder_ids; // which model extruders need this mapping
    // Set when the confirm handler overwrites this recipe onto an EXISTING mixed
    // row (matched by stable id) instead of creating a new one.  Such a mapping
    // stays in the list so cleanup keeps its row (kept_mixed) and apply skips it,
    // but add_batch_custom_filaments / assigned_ids skip it.
    bool                          in_place_edited  = false;
};

/// Result of a full batch color-matching operation.
struct BatchMatchResult
{
    std::vector<unsigned int>      selected_physical_ids;
    std::vector<ColorMappingEntry> mappings;
    std::vector<Slic3r::MixedFilament> mixed_filaments;
    double                         avg_delta_e  = std::numeric_limits<double>::infinity();
    bool                           success      = false;
    std::string                    error_message;
    int                            error_code   = 0; // 0=ok, 1=partial, 2=cancelled, 3=timeout
    bool                           is_recommended_mode       = false;
    std::vector<std::string>       recommended_physical_colors;
};

/// Extract all unique colors from a multi-color 3D model.
/// Uses ModelVolume::get_extruders() → Print::config().filament_colour[].
/// Caps at 64 colors. Logs and skips malformed colors.
std::vector<ModelColorEntry> extract_model_colors(const Slic3r::Print& print);

/// Main entry: batch-match all model colors to filament recipes.
/// Callable from background thread (cancel_token checked per-color).
BatchMatchResult batch_match_model_colors(
    const std::vector<ModelColorEntry>&          model_colors,
    const std::vector<std::string>&             physical_colors,
    int                                          min_component_percent,
    int                                          max_component_percent = 100,
    std::shared_ptr<std::atomic<bool>>           cancel_token = nullptr,
    std::function<void(int,int)>                 progress_callback = nullptr,
    bool                                         check_compatible = true);

#if 0 // Dead code — no deduplication is performed (explicit policy since phase2)
/// Deduplicate mappings where matched colors are visually close (ΔE < 1.5).
std::vector<ColorMappingEntry> deduplicate_batch_mappings(
    const std::vector<ColorMappingEntry>& raw_mappings,
    double                                 merge_threshold = 1.5);
#endif // 0

/// Assign 1-based filament IDs: pure recipes → physical IDs (1-4), mixed → virtual.
/// Virtual IDs start at num_physical + existing_mixed_count + 1 to avoid colliding
/// with pre-existing mixed filaments already painted on the model.
void assign_batch_virtual_filament_ids(
    BatchMatchResult& result,
    size_t             num_physical,
    size_t             existing_mixed_count = 0);

/// Merge mappings whose NON-PURE recipes are byte-identical (same components + ratio +
/// gradient) into a single entry, so identical recipes share one virtual slot instead of
/// each creating a duplicate mixed-filament row. Pure-recipe mappings (is_pure_recipe)
/// are passed through untouched — they target existing physical IDs and never allocate a
/// new slot. The surviving mapping keeps the FIRST occurrence's target_filament_id and
/// accumulates the union of source_extruder_ids (so apply_batch_match_to_model still
/// remaps every source extruder) plus merged_model_indices. Order is preserved.
///
/// Identity fingerprint: {component_a, component_b, mix_b_percent, gradient_component_ids,
/// gradient_component_weights, manual_pattern}. Derived fields (preview_color, delta_e,
/// display_color) are excluded — identical recipes produce identical blends by construction.
std::vector<ColorMappingEntry> merge_duplicate_recipe_mappings(
    const std::vector<ColorMappingEntry>& mappings);

/// Populate result.mixed_filaments from result.mappings.
void populate_mixed_filaments_from_mappings(
    BatchMatchResult&                           result,
    const Slic3r::MixedFilamentDisplayContext&  context);

/// Apply matched recipes back to model painting data.
/// Walks every volume's mmu_segmentation_facets and remaps original
/// extruder_id→target_filament_id from the match result.
void apply_batch_match_to_model(const BatchMatchResult& result);

/// Recommend best 4-color filament combo from available presets.
/// Pre-filters to Top-15 by single-color ΔE, then scores C(15,4)=1365 combos.
/// Returns empty vector if fewer than 4 candidates available.
std::vector<std::string> recommend_best_filament_combo(
    const std::vector<ModelColorEntry>&  model_colors,
    const std::vector<std::string>&      all_preset_colors,
    int                                  min_component_percent = 15,
    int                                  max_component_percent = 100,
    std::shared_ptr<std::atomic<bool>>   cancel_token = nullptr);

}} // namespace Slic3r::GUI
