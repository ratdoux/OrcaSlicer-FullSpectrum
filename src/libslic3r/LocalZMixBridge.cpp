#include "LocalZMixBridge.hpp"
#include "ClipperUtils.hpp"
#include "Layer.hpp"
#include "Fill/FillBase.hpp"
#include <boost/log/trivial.hpp>
#include <unordered_map>
#include "Print.hpp"
#include "BoundingBox.hpp"
#include "AABBTreeLines.hpp"
#include <array>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>

namespace Slic3r {

static ExPolygons union_owned_regions(const ExPolygons& masks)
{
    // Rounded touching holes can leave a negative-winding sliver outside the
    // contour. Non-zero union may turn it into material or misplace a hole
    // when adjacent recipe masks are combined. Only positive winding owns
    // volume; retain the same rule for ownership, paths and distance fields.
    return union_ex(to_polygons(masks), ClipperLib::pftPositive);
}

struct LocalZMixBridgeGeometry
{
    std::vector<AABBTreeLines::LinesDistancer<Line>>              boundaries;
    std::vector<bool>                                             empty_boundaries;
    std::vector<AABBTreeLines::LinesDistancer<Line>>              tongues;
    std::vector<AABBTreeLines::LinesDistancer<Line>>              extras;
    std::vector<std::vector<AABBTreeLines::LinesDistancer<Line>>> split_masks;
    AABBTreeLines::LinesDistancer<Line>                           foreign_boundary;
    explicit LocalZMixBridgeGeometry(const std::vector<LocalZMixBridge::Zone>& zones)
    {
        for (const auto& zone : zones) {
            // Preserve exact volume ownership. Only the distance field ignores
            // pure-color rounding ribbons, so cleaning cannot create new holes
            // for nominal fallback paths inside an otherwise joined island.
            auto masks = union_owned_regions(zone.masks);
            if (zone.preserve_nominal_layers)
                masks = LocalZMixBridge::clean_residual(masks);
            empty_boundaries.push_back(masks.empty());
            boundaries.emplace_back(to_lines(masks));
        }
    }
};

ExPolygons LocalZMixBridge::clean_residual(const ExPolygons& masks)
{
    // Boolean operations on independently rounded circular slices can leave
    // micron-wide ribbons. They cannot carry a bead, but would otherwise act
    // as full-strength pure-color targets or foreign nozzle obstacles.
    return intersection_ex(opening_ex(masks, scale_(.005)), masks);
}

ExPolygons LocalZMixBridge::coalescing_core(const std::vector<ExPolygons>& footprints,
                                            const std::vector<ExPolygons>& actual,
                                            double                         minimum_width)
{
    if (footprints.size() < 2 || footprints.size() != actual.size() || minimum_width <= 0.)
        return {};
    ExPolygons swept, foreign;
    for (size_t i = 0; i < footprints.size(); ++i) {
        append(swept, footprints[i]);
        append(foreign, diff_ex(actual[i], footprints[i]));
    }
    auto       core = diff_ex(union_owned_regions(swept), union_owned_regions(foreign));
    ExPolygons remainder;
    for (const auto& footprint : footprints)
        append(remainder, diff_ex(footprint, core));
    // Reserve a complete wall beside a changing paint edge. Taking the exact
    // intersection alone can strand a sub-width ribbon with no perimeter.
    // Leave enough margin for offset simplification as well: an almost
    // zero-width erosion can disappear before the opening expands it back.
    core = diff_ex(core, offset_ex(union_owned_regions(remainder), scale_(minimum_width + .01)));
    core.erase(std::remove_if(core.begin(), core.end(),
                              [&](const auto& part) { return offset_ex(ExPolygons{part}, -scale_(minimum_width * .5)).empty(); }),
               core.end());
    if (core.empty())
        return {};
    // A wide body can have an unprintable tail even though its erosion is
    // nonempty. Do not reserve such a tail for an independent thick contour.
    if (!clean_residual(diff_ex(core, opening_ex(core, scale_(minimum_width * .5)))).empty())
        return {};
    for (const auto& footprint : footprints)
        for (const auto& part : diff_ex(footprint, core))
            if (!clean_residual(diff_ex(ExPolygons{part}, opening_ex(ExPolygons{part}, scale_(minimum_width * .5)))).empty())
                return {};
    return core;
}

ExPolygons LocalZMixBridge::fallback_core(const ExPolygons& candidate,
                                          const ExPolygons& support,
                                          const ExPolygons& shell,
                                          double            minimum_width)
{
    auto core = diff_ex(intersection_ex(candidate, support), shell);
    // Trimming an already opened island can leave unprintable slivers. Those
    // must return to the nominal band instead of reserving empty volume.
    core = intersection_ex(core, opening_ex(core, float(scale_(minimum_width * .5))));
    core.erase(std::remove_if(core.begin(), core.end(),
                              [](const ExPolygon& polygon) { return polygon.area() < scale_(.001) * double(scale_(.001)); }),
               core.end());
    return core;
}

std::vector<std::pair<unsigned, double>> LocalZMixBridge::allocate_flat_layers(const std::vector<double>& weights,
                                                                               double                     height,
                                                                               double                     minimum,
                                                                               const std::vector<double>& maximum,
                                                                               std::vector<double>&       error,
                                                                               double                     footprint_area)
{
    const double sum = std::accumulate(weights.begin(), weights.end(), 0.);
    if (!std::isfinite(sum) || sum <= 0. || weights.size() != maximum.size() || !std::isfinite(height) || !std::isfinite(minimum) ||
        minimum <= 0. || height < minimum - 1e-6 || !std::isfinite(footprint_area) || footprint_area <= 0. ||
        std::any_of(
            weights.begin(), weights.end(), [](double value) { return value < 0.; }) ||
        std::any_of(maximum.begin(), maximum.end(), [](double value) { return !std::isfinite(value) || value <= 0.; }))
        throw std::runtime_error("Invalid Local-Z fallback allocation");
    error.resize(weights.size(), 0.);
    double cap = 0.;
    for (size_t i = 0; i < weights.size(); ++i) {
        if (weights[i] > 0. && maximum[i] < minimum - 1e-6)
            throw std::runtime_error("A Local-Z fallback component cannot meet its minimum height");
        error[i] = weights[i] > 0. ? error[i] + footprint_area * height * weights[i] / sum : 0.;
        if (weights[i] > 0.)
            cap = std::max(cap, maximum[i]);
    }
    if (minimum <= 0. || cap < minimum)
        throw std::runtime_error("Unprintable Local-Z fallback allocation");
    std::vector<std::pair<unsigned, double>> result;
    double                                   remaining = height;
    while (remaining > 1e-6) {
        size_t selected        = weights.size();
        double selected_height = 0., best_error = -std::numeric_limits<double>::infinity();
        for (size_t id = 0; id < weights.size(); ++id) {
            if (weights[id] <= 0. || error[id] <= best_error)
                continue;
            double bead = 0., distance = std::numeric_limits<double>::infinity();
            for (size_t rest = 0; rest <= size_t(std::floor((remaining + 1e-6) / minimum)); ++rest) {
                const double lo = std::max(minimum, remaining - rest * cap);
                const double hi = std::min(maximum[id], remaining - rest * minimum);
                if (hi < lo - 1e-6)
                    continue;
                const double desired   = error[id] / footprint_area;
                const double candidate = std::clamp(desired, lo, std::max(lo, hi));
                if (std::abs(candidate - desired) < distance) {
                    bead     = candidate;
                    distance = std::abs(candidate - desired);
                }
            }
            if (bead > 0.) {
                selected        = id;
                selected_height = bead;
                best_error      = error[id];
            }
        }
        if (selected == weights.size())
            throw std::runtime_error("No bounded Local-Z fallback stack fits the block");
        result.emplace_back(unsigned(selected + 1), selected_height);
        error[selected] -= footprint_area * selected_height;
        remaining -= selected_height;
    }
    return result;
}

bool LocalZMixBridge::align_flat_layers(std::vector<std::pair<unsigned, double>>& beads,
                                         const std::vector<double>&                footprint_areas,
                                         const std::vector<unsigned>&              order,
                                         const std::vector<double>&                maximum,
                                         std::vector<double>&                      error)
{
    if (beads.size() != footprint_areas.size() || error.size() != maximum.size())
        throw std::runtime_error("Invalid Local-Z nominal cadence dimensions");
    std::vector<size_t> ranks(maximum.size(), size_t(-1));
    unsigned previous = 0;
    for (size_t i = 0; i < order.size(); ++i) {
        const unsigned tool = order[i];
        if (tool == 0 || tool > ranks.size())
            return false;
        if (tool != previous && ranks[tool - 1] != size_t(-1))
            return false; // An interleaved exact stack needs more than one run.
        ranks[tool - 1] = i;
        previous = tool;
    }
    std::vector<unsigned> sorted;
    for (const auto& bead : beads) {
        if (bead.first == 0 || bead.first > ranks.size() || ranks[bead.first - 1] == size_t(-1))
            return false;
        sorted.push_back(bead.first);
    }
    std::stable_sort(sorted.begin(), sorted.end(), [&](unsigned a, unsigned b) { return ranks[a - 1] < ranks[b - 1]; });
    for (size_t i = 0; i < beads.size(); ++i)
        if (!std::isfinite(beads[i].second) || beads[i].second <= 0. || !std::isfinite(footprint_areas[i]) || footprint_areas[i] < 0. ||
            beads[i].second > maximum[sorted[i] - 1] + 1e-6)
            return false;
    for (size_t i = 0; i < beads.size(); ++i) {
        const double volume = footprint_areas[i] * beads[i].second;
        // The allocator has already charged the original tool. Moving a color
        // to a different footprint must update the same carried volume debt.
        error[beads[i].first - 1] += volume;
        error[sorted[i] - 1] -= volume;
        beads[i].first = sorted[i];
    }
    return true;
}

std::vector<std::pair<unsigned, double>> LocalZMixBridge::repeat_flat_layers(const std::vector<std::pair<unsigned, double>>& beads,
                                                                             double                                          minimum,
                                                                             const std::vector<double>&                      maximum,
                                                                             size_t maximum_repeats)
{
    if (!std::isfinite(minimum) || minimum <= 0.)
        throw std::runtime_error("Invalid Local-Z recipe minimum");
    size_t repeats = std::numeric_limits<size_t>::max();
    for (const auto& [tool, height] : beads) {
        if (tool == 0 || tool > maximum.size() || !std::isfinite(height) || height < minimum - 1e-6 || !std::isfinite(maximum[tool - 1]) ||
            height > maximum[tool - 1] + 1e-6)
            throw std::runtime_error("Invalid Local-Z repeated recipe");
        repeats = std::min(repeats, size_t(std::floor((height + 1e-6) / minimum)));
    }
    if (maximum_repeats > 0)
        repeats = std::min(repeats, maximum_repeats);
    if (beads.size() < 2 || repeats <= 1 || beads.front().first == beads.back().first)
        return beads;
    std::vector<std::pair<unsigned, double>> result;
    result.reserve(beads.size() * repeats);
    for (size_t repeat = 0; repeat < repeats; ++repeat)
        for (const auto& [tool, height] : beads)
            result.emplace_back(tool, height / repeats);
    return result;
}

bool LocalZMixBridge::align_flat_layers_to_cadence(std::vector<std::pair<unsigned, double>>&       beads,
                                                   const std::vector<double>&                      areas,
                                                   const std::vector<double>&                      starts,
                                                   const std::vector<std::pair<unsigned, double>>& cadence,
                                                   const std::vector<double>&                      maximum,
                                                   std::vector<double>&                            error)
{
    const size_t n = beads.size();
    if (areas.size() != n || starts.size() != n || error.size() != maximum.size())
        throw std::runtime_error("Invalid Local-Z repeated cadence dimensions");
    std::vector<double> ends;
    double              total = 0.;
    for (const auto& [tool, height] : cadence) {
        if (tool == 0 || tool > maximum.size() || !std::isfinite(height) || height <= 0.)
            return false;
        ends.push_back(total += height);
    }
    const double                     infinity = std::numeric_limits<double>::infinity();
    std::vector<std::vector<double>> cost(n, std::vector<double>(n, infinity));
    for (size_t i = 0; i < n; ++i) {
        const auto [tool, height] = beads[i];
        if (tool == 0 || tool > maximum.size() || !std::isfinite(height) || height <= 0. || !std::isfinite(areas[i]) || areas[i] < 0. ||
            !std::isfinite(starts[i]) || starts[i] < -1e-6 || starts[i] + height > total + 1e-6 ||
            (i && starts[i] < starts[i - 1] + beads[i - 1].second - 1e-6))
            return false;
    }
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n; ++j) {
            const unsigned tool = beads[j].first;
            if (!std::isfinite(maximum[tool - 1]) || beads[i].second > maximum[tool - 1] + 1e-6)
                continue;
            double overlap = 0., bottom = 0.;
            for (size_t k = 0; k < cadence.size(); ++k) {
                if (cadence[k].first == tool)
                    overlap += std::max(0., std::min(starts[i] + beads[i].second, ends[k]) - std::max(starts[i], bottom));
                bottom = ends[k];
            }
            cost[i][j] = -overlap * areas[i];
        }
    // Minimum-cost assignment preserves the allocator's tool counts, while
    // maximizing physical overlap with all repeats rather than sorting by tool.
    std::vector<double> u(n + 1), v(n + 1);
    std::vector<size_t> p(n + 1), way(n + 1);
    for (size_t i = 1; i <= n; ++i) {
        p[0]                   = i;
        size_t              j0 = 0;
        std::vector<double> distance(n + 1, infinity);
        std::vector<bool>   used(n + 1, false);
        do {
            used[j0]           = true;
            const size_t i0    = p[j0];
            double       delta = infinity;
            size_t       j1    = 0;
            for (size_t j = 1; j <= n; ++j) {
                if (used[j])
                    continue;
                const double candidate = cost[i0 - 1][j - 1] - u[i0] - v[j];
                if (candidate < distance[j]) {
                    distance[j] = candidate;
                    way[j]      = j0;
                }
                if (distance[j] < delta) {
                    delta = distance[j];
                    j1    = j;
                }
            }
            if (!std::isfinite(delta))
                return false;
            for (size_t j = 0; j <= n; ++j)
                if (used[j]) {
                    u[p[j]] += delta;
                    v[j] -= delta;
                } else
                    distance[j] -= delta;
            j0 = j1;
        } while (p[j0] != 0);
        do {
            const size_t previous = way[j0];
            p[j0]                 = p[previous];
            j0                    = previous;
        } while (j0 != 0);
    }
    auto aligned = beads;
    for (size_t j = 1; j <= n; ++j)
        aligned[p[j] - 1].first = beads[j - 1].first;
    for (size_t i = 0; i < n; ++i) {
        const double volume = areas[i] * beads[i].second;
        error[beads[i].first - 1] += volume;
        error[aligned[i].first - 1] -= volume;
    }
    beads = std::move(aligned);
    return true;
}

std::vector<LocalZMixBridge> LocalZMixBridge::shorter_cycles(const std::vector<double>&   nominal_heights,
                                                             const std::vector<double>&   maximum,
                                                             const std::vector<unsigned>& terminal_preference,
                                                             double                       transition_length_factor,
                                                             const ExPolygons&            neighbors,
                                                             double                       minimum_contour_width,
                                                             ExPolygons*                  rejected_contours) const
{
    if (rejected_contours)
        rejected_contours->clear();
    // Pure joins and borrowed colors have a different interface topology.
    // Ordinary foreign boundaries can instead be pinned anew for each repeat.
    const auto rejected = [&](const char* stage) -> std::vector<LocalZMixBridge> {
        BOOST_LOG_TRIVIAL(debug) << "Local-Z fine cycle rejected lo=" << z_lo << " hi=" << z_hi
                                 << " first_source=" << (source_filament_ids.empty() ? 0 : source_filament_ids.front())
                                 << " sources=" << source_filament_ids.size() << " stage=" << stage;
        return {};
    };
    if (planar_fallback || !extra_colors.empty() || !tongue_planes.empty() || !split_runs.empty() || !boundary_interfaces.empty() ||
        zones.empty() || authored_zones.size() != zones.size() || (boundary_anchor > 0. && neighbors.empty()) ||
        std::any_of(zones.begin(), zones.end(), [](const auto& zone) { return zone.preserve_nominal_layers; }))
        return rejected("topology");
    if (nominal_heights.size() < 2 ||
        std::abs(std::accumulate(nominal_heights.begin(), nominal_heights.end(), 0.) - (z_hi - z_lo)) > 1e-6 ||
        std::any_of(nominal_heights.begin(), nominal_heights.end(), [](double h) { return !std::isfinite(h) || h <= 0.; }))
        return rejected("nominal_span");
    size_t repeats = std::numeric_limits<size_t>::max();
    for (const auto& zone : authored_zones)
        for (double height : zone.heights)
            repeats = std::min(repeats, size_t(std::floor((height + 1e-6) / minimum_height)));
    if (repeats < 2)
        return rejected("minimum_height");
    const double                 period = (z_hi - z_lo) / repeats;
    std::vector<LocalZMixBridge> result;
    for (size_t repeat = 0; repeat < repeats; ++repeat) {
        auto input = authored_zones;
        for (auto& zone : input)
            for (double& height : zone.heights)
                height /= repeats;
        auto rebuilt = build(z_lo + repeat * period, repeat + 1 == repeats ? z_hi : z_lo + (repeat + 1) * period, minimum_height, maximum,
                             std::move(input), {}, transition_length_factor, nozzle_clearance, terminal_preference, allow_sparse_stack);
        if (rebuilt.size() != 1 || rebuilt.front().source_filament_ids != source_filament_ids)
            return rejected("child_group");
        auto& bridge              = rebuilt.front();
        if (bridge.shared_stack != shared_stack || bridge.terminal_anchor != terminal_anchor || !bridge.extra_colors.empty() ||
            !bridge.tongue_planes.empty() || !bridge.split_runs.empty() || bridge.zones.size() != zones.size())
            return rejected("child_topology");
        for (size_t i = 0; i < zones.size(); ++i)
            if (bridge.zones[i].source_filament_id != zones[i].source_filament_id || bridge.zones[i].component_ids != zones[i].component_ids)
                return rejected("child_order");
        bridge.skin_envelope      = skin_range();
        bridge.follows_surface    = follows_surface;
        bridge.boundary_tolerance = boundary_tolerance;
        // Keep the original sampling context even when a repeat boundary cuts
        // a nominal slab. Ownership is registered only over the child's Z span.
        bridge.layer_footprints    = layer_footprints;
        bridge.lower_support_masks = lower_support_masks;
        if (!result.empty()) {
            bridge.lower_support_masks.clear();
            // Residual passes of a common-anchor stack each cover one zone.
            // Support for the next repeat is the whole printed footprint,
            // including the zones printed before the last scheduled pass.
            for (const auto& pass : result.back().passes)
                append(bridge.lower_support_masks, result.back().masks(pass));
            bridge.lower_support_masks = union_ex(bridge.lower_support_masks);
        }
        if (!neighbors.empty() && !bridge.pin_foreign_boundary(neighbors, nozzle_clearance) &&
            bridge.advances_near(neighbors, nozzle_clearance))
            return rejected("child_clearance");
        std::stable_sort(bridge.passes.begin(), bridge.passes.end(),
                         [&](const auto& a, const auto& b) { return bridge.schedule_z(a) < bridge.schedule_z(b); });
        if (follows_surface && !bridge.fit_contours(boundary_tolerance, rejected_contours, minimum_contour_width))
            return rejected("child_contour");
        result.push_back(std::move(bridge));
    }
    return result;
}

bool LocalZMixBridge::shares_cadence(double bottom, double top, size_t repeats, const std::vector<unsigned>& order) const
{
    if (repeats == 0 || !std::isfinite(bottom) || !std::isfinite(top) || top <= bottom || !shared_stack || zones.empty() ||
        !extra_colors.empty() || !tongue_planes.empty() || !split_runs.empty())
        return false;
    const double period = (top - bottom) / repeats;
    return std::abs(z_hi - z_lo - period) < 1e-6 && std::abs(z_lo - bottom - std::round((z_lo - bottom) / period) * period) < 1e-6 &&
           std::all_of(
               zones.begin(), zones.end(), [&](const auto& zone) { return !zone.preserve_nominal_layers && zone.component_ids == order; });
}

static void clip_owned_perimeters(const ExtrusionEntity&     entity,
                                  const ExPolygons&          masks,
                                  const ExPolygons&          claimed,
                                  ExtrusionEntityCollection& output)
{
    const auto lines = entity.as_polylines();
    if (diff_pl(lines, masks).empty() && intersection_pl(lines, claimed).empty()) {
        output.append(entity);
    } else if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        ExtrusionEntityCollection clipped;
        clipped.no_sort = collection->no_sort;
        for (const auto* child : collection->entities)
            clip_owned_perimeters(*child, masks, claimed, clipped);
        if (!clipped.empty())
            output.append(std::move(clipped));
    } else if (const auto* path = dynamic_cast<const ExtrusionPath*>(&entity)) {
        // A centerline exactly on a paint boundary belongs to the first
        // region only. Area-disjoint polygons still share their edges.
        for (auto line : diff_pl(intersection_pl(Polylines{path->polyline}, masks), claimed))
            output.append(ExtrusionPath(std::move(line), *path));
    } else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(&entity)) {
        for (const auto& path : loop->paths)
            clip_owned_perimeters(path, masks, claimed, output);
    } else if (const auto* multi = dynamic_cast<const ExtrusionMultiPath*>(&entity)) {
        for (const auto& path : multi->paths)
            clip_owned_perimeters(path, masks, claimed, output);
    } else {
        throw std::runtime_error("Unsupported Local-Z nominal perimeter entity");
    }
}

std::shared_ptr<const LocalZMixBridgePaths> LocalZMixBridgePaths::build(Layer&            layer,
                                                                        const ExPolygons& masks,
                                                                        unsigned          filament_id,
                                                                        size_t            owner_region_id)
{
    auto result = std::make_shared<LocalZMixBridgePaths>();
    // A tapered pass or integer polygon intersection may leave only a
    // sub-micron-area remnant. It cannot carry an extrusion or own a region.
    if (masks.empty() || area(masks) < scale_(.001) * double(scale_(.001)))
        return result;
    const LayerRegion* reference = nullptr;
    for (size_t i = 0; i < layer.regions().size(); ++i)
        if (!intersection_ex(masks, to_expolygons(layer.regions()[i]->slices.surfaces)).empty()) {
            reference         = layer.regions()[i];
            result->region_id = i;
            break;
        }
    // The planner validates against the original geometry before painting
    // splits and filters the nominal regions. A small fitted island may have
    // no intersection with any surviving paint fragment at its scheduling Z.
    // Retain that validated configuration owner instead of guessing a nearby
    // material or dropping the island. Region indices survive segmentation.
    if (reference == nullptr && owner_region_id < layer.regions().size()) {
        reference         = layer.regions()[owner_region_id];
        result->region_id = owner_region_id;
    }
    // Integer offsets can themselves retain minute degenerate polygons, so
    // emptiness after erosion alone is not a reliable test for clipping debris.
    if (reference == nullptr && (area(masks) < scale_(.01) * double(scale_(.01)) || offset_ex(masks, -scale_(.005)).empty()))
        return result;
    if (reference == nullptr)
        throw std::runtime_error("Local-Z island has no owning print region at Z=" + std::to_string(layer.print_z) +
                                 " area=" + std::to_string(area(masks) * SCALING_FACTOR * SCALING_FACTOR));
    auto config                         = reference->region().config();
    config.wall_filament.value          = int(filament_id);
    config.sparse_infill_filament.value = int(filament_id);
    config.solid_infill_filament.value  = int(filament_id);
    // Preserve stable region indices needed by the perimeter generator's
    // neighbour lookups while using the physical tool's actual flow settings.
    PrintRegion physical(config, 0, reference->region().print_object_region_id());
    struct IslandRegion : LayerRegion
    {
        IslandRegion(Layer* owner, const PrintRegion* region) : LayerRegion(owner, region) {}
    };
    IslandRegion island(&layer, &physical);
    island.slices.set(masks, stInternal);
    island.raw_slices = masks;
    // Use the requested wall count; every physical pass supplies its own boundary.
    island.make_perimeters(island.slices, {&island}, &island.fill_surfaces, &island.fill_no_overlap_expolygons);
    result->perimeters = std::move(island.perimeters);
    if (!island.thin_fills.empty())
        result->perimeters.entities.emplace_back(new ExtrusionEntityCollection(std::move(island.thin_fills)));
    // Integer offset artifacts can create a short closed loop inside a hole.
    // Such a loop has no material owner and must not reach another stack.
    const auto escaped = diff_pl(result->perimeters.as_polylines(), offset_ex(masks, scale_(.005)));
    if (std::any_of(escaped.begin(), escaped.end(), [](const Polyline& line) { return line.length() > scale_(.01); })) {
        ExtrusionEntityCollection clipped;
        for (const auto* entity : result->perimeters.entities)
            clip_owned_perimeters(*entity, masks, {}, clipped);
        result->perimeters = std::move(clipped);
    }
    result->fill_masks      = intersection_ex(to_expolygons(island.fill_surfaces.surfaces), masks);
    result->fill_no_overlap = intersection_ex(island.fill_no_overlap_expolygons, masks);
    return result;
}

std::vector<std::shared_ptr<const LocalZMixBridgePaths>> LocalZMixBridgePaths::build_partitioned(Layer&                         layer,
                                                                                                 const std::vector<ExPolygons>& masks,
                                                                                                 const std::vector<unsigned>& filament_ids)
{
    assert(masks.size() == filament_ids.size() && !masks.empty());
    ExPolygons domain;
    for (const auto& mask : masks)
        append(domain, mask);
    // Reassigning colors changes boolean-operation order. Nanometer-wide
    // cracks can then split an otherwise identical common wall into islands.
    // Heal only ten integer grid units here; all paths and fills are still
    // clipped to their original owners below.
    const auto common = build(layer, closing_ex(union_owned_regions(domain), coord_t(10)), filament_ids.front());
    std::vector<std::shared_ptr<const LocalZMixBridgePaths>> results;
    ExPolygons                                               claimed;
    for (size_t i = 0; i < masks.size(); ++i) {
        auto result       = std::make_shared<LocalZMixBridgePaths>();
        result->region_id = common->region_id;
        for (size_t region = 0; region < layer.regions().size(); ++region)
            if (!intersection_ex(masks[i], to_expolygons(layer.regions()[region]->slices.surfaces)).empty()) {
                result->region_id = region;
                break;
            }
        for (const auto* entity : common->perimeters.entities)
            clip_owned_perimeters(*entity, masks[i], claimed, result->perimeters);
        result->fill_masks      = intersection_ex(common->fill_masks, masks[i]);
        result->fill_no_overlap = intersection_ex(common->fill_no_overlap, masks[i]);
        results.push_back(std::move(result));
        append(claimed, masks[i]);
        claimed = union_owned_regions(claimed);
    }
    return results;
}

LocalZMixBridgeFillSurfaces LocalZMixBridgeFillSurfaces::build(const std::vector<Layer*>& layers, const LocalZMixBridge& bridge)
{
    LocalZMixBridgeFillSurfaces result;
    const auto [skin_lo, skin_hi] = bridge.skin_range();
    auto begin                    = std::upper_bound(layers.begin(), layers.end(), skin_lo + 1e-6,
                                                     [](double z, const Layer* layer) { return z < layer->print_z; });
    for (auto it = begin; it != layers.end(); ++it) {
        const Layer* source = *it;
        if (source->bottom_z() >= skin_hi - 1e-6)
            break;
        if (source->print_z <= skin_lo + 1e-6)
            continue;
        ExPolygons top, bottom;
        for (const LayerRegion* region : source->regions())
            for (const Surface& surface : region->fill_surfaces.surfaces) {
                if (surface.is_solid())
                    result.solid.push_back(surface.expolygon);
                if (surface.is_top())
                    top.push_back(surface.expolygon);
                else if (surface.surface_type == stBottom)
                    bottom.push_back(surface.expolygon);
            }
        if (!top.empty())
            result.tops.emplace_back(source->print_z, union_ex(top));
        if (!bottom.empty())
            result.bottoms.emplace_back(source->bottom_z(), union_ex(bottom));
    }
    result.solid = union_ex(result.solid);
    return result;
}

std::shared_ptr<const LocalZMixBridgePaths> LocalZMixBridgePaths::with_fills(const LocalZMixBridgePaths&        paths,
                                                                             Layer&                             layer,
                                                                             const LocalZMixBridge&             bridge,
                                                                             size_t                             pass_index,
                                                                             const LocalZMixBridgeFillSurfaces& surfaces,
                                                                             FillAdaptive::Octree*              adaptive,
                                                                             FillAdaptive::Octree*              support,
                                                                             FillLightning::Generator*          lightning)
{
    auto result = std::make_shared<LocalZMixBridgePaths>(paths);
    result->fills.clear();
    result->fill_region_ids.clear();
    const auto& pass = bridge.passes.at(pass_index);
    const auto [skin_lo, skin_hi] = bridge.skin_range();
    const bool last               = std::abs(bridge.z_hi - skin_hi) < 1e-6 && pass.extra_color < 0 && pass.tongue < 0 &&
                      (pass.flat_height > 0.  ? std::abs(pass.ceiling - bridge.z_hi) < 1e-6 :
                       bridge.terminal_anchor ? pass.component == 0 :
                                                pass.component + 1 == bridge.zones[pass.zones.front()].heights.size());
    const bool first = std::abs(bridge.z_lo - skin_lo) < 1e-6 && pass.extra_color < 0 && pass.tongue < 0 &&
                       (pass.flat_height > 0. ? std::abs(pass.ceiling - pass.flat_height - bridge.z_lo) < 1e-6 :
                                                pass.component == (bridge.terminal_anchor ? 1 : 0));
    struct FillRegion
    {
        size_t            region_id;
        PrintRegionConfig config;
        ExPolygons        masks;
    };
    std::vector<FillRegion> regions;
    ExPolygons              claimed;
    for (size_t region_id = 0; region_id < layer.regions().size(); ++region_id) {
        const LayerRegion* original = layer.regions()[region_id];
        auto               masks    = diff_ex(intersection_ex(paths.fill_masks, to_expolygons(original->slices.surfaces)), claimed);
        if (masks.empty())
            continue;
        append(claimed, masks);
        auto           config      = original->region().config();
        const unsigned filament_id = pass.filament_id;
        config.wall_filament.value = config.sparse_infill_filament.value = config.solid_infill_filament.value = int(filament_id);
        auto group = std::find_if(regions.begin(), regions.end(), [&](const auto& region) { return region.config == config; });
        if (group == regions.end())
            regions.push_back({region_id, std::move(config), std::move(masks)});
        else
            append(group->masks, std::move(masks));
    }
    // Fitted contours use their own mid-height. Painting can filter a nominal
    // fragment altogether, and moving contours can extend beyond it. Keep the
    // complete fitted interior with its validated owner configuration.
    const auto unclaimed = diff_ex(paths.fill_masks, union_owned_regions(claimed));
    if (!unclaimed.empty() && paths.region_id < layer.regions().size()) {
        auto config                = layer.regions()[paths.region_id]->region().config();
        config.wall_filament.value = config.sparse_infill_filament.value = config.solid_infill_filament.value = pass.filament_id;
        auto group = std::find_if(regions.begin(), regions.end(), [&](const auto& region) { return region.config == config; });
        if (group == regions.end())
            regions.push_back({paths.region_id, std::move(config), unclaimed});
        else
            append(group->masks, unclaimed);
    }
    // Identical settings need no artificial seam at a paint boundary. Retain
    // separate domains only where a modifier actually changes the settings.
    for (auto& source : regions) {
        const size_t region_id = source.region_id;
        const auto&  config    = source.config;
        const auto   masks     = union_ex(source.masks);
        PrintRegion  physical(config, 0, 0);
        struct IslandLayer : Layer
        {
            explicit IslandLayer(Layer& source) : Layer(source.id(), source.object(), source.height, source.print_z, source.slice_z) {}
        };
        IslandLayer island(layer);
        auto*       region                 = island.add_region(&physical);
        region->fill_no_overlap_expolygons = intersection_ex(paths.fill_no_overlap, masks);
        // A cycle may span a floor or roof. Retain solid coverage from every
        // covered nominal layer instead of borrowing only the scheduling layer's
        // sparse pattern. This conservatively thickens skins within the cycle.
        ExPolygons top, bottom;
        if (last)
            for (const auto& [z, polygons] : surfaces.tops)
                if (z >= bridge.z_hi - 1e-6)
                    append(top, polygons);
        if (first)
            for (const auto& [z, polygons] : surfaces.bottoms)
                if (z <= bridge.z_lo + 1e-6)
                    append(bottom, polygons);
        top        = intersection_ex(union_ex(top), masks);
        bottom     = diff_ex(intersection_ex(union_ex(bottom), masks), top);
        auto skins = union_ex(top, bottom);
        auto solid = diff_ex(intersection_ex(surfaces.solid, masks), skins);
        region->fill_surfaces.append(top, stTop);
        region->fill_surfaces.append(bottom, stBottom);
        region->fill_surfaces.append(solid, stInternalSolid);
        append(solid, skins);
        region->fill_surfaces.append(diff_ex(masks, solid), stInternal);
        try {
            island.make_fills(adaptive, support, lightning);
        } catch (const OutOfRange& error) {
            if (std::string(error.what()).find("Voronoi") == std::string::npos)
                throw;
            // Some tiny skin fragments make the narrow-infill Voronoi solver fail.
            // Retry this physical island with ordinary rectilinear paths, retaining
            // surface density and flow. Do not retain any partially generated fill.
            BOOST_LOG_TRIVIAL(warning) << "Local-Z island infill: rectilinear fallback at Z=" << layer.print_z;
            region->fills.clear();
            for (const Surface& surface : region->fill_surfaces.surfaces) {
                const bool solid = surface.is_solid();
                FillParams params;
                params.density = solid ? 1.f : float(config.sparse_infill_density.value * .01);
                if (params.density <= 0.f)
                    continue;
                params.flow           = region->flow(surface.is_top() ? frTopSolidInfill : solid ? frSolidInfill : frInfill);
                params.extrusion_role = surface.is_top() ? erTopSolidInfill : solid ? erSolidInfill : erInternalInfill;
                params.dont_adjust    = !solid;
                params.resolution     = layer.object()->print()->config().resolution.value;
                params.config         = &physical.config();
                std::unique_ptr<Fill> fill(Fill::new_from_type(ipRectilinear));
                fill->set_bounding_box(layer.object()->bounding_box());
                fill->layer_id = layer.id();
                fill->z        = layer.print_z;
                fill->angle    = float(config.infill_direction.value * PI / 180.);
                fill->spacing  = params.flow.spacing();
                auto polylines = fill->fill_surface(&surface, params);
                if (polylines.empty())
                    continue;
                const Flow flow       = solid ? params.flow.with_spacing(float(fill->spacing)) : params.flow;
                auto*      collection = new ExtrusionEntityCollection;
                extrusion_entities_append_paths(collection->entities, std::move(polylines), params.extrusion_role, flow.mm3_per_mm(),
                                                flow.width(), flow.height());
                region->fills.entities.push_back(collection);
            }
        }
        result->fill_region_ids.insert(result->fill_region_ids.end(), region->fills.entities.size(), region_id);
        append(result->fills.entities, std::move(region->fills.entities));
        region->fills.entities.clear();
    }
    return result;
}

bool LocalZMixBridge::enabled_from_environment()
{
    const char* value = std::getenv("ORCA_LOCAL_Z_MIX_BRIDGES");
    if (value == nullptr || *value == '\0') {
#if ORCA_LOCAL_Z_MIX_BRIDGES_DEFAULT
        return true;
#else
        return false;
#endif
    }
    if (std::string(value) == "0")
        return false;
    if (std::string(value) == "1")
        return true;
    throw std::runtime_error("ORCA_LOCAL_Z_MIX_BRIDGES must be 0 or 1");
}

namespace {
bool mask_contains(const ExPolygons& masks, const AABBTreeLines::LinesDistancer<Line>* index, const Point& point)
{
    if (masks.empty())
        return false;
    if (index) {
        const int outside = index->outside(point);
        if (outside != 0)
            return outside < 0;
    }
    return std::any_of(masks.begin(), masks.end(), [&](const ExPolygon& mask) { return mask.contains(point); });
}

double zone_distance(const LocalZMixBridge& bridge, size_t index, const Point& point)
{
    const auto& zone = bridge.zones[index];
    if (bridge.geometry) {
        if (bridge.geometry->empty_boundaries[index])
            return std::numeric_limits<double>::max();
        const auto& boundary = bridge.geometry->boundaries[index];
        const int   outside  = boundary.outside(point);
        if (outside < 0)
            return 0.;
        if (outside > 0)
            return unscale<double>(boundary.distance_from_lines<false>(point));
    }
    if (std::any_of(zone.masks.begin(), zone.masks.end(), [&point](const ExPolygon& mask) { return mask.contains(point); }))
        return 0.;
    return unscale<double>(point.distance_to(projection_onto(zone.masks, point)));
}

size_t carrier_first(const LocalZMixBridge& bridge)
{
    const auto& ids = bridge.zones.front().component_ids;
    return size_t(std::find(ids.begin(), ids.end(), bridge.extra_carrier) - ids.begin());
}

double common_slope(const LocalZMixBridge& bridge)
{
    if (bridge.extra_colors.empty())
        return bridge.maximum_slope;
    const size_t count = bridge.zones.front().heights.size() - carrier_first(bridge);
    // A single terminal carrier has a constant capped top for the extras.
    // Repeated carriers also budget for the differences of their interfaces.
    return bridge.maximum_slope / (count == 1 ? 2. : 2. * count + 1.);
}

double extra_slope(const LocalZMixBridge& bridge, size_t color)
{
    return (bridge.maximum_slope - common_slope(bridge)) * bridge.extra_colors[color].maximum_height / bridge.extra_maximum_height;
}

double interface_shift(const LocalZMixBridge& bridge, size_t component)
{
    const size_t first = carrier_first(bridge), count = bridge.zones.front().heights.size();
    return component >= first && component < count ? double(count - component - 1) / (count - first) : 0.;
}

double target_interface_z(const LocalZMixBridge& bridge, const std::vector<double>& distances, size_t component, bool omit_pure = false)
{
    if (component == size_t(-1))
        return bridge.z_lo;
    const double slope = common_slope(bridge);
    double       upper = std::numeric_limits<double>::max(), lower = std::numeric_limits<double>::lowest();
    double       minimum = upper, maximum = lower;
    double       floor = bridge.z_lo, ceiling = bridge.z_hi;
    for (size_t i = 0; i < bridge.zones.size(); ++i) {
        const auto& zone = bridge.zones[i];
        if (zone.masks.empty() || (zone.preserve_nominal_layers && (omit_pure || !zone.join_terminal_pass)))
            continue;
        const double distance = distances[i];
        const double target   = std::accumulate(zone.heights.begin(), zone.heights.begin() + component + 1, 0.);
        upper                 = std::min(upper, target + slope * distance);
        lower                 = std::max(lower, target - slope * distance);
        minimum               = std::min(minimum, target);
        maximum               = std::max(maximum, target);
        if (zone.preserve_nominal_layers) {
            floor   = std::max(floor, bridge.z_lo + target - slope * distance);
            ceiling = std::min(ceiling, bridge.z_lo + target + slope * distance);
        }
        if (!bridge.shared_stack) {
            floor   = std::max(floor, bridge.z_lo + bridge.anchor_limits[i].first - bridge.maximum_slope * distance);
            ceiling = std::min(ceiling, bridge.z_lo + bridge.anchor_limits[i].second + bridge.maximum_slope * distance);
        }
    }
    if (minimum > maximum)
        throw std::runtime_error("Local-Z mix bridge has no zones");
    double normalized = std::clamp(bridge.z_lo + std::clamp(0.5 * (lower + upper), minimum, maximum), floor, std::max(floor, ceiling));
    if (bridge.boundary_anchor > 0. && distances.size() > bridge.zones.size()) {
        const double reach = slope * distances.back();
        if (!bridge.boundary_interfaces.empty())
            normalized = std::clamp(normalized, bridge.boundary_interfaces[component] - reach,
                                    bridge.boundary_interfaces[component] + reach);
        else
            normalized = std::min(normalized, (bridge.shared_stack ? bridge.z_lo + minimum : bridge.boundary_anchor) + reach);
    }
    return normalized;
}

std::vector<double> zone_distances(const LocalZMixBridge& bridge, const Point& point, bool omit_pure = false)
{
    std::vector<double> distances(bridge.zones.size(), 0.);
    for (size_t i = 0; i < bridge.zones.size(); ++i)
        if (omit_pure && bridge.zones[i].preserve_nominal_layers)
            distances[i] = std::numeric_limits<double>::max();
        else if (!bridge.extra_colors.empty() || !bridge.zones[i].preserve_nominal_layers || bridge.zones[i].join_terminal_pass) {
            distances[i] = zone_distance(bridge, i, point);
            if (bridge.zones[i].preserve_nominal_layers)
                distances[i] = std::max(0., distances[i] - bridge.nozzle_clearance);
        }
    double capacity = bridge.z_hi - bridge.z_lo;
    if (!bridge.extra_colors.empty()) {
        const size_t first = carrier_first(bridge), count = bridge.zones.front().heights.size() - first;
        double       previous = target_interface_z(bridge, distances, first - 1, omit_pure);
        for (size_t i = first; i < first + count; ++i) {
            const double next = target_interface_z(bridge, distances, i, omit_pure);
            capacity          = std::min(capacity, count * (next - previous - bridge.minimum_height));
            previous          = next;
        }
        capacity = std::max(0., capacity);
    }
    double cumulative = 0., previous_capped = 0.;
    for (size_t color = 0; color < bridge.extra_colors.size(); ++color) {
        const auto&  extra = bridge.extra_colors[color];
        const double slope = extra_slope(bridge, color);
        double       upper = std::numeric_limits<double>::max(), lower = std::numeric_limits<double>::lowest();
        double       absent = std::numeric_limits<double>::max(), smallest = upper;
        for (size_t i = 0; i < bridge.zones.size(); ++i) {
            const double target = i < extra.heights.size() ? extra.heights[i] : 0.;
            if (target <= 0.)
                absent = std::min(absent, distances[i]);
            else {
                upper    = std::min(upper, target + slope * distances[i]);
                lower    = std::max(lower, target - slope * distances[i]);
                smallest = std::min(smallest, target);
            }
        }
        cumulative += std::min(std::clamp(.5 * (lower + upper), smallest, extra.maximum_height), slope * absent);
        const double capped = std::min(cumulative, capacity);
        double       h      = capped - previous_capped;
        previous_capped     = capped;
        if (!omit_pure && bridge.extra_masks_ready &&
            !mask_contains(extra.masks, bridge.geometry ? &bridge.geometry->extras[color] : nullptr, point))
            h = 0.;
        distances.push_back(h);
    }
    if (bridge.boundary_anchor > 0. && bridge.geometry) {
        const auto& boundary = bridge.geometry->foreign_boundary;
        distances.push_back(boundary.outside(point) < 0 ? 0. : unscale<double>(boundary.distance_from_lines<false>(point)));
    }
    return distances;
}

double extra_height(const LocalZMixBridge& bridge, const std::vector<double>& distances, size_t count)
{
    return std::accumulate(distances.begin() + bridge.zones.size(), distances.begin() + bridge.zones.size() + count, 0.);
}

double interface_z(const LocalZMixBridge& bridge, const std::vector<double>& distances, size_t component)
{
    return target_interface_z(bridge, distances, component) +
           interface_shift(bridge, component) * extra_height(bridge, distances, bridge.extra_colors.size());
}

double component_bottom(const LocalZMixBridge& bridge, const std::vector<double>& distances, size_t component)
{
    if (!bridge.extra_colors.empty() && component == carrier_first(bridge))
        return target_interface_z(bridge, distances, component - 1) + extra_height(bridge, distances, bridge.extra_colors.size());
    return interface_z(bridge, distances, component - 1);
}

double pure_distance(const LocalZMixBridge& bridge, const std::vector<double>& distances)
{
    double distance = std::numeric_limits<double>::max();
    for (size_t i = 0; i < bridge.zones.size(); ++i)
        if (bridge.zones[i].join_terminal_pass)
            distance = std::min(distance, distances[i]);
    return distance;
}

double tongue_target(const LocalZMixBridge& bridge, const Point& point, size_t index, double distance)
{
    double target = bridge.tongue_planes[index] - bridge.maximum_slope * distance;
    if (!bridge.extra_colors.empty()) {
        // Let nominal carrier beads fill the material lost specifically at a
        // pure boundary. Preserve the mixed-only terminal thickness instead
        // of inflating the final bead while an extra color retreats.
        const auto   mixed     = zone_distances(bridge, point, true);
        const size_t last      = bridge.zones.front().heights.size() - 1;
        const double shift     = last == carrier_first(bridge) ? 1. : interface_shift(bridge, last - 1);
        const double reference = target_interface_z(bridge, mixed, last - 1, true) +
                                 shift * extra_height(bridge, mixed, bridge.extra_colors.size());
        target = std::max(target, std::min(bridge.tongue_planes[index], reference));
    }
    return target;
}

double terminal_interface(const LocalZMixBridge& bridge, const Point& point, size_t count, double anchor, double distance)
{
    // Polygon sampling can put a tip slightly beyond its exact level set.
    // Keep the shared interface on its nominal ceiling, including at that edge.
    for (size_t i = 0; i < count; ++i)
        if (mask_contains(bridge.tongue_masks[i],
                          bridge.geometry && i < bridge.geometry->tongues.size() ? &bridge.geometry->tongues[i] : nullptr, point))
            anchor = std::max(anchor, std::min(bridge.tongue_planes[i],
                                               std::max(anchor + bridge.tongue_tip_height, tongue_target(bridge, point, i, distance))));
    return anchor;
}

// Restrict the common bottom bead so every residual bead can retain its own
// relative ratio without crossing a tool's height bounds. No material fades
// to zero when the two sides have different component counts.
std::pair<double, double> anchor_range(
    const LocalZMixBridge::Zone& zone, unsigned int anchor, double height, double minimum, const std::vector<double>& maximum)
{
    const size_t index    = size_t(std::find(zone.component_ids.begin(), zone.component_ids.end(), anchor) - zone.component_ids.begin());
    const double residual = height - zone.heights[index];
    double       low = minimum, high = maximum[anchor - 1];
    for (size_t i = 0; i < zone.heights.size(); ++i) {
        if (i == index)
            continue;
        const double fraction = zone.heights[i] / residual;
        low                   = std::max(low, height - maximum[zone.component_ids[i] - 1] / fraction);
        high                  = std::min(high, height - minimum / fraction);
    }
    return {low, high};
}
// Shared grid vertices and edge crossings produce closed physical regions.
// Keep the interior cells whole to avoid unioning two triangles per full cell.
template<class Evaluate> ExPolygons sample_region(const ExPolygons& band, Evaluate evaluate)
{
    if (band.empty())
        return {};
    const auto                                   bbox = get_extents(band);
    const coord_t                                step = scale_(.05);
    // A scan at the row midpoint alone misses partial last rows and thin
    // features lying between scanlines. Include every boundary edge's span
    // through the row, then use midpoint spans for the remaining interior.
    const size_t row_count = size_t((bbox.max.y() - bbox.min.y() + step - 1) / step);
    if (row_count == 0)
        return {};
    std::vector<std::vector<std::pair<coord_t, coord_t>>> boundary_ranges(row_count);
    for (const auto& line : to_lines(band)) {
        const size_t first = size_t((std::min(line.a.y(), line.b.y()) - bbox.min.y()) / step);
        const size_t last  = std::min(row_count - 1, size_t((std::max(line.a.y(), line.b.y()) - bbox.min.y()) / step));
        for (size_t row = first; row <= last; ++row) {
            double a = line.a.x(), b = line.b.x();
            if (line.a.y() != line.b.y()) {
                const double y  = bbox.min.y() + row * double(step);
                const double dy = double(line.b.y() - line.a.y());
                const double dx = double(line.b.x() - line.a.x());
                a               = line.a.x() + std::clamp((y - line.a.y()) / dy, 0., 1.) * dx;
                b               = line.a.x() + std::clamp((y + step - line.a.y()) / dy, 0., 1.) * dx;
            }
            boundary_ranges[row].emplace_back(std::max(coord_t(0), coord_t((std::min(a, b) - bbox.min.x()) / step) - 1),
                                              coord_t((std::max(a, b) - bbox.min.x()) / step) + 1);
        }
    }
    // Clip all independent scanlines in one sweep. Rebuilding Clipper's edge
    // table for every row makes finely fragmented paint prohibitively slow.
    Polylines scans;
    scans.reserve(row_count);
    for (size_t row = 0; row < row_count; ++row) {
        const coord_t y = bbox.min.y() + coord_t(row) * step + step / 2;
        scans.emplace_back(Points{Point(bbox.min.x() - step, y), Point(bbox.max.x() + step, y)});
    }
    for (const auto& span : intersection_pl(scans, band)) {
        const auto   span_box = get_extents(span);
        const size_t row      = size_t((span_box.min.y() - bbox.min.y()) / step);
        assert(span_box.min.y() == span_box.max.y() && row < row_count);
        const coord_t begin = std::max(coord_t(0), (span_box.min.x() - bbox.min.x()) / step - 1);
        const coord_t end   = (span_box.max.x() - bbox.min.x()) / step + 1;
        boundary_ranges[row].emplace_back(begin, end);
    }
    Polygons                                     cells;
    std::unordered_map<Point, double, PointHash> vertex_values;
    auto                                         value = [&](const Point& point) {
        const auto found = vertex_values.find(point);
        if (found != vertex_values.end())
            return found->second;
        const double result = evaluate(point);
        vertex_values.emplace(point, result);
        return result;
    };
    for (coord_t y = bbox.min.y(); y < bbox.max.y(); y += step) {
        // Bound temporary storage even for very long or finely painted edges.
        if (vertex_values.size() > 100000)
            vertex_values.clear();
        auto ranges = std::move(boundary_ranges[size_t((y - bbox.min.y()) / step)]);
        std::sort(ranges.begin(), ranges.end());
        coord_t previous_end = -1;
        for (const auto& range : ranges) {
            const coord_t begin = std::max(range.first, previous_end + 1);
            previous_end        = std::max(previous_end, range.second);
            coord_t full_begin  = -1;
            auto    flush_full  = [&](coord_t end) {
                if (full_begin >= 0) {
                    const coord_t x0 = bbox.min.x() + full_begin * step;
                    const coord_t x1 = bbox.min.x() + end * step;
                    cells.emplace_back(Points{Point(x0, y), Point(x1, y), Point(x1, y + step), Point(x0, y + step)});
                    full_begin = -1;
                }
            };
            for (coord_t cell = begin; cell <= range.second; ++cell) {
                const coord_t               x = bbox.min.x() + cell * step;
                const std::array<Point, 4>  points{Point(x, y), Point(x + step, y), Point(x + step, y + step), Point(x, y + step)};
                const std::array<double, 4> values{value(points[0]), value(points[1]), value(points[2]), value(points[3])};
                if (std::all_of(values.begin(), values.end(), [](double v) { return v >= 0.; })) {
                    if (full_begin < 0)
                        full_begin = cell;
                    continue;
                }
                flush_full(cell);
                for (const auto& triangle : {std::array<size_t, 3>{0, 1, 2}, std::array<size_t, 3>{0, 2, 3}}) {
                    Points polygon;
                    for (size_t edge = 0; edge < 3; ++edge) {
                        const size_t a = triangle[edge], b = triangle[(edge + 1) % 3];
                        if (values[a] >= 0.)
                            polygon.push_back(points[a]);
                        if ((values[a] >= 0.) != (values[b] >= 0.)) {
                            const double t = values[a] / (values[a] - values[b]);
                            polygon.emplace_back(coord_t(std::llround(points[a].x() + t * (points[b].x() - points[a].x()))),
                                                 coord_t(std::llround(points[a].y() + t * (points[b].y() - points[a].y()))));
                        }
                    }
                    if (polygon.size() >= 3)
                        cells.emplace_back(std::move(polygon));
                }
            }
            flush_full(range.second + 1);
        }
    }
    return intersection_ex(union_ex(cells), band);
}

ExPolygons tongue_region(const LocalZMixBridge& bridge, size_t index, const ExPolygons& pure, const ExPolygons& mixed)
{
    const auto started    = std::chrono::steady_clock::now();
    double     anchor_min = bridge.z_hi;
    for (const auto& zone : bridge.zones)
        if (!zone.preserve_nominal_layers)
            anchor_min = std::min(anchor_min, bridge.z_hi - zone.heights.back());
    double reach = std::max(0., (bridge.tongue_planes[index] - anchor_min) / bridge.maximum_slope);
    if (!bridge.extra_colors.empty()) {
        // Outside this band, neither pure interface targets nor the absent
        // extra-color envelope can change the mixed-only terminal surface.
        for (size_t component = 0; component < bridge.zones.front().heights.size(); ++component) {
            double low = bridge.z_hi - bridge.z_lo, high = 0.;
            for (const auto& zone : bridge.zones)
                if (!zone.preserve_nominal_layers || zone.join_terminal_pass) {
                    const double target = std::accumulate(zone.heights.begin(), zone.heights.begin() + component + 1, 0.);
                    low                 = std::min(low, target);
                    high                = std::max(high, target);
                }
            reach = std::max(reach, (high - low) / common_slope(bridge));
        }
        for (size_t color = 0; color < bridge.extra_colors.size(); ++color)
            reach = std::max(reach, bridge.extra_colors[color].maximum_height / extra_slope(bridge, color));
    }
    const auto band   = intersection_ex(offset_ex(pure, scale_(reach + bridge.nozzle_clearance + .05)), mixed);
    auto       result = sample_region(band, [&](const Point& point) {
        const auto   distances = zone_distances(bridge, point);
        const double anchor    = component_bottom(bridge, distances, bridge.zones.front().heights.size() - 1);
        const double distance  = pure_distance(bridge, distances);
        const double lower     = terminal_interface(bridge, point, index, anchor, distance);
        const double upper     = std::max(anchor, tongue_target(bridge, point, index, distance));
        return upper - lower - bridge.tongue_tip_height;
    });
    append(result, pure);
    BOOST_LOG_TRIVIAL(debug) << "Local-Z tongue mask seconds="
                             << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return union_ex(result);
}

const LocalZMixBridge::SplitRun* split_run(const LocalZMixBridge& bridge, const LocalZMixBridge::Pass& pass)
{
    if (pass.flat_height > 0. || pass.tongue >= 0 || pass.extra_color >= 0)
        return nullptr;
    for (const auto& run : bridge.split_runs)
        if (pass.component >= run.first && pass.component <= run.last)
            return &run;
    return nullptr;
}

std::pair<double, double> run_bounds(const LocalZMixBridge&           bridge,
                                     const LocalZMixBridge::SplitRun& run,
                                     const std::vector<double>&       distances)
{
    return {component_bottom(bridge, distances, run.first),
            run.last + 1 == bridge.zones.front().heights.size() ? bridge.z_hi : interface_z(bridge, distances, run.last)};
}

void merge_same_color_runs(LocalZMixBridge& bridge, double minimum, const std::vector<double>& maximum)
{
    if (!bridge.shared_stack)
        return;
    const auto& ids = bridge.zones.front().component_ids;
    ExPolygons  mixed;
    for (const auto& zone : bridge.zones)
        if (!zone.preserve_nominal_layers)
            append(mixed, zone.masks);
    mixed = union_ex(mixed);
    for (size_t first = 0; first < ids.size();) {
        size_t last = first;
        while (last + 1 < ids.size() && ids[last + 1] == ids[first])
            ++last;
        const unsigned            id = ids[first];
        LocalZMixBridge::SplitRun run{first, last};
        first = last + 1;
        if (run.first == run.last || std::any_of(bridge.zones.begin(), bridge.zones.end(), [&](const auto& zone) {
                return zone.join_terminal_pass && zone.component_ids.front() == id;
            }))
            continue;
        double cap_min = minimum;
        // Reserve a tip and the grid interpolation error when an omitted tip
        // is returned to the next bead. Internal interfaces remain s-Lipschitz.
        double cap_max     = maximum[id - 1] - bridge.tongue_tip_height - 2 * bridge.maximum_slope * .05 * std::sqrt(2.);
        bool   needs_split = false;
        bool   valid       = true;
        for (size_t zone_index = 0; zone_index < bridge.zones.size(); ++zone_index) {
            const auto& zone = bridge.zones[zone_index];
            if (zone.preserve_nominal_layers && !zone.join_terminal_pass)
                continue;
            const double normalized      = std::accumulate(zone.heights.begin() + run.first, zone.heights.begin() + run.last + 1, 0.);
            double       authored_extras = 0.;
            for (const auto& extra : bridge.extra_colors)
                if (zone_index < extra.heights.size())
                    authored_extras += extra.heights[zone_index];
            const double total = normalized - (id == bridge.extra_carrier ? authored_extras : 0.);
            const size_t count = std::max<size_t>(1, size_t(std::ceil((total - 1e-6) / maximum[id - 1])));
            valid &= count <= run.last - run.first + 1;
            cap_min = std::max(cap_min, total / count);
            // Near an extra-color tip the common volume can grow back to its
            // normalized value. Retain enough capacity in all available slots.
            cap_min = std::max(cap_min, normalized / (run.last - run.first + 1));
            needs_split |= normalized > maximum[id - 1] + 1e-6;
            if (count > 1) {
                needs_split = true;
                if (!zone.preserve_nominal_layers)
                    cap_max = std::min(cap_max, (total - minimum) / (count - 1));
            }
        }
        if (!needs_split)
            cap_max = maximum[id - 1];
        if (!valid || cap_min > cap_max + 1e-8)
            continue;
        run.cap = cap_max;
        for (size_t component = run.first; component < run.last; ++component) {
            if (!needs_split) {
                run.masks.emplace_back();
                continue;
            }
            run.masks.push_back(sample_region(mixed, [&](const Point& point) {
                const auto [bottom, top] = run_bounds(bridge, run, zone_distances(bridge, point));
                return top - bottom - (run.last - component) * run.cap - bridge.tongue_tip_height;
            }));
        }
        bridge.split_runs.emplace_back(std::move(run));
    }
    bridge.passes.erase(std::remove_if(bridge.passes.begin(), bridge.passes.end(),
                                       [&](const auto& pass) {
                                           const auto* run = split_run(bridge, pass);
                                           return run && pass.component < run->last && run->masks[pass.component - run->first].empty();
                                       }),
                        bridge.passes.end());
    for (auto& pass : bridge.passes)
        if (const auto* run = split_run(bridge, pass)) {
            double bottom_max = bridge.z_lo, top_max = bridge.z_lo;
            for (const auto& zone : bridge.zones) {
                if (zone.preserve_nominal_layers && !zone.join_terminal_pass)
                    continue;
                bottom_max = std::max(bottom_max,
                                      bridge.z_lo + std::accumulate(zone.heights.begin(), zone.heights.begin() + run->first, 0.));
                top_max = std::max(top_max, bridge.z_lo + std::accumulate(zone.heights.begin(), zone.heights.begin() + run->last + 1, 0.));
            }
            bottom_max += bridge.extra_maximum_height *
                          (run->first == carrier_first(bridge) ? 1. : interface_shift(bridge, run->first - 1));
            top_max += bridge.extra_maximum_height * interface_shift(bridge, run->last);
            bottom_max   = std::min(bottom_max, bridge.z_hi);
            top_max      = std::min(top_max, bridge.z_hi);
            pass.ceiling = std::max(bottom_max + bridge.tongue_tip_height, top_max - (run->last - pass.component) * run->cap);
        }
}
// Keep authored common heights and borrow extra-color volume from one carrier.
// Prefer a compatible pure neighbor, so its nominal tongues absorb the taper.
bool make_sparse_stack(std::vector<LocalZMixBridge::Zone>&       zones,
                       double                                    minimum,
                       const std::vector<double>&                maximum,
                       std::vector<LocalZMixBridge::ExtraColor>& extras,
                       unsigned&                                 carrier,
                       const std::vector<unsigned>&              preferred_carriers = {})
{
    if (zones.size() < 2)
        return false;
    auto common = zones.front().component_ids;
    std::sort(common.begin(), common.end());
    std::vector<unsigned> all;
    for (const auto& zone : zones) {
        auto ids = zone.component_ids;
        std::sort(ids.begin(), ids.end());
        std::vector<unsigned> intersection;
        std::set_intersection(common.begin(), common.end(), ids.begin(), ids.end(), std::back_inserter(intersection));
        common = std::move(intersection);
        all.insert(all.end(), ids.begin(), ids.end());
    }
    if (common.empty())
        return false;
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    for (unsigned id : all) {
        if (std::find(common.begin(), common.end(), id) != common.end()) {
            // Shared slots must have identical multiplicity in every zone.
            for (const auto& zone : zones)
                if (std::count(zone.component_ids.begin(), zone.component_ids.end(), id) != std::count(common.begin(), common.end(), id))
                    return false;
            continue;
        }
        LocalZMixBridge::ExtraColor extra;
        extra.filament_id = id;
        for (const auto& zone : zones) {
            if (std::count(zone.component_ids.begin(), zone.component_ids.end(), id) > 1)
                return false;
            double h = 0.;
            for (size_t i = 0; i < zone.heights.size(); ++i)
                if (zone.component_ids[i] == id)
                    h += zone.heights[i];
            extra.heights.push_back(h);
            extra.maximum_height = std::max(extra.maximum_height, h);
        }
        if (id == 0 || id > maximum.size() || extra.maximum_height > maximum[id - 1] + 1e-6)
            return false;
        extras.push_back(std::move(extra));
    }
    if (extras.empty())
        return false;
    double                             best_score = -1.;
    std::vector<LocalZMixBridge::Zone> best;
    for (unsigned id : common) {
        const size_t count     = std::count(common.begin(), common.end(), id);
        auto         candidate = zones;
        bool         valid     = true;
        double       score     = std::numeric_limits<double>::max();
        for (size_t z = 0; z < candidate.size(); ++z) {
            auto&  zone         = candidate[z];
            double extra_height = 0.;
            for (const auto& extra : extras)
                extra_height += extra.heights[z];
            std::vector<unsigned> ids;
            std::vector<double>   heights;
            // All carrier slots are terminal, after the inserted extra islands.
            for (bool carrier_slot : {false, true})
                for (size_t i = 0; i < zone.heights.size(); ++i) {
                    const unsigned tool = zone.component_ids[i];
                    if ((tool == id) != carrier_slot || std::find(common.begin(), common.end(), tool) == common.end())
                        continue;
                    const double h = zone.heights[i] + (carrier_slot ? extra_height / count : 0.);
                    valid &= zone.heights[i] >= minimum - 1e-6 && h <= maximum[tool - 1] + 1e-6;
                    if (carrier_slot)
                        score = std::min(score, zone.heights[i]);
                    ids.push_back(tool);
                    heights.push_back(h);
                }
            zone.component_ids = std::move(ids);
            zone.heights       = std::move(heights);
        }
        if (!valid)
            continue;
        const auto preferred = std::find(preferred_carriers.begin(), preferred_carriers.end(), id);
        if (preferred != preferred_carriers.end())
            score += 1000. * (preferred_carriers.end() - preferred);
        if (score > best_score) {
            best_score = score;
            carrier    = id;
            best       = std::move(candidate);
        }
    }
    if (best.empty())
        return false;
    zones = std::move(best);
    return true;
}
} // namespace

bool LocalZMixBridge::sparse_stack_fits(const std::vector<Zone>& zones, double minimum, const std::vector<double>& maximum)
{
    std::vector<Zone> sorted;
    sorted.reserve(zones.size());
    for (const auto& zone : zones)
        sorted.push_back({{}, zone.heights, zone.component_ids});
    for (auto& zone : sorted) {
        std::vector<std::pair<unsigned, double>> entries;
        for (size_t i = 0; i < zone.heights.size(); ++i)
            entries.emplace_back(zone.component_ids[i], zone.heights[i]);
        std::sort(entries.begin(), entries.end());
        for (size_t i = 0; i < entries.size(); ++i) {
            zone.component_ids[i] = entries[i].first;
            zone.heights[i]       = entries[i].second;
        }
    }
    std::vector<ExtraColor> extras;
    unsigned                carrier = 0;
    return make_sparse_stack(sorted, minimum, maximum, extras, carrier);
}

std::vector<LocalZMixBridge> LocalZMixBridge::build(double                       bottom,
                                                    double                       top,
                                                    double                       minimum,
                                                    const std::vector<double>&   maximum,
                                                    std::vector<Zone>            input,
                                                    const std::vector<double>&   nominal_heights,
                                                    double                       transition_length_factor,
                                                    double                       nozzle_clearance,
                                                    const std::vector<unsigned>& terminal_preference,
                                                    bool                         allow_sparse)
{
    return build_impl(bottom, top, minimum, maximum, std::move(input), nominal_heights, transition_length_factor, nozzle_clearance,
                      terminal_preference, true, allow_sparse);
}

std::vector<LocalZMixBridge> LocalZMixBridge::build_impl(double                       bottom,
                                                         double                       top,
                                                         double                       minimum,
                                                         const std::vector<double>&   maximum,
                                                         std::vector<Zone>            input,
                                                         const std::vector<double>&   nominal_heights,
                                                         double                       transition_length_factor,
                                                         double                       nozzle_clearance,
                                                         const std::vector<unsigned>& terminal_preference,
                                                         bool                         split_disconnected,
                                                         bool                         allow_sparse)
{
    if (!std::isfinite(transition_length_factor) || transition_length_factor < .1 || transition_length_factor > 10.)
        throw std::runtime_error("Invalid Local-Z mix transition length");
    if (!std::isfinite(nozzle_clearance) || nozzle_clearance < 0.)
        throw std::runtime_error("Invalid Local-Z nozzle clearance");
    const double      height = top - bottom;
    std::vector<Zone> solids;
    for (auto it = input.begin(); it != input.end();) {
        if (it->component_ids.size() == 1 && it->heights.size() == 1) {
            if (it->masks.empty() || it->component_ids.front() == 0 || it->component_ids.front() > maximum.size() ||
                std::abs(it->heights.front() - height) > 1e-6)
                throw std::runtime_error("Invalid solid Local-Z bridge zone");
            solids.emplace_back(std::move(*it));
            it = input.erase(it);
        } else
            ++it;
    }
    for (Zone& zone : input) {
        if (zone.masks.empty() || zone.heights.size() < 2 || zone.heights.size() != zone.component_ids.size() ||
            std::abs(std::accumulate(zone.heights.begin(), zone.heights.end(), 0.) - height) > 1e-6)
            throw std::runtime_error("Invalid Local-Z bridge recipe");
        // Canonical ordering also makes input permutations deterministic.
        for (size_t i = 0; i < zone.component_ids.size(); ++i)
            for (size_t j = i + 1; j < zone.component_ids.size(); ++j)
                if (zone.component_ids[j] < zone.component_ids[i]) {
                    std::swap(zone.component_ids[i], zone.component_ids[j]);
                    std::swap(zone.heights[i], zone.heights[j]);
                }
        for (size_t i = 0; i < zone.heights.size(); ++i) {
            const auto id = zone.component_ids[i];
            if (id == 0 || id > maximum.size() || zone.heights[i] < minimum - 1e-6 || zone.heights[i] > maximum[id - 1] + 1e-6)
                throw std::runtime_error("Unprintable Local-Z bridge recipe");
        }
    }
    std::sort(input.begin(), input.end(), [](const Zone& a, const Zone& b) { return a.source_filament_id < b.source_filament_id; });
    // Only geometric contact creates an edge. The tiny offset absorbs integer
    // clipping round-off; it is not a nozzle-width bridge across an air gap.
    std::vector<std::vector<bool>> adjacent(input.size(), std::vector<bool>(input.size(), false));
    for (size_t i = 0; i < input.size(); ++i) {
        const ExPolygons expanded = offset_ex(input[i].masks, scale_(0.00001));
        for (size_t j = i + 1; j < input.size(); ++j)
            adjacent[i][j] = adjacent[j][i] = !intersection_ex(expanded, input[j].masks).empty();
    }
    // A solid common-color region can be the available path between two
    // mixtures. Include that connection before choosing groups; attaching
    // solids afterwards alone would let the first group consume the path.
    for (const Zone& solid : solids) {
        const unsigned      id       = solid.component_ids.front();
        const auto          expanded = offset_ex(solid.masks, scale_(.00001));
        std::vector<size_t> touching;
        for (size_t i = 0; i < input.size(); ++i)
            if (std::find(input[i].component_ids.begin(), input[i].component_ids.end(), id) != input[i].component_ids.end() &&
                std::all_of(input[i].heights.begin(), input[i].heights.end(), [&](double h) { return h <= maximum[id - 1] + 1e-6; }) &&
                !intersection_ex(expanded, input[i].masks).empty())
                touching.push_back(i);
        for (size_t i : touching)
            for (size_t j : touching)
                if (i != j && input[i].component_ids == input[j].component_ids)
                    adjacent[i][j] = true;
    }
    std::vector<bool>            used(input.size(), false);
    std::vector<LocalZMixBridge> result;
    while (std::find(used.begin(), used.end(), false) != used.end()) {
        std::vector<std::set<unsigned>>    evaluated(input.size());
        std::unordered_map<unsigned, bool> reusable_anchors;
        std::vector<size_t>                best;
        unsigned int                       best_anchor = 0;
        size_t                             best_score  = 0;
        bool                               best_sparse = false;
        // Consider complete shared stacks as well as one common anchor. A
        // deterministic greedy forest avoids cyclic material-order demands.
        for (size_t seed = 0; seed < input.size(); ++seed) {
            if (used[seed])
                continue;
            for (size_t mode = 0; mode <= input[seed].component_ids.size() + 1; ++mode) {
                const bool         sparse = mode == input[seed].component_ids.size() + 1;
                if (sparse && !allow_sparse)
                    continue;
                const unsigned int anchor = mode == 0 || sparse ? 0 : input[seed].component_ids[mode - 1];
                if (anchor != 0 && std::count(input[seed].component_ids.begin(), input[seed].component_ids.end(), anchor) != 1)
                    continue;
                if (!sparse && evaluated[seed].count(anchor) != 0)
                    continue;
                bool reusable = mode == 0;
                if (anchor != 0) {
                    auto found = reusable_anchors.find(anchor);
                    if (found == reusable_anchors.end()) {
                        double low = 0., high = height;
                        for (size_t i = 0; i < input.size(); ++i)
                            if (!used[i] && std::count(input[i].component_ids.begin(), input[i].component_ids.end(), anchor) == 1) {
                                const auto range = anchor_range(input[i], anchor, height, minimum, maximum);
                                low              = std::max(low, range.first);
                                high             = std::min(high, range.second);
                            }
                        found = reusable_anchors.emplace(anchor, low <= high + 1e-9).first;
                    }
                    reusable = found->second;
                }
                std::vector<size_t> group{seed};
                auto limits  = anchor == 0 ? std::make_pair(0., height) : anchor_range(input[seed], anchor, height, minimum, maximum);
                bool changed = true;
                while (changed) {
                    changed = false;
                    for (size_t candidate = 0; candidate < input.size(); ++candidate) {
                        if (used[candidate] || std::find(group.begin(), group.end(), candidate) != group.end() ||
                            !std::any_of(group.begin(), group.end(), [&](size_t i) { return adjacent[i][candidate]; }))
                            continue;
                        if (sparse) {
                            std::vector<Zone> trial;
                            for (size_t i : group)
                                trial.push_back({{}, input[i].heights, input[i].component_ids});
                            trial.push_back({{}, input[candidate].heights, input[candidate].component_ids});
                            if (!std::all_of(trial.begin(), trial.end(),
                                             [&](const auto& z) { return z.component_ids == trial.front().component_ids; }) &&
                                !sparse_stack_fits(trial, minimum, maximum))
                                continue;
                        } else if (anchor == 0) {
                            if (input[candidate].component_ids != input[seed].component_ids)
                                continue;
                        } else {
                            if (std::count(input[candidate].component_ids.begin(), input[candidate].component_ids.end(), anchor) != 1)
                                continue;
                            const auto range = anchor_range(input[candidate], anchor, height, minimum, maximum);
                            if (std::max(limits.first, range.first) > std::min(limits.second, range.second) + 1e-9)
                                continue;
                            limits = {std::max(limits.first, range.first), std::min(limits.second, range.second)};
                        }
                        group.push_back(candidate);
                        changed = true;
                    }
                }
                auto common = input[seed].component_ids;
                // With no competing height bounds, every seed in this
                // connected component produces the identical candidate.
                if (reusable)
                    for (size_t i : group)
                        evaluated[i].insert(anchor);
                if (sparse)
                    for (size_t i : group) {
                        std::vector<unsigned> intersection;
                        std::set_intersection(common.begin(), common.end(), input[i].component_ids.begin(), input[i].component_ids.end(),
                                              std::back_inserter(intersection));
                        common = std::move(intersection);
                    }
                const size_t score = (group.size() - 1) * (anchor == 0 ? common.size() : 1);
                if (best.empty() || score > best_score || (score == best_score && group.size() > best.size())) {
                    best        = std::move(group);
                    best_anchor = anchor;
                    best_score  = score;
                    best_sparse = sparse;
                }
            }
        }
        LocalZMixBridge bridge;
        bridge.z_lo           = bottom;
        bridge.z_hi           = top;
        bridge.shared_stack   = best_anchor == 0;
        bridge.minimum_height = minimum;
        bridge.allow_sparse_stack = allow_sparse;
        bridge.nozzle_clearance = nozzle_clearance;
        std::vector<Zone> selected;
        for (size_t i : best)
            selected.push_back(input[i]);
        bridge.authored_zones = selected;
        if (!bridge.shared_stack && !terminal_preference.empty())
            bridge.terminal_anchor = std::all_of(selected.begin(), selected.end(), [&](const auto& zone) {
                const auto preferred = std::find_if(terminal_preference.begin(), terminal_preference.end(), [&](unsigned id) {
                    return std::find(zone.component_ids.begin(), zone.component_ids.end(), id) != zone.component_ids.end();
                });
                return preferred != terminal_preference.end() && *preferred == best_anchor;
            });
        if (best_sparse) {
            std::vector<unsigned> preferred_carriers = terminal_preference;
            for (const auto& solid : solids)
                if (terminal_preference.empty() && std::any_of(selected.begin(), selected.end(), [&](const auto& zone) {
                        return !intersection_ex(offset_ex(zone.masks, scale_(.00001)), solid.masks).empty();
                    }))
                    preferred_carriers.push_back(solid.component_ids.front());
            if (!std::all_of(selected.begin(), selected.end(),
                             [&](const auto& zone) { return zone.component_ids == selected.front().component_ids; }) &&
                !make_sparse_stack(selected, minimum, maximum, bridge.extra_colors, bridge.extra_carrier, preferred_carriers))
                throw std::runtime_error("Invalid shared Local-Z sparse stack");
            for (const auto& extra : bridge.extra_colors)
                bridge.extra_maximum_height += extra.maximum_height;
            if (!bridge.extra_colors.empty())
                bridge.maximum_slope = .24; // Compact experimental ramps; equal-set behavior stays unchanged.
        }
        // Use one object-wide order even when a pure neighbor disappears or a
        // retry removes it. Otherwise adjacent cycles end and start with the
        // same material, producing two thin beads instead of a regular stack.
        unsigned terminal_color = bridge.extra_carrier;
        if (bridge.shared_stack && bridge.extra_colors.empty())
            for (unsigned id : terminal_preference) {
                const auto& ids   = selected.front().component_ids;
                const auto  first = std::find(ids.begin(), ids.end(), id);
                if (first == ids.end())
                    continue;
                const size_t start  = first - ids.begin();
                const size_t finish = std::find_if(first, ids.end(), [&](unsigned value) { return value != id; }) - ids.begin();
                for (auto& zone : selected) {
                    std::rotate(zone.component_ids.begin() + start, zone.component_ids.begin() + finish, zone.component_ids.end());
                    std::rotate(zone.heights.begin() + start, zone.heights.begin() + finish, zone.heights.end());
                }
                terminal_color = id;
                break;
            }
        bridge.maximum_slope /= transition_length_factor;
        for (size_t index = 0; index < best.size(); ++index) {
            const size_t i = best[index];
            used[i]        = true;
            Zone zone      = std::move(selected[index]);
            if (!bridge.shared_stack) {
                bridge.anchor_limits.push_back(anchor_range(zone, best_anchor, height, minimum, maximum));
                const auto   pos   = std::find(zone.component_ids.begin(), zone.component_ids.end(), best_anchor);
                const size_t index = size_t(pos - zone.component_ids.begin());
                std::rotate(zone.component_ids.begin(), pos, pos + 1);
                std::rotate(zone.heights.begin(), zone.heights.begin() + index, zone.heights.begin() + index + 1);
            }
            bridge.source_filament_ids.push_back(zone.source_filament_id);
            bridge.zones.emplace_back(std::move(zone));
        }
        // Keep pure neighbors on their original nominal planes. Put their
        // common color last, so the cycle's final bead can cross the boundary;
        // earlier pure beads remain separate, full nominal-height passes.
        // A shared interface may ramp only on the mixed side of that boundary.
        unsigned            solid_color = 0;
        std::vector<double> solid_targets;
        if (bridge.shared_stack && nominal_heights.size() >= bridge.zones.front().heights.size() &&
            std::abs(std::accumulate(nominal_heights.begin(), nominal_heights.end(), 0.) - height) < 1e-6) {
            std::vector<double> planes;
            std::partial_sum(nominal_heights.begin(), nominal_heights.end(), std::back_inserter(planes));
            for (const Zone& solid : solids) {
                const unsigned id = solid.component_ids.front();
                if (!terminal_preference.empty() && id != terminal_color)
                    continue;
                if (!bridge.extra_colors.empty() && id != bridge.extra_carrier)
                    continue;
                if (!std::all_of(nominal_heights.begin(), nominal_heights.end(),
                                 [&](double h) { return h >= minimum - 1e-6 && h <= maximum[id - 1] + 1e-6; }) ||
                    !std::any_of(bridge.zones.begin(), bridge.zones.end(), [&](const Zone& zone) {
                        return std::find(zone.component_ids.begin(), zone.component_ids.end(), id) != zone.component_ids.end() &&
                               !intersection_ex(offset_ex(zone.masks, scale_(.00001)), solid.masks).empty();
                    }))
                    continue;
                auto candidates = bridge.zones;
                for (Zone& zone : candidates) {
                    const auto index = std::find(zone.component_ids.begin(), zone.component_ids.end(), id) - zone.component_ids.begin();
                    std::rotate(zone.component_ids.begin() + index, zone.component_ids.begin() + index + 1, zone.component_ids.end());
                    std::rotate(zone.heights.begin() + index, zone.heights.begin() + index + 1, zone.heights.end());
                }
                // Choose printable nominal interface targets at the pure boundary.
                // A ramp may cross nominal planes; that is precisely why this
                // wall is non-planar. Only the pure region stays on those planes.
                // Pick low nearby interfaces rather than lifting every foreign
                // color to the penultimate nominal plane. Later pure beads can
                // extend into the remaining common-color volume as tongues.
                const size_t        components = candidates.front().heights.size();
                std::vector<double> levels{0.};
                levels.insert(levels.end(), planes.begin(), planes.end());
                const size_t                     count    = levels.size();
                const double                     infinity = std::numeric_limits<double>::infinity();
                std::vector<std::vector<double>> cost(components + 1, std::vector<double>(count, infinity));
                std::vector<std::vector<size_t>> parent(components + 1, std::vector<size_t>(count, count));
                cost[0][0] = 0.;
                for (size_t component = 0; component < components; ++component) {
                    double preferred = height;
                    for (const Zone& zone : candidates)
                        preferred = std::min(preferred, std::accumulate(zone.heights.begin(), zone.heights.begin() + component + 1, 0.));
                    const double cap = std::min(maximum[id - 1], maximum[candidates.front().component_ids[component] - 1]);
                    for (size_t next = 1; next < count; ++next) {
                        if (component + 1 == components && next + 1 != count)
                            continue;
                        for (size_t previous = 0; previous < next; ++previous) {
                            const double h = levels[next] - levels[previous];
                            if (h < minimum - 1e-6 || h > cap + 1e-6)
                                continue;
                            const double score = cost[component][previous] + std::pow(levels[next] - preferred, 2);
                            if (score < cost[component + 1][next]) {
                                cost[component + 1][next]   = score;
                                parent[component + 1][next] = previous;
                            }
                        }
                    }
                }
                if (!std::isfinite(cost[components].back()))
                    continue;
                std::vector<double> targets(components);
                size_t              next = count - 1;
                for (size_t component = components; component-- > 0;) {
                    const size_t previous = parent[component + 1][next];
                    targets[component]    = levels[next] - levels[previous];
                    next                  = previous;
                }
                solid_color   = id;
                solid_targets = std::move(targets);
                bridge.zones  = std::move(candidates);
                break;
            }
        }
        if (bridge.shared_stack && nominal_heights.size() > 1 &&
            std::abs(std::accumulate(nominal_heights.begin(), nominal_heights.end(), 0.) - height) < 1e-6) {
            bool changed = true;
            while (changed) {
                changed = false;
                for (auto it = solids.begin(); it != solids.end();) {
                    const unsigned id       = it->component_ids.front();
                    auto           neighbor = std::find_if(bridge.zones.begin(), bridge.zones.end(), [&](const Zone& zone) {
                        return std::find(zone.component_ids.begin(), zone.component_ids.end(), id) != zone.component_ids.end() &&
                               !intersection_ex(offset_ex(zone.masks, scale_(.00001)), it->masks).empty();
                    });
                    const bool     fits     = std::all_of(nominal_heights.begin(), nominal_heights.end(),
                                                          [&](double h) { return h >= minimum - 1e-6 && h <= maximum[id - 1] + 1e-6; });
                    if (neighbor == bridge.zones.end() || !fits) {
                        ++it;
                        continue;
                    }
                    it->preserve_nominal_layers = true;
                    it->join_terminal_pass      = id == solid_color;
                    // Even an unjoinable pure neighbor keeps its nominal
                    // passes inside the cycle footprint. This preserves full
                    // coverage of frozen paint without inheriting mix heights.
                    if (it->join_terminal_pass) {
                        it->heights = solid_targets;
                        it->component_ids.assign(it->heights.size(), id);
                    }
                    bridge.source_filament_ids.push_back(it->source_filament_id);
                    bridge.zones.emplace_back(std::move(*it));
                    it      = solids.erase(it);
                    changed = true;
                }
            }
        }
        // A recipe may paint several disconnected islands. A contact on one
        // island must not join its remote fragments into the same height field
        // or make a local contour failure reject every occurrence of the recipe.
        const auto components = closing_ex(bridge.ownership(size_t(-1)), coord_t(10));
        if (split_disconnected && components.size() > 1) {
            auto authored = bridge.authored_zones;
            for (const auto& zone : bridge.zones)
                if (zone.preserve_nominal_layers)
                    authored.push_back({zone.masks, {height}, {zone.component_ids.front()}, zone.source_filament_id});
            for (const auto& component : components) {
                if (nozzle_clearance > 0. && clean_residual(ExPolygons{component}).empty())
                    continue; // Unprintable clipping debris remains with nominal fallback.
                std::vector<Zone> local;
                for (auto zone : authored) {
                    zone.masks = intersection_ex(zone.masks, ExPolygons{component});
                    if (!zone.masks.empty())
                        local.push_back(std::move(zone));
                }
                // Clipping a touching contour can produce integer-sized
                // fragments again. Partition once; never recurse on rounding.
                auto islands = build_impl(bottom, top, minimum, maximum, std::move(local), nominal_heights, transition_length_factor,
                                          nozzle_clearance, terminal_preference, false, allow_sparse);
                for (auto& island : islands)
                    result.push_back(std::move(island));
            }
            continue;
        }
        auto geometry        = std::make_shared<LocalZMixBridgeGeometry>(bridge.zones);
        bridge.geometry      = geometry;
        double extra_ceiling = bottom;
        if (!bridge.extra_colors.empty())
            for (const auto& zone : bridge.zones)
                if (!zone.preserve_nominal_layers || zone.join_terminal_pass)
                    extra_ceiling = std::max(extra_ceiling, bottom + std::accumulate(zone.heights.begin(),
                                                                                     zone.heights.begin() + carrier_first(bridge), 0.));
        for (size_t color = 0; color < bridge.extra_colors.size(); ++color) {
            auto&      extra = bridge.extra_colors[color];
            ExPolygons domain, absent;
            Pass       pass;
            pass.filament_id = extra.filament_id;
            pass.extra_color = int(color);
            for (size_t i = 0; i < bridge.zones.size(); ++i)
                if (i < extra.heights.size() && extra.heights[i] > 0.) {
                    append(domain, bridge.zones[i].masks);
                    pass.zones.push_back(i);
                } else
                    append(absent, bridge.zones[i].masks);
            domain = union_ex(domain);
            // The local carrier capacity can also trim an island between two
            // extra-bearing recipes, away from an absent-color boundary.
            extra.masks = sample_region(domain, [&](const Point& point) {
                return zone_distances(bridge, point)[bridge.zones.size() + color] - bridge.tongue_tip_height;
            });
            if (extra.masks.empty())
                geometry->extras.emplace_back();
            else
                geometry->extras.emplace_back(to_lines(extra.masks));
            extra_ceiling += extra.maximum_height;
            pass.ceiling = std::min(extra_ceiling, top - minimum * (bridge.zones.front().heights.size() - carrier_first(bridge)));
            if (!extra.masks.empty())
                bridge.passes.push_back(std::move(pass));
        }
        bridge.extra_masks_ready = true;
        if (solid_color != 0) {
            const double anchor = top - solid_targets.back();
            double       plane  = bottom;
            for (size_t i = 0; i + 1 < nominal_heights.size(); ++i) {
                plane += nominal_heights[i];
                if (plane > anchor + 1e-6)
                    bridge.tongue_planes.push_back(plane);
            }
            ExPolygons pure, mixed;
            for (const auto& zone : bridge.zones)
                if (zone.join_terminal_pass)
                    append(pure, zone.masks);
                else if (!zone.preserve_nominal_layers)
                    append(mixed, zone.masks);
            pure  = union_ex(pure);
            mixed = union_ex(mixed);
            for (size_t i = 0; i < bridge.tongue_planes.size(); ++i) {
                bridge.tongue_masks.push_back(tongue_region(bridge, i, pure, mixed));
                geometry->tongues.emplace_back(to_lines(bridge.tongue_masks.back()));
            }
        }
        const size_t shared_count = bridge.shared_stack ? bridge.zones.front().heights.size() : 1;
        for (size_t component = 0; component < shared_count; ++component) {
            double ceiling = 0.;
            for (const Zone& zone : bridge.zones)
                if (!zone.preserve_nominal_layers || zone.join_terminal_pass)
                    ceiling = std::max(ceiling, bottom + std::accumulate(zone.heights.begin(), zone.heights.begin() + component + 1, 0.));
            ceiling = std::min(top, ceiling + bridge.extra_maximum_height * interface_shift(bridge, component));
            for (size_t i = 0; i < bridge.zones.size(); ++i) {
                if (bridge.zones[i].preserve_nominal_layers && (!bridge.zones[i].join_terminal_pass || component + 1 != shared_count))
                    continue;
                const unsigned id   = bridge.zones[i].component_ids[component];
                auto           pass = std::find_if(bridge.passes.begin(), bridge.passes.end(), [&](const Pass& p) {
                    return p.extra_color < 0 && p.component == component && p.filament_id == id;
                });
                if (pass == bridge.passes.end())
                    bridge.passes.push_back({{i}, component, id, ceiling});
                else
                    pass->zones.push_back(i);
            }
        }
        if (bridge.shared_stack) {
            double ceiling = bottom;
            for (size_t layer = 0; layer < nominal_heights.size(); ++layer) {
                ceiling += nominal_heights[layer];
                for (size_t i = 0; i < bridge.zones.size(); ++i) {
                    const auto& zone = bridge.zones[i];
                    if (!zone.preserve_nominal_layers || (zone.join_terminal_pass && layer + 1 == nominal_heights.size()))
                        continue;
                    const unsigned id   = zone.component_ids.front();
                    auto           pass = std::find_if(bridge.passes.begin(), bridge.passes.end(), [&](const Pass& p) {
                        return p.flat_height > 0. && p.ceiling == ceiling && p.filament_id == id;
                    });
                    if (pass == bridge.passes.end())
                        bridge.passes.push_back({{i}, 0, id, ceiling, nominal_heights[layer]});
                    else
                        pass->zones.push_back(i);
                }
            }
        }
        for (auto& pass : bridge.passes) {
            if (pass.flat_height <= 0. || pass.filament_id != solid_color)
                continue;
            for (size_t i = 0; i < bridge.tongue_planes.size(); ++i)
                if (std::abs(pass.ceiling - bridge.tongue_planes[i]) < 1e-6) {
                    pass.tongue    = int(i);
                    pass.component = shared_count - 1;
                    pass.masks     = bridge.tongue_masks[i];
                    for (size_t zone = 0; zone < bridge.zones.size(); ++zone)
                        if (!bridge.zones[zone].preserve_nominal_layers)
                            pass.zones.push_back(zone);
                }
        }
        if (!bridge.shared_stack) {
            const double anchor_ceiling = bridge.passes.front().ceiling;
            for (size_t i = 0; i < bridge.zones.size(); ++i) {
                const Zone& zone       = bridge.zones[i];
                double      cumulative = 0.;
                for (size_t component = 1; component < zone.heights.size(); ++component) {
                    cumulative += zone.heights[component];
                    const double fraction = cumulative / (height - zone.heights.front());
                    bridge.passes.push_back(
                        {{i}, component, zone.component_ids[component], anchor_ceiling + fraction * (top - anchor_ceiling)});
                }
            }
        }
        merge_same_color_runs(bridge, minimum, maximum);
        for (const auto& run : bridge.split_runs) {
            geometry->split_masks.emplace_back();
            for (const auto& mask : run.masks)
                if (mask.empty())
                    geometry->split_masks.back().emplace_back();
                else
                    geometry->split_masks.back().emplace_back(to_lines(mask));
        }
        // Schedule by the lowest local top, before adjacent planar interiors
        // advance above it. Keep the maximum top separately for travel.
        if (bridge.terminal_anchor) {
            for (auto& pass : bridge.passes) {
                if (pass.component == 0) {
                    pass.ceiling = top;
                    continue;
                }
                const auto&  zone  = bridge.zones[pass.zones.front()];
                const double after = std::accumulate(zone.heights.begin() + 1, zone.heights.begin() + pass.component + 1, 0.) /
                                     (height - zone.heights.front());
                // Local feasibility clamps may move the anchor beyond its
                // authored extrema. The minimum printable height is a hard
                // lower bound, so this ceiling contains every valid field.
                pass.ceiling = bottom + after * (height - minimum);
            }
        }
        std::stable_sort(bridge.passes.begin(), bridge.passes.end(),
                         [&](const Pass& a, const Pass& b) { return bridge.schedule_z(a) < bridge.schedule_z(b); });
        std::vector<unsigned> sources;
        for (unsigned source : bridge.source_filament_ids)
            if (std::find(sources.begin(), sources.end(), source) == sources.end())
                sources.push_back(source);
        bridge.source_filament_ids = std::move(sources);
        result.emplace_back(std::move(bridge));
    }
    return result;
}

ExPolygons LocalZMixBridge::ownership(size_t layer_id) const
{
    for (const auto& footprint : layer_footprints)
        if (footprint.layer_id == layer_id)
            return footprint.masks;
    ExPolygons result;
    for (const auto& zone : zones)
        append(result, zone.masks);
    return union_owned_regions(result);
}

bool LocalZMixBridge::advances_near(const ExPolygons& neighbors, double clearance) const
{
    if (neighbors.empty())
        return false;
    const auto guard = offset_ex(neighbors, scale_(clearance));
    for (size_t i = 0; i < passes.size(); ++i) {
        const double scheduled = schedule_z(passes[i]);
        if (passes[i].ceiling <= scheduled + 1e-6)
            continue;
        auto domain = masks(passes[i]);
        for (const auto& footprint : layer_footprints)
            append(domain, masks(passes[i], footprint.layer_id));
        const auto band = intersection_ex(union_ex(domain), guard);
        // Most unsafe bands are decided at a contour vertex. Avoid rasterizing
        // the entire painted wall just to find the first advancing point.
        for (const auto& line : to_lines(band))
            if (surfaces(line.a, i).second > scheduled + 1e-6 || surfaces(line.midpoint(), i).second > scheduled + 1e-6)
                return true;
        if (!band.empty() && !sample_region(band, [&](const Point& point) { return surfaces(point, i).second - scheduled - 1e-6; }).empty())
            return true;
    }
    return false;
}

bool LocalZMixBridge::pin_foreign_boundary(const ExPolygons& neighbors, double clearance)
{
    if (neighbors.empty() || !geometry || !extra_colors.empty() || !tongue_planes.empty() || !split_runs.empty())
        return false;
    double target = z_hi - z_lo;
    for (const auto& zone : zones)
        if (!zone.preserve_nominal_layers || zone.join_terminal_pass)
            target = std::min(target, zone.heights.front());
    std::vector<double> interfaces;
    if (shared_stack) {
        // A joined pure region fixes the boundary on nominal planes. Requiring
        // those planes to equal the smallest mixed-recipe target rejects even
        // ordinary two-color hulls. Pin to the pure planes and schedule there;
        // the mixed interior can retain its lower or higher authored interface.
        for (const auto& zone : zones) {
            if (!zone.join_terminal_pass)
                continue;
            std::vector<double> targets;
            std::partial_sum(zone.heights.begin(), zone.heights.end(), std::back_inserter(targets));
            for (double& z : targets)
                z += z_lo;
            if (!interfaces.empty() && !std::equal(interfaces.begin(), interfaces.end(), targets.begin(),
                                                   [](double a, double b) { return std::abs(a - b) < 1e-6; }))
                return false;
            interfaces = std::move(targets);
        }
    }
    // One constant boundary interface must satisfy every residual recipe.
    // Otherwise retain the clearance rejection, never distort its plateau.
    for (const auto& limits : anchor_limits)
        if (target < limits.first - 1e-6 || target > limits.second + 1e-6)
            return false;
    auto updated              = std::make_shared<LocalZMixBridgeGeometry>(*geometry);
    updated->foreign_boundary = AABBTreeLines::LinesDistancer<Line>(to_lines(offset_ex(neighbors, scale_(clearance))));
    geometry                  = std::move(updated);
    boundary_anchor           = z_lo + target;
    boundary_interfaces       = std::move(interfaces);
    for (auto& pass : passes)
        if (!boundary_interfaces.empty() && pass.flat_height == 0.)
            pass.ceiling = std::max(pass.ceiling, boundary_interfaces[pass.component]);
    return true;
}

ExPolygons LocalZMixBridge::masks(const Pass& pass, size_t layer_id) const
{
    auto result = masks(pass);
    if (layer_footprints.empty() || follows_surface)
        return result;
    const auto target = ownership(layer_id);
    // Optional bead fields are clipped to their sampled domain. Extending
    // their path masks alone would create zero-height extrusion in the new
    // outer ring; the complementary carrier already fills that volume.
    const auto* run = split_run(*this, pass);
    if (pass.extra_color >= 0 || pass.tongue >= 0 || (run && pass.component < run->last))
        return intersection_ex(result, target);
    const auto reference = ownership(size_t(-1));
    const auto added     = diff_ex(target, reference);
    auto       extended  = intersection_ex(added, offset_ex(result, scale_(boundary_tolerance)));
    for (const auto& footprint : layer_footprints)
        if (footprint.layer_id == layer_id && !footprint.zones.empty()) {
            ExPolygons allowed;
            for (size_t index : pass.zones)
                append(allowed, footprint.zones[index]);
            extended = intersection_ex(extended, union_ex(allowed));
            break;
        }
    append(extended, intersection_ex(result, target));
    return union_ex(extended);
}

ExPolygons LocalZMixBridge::masks(const Pass& pass) const
{
    if (pass.contour_fitted)
        return pass.contour_masks;
    if (pass.extra_color >= 0)
        return extra_colors[pass.extra_color].masks;
    if (const auto* run = split_run(*this, pass); run && pass.component < run->last)
        return run->masks[pass.component - run->first];
    if (!pass.masks.empty())
        return pass.masks;
    ExPolygons result;
    for (size_t zone : pass.zones)
        append(result, zones[zone].masks);
    return union_owned_regions(result);
}

bool LocalZMixBridge::fit_contours(double maximum_error, ExPolygons* rejected, double minimum_width)
{
    if (rejected)
        rejected->clear();
    ExPolygons invalid;
    unsigned   invalid_reasons = 0;
    if (!follows_surface || layer_footprints.size() < 2)
        return !follows_surface;
    for (auto& pass : passes) {
        pass.contour_fitted = false;
        pass.contour_masks.clear();
    }
    std::vector<AABBTreeLines::LinesDistancer<Line>> outlines;
    ExPolygons                                       common = closing_ex(layer_footprints.front().masks, scale_(.005)), swept;
    for (const auto& footprint : layer_footprints) {
        if (footprint.masks.empty())
            return false;
        const auto outline = closing_ex(footprint.masks, scale_(.005));
        outlines.emplace_back(to_lines(outline));
        common = intersection_ex(common, outline);
        append(swept, outline);
    }
    swept = union_ex(swept);
    // Sampling only the moving boundary keeps the work proportional to the
    // shell, not the filled area. Include a grid cell at stationary junctions.
    const auto band            = intersection_ex(offset_ex(diff_ex(swept, common), scale_(.06)), swept);
    const auto stable          = diff_ex(common, band);
    auto       signed_distance = [&](const Point& point, double z) {
        auto         upper    = std::upper_bound(layer_footprints.begin(), layer_footprints.end(), z,
                                                       [](double height, const auto& footprint) { return height < footprint.slice_z; });
        const size_t b        = std::clamp(size_t(upper - layer_footprints.begin()), size_t(1), layer_footprints.size() - 1);
        const size_t a        = b - 1;
        auto         distance = [&](size_t index) {
            const double d = unscale<double>(outlines[index].distance_from_lines<false>(point));
            return outlines[index].outside(point) > 0 ? -d : d;
        };
        const double t = std::clamp((z - layer_footprints[a].slice_z) / (layer_footprints[b].slice_z - layer_footprints[a].slice_z), 0., 1.);
        return (1. - t) * distance(a) + t * distance(b);
    };
    // Compute all masks before publishing any of them: optional masks and
    // shared fields must continue to use their original domains during fitting.
    std::vector<ExPolygons> fitted;
    for (size_t i = 0; i < passes.size(); ++i) {
        const auto domain = masks(passes[i]);
        auto       moving = sample_region(intersection_ex(band, domain), [&](const Point& point) {
            const auto [lo, hi] = surfaces(point, i);
            return signed_distance(point, .5 * (lo + hi));
        });
        append(moving, intersection_ex(stable, domain));
        auto contour = union_ex(moving);
        // A fitted mask reserves the entire cycle's nominal ownership. If a
        // required component vanishes or cannot carry a perimeter, nominal
        // fallback must retain that footprint instead of leaving an empty pass.
        if (contour.empty()) {
            append(invalid, clean_residual(domain));
            invalid_reasons |= 1;
        }
        if (minimum_width > 0.)
            for (const auto& part : contour)
                if (offset_ex(ExPolygons{part}, -scale_(minimum_width * .5)).empty()) {
                    append(invalid, clean_residual(ExPolygons{part}));
                    invalid_reasons |= 2;
                }
        for (const auto& line : to_lines(contour))
            for (const Point& point : {line.a, line.midpoint()}) {
                const auto [lo, hi]   = surfaces(point, i);
                const double midpoint = .5 * (lo + hi);
                const auto nominal = std::min_element(layer_footprints.begin(), layer_footprints.end(), [&](const auto& a, const auto& b) {
                    return std::abs(a.slice_z - midpoint) < std::abs(b.slice_z - midpoint);
                });
                // Ordinary-height portions retain ordinary slicing's overhang
                // allowance. Limit only the extra stair step of a thicker bead.
                if (hi - lo <= nominal->height + 1e-6)
                    continue;
                // Paint boundaries inside the model are not exterior contours.
                if (std::abs(signed_distance(point, .5 * (lo + hi))) > .01)
                    continue;
                if (std::max(std::abs(signed_distance(point, lo)), std::abs(signed_distance(point, hi))) > maximum_error + .005) {
                    append(invalid, union_ex(offset(Polylines{Polyline{line.a, line.b}}, scale_(.05))));
                    invalid_reasons |= 4;
                    break;
                }
            }
        fitted.push_back(std::move(contour));
    }
    ExPolygons support = lower_support_masks;
    ExPolygons mixed;
    for (const auto& zone : zones)
        if (!zone.preserve_nominal_layers)
            append(mixed, zone.masks);
    mixed = union_ex(mixed);
    for (size_t i = 0; i < passes.size(); ++i) {
        // Midpoint placement permits half a nozzle of lateral step between
        // adjacent beads (at least half-width support). Check the real slice
        // below the cycle too, so a ledge cannot hide at a cycle boundary.
        const auto unsupported = clean_residual(diff_ex(fitted[i], offset_ex(support, scale_(2. * maximum_error))));
        const size_t previous_invalid = invalid.size();
        append(invalid, clean_residual(intersection_ex(unsupported, mixed)));
        if (!unsupported.empty())
            append(invalid, sample_region(unsupported, [&](const Point& point) {
                       const auto [lo, hi]   = surfaces(point, i);
                       const double midpoint = .5 * (lo + hi);
                       const auto   nominal  = std::min_element(layer_footprints.begin(), layer_footprints.end(),
                                                                [&](const auto& a, const auto& b) {
                                                                 return std::abs(a.slice_z - midpoint) < std::abs(b.slice_z - midpoint);
                                                             });
                       return hi - lo - nominal->height - 1e-6;
                   }));
        if (invalid.size() != previous_invalid)
            invalid_reasons |= 8;
        append(support, fitted[i]);
        support = union_ex(support);
        // Contour-only checks miss a component that vanishes completely. Every
        // nominal slab must still be covered where this pass owns positive
        // height, except for the explicitly bounded exterior staircase.
        for (const auto& footprint : layer_footprints) {
            auto disagreement = diff_ex(intersection_ex(masks(passes[i]), footprint.masks), offset_ex(fitted[i], scale_(maximum_error)));
            append(disagreement, diff_ex(fitted[i], offset_ex(footprint.masks, scale_(maximum_error))));
            disagreement = clean_residual(union_ex(disagreement));
            if (disagreement.empty())
                continue;
            const size_t previous_invalid = invalid.size();
            for (double z : {footprint.slice_z - .49 * footprint.height, footprint.slice_z, footprint.slice_z + .49 * footprint.height})
                append(invalid, sample_region(disagreement, [&](const Point& point) {
                           const auto [lo, hi] = surfaces(point, i);
                           return std::min(z - lo, hi - z) - 1e-6;
                       }));
            if (invalid.size() != previous_invalid)
                invalid_reasons |= 16;
        }
    }
    if (!invalid.empty()) {
        BOOST_LOG_TRIVIAL(debug) << "Local-Z contour rejected lo=" << z_lo << " hi=" << z_hi
                                 << " first_source=" << (source_filament_ids.empty() ? 0 : source_filament_ids.front())
                                 << " reasons=" << invalid_reasons;
        if (rejected)
            *rejected = union_ex(invalid);
        return false;
    }
    for (size_t i = 0; i < passes.size(); ++i) {
        passes[i].contour_masks  = std::move(fitted[i]);
        passes[i].contour_fitted = true;
    }
    return true;
}

double LocalZMixBridge::pure_distance(const Point& point) const { return Slic3r::pure_distance(*this, zone_distances(*this, point)); }

double LocalZMixBridge::terminal_interface(const Point& point, size_t tongue_count) const
{
    const auto distances = zone_distances(*this, point);
    return Slic3r::terminal_interface(*this, point, tongue_count, component_bottom(*this, distances, zones.front().heights.size() - 1),
                                      Slic3r::pure_distance(*this, distances));
}

double LocalZMixBridge::schedule_z(const Pass& pass) const
{
    if (terminal_anchor) {
        if (pass.component == 0)
            return z_hi;
        const auto&  zone   = zones[pass.zones.front()];
        const double height = z_hi - z_lo;
        const double after  = std::accumulate(zone.heights.begin() + 1, zone.heights.begin() + pass.component + 1, 0.) /
                             (height - zone.heights.front());
        double maximum_anchor = 0.;
        for (const auto& candidate : zones)
            maximum_anchor = std::max(maximum_anchor, candidate.heights.front());
        double key = z_lo + after * (height - maximum_anchor);
        if (boundary_anchor > 0.)
            key = std::max(key, z_lo + after * (z_hi - boundary_anchor));
        return key;
    }
    if (pass.extra_color >= 0) {
        // Strict order also survives export modes which group equal-Z tools.
        // Every key stays below the smallest printable extra-color tip.
        double base = z_hi;
        for (const auto& zone : zones)
            if (!zone.preserve_nominal_layers || zone.join_terminal_pass)
                base = std::min(base, z_lo + std::accumulate(zone.heights.begin(), zone.heights.begin() + carrier_first(*this), 0.));
        return base + tongue_tip_height * (size_t(pass.extra_color) + 1) / (extra_colors.size() + 1);
    }
    if (const auto* run = split_run(*this, pass); run && pass.component < run->last) {
        double bottom_min = z_hi, top_min = z_hi;
        for (const auto& zone : zones) {
            if (zone.preserve_nominal_layers && !zone.join_terminal_pass)
                continue;
            bottom_min = std::min(bottom_min, z_lo + std::accumulate(zone.heights.begin(), zone.heights.begin() + run->first, 0.));
            top_min    = std::min(top_min, z_lo + std::accumulate(zone.heights.begin(), zone.heights.begin() + run->last + 1, 0.));
        }
        return std::max(bottom_min + tongue_tip_height, top_min - (run->last - pass.component) * run->cap);
    }
    if (pass.tongue >= 0) {
        double floor = pass.ceiling;
        for (const Zone& zone : zones)
            if (!zone.preserve_nominal_layers || zone.join_terminal_pass)
                floor = std::min(floor, z_hi - zone.heights.back() + tongue_tip_height);
        // The pure part must be supported by its preceding nominal bead.
        // Later tongues can reach farther into the mixed region, where their
        // local top is lower; dependency order still follows the pure stack.
        return std::max(floor, pass.ceiling - pass.flat_height + 1e-6);
    }
    if (pass.flat_height > 0.)
        return pass.ceiling;
    double floor = z_hi;
    for (const Zone& zone : zones) {
        if (zone.preserve_nominal_layers && !zone.join_terminal_pass)
            continue;
        const size_t component = shared_stack ? pass.component : 0;
        floor                  = std::min(floor, z_lo + std::accumulate(zone.heights.begin(), zone.heights.begin() + component + 1, 0.));
    }
    if (!shared_stack && pass.component > 0) {
        const auto&  zone     = zones[pass.zones.front()];
        const double fraction = std::accumulate(zone.heights.begin() + 1, zone.heights.begin() + pass.component + 1, 0.) /
                                (z_hi - z_lo - zone.heights.front());
        floor += fraction * (z_hi - floor);
    }
    // Only the clearance band is pinned to this scheduling plane. Interior
    // plateaus may lie below it; they are isolated from foreign paths and
    // still execute in monotonically ordered component passes.
    if (!boundary_interfaces.empty())
        floor = std::max(floor, boundary_interfaces[pass.component]);
    return floor;
}

double LocalZMixBridge::interface_z(const Point& point, size_t component) const
{
    // Evaluate shared distances once. The monotone, translation-equivariant
    // interface operator preserves component bounds and the slope limit.
    const double z = Slic3r::interface_z(*this, zone_distances(*this, point), component);
    return terminal_anchor ? z_lo + z_hi - z : z;
}

std::pair<double, double> LocalZMixBridge::surfaces(const Point& point, size_t pass_index) const
{
    const Pass& pass = passes.at(pass_index);
    if (pass.flat_height > 0. && pass.tongue < 0)
        return {pass.ceiling - pass.flat_height, pass.ceiling};
    const auto distances = zone_distances(*this, point);
    if (pass.extra_color >= 0) {
        const double base = target_interface_z(*this, distances, carrier_first(*this) - 1);
        return {base + extra_height(*this, distances, size_t(pass.extra_color)),
                base + extra_height(*this, distances, size_t(pass.extra_color) + 1)};
    }
    if (const auto* run = split_run(*this, pass)) {
        const auto [bottom, top] = run_bounds(*this, *run, distances);
        double lower             = bottom;
        for (size_t component = run->first; component <= pass.component; ++component) {
            double upper = lower;
            if (component == run->last)
                upper = top;
            else if (mask_contains(run->masks[component - run->first],
                                   geometry ? &geometry->split_masks[size_t(run - split_runs.data())][component - run->first] : nullptr,
                                   point))
                upper = std::max(lower + tongue_tip_height, top - (run->last - component) * run->cap);
            if (component == pass.component)
                return {lower, upper};
            lower = upper;
        }
    }
    auto interface = [&](size_t component) { return Slic3r::interface_z(*this, distances, component); };
    auto terminal  = [&](size_t count) {
        return Slic3r::terminal_interface(*this, point, count, component_bottom(*this, distances, zones.front().heights.size() - 1),
                                           Slic3r::pure_distance(*this, distances));
    };
    if (pass.tongue >= 0) {
        const double ceiling = tongue_planes[size_t(pass.tongue)];
        return {std::min(terminal(size_t(pass.tongue)), ceiling), std::min(terminal(size_t(pass.tongue) + 1), ceiling)};
    }
    if (shared_stack && pass.component + 1 == zones.front().heights.size() && !tongue_planes.empty())
        return {terminal(tongue_planes.size()), z_hi};
    if (shared_stack)
        return {component_bottom(*this, distances, pass.component),
                pass.component + 1 == zones.front().heights.size() ? z_hi : interface(pass.component)};
    const double anchor = interface(0);
    if (pass.component == 0)
        return terminal_anchor ? std::make_pair(z_lo + z_hi - anchor, z_hi) : std::make_pair(z_lo, anchor);
    const Zone&  zone     = zones[pass.zones.front()];
    const double residual = z_hi - z_lo - zone.heights.front();
    const double before   = std::accumulate(zone.heights.begin() + 1, zone.heights.begin() + pass.component, 0.) / residual;
    const double after    = before + zone.heights[pass.component] / residual;
    if (terminal_anchor)
        return {z_lo + before * (z_hi - anchor), z_lo + after * (z_hi - anchor)};
    return {anchor + before * (z_hi - anchor), anchor + after * (z_hi - anchor)};
}

std::vector<ExtrusionPathSloped> LocalZMixBridge::modulate(const ExtrusionPath& path, size_t component, double z_offset) const
{
    std::vector<ExtrusionPathSloped> output;
    if (component >= passes.size() || zones.empty() || path.height <= 0. || z_hi <= z_lo)
        return output;

    // Preserve the authored centerline. Short linear segments approximate the
    // common field; unlike a path-local smoothing filter, this also works when
    // color passes choose different seams or reverse their direction.
    constexpr double sample_spacing = 0.05;
    for (size_t i = 1; i < path.polyline.points.size(); ++i) {
        const Point& a      = path.polyline.points[i - 1];
        const Point& b      = path.polyline.points[i];
        const double length = unscale<double>(a.distance_to(b));
        if (length <= 0.)
            continue;
        const size_t count             = size_t(std::ceil(length / sample_spacing));
        const size_t edge_output_start = output.size();
        Point        previous          = a;
        auto         previous_surfaces = surfaces(a, component);
        if (previous_surfaces.second > passes[component].ceiling + 1e-6)
            throw std::runtime_error(
                "Local-Z extrusion exceeds its travel ceiling: cycle=" + std::to_string(z_lo) + "," + std::to_string(z_hi) +
                " pass=" + std::to_string(component) + " top=" + std::to_string(previous_surfaces.second) +
                " ceiling=" + std::to_string(passes[component].ceiling) + " flat=" + std::to_string(passes[component].flat_height) +
                " tongue=" + std::to_string(passes[component].tongue) + " extra=" + std::to_string(passes[component].extra_color) +
                " x=" + std::to_string(unscale<double>(a.x())) + " y=" + std::to_string(unscale<double>(a.y())));
        bool         previous_flat     = false;
        for (size_t j = 1; j <= count; ++j) {
            const double t = double(j) / double(count);
            const Point  next(coord_t(std::llround(double(a.x()) + double(b.x() - a.x()) * t)),
                              coord_t(std::llround(double(a.y()) + double(b.y() - a.y()) * t)));
            if (next == previous)
                continue;
            const auto   next_surfaces = surfaces(next, component);
            if (next_surfaces.second > passes[component].ceiling + 1e-6 || next_surfaces.second < next_surfaces.first - 1e-6)
                throw std::runtime_error("Invalid Local-Z extrusion surface bounds");
            const double height = 0.5 * (previous_surfaces.second - previous_surfaces.first + next_surfaces.second - next_surfaces.first);
            if (height <= 1e-9) {
                previous          = next;
                previous_surfaces = next_surfaces;
                previous_flat     = false;
                continue;
            }
            const bool   flat   = std::abs(previous_surfaces.first - next_surfaces.first) < 1e-10 &&
                              std::abs(previous_surfaces.second - next_surfaces.second) < 1e-10;
            // Do not flood the firmware with tiny moves on constant plateaus.
            // Merge only within this original straight edge, and only when
            // both the surface and bead height are constant.
            if (flat && previous_flat && output.size() > edge_output_start) {
                output.back().polyline.points.back() = next;
                previous                             = next;
                previous_surfaces                    = next_surfaces;
                continue;
            }
            ExtrusionPath segment(Polyline{previous, next}, path);
            // Match the existing Local-Z volume convention: deposited volume
            // uses projected XY length and scales with the local bead height.
            segment.mm3_per_mm *= height / double(path.height);
            segment.height = float(height);
            segment.polyline.fitting_result.clear();
            ExtrusionPathSloped sloped(std::move(segment), {0., 1.}, {1., 1.});
            sloped.local_z_modulation = true;
            sloped.absolute_z_begin   = previous_surfaces.second + z_offset;
            sloped.absolute_z_end     = next_surfaces.second + z_offset;
            output.emplace_back(std::move(sloped));
            previous          = next;
            previous_surfaces = next_surfaces;
            previous_flat     = flat;
        }
    }
    return output;
}

} // namespace Slic3r
