#ifndef slic3r_ProjectImport_hpp_
#define slic3r_ProjectImport_hpp_

#include "../Model.hpp"
#include "../PrintConfig.hpp"
#include "../Slicing.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace Slic3r {

enum class ProjectImportMode { ModelOnly, CompatiblePrintSettings, FullProject };

inline ProjectImportMode default_3mf_project_import_mode(const DynamicPrintConfig& imported, const DynamicPrintConfig& printer)
{
    bool identified = false;
    for (const char* key : {"printer_model", "printer_variant", "printer_settings_id"}) {
        const auto* source = imported.option<ConfigOptionString>(key);
        if (source == nullptr || source->value.empty())
            continue;
        identified          = true;
        const auto* current = printer.option<ConfigOptionString>(key);
        if (current == nullptr || current->value != source->value)
            return ProjectImportMode::ModelOnly;
    }
    return identified ? ProjectImportMode::FullProject : ProjectImportMode::ModelOnly;
}

inline bool imported_layer_height_fits_printer(double height, const DynamicPrintConfig& printer)
{
    const auto* nozzles = printer.option<ConfigOptionFloats>("nozzle_diameter");
    if (!std::isfinite(height) || nozzles == nullptr || nozzles->values.empty() || !printer.has("min_layer_height") ||
        !printer.has("max_layer_height"))
        return false;
    for (size_t i = 0; i < nozzles->values.size(); ++i) {
        if (height < Slicing::min_layer_height_from_nozzle(printer, int(i + 1)) ||
            height > Slicing::max_layer_height_from_nozzle(printer, int(i + 1)))
            return false;
    }
    return true;
}

// Only portable process choices cross printers. Machine limits, G-code,
// speeds, extrusion widths, purge/AMS settings and preset identities do not.
inline DynamicPrintConfig compatible_3mf_print_settings(const DynamicPrintConfig& imported, const DynamicPrintConfig& printer)
{
    static const std::vector<std::string> keys = {"layer_height",
                                                  "initial_layer_print_height",
                                                  "wall_loops",
                                                  "alternate_extra_wall",
                                                  "top_shell_layers",
                                                  "top_shell_thickness",
                                                  "bottom_shell_layers",
                                                  "bottom_shell_thickness",
                                                  "top_surface_density",
                                                  "bottom_surface_density",
                                                  "sparse_infill_density",
                                                  "sparse_infill_pattern",
                                                  "top_surface_pattern",
                                                  "bottom_surface_pattern",
                                                  "internal_solid_infill_pattern",
                                                  "infill_direction",
                                                  "solid_infill_direction",
                                                  "infill_wall_overlap",
                                                  "top_bottom_infill_wall_overlap",
                                                  "seam_position",
                                                  "staggered_inner_seams",
                                                  "wall_sequence",
                                                  "is_infill_first",
                                                  "wall_generator",
                                                  "detect_thin_wall",
                                                  "detect_overhang_wall",
                                                  "only_one_wall_top",
                                                  "only_one_wall_first_layer",
                                                  "brim_type",
                                                  "brim_width",
                                                  "brim_object_gap",
                                                  "skirt_loops",
                                                  "skirt_distance",
                                                  "skirt_height",
                                                  "enable_support",
                                                  "support_type",
                                                  "support_style",
                                                  "support_threshold_angle",
                                                  "support_on_build_plate_only",
                                                  "support_critical_regions_only",
                                                  "support_remove_small_overhang",
                                                  "support_base_pattern",
                                                  "support_base_pattern_spacing",
                                                  "support_interface_top_layers",
                                                  "support_interface_bottom_layers",
                                                  "support_interface_pattern",
                                                  "support_interface_spacing",
                                                  "support_bottom_interface_spacing",
                                                  "support_top_z_distance",
                                                  "support_bottom_z_distance",
                                                  "support_object_xy_distance",
                                                  "support_object_first_layer_gap",
                                                  "support_expansion",
                                                  "support_angle",
                                                  "bridge_no_support",
                                                  "raft_layers",
                                                  "raft_contact_distance",
                                                  "raft_expansion"};
    DynamicPrintConfig                    result;
    for (const std::string& key : keys) {
        const ConfigOption*    option = imported.option(key);
        const ConfigOptionDef* def    = print_config_def.get(key);
        if (option == nullptr || def == nullptr || option->type() != def->type)
            continue;
        if (key == "layer_height" || key == "initial_layer_print_height") {
            if (!imported_layer_height_fits_printer(option->getFloat(), printer))
                continue;
        }
        if (option->type() == coInt || option->type() == coFloat || option->type() == coPercent) {
            const double value = option->type() == coInt ? option->getInt() : option->getFloat();
            if (!std::isfinite(value) || value < def->min || value > def->max)
                continue;
        }
        result.set_key_value(key, option->clone());
    }
    return result;
}

inline const std::vector<std::string>& imported_model_color_options()
{
    static const std::vector<std::string> keys = {"extruder",
                                                  "wall_filament",
                                                  "sparse_infill_filament",
                                                  "solid_infill_filament",
                                                  "support_filament",
                                                  "support_interface_filament",
                                                  "enable_infill_filament_override",
                                                  "infill_filament_use_base_first_layers",
                                                  "infill_filament_use_base_last_layers"};
    return keys;
}

inline void prepare_3mf_model_for_current_printer(Model&                    model,
                                                  const DynamicPrintConfig& imported,
                                                  const DynamicPrintConfig& printer,
                                                  bool                      import_print_settings)
{
    auto filter = [&](ModelConfig& config) {
        DynamicPrintConfig retained = import_print_settings ? compatible_3mf_print_settings(config.get(), printer) : DynamicPrintConfig();
        // apply_only() also creates defaults for absent keys. Here absence
        // means inheritance, so copy only assignments actually in the model.
        for (const std::string& key : imported_model_color_options()) {
            if (const ConfigOption* option = config.option(key))
                retained.set_key_value(key, option->clone());
        }
        config.assign_config(std::move(retained));
    };
    for (ModelObject* object : model.objects) {
        // Source global color assignments become object defaults so remapping
        // cannot change the meaning of existing objects' global filament IDs.
        for (const std::string& key : imported_model_color_options()) {
            // An object's extruder overrides the global wall/infill choices
            // during normalization. Do not turn those defaults into overrides.
            if ((key == "wall_filament" || key == "sparse_infill_filament" || key == "solid_infill_filament") &&
                object->config.has("extruder") && object->config.extruder() > 0)
                continue;
            if (!object->config.has(key)) {
                if (const ConfigOption* option = imported.option(key))
                    object->config.set_key_value(key, option->clone());
            }
        }
        filter(object->config);
        for (ModelVolume* volume : object->volumes)
            filter(volume->config);
        for (auto it = object->layer_config_ranges.begin(); it != object->layer_config_ranges.end();) {
            filter(it->second);
            if (it->second.empty())
                it = object->layer_config_ranges.erase(it);
            else
                ++it;
        }
        bool       keep_profile = import_print_settings;
        const auto heights      = object->layer_height_profile.get();
        for (size_t i = 1; keep_profile && i < heights.size(); i += 2)
            keep_profile = imported_layer_height_fits_printer(heights[i], printer);
        if (!keep_profile)
            object->layer_height_profile.clear();
    }
    model.plates_custom_gcodes.clear();
}

} // namespace Slic3r

#endif
