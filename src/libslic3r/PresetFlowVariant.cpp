#include "PresetFlowVariant.hpp"

namespace Slic3r {

FilamentVolumeType get_nozzle_volume_type(const ConfigBase &printer_config, unsigned int extruder_id)
{
    const ConfigOption *option = printer_config.option("nozzle_volume_type");
    if (option == nullptr || option->type() != coEnums)
        return fvtStandard;

    const auto *types = static_cast<const ConfigOptionEnumsGeneric *>(option);
    if (extruder_id >= types->values.size())
        return fvtStandard;

    const int value = types->values[extruder_id];
    return value == fvtHighFlow ? fvtHighFlow : fvtStandard;
}

size_t get_preset_flow_variant_idx(const ConfigBase &preset_config, ConfigFlowDomain domain, FilamentVolumeType type)
{
    const ConfigOption *option = preset_config.option(flow_support_key(domain));
    if (option == nullptr || option->type() != coStrings)
        return 0;

    const auto *flow_support = static_cast<const ConfigOptionStrings *>(option);
    if (flow_support->values.empty())
        return 0;

    return flow_variant_index(flow_support->values, to_string(type));
}

} // namespace Slic3r
