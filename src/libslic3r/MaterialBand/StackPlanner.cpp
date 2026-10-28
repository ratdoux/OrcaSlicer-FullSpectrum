#include "StackPlanner.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

namespace Slic3r::MaterialBand {
namespace {
bool same_intent(const RegionIntent& a, const RegionIntent& b)
{
    if (a.fractions != b.fractions || a.height_gradient.size() != b.height_gradient.size())
        return false;
    for (size_t i = 0; i < a.height_gradient.size(); ++i)
        if (a.height_gradient[i].position != b.height_gradient[i].position ||
            a.height_gradient[i].fractions != b.height_gradient[i].fractions)
            return false;
    return true;
}
InterfacePatch upper_patch(const ClippedBand& band, double top, InterfaceId id, double epsilon)
{
    InterfacePatch               result{id, band.id.domain, band.id.macro_cycle, {}};
    std::map<uint32_t, uint32_t> vertices;
    for (const auto& tri : band.solid.triangles) {
        const auto& points = band.solid.vertices;
        if ((points[tri[1]] - points[tri[0]]).cross(points[tri[2]] - points[tri[0]]).z() <= 0. ||
            std::any_of(tri.begin(), tri.end(), [&](uint32_t i) { return std::abs(points[i].z() - top) > epsilon; }))
            continue;
        std::array<uint32_t, 3> face;
        for (size_t k = 0; k < 3; ++k) {
            const auto entry = vertices.emplace(tri[k], uint32_t(result.surface.vertices.size()));
            if (entry.second)
                result.surface.vertices.push_back(points[tri[k]]);
            face[k] = entry.first->second;
        }
        result.surface.triangles.push_back(face);
    }
    return result;
}
} // namespace

BoxPlanResult plan_material_stack(const KernelInput&           input,
                                  const MachineEnvelope&       machine,
                                  DomainId                     domain,
                                  size_t                       perimeter_loops,
                                  const DenseInfillProfile&    infill,
                                  const PathLimits&            limits,
                                  const std::function<bool()>& cancelled,
                                  const std::function<bool()>& stale)
{
    const auto fail = [&](FailureCode code, const char* detail) -> BoxPlanResult {
        return Failure{input.revision, code, detail, {}, {}, {}};
    };
    const auto interrupted = [&]() -> std::optional<Failure> {
        if (cancelled && cancelled())
            return Failure{input.revision, FailureCode::Cancelled, "cancelled_during_stack_paths", {}, {}, {}};
        if (stale && stale())
            return Failure{input.revision, FailureCode::StaleInput, "revision_changed", {}, {}, {}};
        return {};
    };
    if (auto error = interrupted())
        return *error;
    if (input.solid.vertices.empty() || input.regions.empty() || input.material_count != machine.material_count ||
        input.material_count < 2 || input.material_count > MAX_MATERIALS || machine.sections.empty())
        return fail(FailureCode::InvalidInput, "invalid_material_stack_input");
    const auto& intent = input.regions.front();
    if (!std::all_of(input.regions.begin(), input.regions.end(), [&](const RegionIntent& other) { return same_intent(intent, other); }))
        return fail(FailureCode::UnsupportedTopology, "multimaterial_painted_shell_requires_domain_adapter");
    for (size_t i = 0; i < intent.height_gradient.size(); ++i) {
        const auto& knot = intent.height_gradient[i];
        if (!std::isfinite(knot.position) || knot.position < 0. || knot.position > 1. ||
            (i && knot.position <= intent.height_gradient[i - 1].position))
            return fail(FailureCode::InvalidInput, "invalid_height_gradient_curve");
        double sum = 0.;
        for (size_t m = 0; m < MAX_MATERIALS; ++m) {
            if (!std::isfinite(knot.fractions[m]) || knot.fractions[m] < 0. || knot.fractions[m] > 1. ||
                (m >= input.material_count && knot.fractions[m] != 0.))
                return fail(FailureCode::InvalidInput, "invalid_height_gradient_fractions");
            sum += knot.fractions[m];
        }
        if (std::abs(sum - 1.) > 1e-10)
            return fail(FailureCode::InvalidInput, "height_gradient_fractions_do_not_close");
    }
    Vec3d low = input.solid.vertices.front(), high = low;
    for (const auto& vertex : input.solid.vertices) {
        low  = low.cwiseMin(vertex);
        high = high.cwiseMax(vertex);
    }
    if (!low.allFinite() || !high.allFinite() || std::abs(low.z()) > input.tolerances.macro_closure_mm)
        return fail(FailureCode::UnsupportedProfile, "object_requires_support");
    const double total_height = high.z() - low.z(), height = input.process.macro_height_mm;
    if (!std::isfinite(height) || height <= 0. || total_height <= 0.)
        return fail(FailureCode::InvalidInput, "invalid_stack_height");
    const double cycles_double = std::ceil((total_height - input.tolerances.interface_mm) / height);
    if (cycles_double < 1. || cycles_double > double(limits.max_cycles) || cycles_double > 10000.)
        return fail(FailureCode::ResourceLimit, "stack_cycle_limit");
    const size_t count = size_t(cycles_double);
    if (count > limits.max_temporary_bytes / 1024)
        return fail(FailureCode::ResourceLimit, "stack_cycle_memory_limit");
    std::vector<std::shared_ptr<const ClippedBand>> slabs;
    std::vector<StackDoseCycle>                     doses;
    size_t                                          retained_bytes = count * 1024;
    for (size_t cycle = 0; cycle < count; ++cycle) {
        if (auto error = interrupted())
            return *error;
        const double bottom = low.z() + cycle * height, top = cycle + 1 == count ? high.z() : bottom + height;
        auto clipped = clip_pure_slab(input.solid, {domain, cycle, {1}, 0}, input.revision, bottom, top, input.tolerances, {}, cancelled,
                                      stale);
        if (const auto* error = std::get_if<Failure>(&clipped))
            return *error;
        auto         slab  = std::get<std::shared_ptr<const ClippedBand>>(clipped);
        const size_t bytes = slab->solid.vertices.size() * sizeof(Vec3d) + slab->solid.triangles.size() * sizeof(std::array<uint32_t, 3>);
        if (bytes > limits.max_temporary_bytes - retained_bytes)
            return fail(FailureCode::ResourceLimit, "stack_slab_memory_limit");
        retained_bytes += bytes;
        doses.push_back({sample_height_gradient(intent, ((bottom + top) / 2. - low.z()) / total_height), top - bottom, slab->volume_mm3});
        slabs.push_back(std::move(slab));
    }
    const double allowance = intent.height_gradient.empty() ? 0. : input.gradient_fraction_error;
    auto solved = solve_stack_doses(doses, input.material_count, input.process, allowance, input.tolerances, input.revision, {}, cancelled,
                                    stale);
    if (const auto* error = std::get_if<Failure>(&solved))
        return *error;
    const auto& solution = *std::get<std::shared_ptr<const StackDoseSolution>>(solved);
    auto        plan     = std::make_shared<PathPlan>();
    plan->revision       = input.revision;
    plan->machine        = machine;
    plan->solid_min      = low;
    plan->solid_max      = high;
    size_t segments      = 0;
    for (size_t cycle = 0; cycle < count; ++cycle) {
        if (auto error = interrupted())
            return *error;
        const auto&         slab   = *slabs[cycle];
        const double        bottom = low.z() + cycle * height, top = cycle + 1 == count ? high.z() : bottom + height;
        const auto&         fractions = solution.fractions[cycle];
        std::vector<size_t> active;
        for (size_t m = 0; m < input.material_count; ++m)
            if (fractions[m] > 0.)
                active.push_back(m);
        double       previous_z = bottom, cumulative = 0.;
        InterfaceId  lower_interface = 0;
        const size_t smallest        = *std::min_element(active.begin(), active.end(),
                                                         [&](size_t a, size_t b) { return fractions[a] < fractions[b]; });
        const double dose_tolerance  = std::max(input.tolerances.absolute_volume_mm3 / (8. * count),
                                                input.tolerances.relative_volume * slab.volume_mm3 * fractions[size_t(smallest)] / 8.);
        for (size_t ai = 0; ai < active.size(); ++ai) {
            const size_t m = active[ai];
            cumulative += fractions[m];
            double boundary = top;
            if (ai + 1 < active.size()) {
                const double target = slab.volume_mm3 * cumulative;
                double       left = previous_z, right = top;
                boundary     = bottom + (top - bottom) * cumulative;
                bool matched = false;
                for (size_t iteration = 0; iteration < 32; ++iteration) {
                    auto part = clip_pure_slab(slab.solid, {domain, cycle, {uint32_t(m + 1)}, uint32_t(m)}, input.revision, bottom,
                                               boundary, input.tolerances, {}, cancelled, stale);
                    if (const auto* error = std::get_if<Failure>(&part))
                        return *error;
                    const double volume = std::get<std::shared_ptr<const ClippedBand>>(part)->volume_mm3;
                    if (std::abs(volume - target) <= dose_tolerance) {
                        matched = true;
                        break;
                    }
                    if (volume < target)
                        left = boundary;
                    else
                        right = boundary;
                    boundary = (left + right) / 2.;
                }
                if (!matched)
                    return fail(FailureCode::NonConverged, "stack_clipped_volume_correction_limit");
            }
            const double thickness = boundary - previous_z;
            if (thickness < input.process.min_active_thickness_mm - input.tolerances.interface_mm ||
                thickness > input.process.max_active_thickness_mm + input.tolerances.interface_mm)
                return fail(FailureCode::InfeasibleThickness, "clipped_stack_active_thickness");
            auto part = clip_pure_slab(slab.solid, {domain, cycle, {uint32_t(m + 1)}, uint32_t(m)}, input.revision, previous_z, boundary,
                                       input.tolerances, {}, cancelled, stale);
            if (const auto* error = std::get_if<Failure>(&part))
                return *error;
            const auto& band = *std::get<std::shared_ptr<const ClippedBand>>(part);
            if (std::abs(band.volume_mm3 - fractions[m] * slab.volume_mm3) > 3. * dose_tolerance)
                return fail(FailureCode::MaterialToleranceExceeded, "clipped_stack_partition_volume");
            const InterfaceId upper_interface = ai + 1 < active.size() ? 1 + cycle * MAX_MATERIALS + m : 0;
            if (upper_interface) {
                auto patch = upper_patch(band, boundary, upper_interface, input.tolerances.interface_mm);
                if (patch.surface.triangles.empty())
                    return fail(FailureCode::UnsupportedTopology, "empty_stack_interface");
                const size_t bytes = sizeof(InterfacePatch) + patch.surface.vertices.capacity() * sizeof(Vec3d) +
                                     patch.surface.triangles.capacity() * sizeof(std::array<uint32_t, 3>);
                if (bytes > limits.max_temporary_bytes - retained_bytes)
                    return fail(FailureCode::ResourceLimit, "stack_interface_memory_limit");
                retained_bytes += bytes;
                plan->interfaces.push_back(std::move(patch));
            }
            CoverageLimits coverage;
            coverage.infill                = infill.cycle(cycle, count);
            coverage.perimeter_loops       = perimeter_loops;
            coverage.minimum_bead_width_mm = machine.coplanar_tip_contact ?
                                                 .04 :
                                                 std::max(.04, 2.1 * (machine.sections.front().radius_mm + machine.clearance_mm));
            auto generated = plan_pure_cycle(band, input.revision, input.process, input.tolerances, machine, limits, coverage, cancelled,
                                             stale);
            if (const auto* error = std::get_if<Failure>(&generated))
                return *error;
            const auto& paths = *std::get<std::shared_ptr<const PathPlan>>(generated);
            for (auto path : paths.paths) {
                if (plan->paths.size() >= limits.max_paths || path.segments.size() > limits.max_segments - segments)
                    return fail(FailureCode::ResourceLimit, "stack_path_limit");
                segments += path.segments.size();
                size_t bytes = sizeof(BandPath) + path.vertices.capacity() * sizeof(Vec3d) + path.segments.capacity() * sizeof(PathSegment);
                for (const auto& s : path.segments)
                    bytes += s.cells.capacity() * sizeof(BeadCell) + s.coverage_triangles.capacity() * sizeof(std::array<Vec2d, 3>) +
                             s.footprint.capacity() * sizeof(Vec2d);
                if (bytes > limits.max_temporary_bytes - retained_bytes)
                    return fail(FailureCode::ResourceLimit, "stack_path_memory_limit");
                retained_bytes += bytes;
                path.id               = plan->paths.size() + 1;
                path.lower_interface  = lower_interface;
                path.upper_interface  = upper_interface;
                path.shared_interface = upper_interface ? upper_interface : lower_interface;
                plan->paths.push_back(std::move(path));
            }
            plan->volume_mm3[m] += paths.volume_mm3[m];
            auto&       destination = plan->coverage[m];
            const auto& source      = paths.coverage[m];
            destination.target_mm3 += source.target_mm3;
            destination.deposited_mm3 += source.deposited_mm3;
            destination.missing_mm3 += source.missing_mm3;
            destination.excess_mm3 += source.excess_mm3;
            previous_z      = boundary;
            lower_interface = upper_interface;
        }
    }
    for (size_t m = 0; m < input.material_count; ++m)
        if (std::abs(plan->volume_mm3[m] - solution.target_volume_mm3[m]) >
            std::max(input.tolerances.absolute_volume_mm3, input.tolerances.relative_volume * solution.target_volume_mm3[m]))
            return fail(FailureCode::MaterialToleranceExceeded, "stack_planned_material_budget");
    if (auto error = interrupted())
        return *error;
    return std::shared_ptr<const PathPlan>(std::move(plan));
}
} // namespace Slic3r::MaterialBand
