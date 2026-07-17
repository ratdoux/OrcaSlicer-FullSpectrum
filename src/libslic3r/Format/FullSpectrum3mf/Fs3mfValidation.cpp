#include "Fs3mfValidation.hpp"

#include <cmath>
#include <set>
#include <utility>

namespace Slic3r::FullSpectrum3mf {

void ValidationResult::fail(std::string message)
{
    valid = false;
    errors.emplace_back(std::move(message));
}

void ValidationResult::warn(std::string message)
{
    warnings.emplace_back(std::move(message));
}

ValidationResult validate_package_model(const PackageModel &model)
{
    ValidationResult result;

    std::set<std::string> physical_refs;
    std::set<std::string> material_refs;
    for (const PhysicalFilament &filament : model.materials.physical_filaments) {
        if (filament.id.empty())
            result.fail("physical filament has empty id");
        physical_refs.insert(filament.id);
        if (!material_refs.insert(filament.id).second)
            result.fail("duplicate material id: " + filament.id);
    }

    std::set<std::string> volume_refs;
    for (const VolumeBinding &binding : model.identity_map.volume_bindings) {
        if (binding.stable_volume_id.empty())
            result.fail("volume binding has empty stable volume id");
        volume_refs.insert(binding.stable_volume_id);
    }

    if (model.mixed_filaments) {
        for (const VirtualFilament &vf : model.mixed_filaments->virtual_filaments) {
            if (vf.id.empty())
                result.fail("virtual filament has empty id");
            if (!material_refs.insert(vf.id).second)
                result.fail("duplicate material id: " + vf.id);
            if (vf.visibility_state != "active" && vf.visibility_state != "tombstoned")
                result.fail("mixed filament " + vf.id + " has invalid visibility state " + vf.visibility_state);
            if (vf.source_kind != "auto" && vf.source_kind != "custom")
                result.fail("mixed filament " + vf.id + " has invalid source kind " + vf.source_kind);
            if (vf.origin.kind != "pair")
                result.fail("mixed filament " + vf.id + " has invalid origin kind " + vf.origin.kind);
            if (vf.origin.component_refs.size() != 2)
                result.fail("mixed filament " + vf.id + " must have exactly two origin components");
            std::set<std::string> origin_refs;
            for (const std::string &component_ref : vf.origin.component_refs) {
                if (physical_refs.count(component_ref) == 0)
                    result.fail("mixed filament " + vf.id + " references missing physical filament " + component_ref);
                if (!origin_refs.insert(component_ref).second)
                    result.fail("mixed filament " + vf.id + " has duplicate origin component ref " + component_ref);
            }
            if (vf.blend.type != "pair_ratio")
                result.fail("mixed filament " + vf.id + " has invalid blend type " + vf.blend.type);
            if (vf.blend.component_b_percent < 0 || vf.blend.component_b_percent > 100)
                result.fail("mixed filament " + vf.id + " blend percent is outside 0..100");
            if (vf.distribution.mode != "simple" && vf.distribution.mode != "layer_cycle" && vf.distribution.mode != "height_weighted")
                result.fail("mixed filament " + vf.id + " has invalid distribution mode " + vf.distribution.mode);

            if (vf.gradient) {
                const size_t minimum_components = vf.gradient->enabled ? 2 : 3;
                if (vf.gradient->component_refs.size() < minimum_components)
                    result.fail("mixed filament " + vf.id + " gradient has too few components");
                if (!vf.gradient->weights.empty() && vf.gradient->weights.size() != vf.gradient->component_refs.size())
                    result.fail("mixed filament " + vf.id + " gradient weights do not match component refs");
                std::set<std::string> gradient_refs;
                for (const std::string &component_ref : vf.gradient->component_refs) {
                    if (physical_refs.count(component_ref) == 0)
                        result.fail("mixed filament " + vf.id + " gradient references missing physical filament " + component_ref);
                    if (!gradient_refs.insert(component_ref).second)
                        result.fail("mixed filament " + vf.id + " gradient has duplicate component ref " + component_ref);
                }
                for (int weight : vf.gradient->weights) {
                    if (weight < 0)
                        result.fail("mixed filament " + vf.id + " gradient has negative weight");
                }
                if (vf.gradient->enabled) {
                    if (vf.distribution.mode != "layer_cycle" && vf.distribution.mode != "height_weighted")
                        result.fail("mixed filament " + vf.id + " spatial gradient requires gradient distribution");
                    if (!std::isfinite(vf.gradient->component_a_start) || vf.gradient->component_a_start < 0.01 ||
                        vf.gradient->component_a_start > 0.99 || !std::isfinite(vf.gradient->component_a_end) ||
                        vf.gradient->component_a_end < 0.01 || vf.gradient->component_a_end > 0.99)
                        result.fail("mixed filament " + vf.id + " spatial gradient endpoints are outside 0.01..0.99");

                    const size_t expected_stops = 2 * vf.gradient->component_refs.size() - 1;
                    if (!vf.gradient->stop_positions.empty() && vf.gradient->stop_positions.size() != expected_stops) {
                        result.fail("mixed filament " + vf.id + " spatial gradient stop count does not match components");
                    } else {
                        double previous = 0.0;
                        for (size_t stop_idx = 0; stop_idx < vf.gradient->stop_positions.size(); ++stop_idx) {
                            const double position = vf.gradient->stop_positions[stop_idx];
                            if (!std::isfinite(position) || position < 0.0 || position > 1.0)
                                result.fail("mixed filament " + vf.id + " spatial gradient stop is outside 0..1");
                            if (stop_idx != 0 && position < previous)
                                result.fail("mixed filament " + vf.id + " spatial gradient stops are not ordered");
                            previous = position;
                        }
                    }
                }
            }

            if (!vf.surface_bias.component_refs.empty() || !vf.surface_bias.component_offsets_mm.empty()) {
                if (vf.surface_bias.component_refs.size() != vf.surface_bias.component_offsets_mm.size())
                    result.fail("mixed filament " + vf.id + " surface bias offsets do not match component refs");
                std::set<std::string> bias_refs;
                for (const std::string &component_ref : vf.surface_bias.component_refs) {
                    if (physical_refs.count(component_ref) == 0)
                        result.fail("mixed filament " + vf.id + " surface bias references missing physical filament " + component_ref);
                    if (!bias_refs.insert(component_ref).second)
                        result.fail("mixed filament " + vf.id + " surface bias has duplicate component ref " + component_ref);
                }
                for (const double offset_mm : vf.surface_bias.component_offsets_mm) {
                    if (!std::isfinite(offset_mm) || offset_mm < -2.0 || offset_mm > 2.0)
                        result.fail("mixed filament " + vf.id + " surface bias offset is outside -2..2 mm");
                }
            }

            if (vf.manual_pattern) {
                for (const std::vector<std::string> &group : vf.manual_pattern->groups) {
                    if (group.empty())
                        result.fail("mixed filament " + vf.id + " manual pattern has an empty group");
                    for (const std::string &step : group) {
                        if (step == "component_a" || step == "component_b")
                            continue;
                        if (step.rfind("physical:", 0) == 0 && physical_refs.count(step.substr(9)) != 0)
                            continue;
                        result.fail("mixed filament " + vf.id + " manual pattern has invalid step " + step);
                    }
                }
            }

            if (vf.local_z && vf.local_z->max_sublayers < 0)
                result.fail("mixed filament " + vf.id + " Local-Z max_sublayers is negative");
        }
    }

    for (const Assignment &assignment : model.assignments.assignments) {
        if (assignment.id.empty())
            result.fail("assignment has empty id");
        if (material_refs.count(assignment.material_ref) == 0)
            result.fail("assignment " + assignment.id + " references missing material " + assignment.material_ref);
        if (assignment.target.kind == "volume" && volume_refs.count(assignment.target.stable_volume_id) == 0)
            result.fail("assignment " + assignment.id + " references missing volume " + assignment.target.stable_volume_id);
    }

    for (const PaintStateBinding &binding : model.assignments.paint_state_bindings) {
        if (material_refs.count(binding.material_ref) == 0)
            result.fail("paint state binding references missing material " + binding.material_ref);
        if (volume_refs.count(binding.stable_volume_id) == 0)
            result.fail("paint state binding references missing volume " + binding.stable_volume_id);
    }

    return result;
}

} // namespace Slic3r::FullSpectrum3mf
