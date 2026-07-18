#ifndef slic3r_MixedFilament_hpp_
#define slic3r_MixedFilament_hpp_

#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>

namespace Slic3r {

enum class MixedFilamentColorEngine : uint8_t
{
    FilamentMixer = 0,
    FullSpectrumKSPairResidual = 1
};

// Persisted compact row used by existing project settings and older 3MF
// compatibility paths. New domain/UI code should use MixedFilamentDefinition;
// this legacy row is a serialization adapter only.
struct MixedFilamentLegacyRow
{
    enum DistributionMode : uint8_t {
        LayerCycle = 0,
        SameLayerPointillisme = 1,
        Simple = 2
    };

    static constexpr float k_default_gradient_dominant = 0.8f;
    static constexpr float k_default_gradient_minority = 0.2f;
    static constexpr float k_min_gradient_difference   = 0.05f;

    // 1-based physical filament IDs that are combined.
    unsigned int component_a = 1;
    unsigned int component_b = 2;

    // Persistent row identity used to keep painted virtual-tool assignments
    // stable even when the visible mixed-filament list is rebuilt.
    uint64_t stable_id = 0;

    // Layer-alternation ratio.  With ratio_a = 2, ratio_b = 1 the cycle is
    // A, A, B, A, A, B, ...
    int ratio_a = 1;
    int ratio_b = 1;

    // Blend percentage of component B in [0..100].
    int mix_b_percent = 50;

    // Optional manual pattern for this mixed filament. Tokens:
    // '1' => component_a, '2' => component_b, '3'..'9' => direct physical
    // filament IDs (1-based). Example: "11112222" => AAAABBBB repeating.
    std::string manual_pattern;

    // Optional explicit gradient multi-color component list, encoded as
    // compact physical filament IDs (for example "123" -> filaments 1,2,3).
    // Interleaved stripe mode is active for gradient rows only when this list has 3+ IDs.
    std::string gradient_component_ids;
    // Optional explicit multi-color weights aligned with gradient_component_ids.
    // Compact integer list joined by '/': for example "50/25/25".
    std::string gradient_component_weights;
    // Optional explicit gradient stop positions. Encoded as a '/'-joined list
    // matching the UI stop curve: filament stop, midpoint, filament stop, ...
    std::string gradient_stop_positions;

    // Legacy UI fields retained for dialogs and badges that still consume
    // mixed rows directly. Typed definitions remain the source of truth.
    bool pointillism_all_filaments = false;
    bool gradient_enabled = false;
    float gradient_start = k_default_gradient_dominant;
    float gradient_end   = k_default_gradient_minority;

    // How this mixed row is distributed:
    // - LayerCycle: one filament per layer based on cadence.
    // - Simple: regular pair blend. Retired serialized value 1 is accepted by
    //   the loader and normalized before reaching runtime code.
    int distribution_mode = int(Simple);

    // Optional Local-Z cap for this mixed row. 0 disables the cap.
    int local_z_max_sublayers = 0;

    // Additional XY surface offsets, in mm, applied when this mixed row
    // resolves to component A or B for an entire layer. Positive values
    // contract inward; negative values expand outward.
    float component_a_surface_offset = 0.f;
    float component_b_surface_offset = 0.f;
    // Slash-separated offsets aligned with gradient_component_ids. Empty
    // means the legacy A/B fields are authoritative.
    std::string component_surface_offsets;

    // Apply component surface offsets to the visible external-perimeter
    // centerline only. This is used by shared-cadence image maps; ordinary
    // mixed-filament bias continues to offset the complete region mask.
    bool perimeter_modulation = false;

    // Legacy availability bit kept so old project settings and 3MF rows keep
    // their column layout. Runtime treats every non-deleted row as available.
    bool enabled = true;

    // True when this mixed filament row was deleted from UI and should stay hidden.
    bool deleted = false;

    // True when this row was user-created (custom) instead of auto-generated.
    bool custom = false;

    // True when this row originated from an auto-generated pair. This remains
    // true even after editing so delete logic can keep the base auto pair
    // tombstoned instead of letting regeneration resurrect it.
    bool origin_auto = false;

    // Computed display colour as "#RRGGBB".
    std::string display_color;

    // UI mode that created this row (-1=unknown/legacy, 0=RATIO, 1=CYCLE,
    // 2=MATCH, 3=GRADIENT).
    int ui_mode = -1;

    bool operator==(const MixedFilamentLegacyRow &rhs) const
    {
        constexpr float k_surface_offset_epsilon = 1e-6f;
        return component_a == rhs.component_a &&
               component_b == rhs.component_b &&
               stable_id   == rhs.stable_id   &&
               ratio_a     == rhs.ratio_a     &&
               ratio_b     == rhs.ratio_b     &&
               mix_b_percent == rhs.mix_b_percent &&
               manual_pattern == rhs.manual_pattern &&
               gradient_component_ids == rhs.gradient_component_ids &&
               gradient_component_weights == rhs.gradient_component_weights &&
               gradient_stop_positions == rhs.gradient_stop_positions &&
               pointillism_all_filaments == rhs.pointillism_all_filaments &&
               gradient_enabled == rhs.gradient_enabled &&
               std::abs(gradient_start - rhs.gradient_start) <= 1e-4f &&
               std::abs(gradient_end - rhs.gradient_end) <= 1e-4f &&
               distribution_mode == rhs.distribution_mode &&
               local_z_max_sublayers == rhs.local_z_max_sublayers &&
               std::abs(component_a_surface_offset - rhs.component_a_surface_offset) <= k_surface_offset_epsilon &&
               std::abs(component_b_surface_offset - rhs.component_b_surface_offset) <= k_surface_offset_epsilon &&
               component_surface_offsets == rhs.component_surface_offsets &&
               perimeter_modulation == rhs.perimeter_modulation &&
               deleted      == rhs.deleted &&
               custom       == rhs.custom &&
               origin_auto  == rhs.origin_auto &&
               ui_mode      == rhs.ui_mode;
    }
    bool operator!=(const MixedFilamentLegacyRow &rhs) const { return !(*this == rhs); }
};

using MixedFilament = MixedFilamentLegacyRow;
using MixedFilamentStableId = uint64_t;

struct MixedFilamentPhysicalRef
{
    // Runtime physical filament id. Kept 1-based to match the existing slicer pipeline.
    unsigned int id = 0;
};

struct MixedFilamentWeightedComponent
{
    MixedFilamentPhysicalRef filament;
    int                      percent = 0;
};

struct MixedFilamentPrimaryPairView
{
    MixedFilamentPhysicalRef component_a;
    MixedFilamentPhysicalRef component_b;
    int                      component_b_percent = 50;
};

struct MixedFilamentWeightedBlend
{
    // Weighted physical filament refs. A two-component blend is the ordinary
    // pair blend; 3+ components is the explicit multi-color blend path.
    // components[0] and components[1] remain the primary A/B pair for legacy-row
    // compatibility, manual-pattern tokens, Local-Z, and surface bias.
    std::vector<MixedFilamentWeightedComponent> components;

    bool is_pair() const { return components.size() == 2; }

    // Pair-shaped view for legacy adapters and slicer behavior that still
    // needs an A/B interpretation. The weighted component array remains the
    // source of truth.
    std::optional<MixedFilamentPrimaryPairView> primary_pair() const;
    MixedFilamentPrimaryPairView                primary_pair_or(unsigned int component_a = 1,
                                                                unsigned int component_b = 2,
                                                                int          component_b_percent = 50) const;

    // UI/package-friendly array views over the weighted component source.
    std::vector<unsigned int> component_ids(size_t num_physical = 0) const;
    std::vector<int>          component_percents(size_t num_physical = 0) const;
};

struct MixedFilamentIdentity
{
    MixedFilamentStableId stable_id = 0;
};

enum class MixedFilamentSourceKind : uint8_t
{
    AutoGenerated,
    Custom
};

struct MixedFilamentSource
{
    MixedFilamentSourceKind kind = MixedFilamentSourceKind::AutoGenerated;

    // True when this row belongs to an auto-generated base pair, even if it was edited.
    bool origin_auto = false;
};

struct MixedFilamentVisibility
{
    // Legacy `deleted`: remembered so regeneration does not resurrect the row.
    bool tombstoned = false;
};

struct MixedFilamentManualPattern
{
    // One group means a simple repeating cadence; multiple groups map to
    // perimeter/wall groups. Entries are resolved physical filament ids.
    std::vector<std::vector<MixedFilamentPhysicalRef>> groups;
};

enum class MixedFilamentRecipeKind : uint8_t
{
    WeightedBlend,
    ManualPattern
};

struct MixedFilamentRecipe
{
    MixedFilamentWeightedBlend blend;

    // Manual patterns keep their exact perimeter/group sequence. `blend`
    // remains populated as the aggregate weighted view for UI summaries,
    // display color, local-Z, and legacy compatibility.
    std::optional<MixedFilamentManualPattern> manual_pattern;

    MixedFilamentRecipeKind kind = MixedFilamentRecipeKind::WeightedBlend;
};

enum class MixedFilamentDistributionMode : uint8_t
{
    LayerCycle,
    Simple
};

struct MixedFilamentLayerCadence
{
    int component_a_layers = 1;
    int component_b_layers = 1;
};

struct MixedFilamentLocalZBehavior
{
    int max_sublayers = 0;
};

struct MixedFilamentGradientBehavior
{
    bool  enabled = false;
    float component_a_start = MixedFilamentLegacyRow::k_default_gradient_dominant;
    float component_a_end   = MixedFilamentLegacyRow::k_default_gradient_minority;
    // Normalized UI stop curve, length 2 * component_count - 1 when present.
    std::vector<float> stop_positions;
};

struct MixedFilamentSurfaceBias
{
    // Signed offsets aligned with recipe.blend.components. Positive values
    // contract inward and negative values expand outward. When empty, the
    // legacy A/B fields below are expanded as A, B, B, ... for compatibility.
    std::vector<float> component_offsets_mm;
    float component_a_offset_mm = 0.f;
    float component_b_offset_mm = 0.f;
    bool  perimeter_modulation = false;
};

struct MixedFilamentBehavior
{
    MixedFilamentDistributionMode distribution = MixedFilamentDistributionMode::Simple;
    MixedFilamentLayerCadence     layer_cadence;
    MixedFilamentLocalZBehavior   local_z;
    MixedFilamentGradientBehavior gradient;
    MixedFilamentSurfaceBias      surface_bias;
};

struct MixedFilamentPresentation
{
    std::string display_color;
};

// Typed, decoded mixed-filament model. This is the
// preferred boundary for UI and package code; compact legacy strings stay in
// adapters and persistence.
struct MixedFilamentDefinition
{
    MixedFilamentIdentity     identity;
    MixedFilamentSource       source;
    MixedFilamentVisibility   visibility;
    MixedFilamentRecipe       recipe;
    MixedFilamentBehavior     behavior;
    MixedFilamentPresentation presentation;
};

struct MixedFilamentPreviewSettings
{
    double nominal_layer_height { 0.2 };
    double min_sublayer_height { 0.06 };
    double preferred_a_height { 0.0 };
    double preferred_b_height { 0.0 };
    bool   local_z_mode { false };
    bool   local_z_direct_multicolor { false };
    size_t wall_loops { 1 };
};

struct MixedFilamentDisplayContext
{
    size_t                       num_physical { 0 };
    std::vector<std::string>     physical_colors;
    std::vector<double>          nozzle_diameters;
    MixedFilamentPreviewSettings preview_settings;
    bool                         component_bias_enabled { false };
    std::vector<double>          physical_tds;
    std::vector<std::string>     physical_material_ids;
};

struct MixedFilamentColorInput
{
    std::string                color_hex;
    int                        percent = 0;
    std::optional<double>      td_mm;
    std::optional<std::string> material_id;
};

std::pair<double, double> mixed_filament_local_z_pair_heights(double nominal_layer_height,
                                                               double min_sublayer_height,
                                                               int    mix_b_percent);
bool mixed_filament_definition_uses_local_z(const MixedFilamentDefinition &definition,
                                             bool                           global_local_z_enabled);
bool mixed_filament_local_z_uses_full_domain(bool global_full_domain_enabled,
                                              bool mixed_filament_assigned_to_region);
bool mixed_filament_local_z_painted_override_uses_planner(bool painted_state_is_mixed,
                                                           bool painted_mixed_row_uses_local_z);
bool mixed_filament_local_z_should_subdivide_layer(size_t layer_id,
                                                    bool   whole_object_mode,
                                                    bool   preserve_first_layer);
std::vector<double> mixed_filament_local_z_preview_pass_heights(double nominal_layer_height,
                                                                 double min_sublayer_height,
                                                                 double preferred_a_height,
                                                                 double preferred_b_height,
                                                                 int    mix_b_percent,
                                                                 int    max_sublayers_limit = 0);
int mixed_filament_effective_local_z_preview_mix_b_percent(const MixedFilamentDefinition     &definition,
                                                           const MixedFilamentPreviewSettings &preview_settings);
bool mixed_filament_supports_bias_apparent_color(const MixedFilamentDefinition     &definition,
                                                 const MixedFilamentPreviewSettings &preview_settings,
                                                 bool                                bias_mode_enabled);
std::pair<int, int> mixed_filament_apparent_pair_percentages(const MixedFilamentDefinition     &definition,
                                                             const MixedFilamentPreviewSettings &preview_settings,
                                                             const std::vector<double>          &nozzle_diameters,
                                                             bool                                bias_mode_enabled);
std::string compute_mixed_filament_display_color(const MixedFilamentDefinition &definition, const MixedFilamentDisplayContext &context);
std::string compute_mixed_filament_display_color(const MixedFilamentLegacyRow &row, const MixedFilamentDisplayContext &context);
std::vector<float> mixed_filament_component_surface_offsets(const MixedFilamentDefinition &definition);
void set_mixed_filament_component_surface_offsets(MixedFilamentDefinition &definition, const std::vector<float> &offsets_mm);
std::vector<float> mixed_filament_surface_offsets_for_apparent_percentages(
    const std::vector<int> &component_percents,
    const std::vector<int> &target_apparent_percents,
    float                   reference_width_mm = 0.4f);
std::vector<float> mixed_filament_surface_offsets_for_apparent_weights(const std::vector<int>&    component_percents,
                                                                       const std::vector<double>& target_apparent_weights,
                                                                       float                      reference_width_mm = 0.4f);

MixedFilamentDefinition mixed_filament_definition_from_legacy_row(const MixedFilamentLegacyRow &row, size_t num_physical = 0);
void                    apply_mixed_filament_definition_to_legacy_row(const MixedFilamentDefinition &definition,
                                                                      MixedFilamentLegacyRow                 &row);
MixedFilamentLegacyRow           mixed_filament_legacy_row_from_definition(const MixedFilamentDefinition &definition);
std::optional<MixedFilamentManualPattern> mixed_filament_manual_pattern_from_string(const std::string &pattern,
                                                                                    unsigned int       component_a,
                                                                                    unsigned int       component_b,
                                                                                    size_t             num_physical = 0);
std::string mixed_filament_manual_pattern_string(const MixedFilamentDefinition &definition);
std::vector<unsigned int> mixed_filament_manual_pattern_sequence(const MixedFilamentDefinition &definition,
                                                                 size_t                         num_physical = 0);
std::vector<unsigned int> mixed_filament_manual_pattern_preview_sequence(const MixedFilamentDefinition &definition,
                                                                         size_t                         num_physical,
                                                                         size_t                         wall_loops);
std::vector<unsigned int> mixed_filament_weighted_blend_sequence(const MixedFilamentDefinition &definition,
                                                                 size_t                         num_physical = 0);

// ---------------------------------------------------------------------------
// MixedFilamentManager
//
// Owns the list of mixed filaments and provides helpers used by the slicing
// pipeline to resolve virtual IDs back to physical extruders.
//
// Virtual filament IDs are numbered starting at (num_physical + 1).  For a
// 4-extruder printer the first mixed filament has ID 5, the second 6, etc.
// ---------------------------------------------------------------------------
class MixedFilamentManager
{
public:
    MixedFilamentManager() = default;

    static void set_auto_generate_enabled(bool enabled);
    static bool auto_generate_enabled();
    static void set_color_engine(MixedFilamentColorEngine engine);
    static MixedFilamentColorEngine color_engine();
    static MixedFilamentColorEngine color_engine_from_string(const std::string &value);
    static const char *color_engine_to_string(MixedFilamentColorEngine engine);
    static void set_use_td_for_color_prediction(bool enabled);
    static bool use_td_for_color_prediction();

    // ---- Auto-generation ------------------------------------------------

    // Rebuild the mixed-filament list from the current set of physical
    // filament colours.  Generates all C(N,2) pairwise combinations.
    // Previous ratio/deleted state is preserved when a combination still
    // exists.
    void auto_generate(const std::vector<std::string> &filament_colours);

    // Remove a physical filament (1-based ID) from the mixed list.
    // Any mixed filament that contains the removed component is deleted.
    // Remaining component IDs are shifted down to stay aligned with physical IDs.
    void remove_physical_filament(unsigned int deleted_filament_id);

    // Add a custom mixed filament.
    void add_custom_filament(unsigned int component_a, unsigned int component_b, int mix_b_percent, const std::vector<std::string> &filament_colours);
    bool add_custom_filament_definition(MixedFilamentDefinition definition, const std::vector<std::string> &filament_colours);

    // Remove all custom rows, keep auto-generated ones.
    void clear_custom_entries();

    // Recompute cadence ratios from gradient settings.
    // gradient_mode: 0 = Layer cycle weighted, 1 = Height weighted.
    void apply_gradient_settings(int   gradient_mode,
                                 float nominal_layer_height,
                                 float min_sublayer_height,
                                 bool  advanced_dithering = false);

    // Persist mixed rows, including auto/deleted state, into the compact
    // project-settings string.
    std::string serialize_custom_entries();
    void load_custom_entries(const std::string &serialized, const std::vector<std::string> &filament_colours);

    // Normalize a manual mixed-pattern string into compact token form.
    // Accepts separators and A/B aliases. Returns empty string if invalid.
    static constexpr size_t kMaxPhysicalFilaments = 9;
    static std::string normalize_manual_pattern(const std::string &pattern);
    static int         mix_percent_from_manual_pattern(const std::string &pattern);
    static std::vector<unsigned int> decode_gradient_component_ids(const std::string &components,
                                                                   size_t             num_physical = kMaxPhysicalFilaments);
    static std::string encode_gradient_component_ids(const std::vector<unsigned int> &component_ids);
    static std::vector<std::string> split_pattern_groups(const std::string &pattern);
    static std::vector<std::string> split_pattern_group_to_tokens(const std::string &group,
                                                                  size_t             num_physical = 0);
    static unsigned int physical_filament_from_token(const std::string            &token,
                                                     const MixedFilamentLegacyRow &row,
                                                     size_t                       num_physical = kMaxPhysicalFilaments);

    // ---- Queries --------------------------------------------------------

    // True when `filament_id` (1-based) refers to a mixed filament.
    bool is_mixed(unsigned int filament_id, size_t num_physical) const
    {
        return mixed_index_from_filament_id(filament_id, num_physical) >= 0;
    }

    // Resolve a mixed filament ID to a physical extruder (1-based) for the
    // given layer context. Returns `filament_id` unchanged when it is not a
    // mixed filament.
    unsigned int resolve(unsigned int filament_id,
                         size_t       num_physical,
                         int          layer_index,
                         float        layer_print_z = 0.f,
                         float        layer_height  = 0.f,
                         bool         force_height_weighted = false) const;
    unsigned int resolve_perimeter(unsigned int filament_id,
                                   size_t       num_physical,
                                   int          layer_index,
                                   int          perimeter_index,
                                   float        layer_print_z = 0.f,
                                   float        layer_height  = 0.f,
                                   bool         force_height_weighted = false) const;
    // Resolve the filament ID that should own painted regions on this layer.
    // Modes that require virtual identity later in G-code generation keep the
    // original mixed ID; ordinary mixed rows collapse to the current physical
    // extruder so adjacent same-tool regions can merge.
    unsigned int effective_painted_region_filament_id(unsigned int filament_id,
                                                      size_t       num_physical,
                                                      int          layer_index,
                                                      float        layer_print_z = 0.f,
                                                      float        layer_height  = 0.f,
                                                      float        layer_height_a = 0.f,
                                                      float        layer_height_b = 0.f,
                                                      float        base_layer_height = 0.2f) const;
    float component_surface_offset(unsigned int filament_id,
                                   size_t       num_physical,
                                   int          layer_index,
                                   float        layer_print_z = 0.f,
                                   float        layer_height  = 0.f,
                                   bool         force_height_weighted = false) const;
    std::vector<unsigned int> ordered_perimeter_extruders(unsigned int filament_id,
                                                          size_t       num_physical,
                                                          int          layer_index,
                                                          float        layer_print_z = 0.f,
                                                          float        layer_height  = 0.f,
                                                          bool         force_height_weighted = false) const;

    // Map virtual filament ID (1-based, after physical IDs) to index into
    // m_definitions. Virtual IDs enumerate non-deleted mixed rows only.
    int mixed_index_from_filament_id(unsigned int filament_id, size_t num_physical) const;

    // Blend N colours using the selected display-color engine.
    // color_percents: vector of (hex_color, percent) where percents sum to 100.
    static std::string blend_color_multi(
        const std::vector<std::pair<std::string, int>> &color_percents);
    static std::string blend_color_multi(
        const std::vector<MixedFilamentColorInput> &color_percents);

    std::optional<MixedFilamentDefinition> mixed_filament_definition_from_id(unsigned int filament_id, size_t num_physical) const;

    // Compute a display colour by blending two colours with the selected engine.
    static std::string blend_color(const std::string &color_a,
                                   const std::string &color_b,
                                   int ratio_a, int ratio_b);
    static std::string blend_color(const std::string           &color_a,
                                   const std::string           &color_b,
                                   int                          ratio_a,
                                   int                          ratio_b,
                                   const std::optional<double> &td_a_mm,
                                   const std::optional<double> &td_b_mm);
    static std::string blend_color(const std::string                &color_a,
                                   const std::string                &color_b,
                                   int                               ratio_a,
                                   int                               ratio_b,
                                   const std::optional<double>      &td_a_mm,
                                   const std::optional<double>      &td_b_mm,
                                   const std::optional<std::string> &material_id_a,
                                   const std::optional<std::string> &material_id_b);
    static float max_component_surface_offset_mm(float reference_width_mm = 0.4f);
    static float max_pair_bias_mm(float reference_width_mm = 0.4f);
    static std::pair<float, float> surface_offset_pair_from_signed_bias(float bias_mm,
                                                                        float reference_width_mm = 0.4f);
    static float bias_ui_value_from_surface_offsets(float component_a_surface_offset,
                                                    float component_b_surface_offset,
                                                    float reference_width_mm = 0.4f);
    static int apparent_mix_b_percent(int   mix_b_percent,
                                      float component_a_surface_offset,
                                      float component_b_surface_offset,
                                      float reference_width_mm = 0.4f);
    static std::vector<int> apparent_component_percentages(const std::vector<int> &component_percents,
                                                            float                   component_a_surface_offset,
                                                            float                   component_b_surface_offset,
                                                            float                   reference_width_mm = 0.4f);
    static std::vector<int> apparent_component_percentages(const std::vector<int>   &component_percents,
                                                            const std::vector<float> &component_surface_offsets,
                                                            float                    reference_width_mm = 0.4f);

    // ---- Accessors ------------------------------------------------------

    std::vector<MixedFilamentDefinition> mixed_filament_definitions(size_t num_physical = 0) const;
    std::vector<size_t> mixed_filaments_using_physical(unsigned int physical_filament_id) const;
    void expand_virtual_extruder_ids(std::vector<int> &filament_ids, size_t num_physical) const;
    bool set_mixed_filament_definition(size_t                          index,
                                       const MixedFilamentDefinition  &definition,
                                       const std::vector<std::string> &filament_colours = {});

    void set_mixed_filament_definitions(std::vector<MixedFilamentDefinition> definitions,
                                        const std::vector<std::string>       &filament_colours = {});
    bool set_mixed_filament_legacy_row(size_t                          index,
                                       const MixedFilamentLegacyRow             &row,
                                       size_t                          num_physical = 0,
                                       const std::vector<std::string> &filament_colours = {});
    void set_mixed_filament_legacy_rows(const std::vector<MixedFilamentLegacyRow> &rows,
                                        size_t                            num_physical = 0,
                                        const std::vector<std::string>   &filament_colours = {});

    // Read-only legacy-row snapshot for persistence and compatibility tests.
    // The manager owns typed definitions; this vector is rebuilt from them on demand.
    const std::vector<MixedFilamentLegacyRow> &mixed_filament_legacy_rows() const;
    std::vector<MixedFilamentLegacyRow>       &mixed_filaments();
    const std::vector<MixedFilamentLegacyRow> &mixed_filaments() const;
    MixedFilamentLegacyRow                    *mixed_filament_from_id(unsigned int filament_id, size_t num_physical);
    const MixedFilamentLegacyRow              *mixed_filament_from_id(unsigned int filament_id, size_t num_physical) const;
    size_t                           mixed_filament_count() const;

    size_t visible_count() const;

    // Total filament count = num_physical + number of visible mixed filaments.
    size_t total_filaments(size_t num_physical) const { return num_physical + visible_count(); }

    // Return the display colours of all visible mixed filaments (in order).
    std::vector<std::string> display_colors() const;
    void set_display_context(const MixedFilamentDisplayContext &context);
    void refresh_display_colors(const std::vector<std::string> &filament_colours);

private:
    // Convert a 1-based virtual ID to a 0-based index into m_definitions.
    size_t index_of(unsigned int filament_id, size_t num_physical) const
    {
        return static_cast<size_t>(filament_id - num_physical - 1);
    }

    void invalidate_legacy_cache() const;
    void rebuild_legacy_cache() const;
    void sync_mutable_legacy_cache_to_definitions(size_t num_physical = 0);
    uint64_t allocate_stable_id();
    uint64_t normalize_stable_id(uint64_t stable_id);

    std::vector<MixedFilamentDefinition> m_definitions;
    mutable std::vector<MixedFilamentLegacyRow>   m_legacy_cache;
    mutable bool                         m_legacy_cache_dirty = true;
    mutable bool                         m_legacy_cache_mutable_borrowed = false;
    int                                  m_gradient_mode          = 0;
    float                                m_nominal_layer_height   = 0.2f;
    float                                m_min_sublayer_height    = 0.06f;
    bool                                 m_advanced_dithering  = false;
    uint64_t                             m_next_stable_id      = 1;
    MixedFilamentDisplayContext m_display_context;
};

} // namespace Slic3r

#endif /* slic3r_MixedFilament_hpp_ */
