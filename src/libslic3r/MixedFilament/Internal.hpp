#ifndef slic3r_MixedFilament_Internal_hpp_
#define slic3r_MixedFilament_Internal_hpp_

#include "../MixedFilament.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Slic3r { namespace MixedFilamentInternal {

struct RGB
{
    int r = 0, g = 0, b = 0;
};

struct MixedFilamentLegacyPair
{
    MixedFilamentPhysicalRef component_a { 1 };
    MixedFilamentPhysicalRef component_b { 2 };
};

// Common identity, colour, numeric, and cadence helpers.
uint64_t    canonical_pair_key(unsigned int a, unsigned int b);
RGB         parse_hex_color(const std::string& hex);
std::string rgb_to_hex(const RGB& c);
int         clamp_int(int v, int lo, int hi);
float       clamp_surface_offset(float v);
bool        parse_invariant_float(std::string_view text, float& output);
std::string format_invariant_decimal(double value, int precision);
float       canonical_signed_bias_value(float component_a_surface_offset, float component_b_surface_offset);
std::string format_surface_offset_token(float value);
void compute_gradient_heights_from_mix(int mix_b_percent,
                                       float nominal_layer_height,
                                       float min_sublayer_height,
                                       float& h_a,
                                       float& h_b);
std::pair<int, int> gradient_ratios_from_mix(int mix_b_percent,
                                             int gradient_mode,
                                             float nominal_layer_height,
                                             float min_sublayer_height);
int         safe_mod(int x, int m);
bool        use_component_b_advanced_dither(int layer_index, int ratio_a, int ratio_b);
double mixed_filament_reference_nozzle_mm(unsigned int component_a, unsigned int component_b, const std::vector<double>& nozzle_diameters);

// Compact mixed_filament_definitions row compatibility.
bool                          parse_row_definition(const std::string& row,
                                                   unsigned int&      a,
                                                   unsigned int&      b,
                                                   uint64_t&          stable_id,
                                                   bool&              enabled,
                                                   bool&              custom,
                                                   bool&              origin_auto,
                                                   int&               mix_b_percent,
                                                   std::string&       gradient_component_ids,
                                                   std::string&       gradient_component_weights,
                                                   std::string&       gradient_stop_positions,
                                                   std::string&       manual_pattern,
                                                   int&               distribution_mode,
                                                   int&               local_z_max_sublayers,
                                                   float&             component_a_surface_offset,
                                                   float&             component_b_surface_offset,
                                                   std::string&       component_surface_offsets,
                                                   bool&              perimeter_modulation,
                                                   bool&              deleted,
                                                   bool&              gradient_enabled,
                                                   float&             gradient_start,
                                                   float&             gradient_end,
                                                   int&               ui_mode);
int                           normalize_legacy_distribution_mode(int distribution_mode, const std::string& gradient_component_ids);
void                          normalize_legacy_row(MixedFilamentLegacyRow& mf);
MixedFilamentDistributionMode mixed_filament_distribution_from_legacy_mode(int                distribution_mode,
                                                                            const std::string& gradient_component_ids);
int                           legacy_distribution_mode_from_mixed_filament_distribution(MixedFilamentDistributionMode distribution);

// Manual pattern parsing and legacy-row adapters.
bool                                      is_pattern_separator(char c);
bool                                      decode_pattern_step(char c, char& out);
std::vector<std::string>                  split_manual_pattern_groups(const std::string& pattern);
std::string                               flatten_manual_pattern_groups(const std::string& pattern);
int                                       mix_percent_from_normalized_pattern(const std::string& pattern);
unsigned int                              physical_filament_from_legacy_pattern_token(char token, const MixedFilamentLegacyPair& pair);
std::string legacy_manual_pattern_from_mixed_filament_pattern(const MixedFilamentManualPattern& pattern, const MixedFilamentLegacyPair& pair);

// Legacy-row weighted component and weight helpers.
std::string               normalize_gradient_component_ids(const std::string& components);
std::vector<unsigned int> decode_gradient_component_ids(const std::string& components, size_t num_physical);
std::vector<int>          parse_gradient_weight_tokens(const std::string& weights);
std::vector<int>          normalize_weight_vector_to_percent(const std::vector<int>& weights);
std::string               normalize_gradient_component_weights(const std::string& weights, size_t expected_components);
std::vector<int>          decode_gradient_component_weights(const std::string& weights, size_t expected_components);
std::vector<unsigned int> build_weighted_gradient_sequence(const std::vector<unsigned int>& ids, const std::vector<int>& weights);
std::vector<float>        parse_gradient_stop_position_tokens(const std::string& positions);
std::vector<float>        normalize_gradient_stop_position_vector(const std::vector<float>& positions, size_t expected_stops);
std::string               normalize_gradient_stop_positions(const std::string& positions, size_t expected_stops);
std::vector<float>        decode_gradient_stop_positions(const std::string& positions, size_t expected_stops);
std::string               legacy_gradient_positions_from_float_vector(const std::vector<float>& positions);
std::vector<int>          equal_percent_vector(size_t count);
bool                      has_positive_sum(const std::vector<int>& values);
std::vector<int>          normalized_percent_vector_or_equal(const std::vector<int>& weights, size_t count);
std::optional<MixedFilamentWeightedBlend> mixed_filament_weighted_blend_from_legacy_row(const MixedFilamentLegacyRow& row,
                                                                                       size_t                        num_physical);
std::string                               legacy_gradient_weights_from_percent_vector(const std::vector<int>& weights);
void apply_mixed_filament_blend_to_legacy_row(const MixedFilamentWeightedBlend& blend, MixedFilamentLegacyRow& row);

// Preview/display sequence helpers.
unsigned int decode_manual_pattern_preview_token(char token, unsigned int component_a, unsigned int component_b, size_t num_physical);
std::vector<unsigned int> build_grouped_manual_pattern_preview_sequence(
    const std::string& pattern, unsigned int component_a, unsigned int component_b, size_t num_physical, size_t wall_loops);
std::pair<int, int>       effective_pair_preview_ratios(int percent_b);
std::vector<unsigned int> build_effective_pair_preview_sequence(unsigned int component_a,
                                                                unsigned int component_b,
                                                                int          percent_b,
                                                                bool         limit_cycle);
std::string               blend_display_color_from_sequence(const std::vector<std::string>&  colors,
                                                            const std::vector<double>&       tds,
                                                            const std::vector<std::string>&  material_ids,
                                                            size_t                           num_physical,
                                                            const std::vector<unsigned int>& sequence,
                                                            const std::string&               fallback);
}} // namespace Slic3r::MixedFilamentInternal

#endif /* slic3r_MixedFilament_Internal_hpp_ */
