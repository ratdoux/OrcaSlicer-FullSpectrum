#include "Production.hpp"
#include "StackPlanner.hpp"
#include "SnapmakerU1.hpp"

#include "ClippedPlanner.hpp"
#include "InputAdapter.hpp"
#include "PaintedBox.hpp"
#include "PaintedDomains.hpp"
#include "LayerProcess.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <chrono>
#include <locale>
#include <sstream>
#include <set>

namespace Slic3r::MaterialBand {
namespace {
struct Abort
{
    FailureCode code;
    std::string detail;
};
double number(const DynamicPrintConfig& config, const char* key, double fallback, size_t index = 0)
{
    const auto* option = config.option(key);
    if (!option)
        return fallback;
    if (const auto* a = dynamic_cast<const ConfigOptionFloats*>(option))
        return a->values.empty() ? fallback : a->get_at(index);
    if (const auto* a = dynamic_cast<const ConfigOptionInts*>(option))
        return a->values.empty() ? fallback : a->get_at(index);
    if (const auto* a = dynamic_cast<const ConfigOptionFloatOrPercent*>(option))
        return a->get_abs_value(.4);
    if (option->type() == coInt)
        return option->getInt();
    return option->getFloat();
}
bool flag(const DynamicPrintConfig& config, const char* key)
{
    const auto* option = config.option<ConfigOptionBool>(key);
    return option && option->value;
}
DenseInfillOptions dense_infill(InfillPattern pattern, double angle)
{
    DenseInfillOptions result;
    result.angle_degrees       = angle;
    result.alternate_direction = true;
    switch (pattern) {
    case ipRectilinear: result.pattern = DenseInfillPattern::Rectilinear; break;
    case ipMonotonic:
    case ipMonotonicLine: result.pattern = DenseInfillPattern::Monotonic; break;
    case ipConcentric: result.pattern = DenseInfillPattern::Concentric; break;
    default: throw Abort{FailureCode::UnsupportedProfile, "dense_infill_pattern_requires_adapter"};
    }
    return result;
}
size_t solid_layer_count(int count, double thickness, double height)
{
    if (count < 0 || !std::isfinite(thickness) || thickness < 0. || !std::isfinite(height) || height <= 0.)
        throw Abort{FailureCode::UnsupportedProfile, "invalid_dense_infill_options"};
    const double layers = std::max(double(count), std::ceil(thickness / height - 1e-9));
    if (layers > 10000.)
        throw Abort{FailureCode::ResourceLimit, "solid_infill_layer_limit"};
    return size_t(layers);
}
bool supported_state_code(const std::string& text, bool ending)
{
    std::istringstream          stream(text);
    const std::set<std::string> start{"G21", "G90", "M82", "M83", "G28"};
    const std::set<std::string> end{"M104S0", "M140S0", "M107"};
    for (std::string line; std::getline(stream, line);) {
        line = line.substr(0, line.find(';'));
        line.erase(std::remove_if(line.begin(), line.end(), [](unsigned char c) { return std::isspace(c) != 0; }), line.end());
        if (!line.empty() && (ending ? end : start).count(line) == 0)
            return false;
    }
    return true;
}
size_t plan_bytes(const PathPlan& plan)
{
    size_t bytes = sizeof(PathPlan);
    for (const auto& path : plan.paths) {
        bytes += sizeof(BandPath) + path.vertices.capacity() * sizeof(Vec3d) + path.segments.capacity() * sizeof(PathSegment);
        for (const auto& segment : path.segments)
            bytes += segment.cells.capacity() * sizeof(BeadCell) + segment.footprint.capacity() * sizeof(Vec2d) +
                     segment.coverage_triangles.capacity() * sizeof(std::array<Vec2d, 3>);
    }
    for (const auto& surface : plan.interfaces)
        bytes += sizeof(InterfacePatch) + surface.surface.vertices.capacity() * sizeof(Vec3d) +
                 surface.surface.triangles.capacity() * sizeof(std::array<uint32_t, 3>);
    return bytes;
}
std::shared_ptr<const PathPlan> translated(const std::shared_ptr<const PathPlan>& source, const Vec3d& delta)
{
    if (delta.isZero())
        return source;
    auto copy = std::make_shared<PathPlan>(*source);
    copy->solid_min += delta;
    copy->solid_max += delta;
    for (auto& patch : copy->interfaces)
        for (auto& v : patch.surface.vertices)
            v += delta;
    for (auto& path : copy->paths) {
        for (auto& v : path.vertices)
            v += delta;
        for (auto& segment : path.segments) {
            for (auto& p : segment.footprint)
                p += delta.head<2>();
            for (auto& tri : segment.coverage_triangles)
                for (auto& p : tri)
                    p += delta.head<2>();
            for (auto& cell : segment.cells)
                for (size_t i = 0; i < 3; ++i) {
                    cell.upper[i] += delta;
                    cell.lower_z[i] += delta.z();
                }
        }
    }
    return copy;
}
void append_plan(PathPlan& to, const PathPlan& from, const PathLimits& limits, size_t& segments, size_t& bytes)
{
    to.ordered_region_paths = to.ordered_region_paths || from.ordered_region_paths;
    to.layered_process      = to.layered_process || from.layered_process;
    to.intentional_void_mm3 += from.intentional_void_mm3;
    if (from.paths.size() > limits.max_paths - to.paths.size())
        throw Abort{FailureCode::ResourceLimit, "production_path_limit"};
    const size_t added_bytes = plan_bytes(from);
    if (added_bytes > limits.max_temporary_bytes - bytes)
        throw Abort{FailureCode::ResourceLimit, "production_path_memory_limit"};
    bytes += added_bytes;
    for (const auto& source : from.paths) {
        if (source.segments.size() > limits.max_segments - segments)
            throw Abort{FailureCode::ResourceLimit, "production_segment_limit"};
        segments += source.segments.size();
        auto path = source;
        path.id   = to.paths.size() + 1;
        to.paths.push_back(std::move(path));
    }
    to.interfaces.insert(to.interfaces.end(), from.interfaces.begin(), from.interfaces.end());
    for (size_t i = 0; i < MAX_MATERIALS; ++i) {
        to.volume_mm3[i] += from.volume_mm3[i];
        to.coverage[i].target_mm3 += from.coverage[i].target_mm3;
        to.coverage[i].deposited_mm3 += from.coverage[i].deposited_mm3;
        to.coverage[i].missing_mm3 += from.coverage[i].missing_mm3;
        to.coverage[i].excess_mm3 += from.coverage[i].excess_mm3;
    }
}
BoxPlanResult uniform_paths(const KernelInput&           input,
                            double                       fraction,
                            const StackPhase&            phase,
                            const MachineEnvelope&       machine,
                            DomainId                     domain,
                            size_t                       perimeter_loops,
                            const DenseInfillProfile&    infill,
                            const std::function<bool()>& cancelled,
                            const std::function<bool()>& stale)
{
    const auto failure = [&](FailureCode code, const char* detail) -> BoxPlanResult {
        return Failure{input.revision, code, detail, {}, {}, {}};
    };
    if (input.solid.vertices.empty())
        return failure(FailureCode::InvalidInput, "empty_solid");
    Vec3d minimum = input.solid.vertices.front(), maximum = minimum;
    for (const auto& p : input.solid.vertices) {
        minimum = minimum.cwiseMin(p);
        maximum = maximum.cwiseMax(p);
    }
    if (std::abs(minimum.z()) > input.tolerances.macro_closure_mm)
        return failure(FailureCode::UnsupportedProfile, "object_requires_support");
    SlabProblem p;
    p.revision     = input.revision;
    p.domain       = domain;
    p.interface_id = 1;
    p.bottom_mm    = minimum.z();
    p.top_mm       = minimum.z() + input.process.macro_height_mm;
    p.process      = input.process;
    p.tolerances   = input.tolerances;
    p.boundary     = input.boundary;
    p.vertices     = {{minimum.x(), minimum.y()}, {maximum.x(), minimum.y()}, {maximum.x(), maximum.y()}, {minimum.x(), maximum.y()}};
    p.triangles    = {{0, 1, 2}, {0, 2, 3}};
    p.target_fraction_a.assign(4, fraction);
    p.local_fraction_error.assign(4, input.tolerances.plateau_fraction);
    p.boundary_distance_mm.assign(4, input.boundary.transition_width_mm + 1.);
    const double area     = (maximum.x() - minimum.x()) * (maximum.y() - minimum.y());
    p.target_volume_a_mm3 = area * input.process.macro_height_mm * fraction;
    const bool pure       = fraction == 0. || fraction == 1.;
    const auto straight   = [](const DenseInfillOptions& option) {
        return option.pattern == DenseInfillPattern::Monotonic && option.angle_degrees == 0. && !option.alternate_direction;
    };
    if (!pure && straight(infill.internal) && straight(infill.top) && straight(infill.bottom)) {
        auto fast = plan_box(input.solid, p, phase, machine, {}, cancelled, {perimeter_loops, false});
        if (std::holds_alternative<std::shared_ptr<const PathPlan>>(fast))
            return fast;
        const auto& error = std::get<Failure>(fast);
        if (error.code != FailureCode::UnsupportedTopology ||
            (error.detail != "constant_box_solid_required" && error.detail != "partial_macro_cycle"))
            return error;
    }
    // Imported mesh vertices are floats. A nominal 2.4 mm top may be stored as
    // 2.400000095: do not manufacture a spurious 95 nm final cycle.
    const double count = std::ceil((maximum.z() - minimum.z() - input.tolerances.interface_mm) / input.process.macro_height_mm);
    if (!std::isfinite(count) || count < 1. || count > 10000.)
        return failure(FailureCode::ResourceLimit, "production_cycle_limit");
    auto result               = std::make_shared<PathPlan>();
    result->revision          = input.revision;
    result->machine           = machine;
    result->solid_min         = minimum;
    result->solid_max         = maximum;
    size_t generated_segments = 0, generated_bytes = 0;
    for (size_t cycle = 0; cycle < size_t(count); ++cycle) {
        if (cancelled && cancelled())
            return failure(FailureCode::Cancelled, "cancelled_during_production_paths");
        if (stale && stale())
            return failure(FailureCode::StaleInput, "revision_changed");
        p.bottom_mm           = minimum.z() + cycle * input.process.macro_height_mm;
        p.top_mm              = cycle + 1 == size_t(count) ? maximum.z() : p.bottom_mm + input.process.macro_height_mm;
        p.macro_cycle         = phase.first_active_cycle + cycle;
        p.interface_id        = cycle + 1;
        p.target_volume_a_mm3 = area * (p.top_mm - p.bottom_mm) * fraction;
        BoxPlanResult  paths;
        CoverageLimits coverage;
        coverage.perimeter_loops       = perimeter_loops;
        coverage.infill                = infill.cycle(cycle, size_t(count));
        coverage.minimum_bead_width_mm = machine.coplanar_tip_contact ?
                                             .04 :
                                             std::max(.04, 2.1 * (machine.sections.front().radius_mm + machine.clearance_mm));
        if (pure) {
            const uint32_t physical = fraction == 1. ? 1 : 2;
            auto clipped = clip_pure_slab(input.solid, {domain, p.macro_cycle, {physical}, physical - 1}, input.revision, p.bottom_mm,
                                          p.top_mm, input.tolerances, {}, cancelled, stale);
            if (const auto* error = std::get_if<Failure>(&clipped))
                return *error;
            paths = plan_pure_cycle(*std::get<std::shared_ptr<const ClippedBand>>(clipped), input.revision, input.process, input.tolerances,
                                    machine, {}, coverage, cancelled, stale);
        } else {
            const auto solved = solve_slab(p, {}, cancelled, stale);
            if (const auto* error = std::get_if<Failure>(&solved))
                return *error;
            const auto clipped = clip_bands(input.solid, p, *std::get<std::shared_ptr<const SlabSolution>>(solved), phase,
                                            p.target_volume_a_mm3, {}, cancelled, stale, fraction);
            if (const auto* error = std::get_if<Failure>(&clipped))
                return *error;
            paths = plan_clipped_cycle(*std::get<std::shared_ptr<const MaterialBandComplex>>(clipped), input.process, input.tolerances,
                                       machine, {}, coverage, cancelled, stale);
        }
        if (const auto* error = std::get_if<Failure>(&paths))
            return *error;
        append_plan(*result, *std::get<std::shared_ptr<const PathPlan>>(paths), {}, generated_segments, generated_bytes);
    }
    return std::shared_ptr<const PathPlan>(std::move(result));
}
} // namespace

bool enabled(const DynamicPrintConfig& config)
{
    const auto* option = config.option("local_z_engine");
    return option && option->serialize() == "material_bands";
}
bool is_engine_option(const std::string& key) { return key == "local_z_engine" || key.compare(0, 14, "material_band_") == 0; }

std::shared_ptr<const ProductionResult> plan_production_print(const Print&                 print,
                                                              const RevisionKey&           revision,
                                                              const std::function<bool()>& cancelled,
                                                              const std::function<bool()>& stale)
{
    ProductionResult result;
    result.revision = revision;
    const auto fail = [&](FailureCode code, const std::string& detail) {
        result.plan.reset();
        result.fallback = Failure{revision, code, detail, {}, {}, {}};
        return std::make_shared<const ProductionResult>(std::move(result));
    };
    try {
        using Clock = std::chrono::steady_clock;
        const auto elapsed = [](Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); };
        const auto& config   = print.full_print_config();
        const auto& settings = print.config();
        if (!enabled(config))
            return fail(FailureCode::UnsupportedProfile, "material_band_engine_not_selected");
        if (cancelled && cancelled())
            return fail(FailureCode::Cancelled, "cancelled_before_production");
        const bool u1 = snapmaker_u1(config);
        PathLimits production_paths;
        production_paths.max_temporary_bytes = 512 * 1024 * 1024;
        if (u1) {
            production_paths.max_paths           = 500000;
            production_paths.max_segments        = 2000000;
            production_paths.max_temporary_bytes = size_t(1024) * 1024 * 1024;
        }
        const auto issue = [&](bool unsupported, const std::string& message) {
            if (unsupported && std::find(result.unsupported_settings.begin(), result.unsupported_settings.end(), message) ==
                                   result.unsupported_settings.end())
                result.unsupported_settings.push_back(message);
        };
        issue(settings.print_sequence.value != PrintSequence::ByLayer, "sequential printing is not supported");
        issue(settings.spiral_mode.value, "spiral mode is not supported");
        issue(settings.skirt_loops.value != 0, "skirts are not supported");
        issue(!u1 && number(config, "brim_width", 0.) != 0.,
              "brim requires the U1 layer process adapter (brim_width=" + config.opt_serialize("brim_width") + " mm)");
        issue(flag(config, "enable_arc_fitting"), "arc fitting is not supported");
        issue(flag(config, "use_firmware_retraction"), "firmware retraction is not supported");
        issue(number(config, "z_offset", 0.) != 0., "Z offset is not supported");
        issue(!settings.post_process.values.empty(), "post-processing scripts are not supported");
        for (const auto* object : print.objects()) {
            const auto& c = object->config();
            issue(c.enable_support.value, "support generation is not supported");
            issue(!object->model_object()->layer_config_ranges.empty() || !object->model_object()->layer_height_profile.get().empty(),
                  "variable layer settings are not supported");
            issue(c.wall_generator.value != PerimeterGeneratorType::Classic, "Arachne walls are not supported");
            for (const PrintRegion& region : object->all_regions()) {
                const auto& r = region.config();
                issue(!u1 && r.sparse_infill_density.value != 100., "sparse infill is not supported (" +
                                                                        r.sparse_infill_density.serialize() + "; " +
                                                                        r.sparse_infill_pattern.serialize() + ")");
                issue(r.wall_loops.value < 1, "at least one wall is required");
                issue(r.ironing_type.value != IroningType::NoIroning, "ironing is not supported");
                issue(r.fuzzy_skin.value != FuzzySkinType::None, "fuzzy skin is not supported");
            }
        }

        if (u1 && (settings.gcode_flavor.value != gcfKlipper || settings.single_extruder_multi_material.value))
            return fail(FailureCode::UnsupportedProfile, "u1_requires_klipper_independent_tools");
        if (!u1 && (settings.gcode_flavor.value != gcfRepRapFirmware || !flag(config, "material_band_stationary_toolchange")))
            return fail(FailureCode::UnsupportedProfile, "stationary_reprap_tool_selection_required");
        const size_t material_count = settings.filament_colour.values.size();
        if (material_count < 2 || material_count > MAX_MATERIALS || settings.nozzle_diameter.values.empty())
            return fail(FailureCode::UnsupportedProfile, "two_to_four_physical_materials_required");
        if (std::any_of(settings.bed_exclude_area.values.begin(), settings.bed_exclude_area.values.end(),
                        [](const Vec2d& point) { return !point.isZero(); }) ||
            !print.shrinkage_compensation().isApprox(Vec3d::Ones(), 1e-12))
            return fail(FailureCode::UnsupportedProfile, "bed_exclusion_or_shrink_compensation_requires_adapter");
        for (const auto& offset : settings.extruder_offset.values)
            if (!offset.isZero())
                return fail(FailureCode::UnsupportedProfile, "tool_offsets_require_adapter");
        if (settings.manual_filament_change.value)
            return fail(FailureCode::UnsupportedProfile, "manual_tool_selection_requires_adapter");
        if (settings.print_sequence.value != PrintSequence::ByLayer || settings.spiral_mode.value || settings.skirt_loops.value != 0 ||
            (!u1 && number(config, "brim_width", 0.) != 0.) || flag(config, "enable_arc_fitting") ||
            flag(config, "use_firmware_retraction") || number(config, "z_offset", 0.) != 0. || !settings.post_process.values.empty())
            return fail(FailureCode::UnsupportedProfile, "unsupported_auxiliary_or_sequential_profile");
        if (u1) {
            const auto unsupported = validate_u1_scripts(config);
            if (!unsupported.empty())
                return fail(FailureCode::UnsupportedProfile, unsupported);
        }
        if (!u1 && (!supported_state_code(settings.machine_start_gcode.value, false) ||
                    !supported_state_code(settings.machine_end_gcode.value, true)))
            return fail(FailureCode::UnsupportedProfile, "opaque_start_or_end_gcode");
        for (const char* key : {"change_filament_gcode", "before_layer_change_gcode", "layer_change_gcode", "time_lapse_gcode",
                                "change_extrusion_role_gcode", "printing_by_object_gcode"}) {
            const auto* code = config.option<ConfigOptionString>(key);
            if (!u1 && code && !code->value.empty())
                return fail(FailureCode::UnsupportedProfile, std::string("opaque_custom_gcode:") + key);
        }
        for (const char* key : {"filament_start_gcode", "filament_end_gcode"}) {
            const auto* codes = config.option<ConfigOptionStrings>(key);
            if (!u1 && codes &&
                std::any_of(codes->values.begin(), codes->values.end(), [](const std::string& code) { return !code.empty(); }))
                return fail(FailureCode::UnsupportedProfile, std::string("opaque_custom_gcode:") + key);
        }
        if (!print.model().get_curr_plate_custom_gcodes().gcodes.empty())
            return fail(FailureCode::UnsupportedProfile, "custom_layer_commands");
        const auto*       envelope      = config.option<ConfigOptionString>("material_band_tool_sections");
        const std::string envelope_text = u1 && (!envelope || envelope->value.empty()) ? "0,140,72.5" : (envelope ? envelope->value : "");
        if (envelope_text.empty() || envelope_text.size() > 65536 || envelope_text.back() == ',')
            return fail(FailureCode::UnsupportedProfile, "complete_tool_envelope_required");
        std::vector<double> sections;
        std::istringstream  envelope_stream(envelope_text);
        for (std::string item; std::getline(envelope_stream, item, ',');) {
            std::istringstream value_stream(item);
            value_stream.imbue(std::locale::classic());
            double value = 0.;
            if (!(value_stream >> value) || !std::isfinite(value) || value < 0. || sections.size() == 3072)
                return fail(FailureCode::UnsupportedProfile, "complete_tool_envelope_required");
            value_stream >> std::ws;
            if (!value_stream.eof())
                return fail(FailureCode::UnsupportedProfile, "complete_tool_envelope_required");
            sections.push_back(value);
        }
        if (sections.empty() || sections.size() % 3 != 0)
            return fail(FailureCode::UnsupportedProfile, "complete_tool_envelope_required");
        MachineEnvelope machine;
        machine.material_count = material_count;
        machine.shared_nozzle  = settings.single_extruder_multi_material.value;
        if (u1) {
            machine.travel_lift_mm       = 2.;
            machine.coplanar_tip_contact = !envelope || envelope->value.empty();
            // Straight affine runs need no half-millimetre tessellation. Keep
            // their exact contour vertices while bounding each linear segment
            // at 2 mm, avoiding redundant occupancy cells on ordinary U1 parts.
            machine.max_segment_mm = 2.;
        }
        for (size_t i = 0; i < sections.size(); i += 3)
            machine.sections.push_back({sections[i], sections[i + 1], sections[i + 2]});
        for (size_t i = 0; i < material_count; ++i) {
            machine.filament_diameter_mm[i] = number(config, "filament_diameter", 1.75, i);
            const double flow               = number(config, "filament_max_volumetric_speed", 0., i);
            if (flow > 0.)
                machine.max_volume_mm3_s = std::min(machine.max_volume_mm3_s, flow);
        }
        const double z_speed = number(config, "machine_max_feedrate_z", 5.);
        if (z_speed > 0.)
            machine.max_z_mm_s = std::min(machine.max_z_mm_s, z_speed);
        machine.deposition_mm_s = std::min(
            {machine.deposition_mm_s, number(config, "outer_wall_speed", 30.), number(config, "internal_solid_infill_speed", 30.)});
        FinalizeProfile profile;
        if (u1) {
            profile.adapter              = MachineAdapter::SnapmakerU1;
            profile.home                 = false; // U1Start owns the empty-bed homing/calibration phase.
            profile.chamber_cooling_mode = int(number(config, "chamber_cooling_mode", 1.));
            profile.bed_probe_offset_mm  = settings.curr_bed_type == btPEI || settings.curr_bed_type == btGESP ? -.07 : 0.;
        }
        profile.relative_extrusion = settings.use_relative_e_distances.value;
        const auto& bed            = settings.printable_area.values;
        if (bed.size() != 4)
            return fail(FailureCode::UnsupportedProfile, "rectangular_build_plate_required");
        Vec2d bed_low = bed.front(), bed_high = bed_low;
        for (const auto& p : bed) {
            bed_low  = bed_low.cwiseMin(p);
            bed_high = bed_high.cwiseMax(p);
        }
        for (const auto& p : bed)
            if ((p.x() != bed_low.x() && p.x() != bed_high.x()) || (p.y() != bed_low.y() && p.y() != bed_high.y()))
                return fail(FailureCode::UnsupportedProfile, "rectangular_build_plate_required");
        if (!u1 && bed_low.norm() > 1e-9)
            return fail(FailureCode::UnsupportedProfile, "zero_origin_homing_required");
        if (u1 && ((bed_low - Vec2d(.5, 1.)).norm() > 1e-6 || (bed_high - Vec2d(270.5, 271.)).norm() > 1e-6 ||
                   settings.printable_height.value > 270.05 || settings.nozzle_diameter.values.size() != 4))
            return fail(FailureCode::UnsupportedProfile, "u1_build_volume_or_four_head_profile_required");
        if (u1 && std::any_of(settings.nozzle_diameter.values.begin(), settings.nozzle_diameter.values.end(),
                              [&](double diameter) { return diameter != settings.nozzle_diameter.values.front(); }))
            return fail(FailureCode::UnsupportedProfile, "u1_matching_nozzle_diameters_required");
        const Vec3d plate_origin = print.get_plate_origin();
        if (!plate_origin.allFinite())
            return fail(FailureCode::InvalidInput, "invalid_plate_origin");
        profile.bed_min             = bed_low;
        profile.bed_max             = bed_high;
        profile.max_print_height_mm = settings.printable_height.value;
        profile.purge.enabled       = settings.enable_prime_tower.value;
        profile.purge.origin        = Vec2d(settings.wipe_tower_x.get_at(print.get_plate_index()),
                                            settings.wipe_tower_y.get_at(print.get_plate_index()));
        profile.purge.width_mm      = settings.prime_tower_width.value;
        const auto purge            = WipeTower2::extract_wipe_volumes(settings);
        if (purge.size() != material_count ||
            std::any_of(purge.begin(), purge.end(), [&](const auto& row) { return row.size() != material_count; }))
            return fail(FailureCode::UnsupportedProfile, "material_purge_matrix_size");
        for (size_t i = 0; i < material_count; ++i) {
            profile.purge.prime_mm3[i] = u1 && !profile.purge.enabled ? 0. : settings.prime_volume.value;
            for (size_t j = 0; j < material_count; ++j)
                profile.purge.volume_mm3[i][j] = i == j ? 0. : u1 ? profile.purge.prime_mm3[i] : purge[i][j];
            profile.nozzle_temperature[i]         = number(config, "nozzle_temperature", 200., i);
            profile.retract_mm[i]                 = number(config, "retraction_length", 1., i);
            profile.retract_speed_mm_s[i]         = number(config, "retraction_speed", 30., i);
            profile.retract_min_travel_mm[i]      = number(config, "retraction_minimum_travel", 0., i);
            profile.initial_nozzle_temperature[i] = number(config, "nozzle_temperature_initial_layer", 200., i);
            profile.initial_wall_speed_mm_s[i]    = number(config, "initial_layer_speed", 30., i);
            profile.initial_infill_speed_mm_s[i]  = number(config, "initial_layer_infill_speed", 30., i);
            profile.material_fan_percent[i]       = number(config, "fan_min_speed", 0., i);
            profile.fan_disabled_cycles[i]        = int(number(config, "close_fan_the_first_x_layers", 1., i));
        }
        const std::string bed_key = get_bed_temp_key(settings.curr_bed_type);
        profile.bed_temperature   = 0.;
        for (size_t i = 0; i < material_count; ++i)
            profile.bed_temperature = std::max(profile.bed_temperature, number(config, bed_key.c_str(), 60., i));
        profile.initial_bed_temperature = 0.;
        for (size_t i = 0; i < material_count; ++i)
            profile.initial_bed_temperature = std::max(profile.initial_bed_temperature,
                                                       number(config, (bed_key + "_initial_layer").c_str(), profile.bed_temperature, i));
        ScheduleInput schedule;
        size_t        print_bytes = 0, print_paths = 0;
        schedule.revision         = revision;
        schedule.machine          = machine;
        schedule.initial_position = Vec3d(profile.bed_min.x(), profile.bed_min.y(), 0.);
        for (const auto* object : print.objects()) {
            if (cancelled && cancelled())
                return fail(FailureCode::Cancelled, "cancelled_during_production");
            if (stale && stale())
                return fail(FailureCode::StaleInput, "revision_changed");
            const auto& object_settings = object->config();
            if (object_settings.enable_support.value || !object->model_object()->layer_config_ranges.empty() ||
                !object->model_object()->layer_height_profile.get().empty())
                return fail(FailureCode::UnsupportedProfile, "support_or_variable_process_requires_adapter");
            const auto regions = object->all_regions();
            if (regions.empty())
                return fail(FailureCode::InvalidInput, "empty_print_regions");
            const size_t       perimeter_loops = regions.front().get().config().wall_loops.value;
            const auto&        infill_config   = regions.front().get().config();
            DenseInfillProfile infill;
            infill.internal   = dense_infill(infill_config.internal_solid_infill_pattern.value, infill_config.solid_infill_direction.value);
            infill.top        = dense_infill(infill_config.top_surface_pattern.value, infill_config.solid_infill_direction.value);
            infill.bottom     = dense_infill(infill_config.bottom_surface_pattern.value, infill_config.solid_infill_direction.value);
            infill.top_layers = solid_layer_count(infill_config.top_shell_layers.value, infill_config.top_shell_thickness.value,
                                                  object_settings.layer_height.value);
            infill.bottom_layers = solid_layer_count(infill_config.bottom_shell_layers.value, infill_config.bottom_shell_thickness.value,
                                                     object_settings.layer_height.value);
            for (const PrintRegion& region : regions) {
                const auto& c = region.config();
                if ((!u1 && c.sparse_infill_density.value != 100.) || c.wall_loops.value < 1 || c.wall_loops.value != perimeter_loops ||
                    object_settings.wall_generator.value != PerimeterGeneratorType::Classic ||
                    c.ironing_type.value != IroningType::NoIroning || c.fuzzy_skin.value != FuzzySkinType::None)
                    return fail(FailureCode::UnsupportedProfile, "dense_classic_profile_required");
                if (!c.solid_infill_rotate_template.value.empty() || !c.sparse_infill_rotate_template.value.empty() ||
                    c.align_infill_direction_to_model.value || c.sparse_infill_density.value != infill_config.sparse_infill_density.value ||
                    c.sparse_infill_pattern.value != infill_config.sparse_infill_pattern.value ||
                    c.infill_direction.value != infill_config.infill_direction.value ||
                    c.internal_solid_infill_pattern.value != infill_config.internal_solid_infill_pattern.value ||
                    c.top_surface_pattern.value != infill_config.top_surface_pattern.value ||
                    c.bottom_surface_pattern.value != infill_config.bottom_surface_pattern.value ||
                    c.solid_infill_direction.value != infill_config.solid_infill_direction.value)
                    return fail(FailureCode::UnsupportedProfile, "infill_direction_or_region_override_requires_adapter");
            }
            InputContext context;
            context.revision                        = revision;
            context.phase.order                     = {{{1}, {2}}};
            context.revision.object                 = object->model_object()->id().id;
            context.process.macro_height_mm         = object_settings.layer_height.value;
            context.process.min_active_thickness_mm = number(config, "mixed_filament_height_lower_bound", .04);
            context.process.max_active_thickness_mm = std::min(context.process.macro_height_mm,
                                                               number(config, "mixed_filament_height_upper_bound", .24));
            if (context.process.max_active_thickness_mm <= 0.)
                context.process.max_active_thickness_mm = context.process.macro_height_mm;
            context.process.bead_width_mm = object_settings.line_width.get_abs_value(settings.nozzle_diameter.get_at(0));
            if (context.process.bead_width_mm <= 0.)
                context.process.bead_width_mm = settings.nozzle_diameter.get_at(0) * 1.125;
            context.boundary.policy = settings.material_band_boundary_policy.value == MaterialBandBoundaryPolicy::AllowTransition ?
                                          BoundaryPolicy::AllowTransition :
                                          BoundaryPolicy::PreservePaintedEdge;
            context.boundary.transition_width_mm       = settings.material_band_transition_width.value;
            context.boundary.transition_fraction_error = settings.material_band_transition_error.value / 100.;
            const auto snapshot_start                  = Clock::now();
            auto       snapshot                        = snapshot_input(*object->model_object(), config, context, cancelled);
            result.snapshot_ms += elapsed(snapshot_start);
            if (const auto* error = std::get_if<Failure>(&snapshot)) {
                result.fallback = *error;
                return std::make_shared<const ProductionResult>(std::move(result));
            }
            const auto& input = *std::get<std::shared_ptr<const KernelInput>>(snapshot);
            if (input.regions.empty())
                return fail(FailureCode::InvalidInput, "empty_material_intent");
            LayerProcessProfile layer_profile;
            layer_profile.nominal_height_mm    = object_settings.layer_height.value;
            layer_profile.initial_height_mm    = settings.initial_layer_print_height.value;
            layer_profile.density              = infill_config.sparse_infill_density.value / 100.;
            layer_profile.sparse_pattern       = infill_config.sparse_infill_pattern.value;
            layer_profile.sparse_angle_degrees = infill_config.infill_direction.value;
            layer_profile.brim_width_mm        = object_settings.brim_type.value == btNoBrim ? 0. : object_settings.brim_width.value;
            layer_profile.brim_gap_mm          = object_settings.brim_object_gap.value;
            layer_profile.inner_brim = object_settings.brim_type.value == btInnerOnly || object_settings.brim_type.value == btOuterAndInner;
            layer_profile.outer_brim = object_settings.brim_type.value != btInnerOnly;
            layer_profile.wall_loops = perimeter_loops;
            layer_profile.top_shell_mm    = infill.top_layers * layer_profile.nominal_height_mm;
            layer_profile.bottom_shell_mm = infill.bottom_layers * layer_profile.nominal_height_mm;
            layer_profile.dense           = infill;
            const bool layer_process      = u1 && (layer_profile.brim_width_mm > 0. || layer_profile.density != 1. ||
                                              input.solid.triangles.size() > 20000 || input.discarded_degenerate_faces != 0 ||
                                              std::abs(layer_profile.initial_height_mm - layer_profile.nominal_height_mm) >
                                                  input.tolerances.interface_mm);
            if (layer_process)
                context.process.max_active_thickness_mm = number(config, "mixed_filament_height_upper_bound", .24);
            const double fraction = input.regions.front().fraction_a;
            const bool   stack    = material_count > 2 ||
                               std::any_of(input.regions.begin(), input.regions.end(),
                                           [](const RegionIntent& region) { return !region.height_gradient.empty(); });
            const bool  uniform         = std::all_of(input.regions.begin(), input.regions.end(),
                                                      [&](const RegionIntent& region) { return std::abs(region.fraction_a - fraction) <= 1e-12; });
            const auto& first_intent    = input.regions.front();
            const bool  painted_regions = std::any_of(input.regions.begin(), input.regions.end(), [&](const RegionIntent& region) {
                if (region.fractions != first_intent.fractions || region.height_gradient.size() != first_intent.height_gradient.size())
                    return true;
                for (size_t i = 0; i < region.height_gradient.size(); ++i)
                    if (region.height_gradient[i].position != first_intent.height_gradient[i].position ||
                        region.height_gradient[i].fractions != first_intent.height_gradient[i].fractions)
                        return true;
                return false;
            });
            if (result.object_count != 0 && (profile.process.macro_height_mm != context.process.macro_height_mm ||
                                             profile.process.bead_width_mm != context.process.bead_width_mm ||
                                             profile.process.min_active_thickness_mm != context.process.min_active_thickness_mm ||
                                             profile.process.max_active_thickness_mm != context.process.max_active_thickness_mm))
                return fail(FailureCode::UnsupportedProfile, "incompatible_object_processes");
            profile.process = context.process;
            ++result.object_count;
            std::shared_ptr<const PathPlan> cached_geometry;
            Transform3d                     cached_transform = Transform3d::Identity();
            for (const auto& instance : object->instances()) {
                const auto paths_start = Clock::now();
                auto       matrix      = instance.model_instance->get_matrix();
                matrix.translation() -= plate_origin;
                if (!matrix.matrix().allFinite() || std::abs(matrix.linear().determinant()) < 1e-12)
                    return fail(FailureCode::InvalidInput, "invalid_instance_transform");
                std::shared_ptr<const PathPlan> instance_paths;
                if (cached_geometry && matrix.linear().isApprox(cached_transform.linear(), 1e-12) &&
                    matrix.translation().z() == cached_transform.translation().z()) {
                    if (plan_bytes(*cached_geometry) > production_paths.max_temporary_bytes - print_bytes ||
                        cached_geometry->paths.size() > production_paths.max_paths - print_paths)
                        return fail(FailureCode::ResourceLimit, "combined_instance_geometry_limit");
                    instance_paths = translated(cached_geometry, matrix.translation() - cached_transform.translation());
                    ++result.geometry_reuses;
                } else {
                    auto transformed    = input;
                    transformed.process = context.process;
                    for (auto& point : transformed.solid.vertices)
                        point = matrix * point;
                    if (matrix.linear().determinant() < 0.)
                        for (auto& face : transformed.solid.triangles)
                            std::swap(face[1], face[2]);
                    for (auto& region : transformed.regions) {
                        for (auto& point : region.painted_surface.vertices)
                            point = matrix * point;
                        if (matrix.linear().determinant() < 0.)
                            for (auto& face : region.painted_surface.triangles)
                                std::swap(face[1], face[2]);
                    }
                    // Region stacks can require several tool changes at the
                    // same Z. The U1 finalizer allocates separate tower strips
                    // for that sequence; other adapters retain their scope.
                    auto planned = layer_process ? plan_layer_process(transformed, machine, object->model_object()->id().id, layer_profile,
                                                                      production_paths, cancelled, stale) :
                                   painted_regions && u1 ? plan_painted_regions(transformed, machine, object->model_object()->id().id,
                                                                                perimeter_loops, infill, {}, cancelled, stale) :
                                   stack   ? plan_material_stack(transformed, machine, object->model_object()->id().id, perimeter_loops,
                                                                 infill, {}, cancelled, stale) :
                                   uniform ? uniform_paths(transformed, fraction, context.phase, machine, object->model_object()->id().id,
                                                           perimeter_loops, infill, cancelled, stale) :
                                             plan_painted_box(transformed, context.phase, machine, object->model_object()->id().id,
                                                              perimeter_loops, {}, cancelled, stale, infill);
                    if (const auto* error = std::get_if<Failure>(&planned)) {
                        result.paths_ms += elapsed(paths_start);
                        result.fallback = *error;
                        return std::make_shared<const ProductionResult>(std::move(result));
                    }
                    instance_paths   = std::get<std::shared_ptr<const PathPlan>>(planned);
                    cached_geometry  = instance_paths;
                    cached_transform = matrix;
                }
                const size_t bytes = plan_bytes(*instance_paths);
                if (bytes > production_paths.max_temporary_bytes - print_bytes ||
                    instance_paths->paths.size() > production_paths.max_paths - print_paths)
                    return fail(FailureCode::ResourceLimit, "combined_instance_geometry_limit");
                print_bytes += bytes;
                print_paths += instance_paths->paths.size();
                result.paths_ms += elapsed(paths_start);
                schedule.objects.push_back({instance.model_instance->id().id, std::move(instance_paths)});
                ++result.instance_count;
            }
        }
        if (u1) {
            double top = 5.;
            for (const auto& object : schedule.objects)
                top = std::max(top, std::ceil(object.paths->solid_max.z() /
                                                  std::min(profile.process.macro_height_mm, profile.process.max_active_thickness_mm) -
                                              1e-8) *
                                            std::min(profile.process.macro_height_mm, profile.process.max_active_thickness_mm) +
                                        machine.travel_lift_mm);
            if (top > profile.max_print_height_mm)
                return fail(FailureCode::UnsupportedProfile, "u1_dock_clearance_outside_build_volume");
            schedule.initial_position = Vec3d(5., 5., top);
        }
        const auto     finalize_start = Clock::now();
        FinalizeLimits finalize_limits;
        // A painted U1 job can contain many independent region caps. Retain
        // the memory/cell/candidate bounds while allowing their local spatial
        // queries a larger, still finite work budget. Queries remain cancellable.
        if (u1)
            finalize_limits.schedule.max_index_visits = 500000000;
        if (std::any_of(schedule.objects.begin(), schedule.objects.end(),
                        [](const ScheduleObject& object) { return object.paths->layered_process; })) {
            // Painted process layers produce many short, separately owned runs.
            // Keep a bounded production budget across paths, spatial indexing,
            // dependencies and final commands instead of a small-fixture limit.
            finalize_limits.paths               = production_paths;
            finalize_limits.max_operations      = 4000000;
            finalize_limits.max_temporary_bytes = size_t(2) * 1024 * 1024 * 1024;
            auto& scheduling                    = finalize_limits.schedule;
            scheduling.max_paths                = production_paths.max_paths;
            scheduling.max_cells                = 4000000;
            scheduling.max_index_nodes          = 16000000;
            scheduling.max_edges                = 4000000;
            scheduling.max_candidates           = 100000000;
            scheduling.max_index_visits         = 2000000000;
            // Three million compact cells need a larger conservative index
            // estimate; the final-command allocation remains bounded separately.
            scheduling.max_temporary_bytes = size_t(3) * 1024 * 1024 * 1024;
        }
        auto finalized     = finalize_print(schedule, profile, finalize_limits, cancelled, stale);
        result.finalize_ms = elapsed(finalize_start);
        if (const auto* error = std::get_if<Failure>(&finalized))
            result.fallback = *error;
        else
            result.plan = std::get<std::shared_ptr<const FinalPrintPlan>>(finalized);
        return std::make_shared<const ProductionResult>(std::move(result));
    } catch (const Abort& error) {
        return fail(error.code, error.detail);
    } catch (const std::bad_alloc&) {
        return fail(FailureCode::ResourceLimit, "production_allocation");
    } catch (const std::exception&) {
        return fail(FailureCode::InvalidInput, "production_snapshot_or_profile");
    }
}
} // namespace Slic3r::MaterialBand
