#pragma once

#include "PrintConfig.hpp"

namespace Slic3r {

// Read the per-extruder nozzle flow type from a printer preset; missing or invalid values are standard.
FilamentVolumeType get_nozzle_volume_type(const ConfigBase &printer_config, unsigned int extruder_id = 0);

// Resolve an index inside a single preset's variant array. Unlike get_config_idx(), this helper
// does not expect filament_flow_step_size or a composed multi-filament config.
size_t get_preset_flow_variant_idx(const ConfigBase &preset_config, ConfigFlowDomain domain, FilamentVolumeType type);

template<typename VectorOption>
inline auto get_preset_value_at(const ConfigBase &preset_config, const VectorOption &opt, ConfigFlowDomain domain, FilamentVolumeType type)
    -> decltype(opt.get_at(0))
{
    return opt.get_at(get_preset_flow_variant_idx(preset_config, domain, type));
}

} // namespace Slic3r
