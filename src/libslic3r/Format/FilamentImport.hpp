#ifndef slic3r_FilamentImport_hpp_
#define slic3r_FilamentImport_hpp_

#include "../PrintConfig.hpp"

#include <cstddef>

namespace Slic3r {

struct FilamentImportPlan
{
    size_t physical_count      = 0;
    bool   map_overflow_colors = false;
};

inline FilamentImportPlan plan_filament_import(const DynamicPrintConfig& config, size_t physical_count, size_t conversion_limit)
{
    // Existing mixed IDs follow the saved physical palette. A map built only
    // from physical colors cannot preserve those IDs or their recipes.
    const auto* mixed        = config.option<ConfigOptionString>("mixed_filament_definitions");
    const bool  map_overflow = physical_count > conversion_limit && (mixed == nullptr || mixed->value.empty());
    return {map_overflow ? conversion_limit : physical_count, map_overflow};
}

} // namespace Slic3r

#endif
