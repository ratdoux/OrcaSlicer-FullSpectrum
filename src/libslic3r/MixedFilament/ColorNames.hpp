#ifndef slic3r_MixedFilament_ColorNames_hpp_
#define slic3r_MixedFilament_ColorNames_hpp_

#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace Slic3r {

struct MixedFilamentDefinition;
struct MixedFilamentLegacyRow;
struct MixedFilamentDisplayContext;

namespace ColorNames {

struct DescriptionOptions
{
    bool include_details = false; // e.g. "  [2] 33% + [4] 67%", "  [3][4][2][4]", "  [3]->[2]"
    bool include_hex     = false; // e.g. " (#f5f5dc)"
    bool include_letter  = false; // e.g. "A: ..."
};

// Match input RGB / Hex color to closest standard CSS / SVG color keyword.
std::string closest_css_color_name(uint8_t r, uint8_t g, uint8_t b);
std::string closest_css_color_name(const std::string& hex_color);

// Standard Template Formatters:
// Format: "[Color name] [Material Mix/Gradient/Pattern] (optional)[physical index and percentages / pattern / gradient indices] (optional)[Hex Code]"
std::string format_description(const MixedFilamentDefinition&       definition,
                              const std::vector<std::string>&       physical_materials,
                              const std::vector<std::string>&       physical_colors,
                              const DescriptionOptions&             options = {},
                              const std::string&                    letter = "");

std::string format_description(const MixedFilamentLegacyRow&        row,
                              const std::vector<std::string>&       physical_materials,
                              const std::vector<std::string>&       physical_colors,
                              const DescriptionOptions&             options = {},
                              const std::string&                    letter = "");

std::string format_description(const std::vector<int>&              physical_indices_0based,
                              const std::vector<int>&               percentages,
                              const std::string&                    display_color_hex,
                              const std::vector<std::string>&       physical_materials,
                              const std::vector<std::string>&       physical_colors,
                              bool                                  is_gradient = false,
                              const DescriptionOptions&             options = {},
                              const std::string&                    letter = "");

// Convenience helpers
std::string descriptive_name(const MixedFilamentDefinition& definition,
                            const MixedFilamentDisplayContext& context,
                            const std::string& letter = "");

std::string tooltip_text(const MixedFilamentDefinition& definition,
                        const MixedFilamentDisplayContext& context,
                        bool include_hex = false,
                        const std::string& letter = "");

// Returns trimmed extra details string, e.g. "[1] 50% + [2] 50% (#00ffff)" or "[3]->[2] (#5f9ea0)" or "[1][2][1][2] (#ffffff)"
std::string extra_details(const MixedFilamentDefinition& definition, 
                         bool include_details = true,
                         bool include_hex = true);

std::string extra_details(const MixedFilamentLegacyRow& row,
                         size_t num_physical_filaments = 0,
                         bool include_details = true,
                         bool include_hex = true);

} // namespace ColorNames
} // namespace Slic3r

#endif // slic3r_MixedFilament_ColorNames_hpp_
