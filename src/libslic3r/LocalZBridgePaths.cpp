#include "LocalZBridgePaths.hpp"
#include "LocalZBridge.hpp"
#include "LocalZBridgeProjector.hpp"
#include "LocalZClipper.hpp"
#include "ClipperUtils.hpp"
#include "Layer.hpp"
#include "Print.hpp"
#include "ShortestPath.hpp"
#include "VariableWidth.hpp"
#include <algorithm>
#include <limits>
namespace Slic3r {
static bool bridge_collection_prints(const ExtrusionEntityCollection& collection,
                                     const SubLayerPlan&              plan,
                                     const LocalZClipper*             clipper = nullptr)
{
    for (const ExtrusionEntity* entity : collection.flatten_refs()) {
        Polylines lines;
        entity->collect_polylines(lines);
        for (const Polyline& line : lines) {
            for (Polyline& piece : clipper ? clipper->intersection(line) : Polylines{line}) {
                ExtrusionPath path(entity->role(), 0.1, 0.4f, float(plan.flow_height));
                path.polyline = std::move(piece);
                if (!project_local_z_bridge_path(*plan.bridge_field, path, plan.bridge_slot_index, 0.,
                                                 {plan.bridge_top_lower, plan.bridge_top_upper})
                         .empty())
                    return true;
            }
        }
    }
    return false;
}

LocalZBridgeIsland prepare_local_z_bridge_pass(const Layer& layer, const SubLayerPlan& plan)
{
    if (!plan.bridge_field)
        return {};
    ExPolygons masks;
    for (const auto& tool_masks : plan.painted_masks_by_extruder)
        append(masks, tool_masks);
    LocalZBridgeIsland result = make_local_z_bridge_island(layer, union_ex(masks));
    if (result.walls && bridge_collection_prints(*result.walls, plan)) {
        result.has_extrusions = true;
    } else if (!result.infill_area.empty()) {
        const LocalZClipper clipper(result.infill_area);
        for (const LayerRegion* region : layer.regions())
            if (region && bridge_collection_prints(region->fills, plan, &clipper)) {
                result.has_extrusions = true;
                break;
            }
    }
    return result;
}

LocalZBridgeIsland make_local_z_bridge_island(const Layer& layer, ExPolygons island)
{
    LocalZBridgeIsland result;
    // The region covering most of the island supplies flows and wall count.
    double best_area = 0.;
    for (const LayerRegion* layerm : layer.regions()) {
        if (layerm == nullptr || layerm->slices.empty())
            continue;
        const double covered = std::abs(area(intersection_ex(to_expolygons(layerm->slices.surfaces), island)));
        if (covered > best_area) {
            best_area     = covered;
            result.region = layerm;
        }
    }
    if (result.region == nullptr)
        return result;

    const Flow ext_flow   = result.region->flow(frExternalPerimeter);
    const Flow perim_flow = result.region->flow(frPerimeter);
    // Zones can leave specks between them that no zone covers. A hole narrower
    // than an outer wall cannot hold a wall loop, and ringing it with loops
    // leaves a knot of seams on the surface, so the island prints over it.
    const float min_hole_half_width = float(0.5 * double(ext_flow.scaled_width()));
    for (ExPolygon& piece : island)
        piece.holes.erase(std::remove_if(piece.holes.begin(), piece.holes.end(),
                                         [min_hole_half_width](Polygon hole) {
                                             hole.make_counter_clockwise();
                                             return offset(hole, -min_hole_half_width).empty();
                                         }),
                          piece.holes.end());
    const PrintRegionConfig& region_config = result.region->region().config();
    // An island needs at least two walls to hold its own edge.
    const int wall_count = std::max(2, region_config.wall_loops.value);
    // Where a wall is too narrow for a loop, the classic generator's gap fill
    // puts one bead of matching width along the middle of the space left.
    const bool   gap_fill      = !region_config.gap_infill_speed.values.empty() && region_config.gap_infill_speed.values.front() > 0;
    const Flow   gap_flow      = result.region->flow(frSolidInfill);
    const double gap_min_width = 0.2 * double(perim_flow.scaled_width()) * (1. - INSET_OVERLAP_TOLERANCE);
    const double gap_max_width = 2. * double(perim_flow.scaled_spacing());
    result.walls               = std::make_shared<ExtrusionEntityCollection>();
    double last_inset          = 0.;
    // Walls print in the order they are added, so each part of the island gets
    // all its walls before the next part starts.
    for (const size_t piece_idx : chain_expolygons(island)) {
        const ExPolygons piece{island[piece_idx]};
        ThickPolylines   gap_lines;
        // Classic spacing: the outer wall sits half a width inside the outline and
        // every further wall one spacing deeper.
        double inset = 0.5 * double(ext_flow.scaled_width()), bead_edge = 0.;
        for (int wall_idx = 0; wall_idx < wall_count; ++wall_idx) {
            if (wall_idx == 1)
                inset += 0.5 * double(ext_flow.scaled_spacing() + perim_flow.scaled_spacing());
            else if (wall_idx > 1)
                inset += double(perim_flow.scaled_spacing());
            const Flow&      flow    = wall_idx == 0 ? ext_flow : perim_flow;
            const double     spacing = double(flow.scaled_spacing());
            ExPolygons       narrow;
            const ExPolygons rings = bridge_island_wall_area(piece, inset, spacing, gap_fill ? &narrow : nullptr);
            if (!narrow.empty()) {
                // The space between the previous wall's beads and this wall's,
                // around where this wall is too narrow for a loop.
                const ExPolygons gaps = diff_ex(intersection_ex(wall_idx == 0 ? piece : bridge_island_inset(piece, bead_edge),
                                                                offset_ex(narrow, float(spacing))),
                                                offset_ex(rings, float(0.5 * spacing) + ClipperSafetyOffset));
                for (const ExPolygon& gap :
                     diff_ex(opening_ex(gaps, float(0.5 * gap_min_width)),
                             offset2_ex(gaps, -float(0.5 * gap_max_width), float(0.5 * gap_max_width) + ClipperSafetyOffset)))
                    gap.medial_axis(gap_min_width, gap_max_width, &gap_lines);
            }
            if (rings.empty())
                break;
            const ExtrusionRole role     = wall_idx == 0 ? erExternalPerimeter : erPerimeter;
            auto                add_loop = [&](const Polygon& polygon, ExtrusionLoopRole loop_role) {
                ExtrusionPath path(role, flow.mm3_per_mm(), flow.width(), flow.height());
                path.polyline = polygon.split_at_first_point();
                result.walls->append(ExtrusionLoop(path, loop_role));
            };
            for (const ExPolygon& ring : rings) {
                add_loop(ring.contour, elrDefault);
                for (const Polygon& hole : ring.holes)
                    add_loop(hole, elrHole);
            }
            bead_edge  = inset + 0.5 * spacing;
            last_inset = std::max(last_inset, inset);
        }
        gap_lines.erase(std::remove_if(gap_lines.begin(), gap_lines.end(),
                                       [&region_config](const ThickPolyline& line) {
                                           return line.length() < scale_(region_config.filter_out_gap_fill.value);
                                       }),
                        gap_lines.end());
        variable_width(gap_lines, erGapFill, gap_flow, result.walls->entities);
    }
    if (last_inset > 0.)
        result.infill_area = bridge_island_inset(island, last_inset + 0.5 * double(perim_flow.scaled_spacing()));
    return result;
}

// Clipping a region's monotonic infill to a bridge island cuts every line where
// the island ends. Where the island wraps around a hole or falls apart, a line
// leaves pieces on both sides and the region's order alternates between them,
// so the nozzle would cross the hole for every line. Each piece joins the run
// whose last piece lies next to it, which keeps every run in the region's
// order, and the runs are chained by distance like separate fills.
std::vector<std::unique_ptr<ExtrusionEntityCollection>> split_local_z_collection_into_runs(
    std::unique_ptr<ExtrusionEntityCollection> collection)
{
    std::vector<std::unique_ptr<ExtrusionEntityCollection>> runs;
    if (!collection->no_sort || collection->entities.size() < 2) {
        runs.emplace_back(std::move(collection));
        return runs;
    }
    // Pieces of neighboring lines do not cross, so they come closest at a vertex
    // of one of them.
    auto squared_distance = [](const Points& lhs, const Points& rhs) {
        auto one_way = [](const Points& from, const Points& to) {
            double best = std::numeric_limits<double>::max();
            for (const Point& point : from)
                for (size_t idx = 1; idx < to.size(); ++idx)
                    best = std::min(best, Line::distance_to_squared(point, to[idx - 1], to[idx]));
            return best;
        };
        return std::min(one_way(lhs, rhs), one_way(rhs, lhs));
    };
    struct Run
    {
        std::unique_ptr<ExtrusionEntityCollection> collection;
        Points                                     last_points;
        BoundingBox                                last_box;
    };
    std::vector<Run> open_runs;
    for (const ExtrusionEntity* entity : collection->entities) {
        Points points;
        entity->collect_points(points);
        const BoundingBox box(points);
        // Neighboring lines of a fill lie about one width apart.
        const auto*   path      = dynamic_cast<const ExtrusionPath*>(entity);
        const coord_t reach     = path != nullptr ? coord_t(scale_(2. * path->width)) : 0;
        Run*          best      = nullptr;
        double        best_dist = double(reach) * double(reach);
        for (Run& run : open_runs) {
            BoundingBox near_box = run.last_box;
            near_box.offset(reach);
            if (!near_box.overlap(box))
                continue;
            // Ties go to the latest run, which the region was printing.
            if (const double dist = squared_distance(points, run.last_points); dist <= best_dist) {
                best      = &run;
                best_dist = dist;
            }
        }
        if (best == nullptr) {
            open_runs.push_back({std::make_unique<ExtrusionEntityCollection>(), {}, {}});
            best                      = &open_runs.back();
            best->collection->no_sort = true;
        }
        best->collection->append(*entity);
        best->last_points = std::move(points);
        best->last_box    = box;
    }
    for (Run& run : open_runs)
        runs.emplace_back(std::move(run.collection));
    return runs;
}
} // namespace Slic3r
