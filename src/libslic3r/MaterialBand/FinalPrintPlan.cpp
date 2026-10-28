#include "FinalPrintPlan.hpp"

#include "ClippedPlanner.hpp"
#include "SnapmakerU1.hpp"
#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/Utils.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace Slic3r::MaterialBand {
namespace {
constexpr uint64_t TOWER_OBJECT = std::numeric_limits<uint64_t>::max();
struct Abort
{
    FailureCode code;
    std::string detail;
};
Mesh box(const Vec2d& origin, double width, double depth, double low, double high)
{
    Mesh m;
    for (double z : {low, high})
        for (const Vec2d& p :
             {origin, Vec2d(origin.x() + width, origin.y()), Vec2d(origin + Vec2d(width, depth)), Vec2d(origin.x(), origin.y() + depth)})
            m.vertices.emplace_back(p.x(), p.y(), z);
    m.triangles = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                   {1, 2, 6}, {1, 6, 5}, {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7}};
    return m;
}
const BandPath& lookup(const std::map<OperationRef, const BandPath*>& paths, OperationRef ref) { return *paths.at(ref); }
} // namespace

FinalizeResult finalize_print(const ScheduleInput&         input,
                              const FinalizeProfile&       profile,
                              const FinalizeLimits&        limits,
                              const std::function<bool()>& cancelled,
                              const std::function<bool()>& stale)
{
    const auto fail = [&](FailureCode code, const std::string& detail) -> FinalizeResult {
        return Failure{input.revision, code, detail, {}, {}, {}};
    };
    try {
        const auto poll = [&] {
            if (cancelled && cancelled())
                throw Abort{FailureCode::Cancelled, "cancelled_during_finalization"};
            if (stale && stale())
                throw Abort{FailureCode::StaleInput, "revision_changed"};
        };
        poll();
        const size_t material_count = input.machine.material_count;
        const bool   u1             = profile.adapter == MachineAdapter::SnapmakerU1;
        if (material_count < 2 || material_count > MAX_MATERIALS)
            return fail(FailureCode::InvalidInput, "invalid_material_count");
        if (!profile.bed_min.allFinite() || !profile.bed_max.allFinite() || (profile.bed_max.array() <= profile.bed_min.array()).any() ||
            !std::isfinite(profile.max_print_height_mm) || profile.max_print_height_mm <= input.bed_z_mm ||
            !std::isfinite(profile.fan_percent) || profile.fan_percent < 0. || profile.fan_percent > 100. ||
            !std::isfinite(profile.bed_temperature) || profile.bed_temperature < 0. || profile.bed_temperature > 150.)
            return fail(FailureCode::InvalidInput, "invalid_finalize_profile");
        if (profile.home && (!input.occupied.empty() ||
                             (input.initial_position - Vec3d(profile.bed_min.x(), profile.bed_min.y(), input.bed_z_mm)).norm() > 1e-9))
            return fail(FailureCode::UnsupportedProfile, "homing_requires_empty_bed_and_declared_origin");
        if (u1 && (input.machine.shared_nozzle || !input.occupied.empty() || input.machine.travel_lift_mm < 2. || profile.home))
            return fail(FailureCode::UnsupportedProfile, "u1_requires_empty_bed_independent_tools_and_dock_clearance");
        if (u1 && (!std::isfinite(profile.initial_bed_temperature) || profile.initial_bed_temperature < 0. ||
                   profile.initial_bed_temperature > 100. || profile.bed_temperature > 100.))
            return fail(FailureCode::UnsupportedProfile, "u1_bed_temperature_limit");
        for (size_t i = 0; i < material_count; ++i) {
            if (!std::isfinite(profile.initial_wall_speed_mm_s[i]) || profile.initial_wall_speed_mm_s[i] <= 0. ||
                !std::isfinite(profile.initial_infill_speed_mm_s[i]) || profile.initial_infill_speed_mm_s[i] <= 0.)
                return fail(FailureCode::InvalidInput, "invalid_initial_layer_speed");
            for (double v : {profile.nozzle_temperature[i], profile.retract_mm[i], profile.retract_speed_mm_s[i],
                             profile.retract_min_travel_mm[i], profile.purge.prime_mm3[i]})
                if (!std::isfinite(v) || v < 0.)
                    return fail(FailureCode::InvalidInput, "invalid_filament_profile");
            if (profile.nozzle_temperature[i] > 400. || profile.retract_mm[i] > 100. || profile.retract_speed_mm_s[i] <= 0.)
                return fail(FailureCode::UnsupportedProfile, "filament_state_limits");
            if (u1 && (!std::isfinite(profile.initial_nozzle_temperature[i]) || profile.initial_nozzle_temperature[i] < 170. ||
                       profile.initial_nozzle_temperature[i] > 300. || profile.nozzle_temperature[i] < 170. ||
                       profile.nozzle_temperature[i] > 300. || !std::isfinite(profile.material_fan_percent[i]) ||
                       profile.material_fan_percent[i] < 0. || profile.material_fan_percent[i] > 100. || profile.fan_disabled_cycles[i] < 0))
                return fail(FailureCode::UnsupportedProfile, "u1_filament_temperature_or_fan_limits");
            for (double v : profile.purge.volume_mm3[i])
                if (!std::isfinite(v) || v < 0. || v > 100000.)
                    return fail(FailureCode::InvalidInput, "invalid_purge_volume");
        }
        if (input.machine.shared_nozzle)
            for (size_t i = 1; i < material_count; ++i)
                if (profile.retract_mm[0] != profile.retract_mm[i] || profile.retract_speed_mm_s[0] != profile.retract_speed_mm_s[i])
                    return fail(FailureCode::UnsupportedProfile, "shared_nozzle_retraction_must_match");
        std::map<OperationRef, const BandPath*> paths;
        for (const auto& object : input.objects) {
            if (object.object == TOWER_OBJECT || !object.paths)
                return fail(FailureCode::InvalidInput, "reserved_tower_identity");
            for (const auto& path : object.paths->paths) {
                paths.emplace(OperationRef{object.object, path.id}, &path);
                for (const auto& segment : path.segments)
                    for (const auto& cell : segment.cells)
                        for (const auto& point : cell.upper)
                            if ((point.head<2>().array() < profile.bed_min.array() - 1e-6).any() ||
                                (point.head<2>().array() > profile.bed_max.array() + 1e-6).any() || point.z() > profile.max_print_height_mm)
                                return fail(FailureCode::UnsupportedProfile, "deposition_outside_build_volume");
            }
        }
        auto object_schedule = schedule_paths(input, limits.schedule, cancelled, stale);
        if (const auto* error = std::get_if<Failure>(&object_schedule))
            return *error;
        const auto& initial = *std::get<std::shared_ptr<const Schedule>>(object_schedule);
        if (initial.order.empty())
            return fail(FailureCode::InvalidInput, "empty_final_print");
        struct Run
        {
            size_t begin, end;
            double target_z;
            size_t transition;
            size_t tower_level = 0;
            double slot_begin = 0., slot_depth = 0.;
        };
        std::vector<Run> runs;
        FinalPrintPlan   plan;
        plan.revision = input.revision;
        plan.machine  = input.machine;
        plan.profile  = profile;
        std::array<bool, MAX_MATERIALS> used{};
        PhysicalMaterialId              previous;
        double                          tower_z = input.bed_z_mm, area_needed = 0.;
        for (size_t begin = 0; begin < initial.order.size();) {
            poll();
            const auto& first    = lookup(paths, initial.order[begin].operation);
            const auto  material = first.band.material;
            size_t      end      = begin;
            double      target_z = std::numeric_limits<double>::max();
            while (end < initial.order.size() && lookup(paths, initial.order[end].operation).band.material.value == material.value) {
                for (const auto& p : lookup(paths, initial.order[end].operation).vertices)
                    target_z = std::min(target_z, p.z());
                ++end;
            }
            const size_t physical = material.value - 1;
            double       required = used[physical] ? 0. : profile.purge.prime_mm3[physical];
            if (u1)
                required = std::max(profile.purge.prime_mm3[physical],
                                    previous.value ? profile.purge.volume_mm3[previous.value - 1][physical] : 0.);
            else if (previous.value != 0)
                required += profile.purge.volume_mm3[previous.value - 1][physical];
            if (input.machine.shared_nozzle && previous.value != 0 && required <= 0.)
                return fail(FailureCode::UnsupportedProfile, "shared_nozzle_purge_required");
            if (required > 0. && !profile.purge.enabled)
                return fail(FailureCode::UnsupportedProfile, "prime_or_purge_tower_required");
            if (profile.purge.enabled && required > 0. && !u1) {
                if (target_z - tower_z < profile.process.min_active_thickness_mm - profile.tolerances.interface_mm)
                    return fail(FailureCode::InfeasibleThickness, "purge_height_does_not_advance");
                area_needed = std::max(area_needed, required / (target_z - tower_z));
                tower_z     = target_z;
            }
            plan.transitions.push_back({first.band.macro_cycle, initial.order[begin].operation, previous, material, required, 0.});
            runs.push_back({begin, end, target_z, plan.transitions.size() - 1});
            previous       = material;
            used[physical] = true;
            begin          = end;
        }
        if (runs.size() > limits.max_operations || runs.size() > limits.max_temporary_bytes / 512)
            return fail(FailureCode::ResourceLimit, "tool_transition_limit");
        if (std::any_of(plan.transitions.begin(), plan.transitions.end(),
                        [](const ToolTransition& t) { return t.required_purge_mm3 > 0.; }) &&
            (!profile.purge.origin.allFinite() || !std::isfinite(profile.purge.width_mm) ||
             profile.purge.width_mm < 4. * profile.process.bead_width_mm))
            return fail(FailureCode::UnsupportedProfile, "invalid_purge_footprint");
        // Independent U1 nozzles prime into separate strips on a common tower
        // layer. A tool change does not require another increase in tower Z.
        // Quantized tower layers also accommodate unequal painted sublayer Zs.
        const double             tower_step = std::min(profile.process.macro_height_mm, profile.process.max_active_thickness_mm);
        std::map<size_t, double> layer_depths;
        if (u1) {
            if (!std::isfinite(tower_step) || tower_step < profile.process.min_active_thickness_mm)
                return fail(FailureCode::InfeasibleThickness, "purge_layer_thickness");
            size_t level = 1;
            for (auto& run : runs) {
                const double required = plan.transitions[run.transition].required_purge_mm3;
                if (required <= 0.)
                    continue;
                const double wanted = std::ceil((run.target_z - input.bed_z_mm) / tower_step - 1e-8);
                if (!std::isfinite(wanted) || wanted < 1. || wanted > double(limits.max_purge_layers))
                    return fail(FailureCode::ResourceLimit, "purge_layer_limit");
                level           = std::max(level, size_t(wanted));
                run.tower_level = level;
                run.slot_begin  = layer_depths[level];
                run.slot_depth  = std::max(4. * profile.process.bead_width_mm,
                                           std::ceil(required / (profile.purge.width_mm * tower_step) / profile.process.bead_width_mm) *
                                               profile.process.bead_width_mm);
                layer_depths[level] += run.slot_depth;
                area_needed = std::max(area_needed, layer_depths[level] * profile.purge.width_mm);
            }
        }
        ScheduleInput                                           combined = input;
        std::vector<OperationRef>                               sequence;
        std::map<uint64_t, std::pair<uint64_t, DepositionKind>> purge_metadata;
        auto                                                    tower = std::make_shared<PathPlan>();
        tower->revision                                               = input.revision;
        tower->machine                                                = input.machine;
        tower->solid_min = Vec3d(profile.purge.origin.x(), profile.purge.origin.y(), input.bed_z_mm);
        tower->solid_max = tower->solid_min;
        double depth     = 0.;
        if (area_needed > 0.) {
            if (!profile.purge.origin.allFinite() || !std::isfinite(profile.purge.width_mm) ||
                profile.purge.width_mm < 4. * profile.process.bead_width_mm)
                return fail(FailureCode::UnsupportedProfile, "invalid_purge_footprint");
            depth = std::max(4. * profile.process.bead_width_mm,
                             std::ceil(area_needed / profile.purge.width_mm / profile.process.bead_width_mm) *
                                 profile.process.bead_width_mm);
            if (!std::isfinite(depth) || (profile.purge.origin.array() < profile.bed_min.array()).any() ||
                ((profile.purge.origin + Vec2d(profile.purge.width_mm, depth)).array() > profile.bed_max.array()).any())
                return fail(FailureCode::UnsupportedProfile, "purge_tower_outside_bed");
        }
        size_t purge_layers = 0, purge_segments = 0;
        tower_z = input.bed_z_mm;
        for (const auto& run : runs) {
            poll();
            auto& transition = plan.transitions[run.transition];
            if (transition.required_purge_mm3 > 0.) {
                struct Patch
                {
                    Vec2d  origin;
                    double depth, bottom, top;
                };
                std::vector<Patch> patches;
                if (u1) {
                    // Complete supporting layers before opening slots at the
                    // next height. The last slot also fills unused footprint,
                    // so later layers never rely on unprinted tower material.
                    const double bottom = input.bed_z_mm + (run.tower_level - 1) * tower_step;
                    for (double z = tower_z; z < bottom - 1e-8; z += tower_step)
                        patches.push_back({profile.purge.origin, depth, z, z + tower_step});
                    tower_z               = bottom;
                    const double slot_end = run.slot_begin + run.slot_depth;
                    const bool   last     = std::abs(slot_end - layer_depths.at(run.tower_level)) < 1e-8;
                    patches.push_back({profile.purge.origin + Vec2d(0., run.slot_begin), last ? depth - run.slot_begin : run.slot_depth,
                                       bottom, bottom + tower_step});
                    if (last)
                        tower_z = bottom + tower_step;
                } else {
                    const double delta = run.target_z - tower_z;
                    const double count = std::ceil(delta / profile.process.max_active_thickness_mm - 1e-10);
                    if (!std::isfinite(count) || count < 1. || count > double(limits.max_purge_layers - purge_layers))
                        return fail(FailureCode::ResourceLimit, "purge_layer_limit");
                    const double height = delta / count;
                    if (height < profile.process.min_active_thickness_mm - profile.tolerances.interface_mm)
                        return fail(FailureCode::InfeasibleThickness, "purge_layer_thickness");
                    for (size_t layer = 0; layer < size_t(count); ++layer)
                        patches.push_back({profile.purge.origin, depth, tower_z + layer * height, tower_z + (layer + 1) * height});
                    tower_z = run.target_z;
                }
                for (const auto& patch : patches) {
                    poll();
                    if (purge_layers >= limits.max_purge_layers)
                        return fail(FailureCode::ResourceLimit, "purge_layer_limit");
                    const double bottom = patch.bottom, top = patch.top;
                    ClippedBand  band;
                    band.id         = {++purge_layers, purge_layers, transition.to, transition.to.value - 1};
                    band.solid      = box(patch.origin, profile.purge.width_mm, patch.depth, bottom, top);
                    band.volume_mm3 = profile.purge.width_mm * patch.depth * (top - bottom);
                    CoverageLimits coverage;
                    coverage.minimum_bead_width_mm = input.machine.coplanar_tip_contact ?
                                                         .04 :
                                                         std::max(.04, 2.1 * (input.machine.sections.front().radius_mm +
                                                                              input.machine.clearance_mm));
                    NumericTolerances tolerance    = profile.tolerances;
                    tolerance.absolute_volume_mm3  = std::max(tolerance.absolute_volume_mm3, band.volume_mm3 * tolerance.relative_volume);
                    auto built = plan_pure_cycle(band, input.revision, profile.process, tolerance, input.machine, limits.paths, coverage,
                                                 cancelled, stale);
                    if (const auto* error = std::get_if<Failure>(&built))
                        return *error;
                    const auto& paths = *std::get<std::shared_ptr<const PathPlan>>(built);
                    for (const auto& source : paths.paths) {
                        if (tower->paths.size() >= limits.paths.max_paths ||
                            source.segments.size() > limits.paths.max_segments - purge_segments)
                            return fail(FailureCode::ResourceLimit, "purge_path_limit");
                        purge_segments += source.segments.size();
                        if (purge_segments > limits.max_temporary_bytes / 2048)
                            return fail(FailureCode::ResourceLimit, "purge_memory_limit");
                        auto path = source;
                        path.id   = tower->paths.size() + 1;
                        sequence.push_back({TOWER_OBJECT, path.id});
                        purge_metadata.emplace(path.id,
                                               std::make_pair(transition.logical_cycle,
                                                              transition.from.value == 0 ? DepositionKind::Prime : DepositionKind::Purge));
                        for (const auto& segment : path.segments)
                            transition.deposited_purge_mm3 += segment.volume_mm3;
                        tower->paths.push_back(std::move(path));
                    }
                    tower->solid_max = Vec3d(profile.purge.origin.x() + profile.purge.width_mm, profile.purge.origin.y() + depth, top);
                }
                if (transition.deposited_purge_mm3 + profile.tolerances.absolute_volume_mm3 < transition.required_purge_mm3)
                    return fail(FailureCode::MaterialToleranceExceeded, "insufficient_planned_purge");
            }
            for (size_t index = run.begin; index < run.end; ++index)
                sequence.push_back(initial.order[index].operation);
        }
        if (!tower->paths.empty()) {
            combined.objects.push_back({TOWER_OBJECT, tower});
            for (const auto& path : tower->paths)
                paths.emplace(OperationRef{TOWER_OBJECT, path.id}, &path);
        }
        if (sequence.size() > limits.schedule.max_edges)
            return fail(FailureCode::ResourceLimit, "combined_sequence_limit");
        for (size_t i = 1; i < sequence.size(); ++i)
            combined.dependencies.push_back({sequence[i - 1], sequence[i], DependencyKind::Explicit});
        auto scheduled = schedule_paths(combined, limits.schedule, cancelled, stale);
        if (const auto* error = std::get_if<Failure>(&scheduled))
            return *error;
        plan.schedule = std::get<std::shared_ptr<const Schedule>>(scheduled);
        if (plan.schedule->order.size() != sequence.size())
            return fail(FailureCode::InvalidInput, "combined_sequence_size");
        double dock_clearance_z = input.bed_z_mm + 2.;
        for (const auto& object : plan.schedule->objects)
            for (const auto& path : object.paths->paths)
                for (const auto& segment : path.segments)
                    for (const auto& cell : segment.cells)
                        for (const auto& point : cell.upper)
                            dock_clearance_z = std::max(dock_clearance_z, point.z() + 2.);
        if (u1 && (input.initial_position.z() < dock_clearance_z - 1e-6 || input.initial_position.z() > profile.max_print_height_mm ||
                   dock_clearance_z > profile.max_print_height_mm))
            return fail(FailureCode::UnsupportedProfile, "u1_dock_clearance_outside_build_volume");
        Vec3d      current_position = input.initial_position;
        const auto append           = [&](PrintOperation op) {
            if (plan.operations.size() >= limits.max_operations ||
                plan.operations.size() >= limits.max_temporary_bytes / (2 * sizeof(PrintOperation)))
                throw Abort{FailureCode::ResourceLimit, "final_operation_limit"};
            if (op.kind == OperationKind::Travel || op.kind == OperationKind::Deposit) {
                if ((op.position.head<2>().array() < profile.bed_min.array() - 1e-6).any() ||
                    (op.position.head<2>().array() > profile.bed_max.array() + 1e-6).any() || op.position.z() < input.bed_z_mm - 1e-6 ||
                    op.position.z() > profile.max_print_height_mm)
                    throw Abort{FailureCode::UnsupportedProfile, "final_move_outside_build_volume"};
            }
            op.id = plan.operations.size() + 1;
            if (op.kind == OperationKind::Travel || op.kind == OperationKind::Deposit)
                current_position = op.position;
            plan.operations.push_back(std::move(op));
        };
        const auto state = [&](OperationKind kind, double value = 0., PhysicalMaterialId material = {}, bool wait = false) {
            PrintOperation op;
            op.kind     = kind;
            op.value    = value;
            op.material = material;
            op.wait     = wait;
            append(op);
        };
        state(OperationKind::Initialize);
        if (u1)
            state(OperationKind::U1Start, 0., plan.transitions.front().to);
        if (profile.home) {
            PrintOperation op;
            op.kind     = OperationKind::Home;
            op.position = input.initial_position;
            append(op);
        }
        state(OperationKind::BedTemperature, u1 ? profile.initial_bed_temperature : profile.bed_temperature, {}, true);
        for (uint32_t physical = 1; physical <= material_count; ++physical)
            if (used[physical - 1])
                state(OperationKind::NozzleTemperature, profile.nozzle_temperature[physical - 1], {physical}, false);
        state(OperationKind::Fan, profile.fan_percent);
        std::map<uint64_t, double> cycle_tops;
        for (const auto& object : input.objects)
            for (const auto& path : object.paths->paths)
                for (const auto& vertex : path.vertices)
                    cycle_tops[path.band.macro_cycle] = std::max(cycle_tops[path.band.macro_cycle], vertex.z());
        PhysicalMaterialId                active;
        std::array<double, MAX_MATERIALS> heated{};
        double                            fan            = profile.fan_percent;
        bool                              initial_bed    = u1;
        uint64_t                          reported_cycle = 0;
        for (size_t index = 0; index < plan.schedule->order.size(); ++index) {
            poll();
            const auto& item = plan.schedule->order[index];
            if (!(item.operation == sequence[index]))
                return fail(FailureCode::IncompatibleOrder, "combined_tool_sequence_changed");
            const auto&    path = lookup(paths, item.operation);
            PrintOperation base;
            base.source          = item.operation;
            base.band            = path.band;
            base.interface_id    = path.shared_interface;
            base.lower_interface = path.lower_interface;
            base.upper_interface = path.upper_interface;
            base.role            = item.requires_bridge && path.role != PathRole::Perimeter ? PathRole::Bridge : path.role;
            if (path.role == PathRole::Brim)
                base.deposition = DepositionKind::Brim;
            base.material      = path.band.material;
            base.logical_cycle = path.band.macro_cycle;
            if (item.operation.object == TOWER_OBJECT) {
                const auto& metadata = purge_metadata.at(item.operation.path);
                base.logical_cycle   = metadata.first;
                base.deposition      = metadata.second;
            }
            if (initial_bed && base.logical_cycle > 0) {
                state(OperationKind::BedTemperature, profile.bed_temperature);
                initial_bed = false;
            }
            if (u1 && base.logical_cycle + 1 > reported_cycle)
                state(OperationKind::U1Layer, double(reported_cycle = base.logical_cycle + 1));
            if (active.value != 0) {
                double distance = 0.;
                bool   lifted   = false;
                for (size_t i = 1; i < item.travel.size(); ++i) {
                    distance += (item.travel[i] - item.travel[i - 1]).norm();
                    lifted |= std::abs(item.travel[i].z() - item.travel[i - 1].z()) > 1e-12;
                }
                if (active.value != path.band.material.value || lifted || distance >= profile.retract_min_travel_mm[active.value - 1])
                    state(OperationKind::Retract, profile.retract_mm[active.value - 1], active);
            }
            const auto change = [&] {
                const auto   next        = path.band.material;
                const double temperature = u1 && base.logical_cycle == 0 ? profile.initial_nozzle_temperature[next.value - 1] :
                                                                           profile.nozzle_temperature[next.value - 1];
                if (u1 && heated[next.value - 1] != temperature) {
                    // Wait while the incoming tool is still parked.
                    state(OperationKind::NozzleTemperature, temperature, next, true);
                    heated[next.value - 1] = temperature;
                }
                if (active.value != path.band.material.value) {
                    if (u1) {
                        if (current_position.z() < dock_clearance_z - 1e-6)
                            throw Abort{FailureCode::ClearanceFailure, "u1_toolchange_below_dock_clearance"};
                        auto op     = base;
                        op.kind     = OperationKind::U1ToolChange;
                        op.position = current_position;
                        append(op);
                    } else
                        state(OperationKind::ToolChange, 0., path.band.material);
                    active = path.band.material;
                }
                if (heated[active.value - 1] != temperature) {
                    state(OperationKind::NozzleTemperature, temperature, active, true);
                    heated[active.value - 1] = temperature;
                }
                if (u1) {
                    const double target_fan = base.logical_cycle < uint64_t(profile.fan_disabled_cycles[active.value - 1]) ?
                                                  0. :
                                                  profile.material_fan_percent[active.value - 1];
                    if (fan != target_fan)
                        state(OperationKind::Fan, fan = target_fan);
                }
            };
            const bool change_before_travel = u1 && current_position.z() >= dock_clearance_z - 1e-6;
            if (item.travel.size() <= 1 || change_before_travel)
                change();
            for (size_t i = 1; i < item.travel.size(); ++i) {
                auto op     = base;
                op.kind     = OperationKind::Travel;
                op.position = item.travel[i];
                append(op);
                if (i == 1 && !change_before_travel)
                    change();
            }
            state(OperationKind::Recover, 0., active);
            for (size_t segment = 0; segment < path.segments.size(); ++segment) {
                const auto& s  = path.segments[segment];
                auto        op = base;
                op.kind        = OperationKind::Deposit;
                op.position    = path.vertices[segment + 1];
                op.volume_mm3  = s.volume_mm3;
                op.speed_mm_s  = item.requires_bridge || base.role == PathRole::Bridge ? std::min(s.speed_mm_s, 20.) : s.speed_mm_s;
                if (base.logical_cycle == 0) {
                    const bool wall = base.role == PathRole::Perimeter || base.deposition == DepositionKind::Brim;
                    op.speed_mm_s   = std::min(op.speed_mm_s, wall ? profile.initial_wall_speed_mm_s[active.value - 1] :
                                                                     profile.initial_infill_speed_mm_s[active.value - 1]);
                }
                op.width_mm        = s.width_mm;
                op.height_start_mm = s.height_start_mm;
                op.height_end_mm   = s.height_end_mm;
                append(op);
                (op.deposition == DepositionKind::Object ? plan.object_volume_mm3 :
                 op.deposition == DepositionKind::Brim   ? plan.brim_volume_mm3 :
                                                           plan.purge_volume_mm3)[active.value - 1] += op.volume_mm3;
            }
        }
        if (active.value != 0)
            state(OperationKind::Retract, profile.retract_mm[active.value - 1], active);
        for (size_t i = 1; i < plan.schedule->final_travel.size(); ++i) {
            PrintOperation op;
            op.kind     = OperationKind::Travel;
            op.position = plan.schedule->final_travel[i];
            append(op);
        }
        if (u1)
            state(OperationKind::U1End);
        state(OperationKind::Fan, 0.);
        for (uint32_t physical = 1; physical <= material_count; ++physical)
            if (used[physical - 1])
                state(OperationKind::NozzleTemperature, 0., {physical});
        state(OperationKind::BedTemperature, 0.);
        // Logical rows own potentially disjoint operation ranges. This remains
        // correct for explicit access dependencies or sequential object order.
        std::map<uint64_t, size_t> cycle_index;
        for (const auto& entry : cycle_tops) {
            cycle_index.emplace(entry.first, plan.cycles.size());
            plan.cycles.push_back({entry.first, entry.second, {}});
        }
        for (size_t i = 0; i < plan.operations.size(); ++i) {
            const auto& op = plan.operations[i];
            if (op.kind != OperationKind::Deposit && op.kind != OperationKind::Travel)
                continue;
            if (op.source.object == 0)
                continue;
            auto& ranges = plan.cycles.at(cycle_index.at(op.logical_cycle)).operation_ranges;
            if (!ranges.empty() && ranges.back().second + 1 == i)
                ranges.back().second = i;
            else
                ranges.emplace_back(i, i);
        }
        poll();
        return std::make_shared<const FinalPrintPlan>(std::move(plan));
    } catch (const Abort& error) {
        return fail(error.code, error.detail);
    } catch (const std::bad_alloc&) {
        return fail(FailureCode::ResourceLimit, "final_print_allocation");
    } catch (const std::exception&) {
        return fail(FailureCode::InvalidInput, "final_print_construction");
    }
}

void emit_print(const FinalPrintPlan&                          plan,
                GCodeWriter&                                   writer,
                const std::function<void(const std::string&)>& emit,
                const std::function<bool()>&                   cancelled)
{
    if (plan.profile.adapter == MachineAdapter::SnapmakerU1)
        writer.config.gcode_flavor.value = gcfKlipper;
    writer.config.use_relative_e_distances.value = plan.profile.relative_extrusion;
    writer.config.filament_diameter.values.assign(plan.machine.filament_diameter_mm.begin(),
                                                  plan.machine.filament_diameter_mm.begin() + plan.machine.material_count);
    writer.config.retraction_length.values.assign(plan.profile.retract_mm.begin(),
                                                  plan.profile.retract_mm.begin() + plan.machine.material_count);
    writer.config.retraction_speed.values.assign(plan.profile.retract_speed_mm_s.begin(),
                                                 plan.profile.retract_speed_mm_s.begin() + plan.machine.material_count);
    writer.config.deretraction_speed.values = writer.config.retraction_speed.values;
    writer.config.retract_restart_extra.values.assign(plan.machine.material_count, 0.);
    writer.config.use_firmware_retraction.value        = false;
    writer.config.single_extruder_multi_material.value = plan.machine.shared_nozzle;
    writer.config.travel_speed_z.value                 = plan.machine.max_z_mm_s;
    std::vector<unsigned int> extruders;
    for (unsigned int i = 0; i < plan.machine.material_count; ++i)
        extruders.push_back(i);
    writer.set_extruders(extruders);
    writer.set_is_first_layer(false);
    writer.set_position(plan.schedule->initial_position);
    std::map<uint64_t, double> tops;
    for (const auto& cycle : plan.cycles)
        tops.emplace(cycle.id, cycle.top_z_mm);
    for (const auto& op : plan.operations) {
        if (cancelled && cancelled())
            throw std::runtime_error("Material band export cancelled");
        if (op.source.object != 0)
            emit(";MB_CYCLE " + std::to_string(op.logical_cycle) + " TOP " + std::to_string(tops.at(op.logical_cycle)) + " OBJECT " +
                 std::to_string(op.source.object) + " DOMAIN " + std::to_string(op.band.domain) + " MATERIAL " +
                 std::to_string(op.material.value) + " SLOT " + std::to_string(op.band.stack_slot) + " INTERFACE " +
                 std::to_string(op.interface_id) + " OPERATION " + std::to_string(op.id) + "\n");
        switch (op.kind) {
        case OperationKind::Initialize:
            emit(plan.profile.adapter == MachineAdapter::SnapmakerU1 ? "; generated by " + header_slic3r_generated() + "\n" :
                                                                       "; generated by OrcaSlicer FullSpectrum\n");
            emit("; FullSpectrum material_bands final plan v1\n; physical qualification "
                 "deferred\nG21\nG90\n");
            emit(plan.profile.relative_extrusion ? "M83\n" : "M82\n");
            break;
        case OperationKind::Home:
            emit("G28\n");
            writer.set_position(op.position);
            break;
        case OperationKind::U1Start:
            emit(u1_start_code(plan, op.material));
            for (unsigned i = 0; i < plan.machine.material_count; ++i)
                if (plan.object_volume_mm3[i] + plan.purge_volume_mm3[i] + plan.brim_volume_mm3[i] > 0.) {
                    // The startup bin-prime macro already performed this retract.
                    (void) writer.set_extruder(i);
                    (void) writer.retract(false, plan.profile.retract_mm[i]);
                }
            (void) writer.set_extruder(op.material.value - 1);
            emit(writer.reset_e(true));
            writer.set_position(plan.schedule->initial_position);
            break;
        case OperationKind::U1ToolChange:
            emit(";MB_U1_DOCK_BEGIN\nM400\n");
            emit(writer.set_extruder(op.material.value - 1));
            emit(u1_return_code(op.position, plan.machine.max_z_mm_s));
            writer.set_position(op.position);
            emit(plan.profile.relative_extrusion ? "M83\n" : "M82\n");
            emit(";MB_U1_DOCK_END\n");
            break;
        case OperationKind::U1End: emit(";MB_U1_END_BEGIN\nM400\nPRINT_END\nTIMELAPSE_STOP\n;MB_U1_END_END\n"); break;
        case OperationKind::U1Layer:
            emit("SET_PRINT_STATS_INFO TOTAL_LAYER=" + std::to_string(plan.cycles.size()) +
                 " CURRENT_LAYER=" + std::to_string(int(op.value)) + "\n");
            break;
        case OperationKind::BedTemperature: emit(writer.set_bed_temperature(int(std::lround(op.value)), op.wait)); break;
        case OperationKind::NozzleTemperature:
            // RRF distinguishes temperature from firmware retraction by P.
            // The instance overload suppresses P for shared-nozzle profiles.
            emit(GCodeWriter::set_temperature(unsigned(std::lround(op.value)), writer.config.gcode_flavor.value, op.wait,
                                              int(op.material.value - 1)));
            break;
        case OperationKind::Fan: emit(writer.set_fan(unsigned(std::lround(op.value)))); break;
        case OperationKind::ToolChange: emit(writer.set_extruder(op.material.value - 1)); break;
        case OperationKind::Retract:
            emit(";MB_OPERATION RETRACT\n");
            emit(writer.retract(false, op.value));
            break;
        case OperationKind::Recover:
            emit(";MB_OPERATION RECOVER\n");
            emit(writer.unretract());
            break;
        case OperationKind::Travel: {
            emit(";MB_OPERATION TRAVEL\n");
            const Vec3d from = writer.get_position();
            if ((from.head<2>() - op.position.head<2>()).norm() <= 1e-12)
                emit(writer.travel_to_z(op.position.z(), "material band travel", true));
            else if (std::abs(from.z() - op.position.z()) <= 1e-12)
                emit(writer.travel_to_xy(op.position.head<2>(), "material band travel"));
            else
                throw std::logic_error("Unvalidated mixed-axis travel in final plan");
            break;
        }
        case OperationKind::Deposit: {
            emit(";MB_BEAD WIDTH " + std::to_string(op.width_mm) + " HEIGHT_START " + std::to_string(op.height_start_mm) + " HEIGHT_END " +
                 std::to_string(op.height_end_mm) + "\n");
            emit(op.deposition == DepositionKind::Object ? ";MB_OPERATION DEPOSITION\n" :
                 op.deposition == DepositionKind::Brim   ? ";MB_OPERATION BRIM\n" :
                 op.deposition == DepositionKind::Prime  ? ";MB_OPERATION PRIME\n" :
                                                           ";MB_OPERATION PURGE\n");
            emit(std::string(";TYPE:") +
                 (op.deposition == DepositionKind::Brim   ? "Skirt" :
                  op.deposition != DepositionKind::Object ? "Wipe tower" :
                  op.role == PathRole::Perimeter          ? "External perimeter" :
                  op.role == PathRole::SparseFill         ? "Internal infill" :
                  op.role == PathRole::Bridge             ? "Bridge infill" :
                                                            "Internal solid infill") +
                 "\n");
            emit(";WIDTH:" + std::to_string(op.width_mm) + "\n;HEIGHT:" + std::to_string((op.height_start_mm + op.height_end_mm) / 2.) +
                 "\n");
            emit(writer.set_speed(op.speed_mm_s * 60.));
            const double d = plan.machine.filament_diameter_mm.at(op.material.value - 1);
            emit(writer.extrude_to_xyz(op.position, op.volume_mm3 / (PI * d * d / 4.)));
            break;
        }
        }
    }
}
} // namespace Slic3r::MaterialBand
