#ifndef slic3r_MixedFilament_ColorNames_hpp_
#define slic3r_MixedFilament_ColorNames_hpp_

#include <string>
#include <vector>
#include <cstdint>
#include <optional>
#include "../MixedFilament.hpp"

namespace Slic3r {

struct MixedFilamentDefinition;
struct MixedFilamentLegacyRow;
struct MixedFilamentDisplayContext;
struct MixedFilamentWeightedComponent;
struct MixedFilamentManualPattern;

namespace ColorNames {

struct DescriptionOptions
{
    bool include_letter     = false; // e.g. "A: "
    bool include_color      = true; // e.g. "Tan"
    bool include_material   = true; // e.g. "PLA + PETG"
    bool include_kind       = true; // e.g. "Mix", "Gradient", "Pattern"
    bool include_components = false; // e.g. "  [2] 33% + [4] 67%", "  [3][4][2][4]", "  [3]->[2]"
    bool include_hex        = false; // e.g. " (#f5f5dc)"

    DescriptionOptions() = default;
};


// Match input RGB / Hex color to closest standard CSS / SVG color keyword.
std::string closest_css_color_name(uint8_t r, uint8_t g, uint8_t b);
std::string closest_css_color_name(const std::string& hex_color);

// Standard Template Formatters:
// Format: "[Color name] [Material Mix/Gradient/Pattern] (optional)[physical index and percentages / pattern / gradient indices] (optional)[Hex Code]"
std::string mixed_filament_name(const MixedFilamentDefinition&      definition,
                              const std::vector<std::string>&       physical_materials,
                              const std::vector<std::string>&       physical_colors,
                              const DescriptionOptions&             options = DescriptionOptions(),
                              const std::string&                    letter = "");

std::string mixed_filament_name(const MixedFilamentLegacyRow&        row,
                              const std::vector<std::string>&       physical_materials,
                              const std::vector<std::string>&       physical_colors,
                              const DescriptionOptions&             options = DescriptionOptions(),
                              const std::string&                    letter = "");

std::string mixed_filament_name(const std::vector<int>&             physical_indices_0based,
                              const std::vector<int>&               percentages,
                              const std::string&                    display_color_hex,
                              const std::vector<std::string>&       physical_materials,
                              const std::vector<std::string>&       physical_colors,
                              const DescriptionOptions&             options = DescriptionOptions(),
                              const std::string&                    letter = "");

bool is_gradient(const MixedFilamentDefinition& definition);
bool is_pattern(const MixedFilamentDefinition& definition);

std::string mf_color(const MixedFilamentDefinition& definition, const std::vector<std::string>& physical_colors);
std::string mf_material(const std::vector<unsigned int>& component_ids, const std::vector<std::string>& physical_materials);
std::string mf_kind(const MixedFilamentDefinition& definition);
std::string mf_components(const MixedFilamentDefinition& definition);
std::string mf_components_mix(const std::vector<MixedFilamentWeightedComponent>& weightedComponents);
std::string mf_components_pattern(const MixedFilamentManualPattern& manualPattern);
std::string mf_components_gradient(const std::vector<MixedFilamentWeightedComponent>& weightedComponents);
std::string mf_hex(const std::string& hex_color);

} // namespace ColorNames
} // namespace Slic3r

#endif // slic3r_MixedFilament_ColorNames_hpp_
