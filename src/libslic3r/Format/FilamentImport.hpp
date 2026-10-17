#ifndef slic3r_FilamentImport_hpp_
#define slic3r_FilamentImport_hpp_

#include "../PrintConfig.hpp"
#include "../MixedFilament.hpp"
#include "../Preset.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

enum class FilamentImportMode { Automatic, KeepLoaded, ReplaceLoaded };

struct FilamentImportPlan
{
    size_t physical_count      = 0;
    bool   map_overflow_colors = false;
};

inline FilamentImportPlan plan_filament_import(const DynamicPrintConfig& config,
                                               size_t                    physical_count,
                                               size_t                    conversion_limit,
                                               FilamentImportMode        mode         = FilamentImportMode::Automatic,
                                               size_t                    loaded_count = 0)
{
    if (mode == FilamentImportMode::KeepLoaded)
        return {loaded_count, false};
    if (mode == FilamentImportMode::ReplaceLoaded)
        conversion_limit = MAXIMUM_EXTRUDER_NUMBER;
    // Existing mixed IDs follow the saved physical palette. A map built only
    // from physical colors cannot preserve those IDs or their recipes.
    const auto* mixed        = config.option<ConfigOptionString>("mixed_filament_definitions");
    const bool  map_overflow = physical_count > conversion_limit && (mixed == nullptr || mixed->value.empty());
    return {map_overflow ? conversion_limit : physical_count, map_overflow};
}

// Include virtual colors in their saved ID order. Otherwise an imported mixed
// assignment would fall outside a physical-only map and become filament 1.
inline std::vector<std::string> imported_filament_color_targets(const DynamicPrintConfig&       config,
                                                                const std::vector<std::string>& physical_colors)
{
    std::vector<std::string> colors = physical_colors;
    const auto*              mixed  = config.option<ConfigOptionString>("mixed_filament_definitions");
    if (mixed != nullptr && !mixed->value.empty()) {
        MixedFilamentManager manager;
        manager.auto_generate(physical_colors);
        manager.load_custom_entries(mixed->value, physical_colors);
        const auto mixed_colors = manager.display_colors();
        colors.insert(colors.end(), mixed_colors.begin(), mixed_colors.end());
    }
    return colors;
}

// Compose filament profiles with the current printer/process configuration.
// The grouped metadata stores print, filament(s), printer in that order and
// must be rebuilt when the two palettes have different numbers of slots.
inline DynamicPrintConfig config_with_imported_filaments(const DynamicPrintConfig& loaded, const DynamicPrintConfig& imported)
{
    const auto* colors = imported.option<ConfigOptionStrings>("filament_colour");
    if (colors == nullptr || colors->values.empty())
        return loaded;
    DynamicPrintConfig result = loaded;
    // Missing foreign profile fields use defaults, not calibrations or spool
    // metadata belonging to the filaments being replaced.
    const auto& defaults = FullPrintConfig::defaults();
    result.apply_only(defaults, Preset::filament_options(), true);
    for (const std::string& key : result.keys()) {
        if (key.compare(0, 9, "filament_") == 0) {
            if (const ConfigOption* option = defaults.option(key))
                result.set_key_value(key, option->clone());
        }
    }
    result.apply_only(imported, Preset::filament_options(), true);
    const size_t count = colors->values.size();
    result.set_key_value("mixed_filament_definitions", new ConfigOptionString(""));
    for (const std::string& key : imported.keys()) {
        if (key.compare(0, 9, "filament_") == 0 || key.compare(0, 6, "mixed_") == 0 || key.compare(0, 10, "dithering_") == 0)
            result.set_key_value(key, imported.option(key)->clone());
    }
    for (const char* key : {"inherits_group", "compatible_machine_expression_group", "different_settings_to_system"}) {
        std::vector<std::string> values(count + 2);
        if (const auto* previous = loaded.option<ConfigOptionStrings>(key); previous != nullptr && !previous->values.empty()) {
            values.front() = previous->values.front();
            values.back()  = previous->values.back();
        }
        if (const auto* source = imported.option<ConfigOptionStrings>(key)) {
            for (size_t i = 1; i <= count && i + 1 < source->values.size(); ++i)
                values[i] = source->values[i];
        }
        result.set_key_value(key, new ConfigOptionStrings(std::move(values)));
    }
    for (const char* key : {"filament_settings_id", "filament_ids", "compatible_process_expression_group"}) {
        std::vector<std::string> values(count);
        if (const auto* source = imported.option<ConfigOptionStrings>(key)) {
            values = source->values;
            values.resize(count);
        }
        result.set_key_value(key, new ConfigOptionStrings(std::move(values)));
    }
    if (!imported.has("filament_flow_step_size"))
        result.set_key_value("filament_flow_step_size", new ConfigOptionInts(std::vector<int>(count, 1)));
    return result;
}

} // namespace Slic3r

#endif
