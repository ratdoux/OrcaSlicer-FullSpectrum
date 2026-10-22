#include "LocalZMixBridge.hpp"
#include "ClipperUtils.hpp"
#include "Layer.hpp"
#include "Fill/FillBase.hpp"
#include <boost/log/trivial.hpp>
#include <unordered_map>
#include "Print.hpp"
#include "BoundingBox.hpp"
#include <array>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>

namespace Slic3r {

std::shared_ptr<const LocalZMixBridgePaths> LocalZMixBridgePaths::build(Layer& layer, const ExPolygons& masks, unsigned filament_id)
{
    auto result = std::make_shared<LocalZMixBridgePaths>();
    if (masks.empty())
        return result;
    const LayerRegion* reference = nullptr;
    for (size_t i = 0; i < layer.regions().size(); ++i)
        if (!intersection_ex(masks, to_expolygons(layer.regions()[i]->slices.surfaces)).empty()) {
            reference         = layer.regions()[i];
            result->region_id = i;
            break;
        }
    if (reference == nullptr)
        throw std::runtime_error("Local-Z island has no owning print region");
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
    result->fill_masks      = union_ex(to_expolygons(island.fill_surfaces.surfaces));
    result->fill_no_overlap = std::move(island.fill_no_overlap_expolygons);
    return result;
}

std::shared_ptr<const LocalZMixBridgePaths> LocalZMixBridgePaths::with_fills(const LocalZMixBridgePaths& paths,
                                                                             Layer&                      layer,
                                                                             const LocalZMixBridge&      bridge,
                                                                             unsigned                    filament_id,
                                                                             FillAdaptive::Octree*       adaptive,
                                                                             FillAdaptive::Octree*       support,
                                                                             FillLightning::Generator*   lightning)
{
    auto result = std::make_shared<LocalZMixBridgePaths>(paths);
    result->fills.clear();
    auto config                = layer.regions()[paths.region_id]->region().config();
    config.wall_filament.value = config.sparse_infill_filament.value = config.solid_infill_filament.value = int(filament_id);
    PrintRegion physical(config, 0, 0);
    struct IslandLayer : Layer
    {
        explicit IslandLayer(Layer& source) : Layer(source.id(), source.object(), source.height, source.print_z, source.slice_z) {}
    };
    IslandLayer island(layer);
    auto*       region                 = island.add_region(&physical);
    region->fill_no_overlap_expolygons = paths.fill_no_overlap;
    // A cycle may span a floor or roof. Retain solid coverage from every
    // covered nominal layer instead of borrowing only the scheduling layer's
    // sparse pattern. This conservatively thickens skins within the cycle.
    ExPolygons top, solid;
    for (const Layer* source : layer.object()->layers()) {
        if (source->print_z <= bridge.z_lo + 1e-6 || source->bottom_z() >= bridge.z_hi - 1e-6)
            continue;
        for (const LayerRegion* original : source->regions())
            for (const Surface& surface : original->fill_surfaces.surfaces) {
                if (surface.is_top())
                    top.push_back(surface.expolygon);
                else if (surface.is_solid())
                    solid.push_back(surface.expolygon);
            }
    }
    top   = intersection_ex(union_ex(top), paths.fill_masks);
    solid = diff_ex(intersection_ex(union_ex(solid), paths.fill_masks), top);
    region->fill_surfaces.append(top, stTop);
    region->fill_surfaces.append(solid, stInternalSolid);
    append(solid, top);
    region->fill_surfaces.append(diff_ex(paths.fill_masks, solid), stInternal);
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
    result->fills = std::move(region->fills);
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
double zone_distance(const LocalZMixBridge::Zone& zone, const Point& point)
{
    if (std::any_of(zone.masks.begin(), zone.masks.end(), [&point](const ExPolygon& mask) { return mask.contains(point); }))
        return 0.;
    return unscale<double>(point.distance_to(projection_onto(zone.masks, point)));
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
// Clip a narrow band by the tongue-height field on a 0.05 mm grid. The
// triangulated cells share edge crossings, so their union is a closed region.
// Rasterizing only scanline spans of the band avoids sampling the whole object.
ExPolygons tongue_region(const LocalZMixBridge& bridge, size_t index, const ExPolygons& pure, const ExPolygons& mixed)
{
    double anchor_min = bridge.z_hi;
    for (const auto& zone : bridge.zones)
        if (!zone.preserve_nominal_layers)
            anchor_min = std::min(anchor_min, bridge.z_hi - zone.heights.back());
    const double reach = std::max(0., (bridge.tongue_planes[index] - anchor_min) / bridge.maximum_slope);
    const auto   band  = intersection_ex(offset_ex(pure, scale_(reach + .05)), mixed);
    if (band.empty())
        return pure;
    const auto                                   bbox = get_extents(band);
    const coord_t                                step = scale_(.05);
    Polygons                                     cells;
    std::unordered_map<Point, double, PointHash> vertex_values;
    auto                                         value = [&](const Point& point) {
        const auto found = vertex_values.find(point);
        if (found != vertex_values.end())
            return found->second;
        const double anchor = bridge.interface_z(point, bridge.zones.front().heights.size() - 2);
        const double lower  = bridge.terminal_interface(point, index);
        const double upper = std::max(anchor, bridge.tongue_planes[index] - bridge.maximum_slope * bridge.pure_distance(point));
        const double result = upper - lower - bridge.tongue_tip_height;
        vertex_values.emplace(point, result);
        return result;
    };
    for (coord_t y = bbox.min.y(); y < bbox.max.y(); y += step) {
        // Bound temporary storage even for very long or finely painted edges.
        if (vertex_values.size() > 100000)
            vertex_values.clear();
        const auto                               spans = intersection_pl(Polylines{Polyline{Point(bbox.min.x() - step, y + step / 2),
                                                              Point(bbox.max.x() + step, y + step / 2)}},
                                                                         band);
        std::vector<std::pair<coord_t, coord_t>> ranges;
        for (const auto& span : spans) {
            const auto    span_box = get_extents(span);
            const coord_t begin    = std::max(coord_t(0), (span_box.min.x() - bbox.min.x()) / step - 1);
            const coord_t end      = (span_box.max.x() - bbox.min.x()) / step + 1;
            ranges.emplace_back(begin, end);
        }
        std::sort(ranges.begin(), ranges.end());
        coord_t previous_end = -1;
        for (const auto& range : ranges) {
            const coord_t begin = std::max(range.first, previous_end + 1);
            previous_end        = std::max(previous_end, range.second);
            for (coord_t cell = begin; cell <= range.second; ++cell) {
                const coord_t               x = bbox.min.x() + cell * step;
                const std::array<Point, 4>  points{Point(x, y), Point(x + step, y), Point(x + step, y + step), Point(x, y + step)};
                const std::array<double, 4> values{value(points[0]), value(points[1]), value(points[2]), value(points[3])};
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
        }
    }
    auto result = intersection_ex(union_ex(cells), band);
    append(result, pure);
    return union_ex(result);
}
} // namespace

std::vector<LocalZMixBridge> LocalZMixBridge::build(double                     bottom,
                                                    double                     top,
                                                    double                     minimum,
                                                    const std::vector<double>& maximum,
                                                    std::vector<Zone>          input,
                                                    const std::vector<double>& nominal_heights)
{
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
            if (id == 0 || id > maximum.size() || (i > 0 && id == zone.component_ids[i - 1]) || zone.heights[i] < minimum - 1e-6 ||
                zone.heights[i] > maximum[id - 1] + 1e-6)
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
        std::vector<size_t> best;
        unsigned int        best_anchor = 0;
        size_t              best_score  = 0;
        // Consider complete shared stacks as well as one common anchor. A
        // deterministic greedy forest avoids cyclic material-order demands.
        for (size_t seed = 0; seed < input.size(); ++seed) {
            if (used[seed])
                continue;
            for (size_t mode = 0; mode <= input[seed].component_ids.size(); ++mode) {
                const unsigned int  anchor = mode == 0 ? 0 : input[seed].component_ids[mode - 1];
                std::vector<size_t> group{seed};
                auto limits  = anchor == 0 ? std::make_pair(0., height) : anchor_range(input[seed], anchor, height, minimum, maximum);
                bool changed = true;
                while (changed) {
                    changed = false;
                    for (size_t candidate = 0; candidate < input.size(); ++candidate) {
                        if (used[candidate] || std::find(group.begin(), group.end(), candidate) != group.end() ||
                            !std::any_of(group.begin(), group.end(), [&](size_t i) { return adjacent[i][candidate]; }))
                            continue;
                        if (anchor == 0) {
                            if (input[candidate].component_ids != input[seed].component_ids)
                                continue;
                        } else {
                            if (std::find(input[candidate].component_ids.begin(), input[candidate].component_ids.end(), anchor) ==
                                input[candidate].component_ids.end())
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
                const size_t score = (group.size() - 1) * (anchor == 0 ? input[seed].component_ids.size() : 1);
                if (best.empty() || score > best_score || (score == best_score && group.size() > best.size())) {
                    best        = std::move(group);
                    best_anchor = anchor;
                    best_score  = score;
                }
            }
        }
        LocalZMixBridge bridge;
        bridge.z_lo         = bottom;
        bridge.z_hi         = top;
        bridge.shared_stack = best_anchor == 0;
        for (size_t i : best) {
            used[i]   = true;
            Zone zone = input[i];
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
            for (size_t i = 0; i < bridge.tongue_planes.size(); ++i)
                bridge.tongue_masks.push_back(tongue_region(bridge, i, pure, mixed));
        }
        const size_t shared_count = bridge.shared_stack ? bridge.zones.front().heights.size() : 1;
        for (size_t component = 0; component < shared_count; ++component) {
            double ceiling = 0.;
            for (const Zone& zone : bridge.zones)
                if (!zone.preserve_nominal_layers || zone.join_terminal_pass)
                    ceiling = std::max(ceiling, bottom + std::accumulate(zone.heights.begin(), zone.heights.begin() + component + 1, 0.));
            for (size_t i = 0; i < bridge.zones.size(); ++i) {
                if (bridge.zones[i].preserve_nominal_layers && (!bridge.zones[i].join_terminal_pass || component + 1 != shared_count))
                    continue;
                const unsigned id   = bridge.zones[i].component_ids[component];
                auto           pass = std::find_if(bridge.passes.begin(), bridge.passes.end(),
                                                   [&](const Pass& p) { return p.component == component && p.filament_id == id; });
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
        // Schedule by the lowest local top, before adjacent planar interiors
        // advance above it. Keep the maximum top separately for travel.
        std::stable_sort(bridge.passes.begin(), bridge.passes.end(),
                         [&](const Pass& a, const Pass& b) { return bridge.schedule_z(a) < bridge.schedule_z(b); });
        result.emplace_back(std::move(bridge));
    }
    return result;
}

ExPolygons LocalZMixBridge::masks(const Pass& pass) const
{
    if (!pass.masks.empty())
        return pass.masks;
    ExPolygons result;
    for (size_t zone : pass.zones)
        append(result, zones[zone].masks);
    return union_ex(result);
}

double LocalZMixBridge::pure_distance(const Point& point) const
{
    double distance = std::numeric_limits<double>::max();
    for (const auto& zone : zones)
        if (zone.join_terminal_pass)
            distance = std::min(distance, zone_distance(zone, point));
    return distance;
}

double LocalZMixBridge::terminal_interface(const Point& point, size_t tongue_count) const
{
    double       previous = interface_z(point, zones.front().heights.size() - 2);
    const double distance = pure_distance(point);
    for (size_t i = 0; i < tongue_count; ++i)
        if (std::any_of(tongue_masks[i].begin(), tongue_masks[i].end(), [&](const ExPolygon& mask) { return mask.contains(point); }))
            previous = std::max(previous + tongue_tip_height, tongue_planes[i] - maximum_slope * distance);
    return previous;
}

double LocalZMixBridge::schedule_z(const Pass& pass) const
{
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
    return floor;
}

double LocalZMixBridge::interface_z(const Point& point, size_t component) const
{
    double upper   = std::numeric_limits<double>::max();
    double lower   = std::numeric_limits<double>::lowest();
    double minimum = upper;
    double maximum = lower;
    double floor = z_lo, ceiling = z_hi;
    for (size_t i = 0; i < zones.size(); ++i) {
        const Zone& zone = zones[i];
        if (zone.masks.empty() || (zone.preserve_nominal_layers && !zone.join_terminal_pass))
            continue;
        const double distance = zone_distance(zone, point);
        const double target   = std::accumulate(zone.heights.begin(), zone.heights.begin() + component + 1, 0.);
        upper                 = std::min(upper, target + maximum_slope * distance);
        lower                 = std::max(lower, target - maximum_slope * distance);
        minimum               = std::min(minimum, target);
        maximum               = std::max(maximum, target);
        if (zone.preserve_nominal_layers) {
            floor   = std::max(floor, z_lo + target - maximum_slope * distance);
            ceiling = std::min(ceiling, z_lo + target + maximum_slope * distance);
        }
        if (!shared_stack) {
            floor   = std::max(floor, z_lo + anchor_limits[i].first - maximum_slope * distance);
            ceiling = std::min(ceiling, z_lo + anchor_limits[i].second + maximum_slope * distance);
        }
    }
    if (minimum > maximum)
        throw std::runtime_error("Local-Z mix bridge has no zones");
    // This operator is monotone and translation-equivariant. Applying it to
    // cumulative heights preserves each component's min/max difference, even
    // at multi-zone junctions. Every interface is maximum_slope-Lipschitz.
    // For unlike sets, spatial bounds keep residual beads printable in their
    // own zones without clamping the recipe's plateau to a remote zone's cap.
    return std::clamp(z_lo + std::clamp(0.5 * (lower + upper), minimum, maximum), floor, std::max(floor, ceiling));
}

std::pair<double, double> LocalZMixBridge::surfaces(const Point& point, size_t pass_index) const
{
    const Pass& pass = passes.at(pass_index);
    if (pass.tongue >= 0)
        return {terminal_interface(point, size_t(pass.tongue)), terminal_interface(point, size_t(pass.tongue) + 1)};
    if (pass.flat_height > 0.)
        return {pass.ceiling - pass.flat_height, pass.ceiling};
    if (shared_stack && pass.component + 1 == zones.front().heights.size() && !tongue_planes.empty())
        return {terminal_interface(point, tongue_planes.size()), z_hi};
    if (shared_stack)
        return {pass.component == 0 ? z_lo : interface_z(point, pass.component - 1),
                pass.component + 1 == zones.front().heights.size() ? z_hi : interface_z(point, pass.component)};
    const double anchor = interface_z(point);
    if (pass.component == 0)
        return {z_lo, anchor};
    const Zone&  zone     = zones[pass.zones.front()];
    const double residual = z_hi - z_lo - zone.heights.front();
    const double before   = std::accumulate(zone.heights.begin() + 1, zone.heights.begin() + pass.component, 0.) / residual;
    const double after    = before + zone.heights[pass.component] / residual;
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
        bool         previous_flat     = false;
        for (size_t j = 1; j <= count; ++j) {
            const double t = double(j) / double(count);
            const Point  next(coord_t(std::llround(double(a.x()) + double(b.x() - a.x()) * t)),
                              coord_t(std::llround(double(a.y()) + double(b.y() - a.y()) * t)));
            if (next == previous)
                continue;
            const auto   next_surfaces = surfaces(next, component);
            const double height = 0.5 * (previous_surfaces.second - previous_surfaces.first + next_surfaces.second - next_surfaces.first);
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
