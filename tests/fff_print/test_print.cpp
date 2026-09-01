#include <catch2/catch_test_macros.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/FullSpectrumKSPairResidual.hpp"
#include "libslic3r/ImageMap/AdaptiveLocalZRenderer.hpp"
#include "libslic3r/ImageMap/PerimeterEnvelopeRenderer.hpp"
#include "libslic3r/ImageMap/VolumeData.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "test_data.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <set>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

bool is_image_map_outer_wall(ExtrusionRole role)
{
    // Arachne may classify a one-wall or fully unsupported outer boundary as
    // a regular/overhang perimeter. All three roles contribute to boundary
    // coverage; width-specific checks below stay restricted to the externally
    // controlled roles.
    return is_perimeter(role);
}

void collect_external_perimeter_points(const ExtrusionEntity& entity, Points& points)
{
    if (const auto* path = dynamic_cast<const ExtrusionPath*>(&entity)) {
        if (is_image_map_outer_wall(path->role()))
            append(points, path->polyline.points);
    } else if (const auto* multipath = dynamic_cast<const ExtrusionMultiPath*>(&entity)) {
        for (const ExtrusionPath& path : multipath->paths)
            if (is_image_map_outer_wall(path.role()))
                append(points, path.polyline.points);
    } else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(&entity)) {
        for (const ExtrusionPath& path : loop->paths)
            if (is_image_map_outer_wall(path.role()))
                append(points, path.polyline.points);
    } else if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        for (const ExtrusionEntity* child : collection->entities)
            if (child != nullptr)
                collect_external_perimeter_points(*child, points);
    }
}

void collect_external_perimeter_widths(const ExtrusionEntity& entity, std::vector<float>& widths)
{
    if (const auto* path = dynamic_cast<const ExtrusionPath*>(&entity)) {
        if (is_image_map_outer_wall(path->role()))
            widths.emplace_back(path->width);
    } else if (const auto* multipath = dynamic_cast<const ExtrusionMultiPath*>(&entity)) {
        for (const ExtrusionPath& path : multipath->paths)
            if (is_image_map_outer_wall(path.role()))
                widths.emplace_back(path.width);
    } else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(&entity)) {
        for (const ExtrusionPath& path : loop->paths)
            if (is_image_map_outer_wall(path.role()))
                widths.emplace_back(path.width);
    } else if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        for (const ExtrusionEntity* child : collection->entities)
            if (child != nullptr)
                collect_external_perimeter_widths(*child, widths);
    }
}

void collect_texture_external_perimeter_widths(const ExtrusionEntity& entity, std::vector<float>& widths)
{
    auto collect_path = [&widths](const ExtrusionPath& path) {
        if (path.role() == erExternalPerimeter)
            widths.emplace_back(path.width);
    };
    if (const auto* path = dynamic_cast<const ExtrusionPath*>(&entity)) {
        collect_path(*path);
    } else if (const auto* multipath = dynamic_cast<const ExtrusionMultiPath*>(&entity)) {
        for (const ExtrusionPath& path : multipath->paths)
            collect_path(path);
    } else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(&entity)) {
        for (const ExtrusionPath& path : loop->paths)
            collect_path(path);
    } else if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        for (const ExtrusionEntity* child : collection->entities)
            if (child != nullptr)
                collect_texture_external_perimeter_widths(*child, widths);
    }
}

void collect_texture_external_perimeter_points(const ExtrusionEntity& entity, Points& points)
{
    auto collect_path = [&points](const ExtrusionPath& path) {
        if (path.role() == erExternalPerimeter)
            append(points, path.polyline.points);
    };
    if (const auto* path = dynamic_cast<const ExtrusionPath*>(&entity)) {
        collect_path(*path);
    } else if (const auto* multipath = dynamic_cast<const ExtrusionMultiPath*>(&entity)) {
        for (const ExtrusionPath& path : multipath->paths)
            collect_path(path);
    } else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(&entity)) {
        for (const ExtrusionPath& path : loop->paths)
            collect_path(path);
    } else if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        for (const ExtrusionEntity* child : collection->entities)
            if (child != nullptr)
                collect_texture_external_perimeter_points(*child, points);
    }
}

double texture_external_perimeter_length(const ExtrusionEntity& entity)
{
    auto path_length = [](const ExtrusionPath& path) {
        return path.role() == erExternalPerimeter ? unscale<double>(path.polyline.length()) : 0.;
    };
    if (const auto* path = dynamic_cast<const ExtrusionPath*>(&entity))
        return path_length(*path);
    if (const auto* multipath = dynamic_cast<const ExtrusionMultiPath*>(&entity)) {
        double length = 0.;
        for (const ExtrusionPath& path : multipath->paths)
            length += path_length(path);
        return length;
    }
    if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(&entity)) {
        double length = 0.;
        for (const ExtrusionPath& path : loop->paths)
            length += path_length(path);
        return length;
    }
    if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        double length = 0.;
        for (const ExtrusionEntity* child : collection->entities)
            if (child != nullptr)
                length += texture_external_perimeter_length(*child);
        return length;
    }
    return 0.;
}

double perimeter_length(const ExtrusionEntity& entity)
{
    auto path_length = [](const ExtrusionPath& path) {
        return is_perimeter(path.role()) ? unscale<double>(path.polyline.length()) : 0.;
    };
    if (const auto* path = dynamic_cast<const ExtrusionPath*>(&entity))
        return path_length(*path);
    if (const auto* multipath = dynamic_cast<const ExtrusionMultiPath*>(&entity)) {
        double length = 0.;
        for (const ExtrusionPath& path : multipath->paths)
            length += path_length(path);
        return length;
    }
    if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(&entity)) {
        double length = 0.;
        for (const ExtrusionPath& path : loop->paths)
            length += path_length(path);
        return length;
    }
    if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        double length = 0.;
        for (const ExtrusionEntity* child : collection->entities)
            if (child != nullptr)
                length += perimeter_length(*child);
        return length;
    }
    return 0.;
}

double regular_perimeter_length(const ExtrusionEntity& entity)
{
    auto path_length = [](const ExtrusionPath& path) {
        return path.role() == erPerimeter ? unscale<double>(path.polyline.length()) : 0.;
    };
    if (const auto* path = dynamic_cast<const ExtrusionPath*>(&entity))
        return path_length(*path);
    if (const auto* multipath = dynamic_cast<const ExtrusionMultiPath*>(&entity)) {
        double length = 0.;
        for (const ExtrusionPath& path : multipath->paths)
            length += path_length(path);
        return length;
    }
    if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(&entity)) {
        double length = 0.;
        for (const ExtrusionPath& path : loop->paths)
            length += path_length(path);
        return length;
    }
    if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        double length = 0.;
        for (const ExtrusionEntity* child : collection->entities)
            if (child != nullptr)
                length += regular_perimeter_length(*child);
        return length;
    }
    return 0.;
}

void collect_inner_perimeter_polylines(const ExtrusionEntity& entity, Polylines& polylines)
{
    if (const auto* path = dynamic_cast<const ExtrusionPath*>(&entity)) {
        if (is_perimeter(path->role()) && path->inset_idx > 0)
            polylines.emplace_back(path->polyline);
    } else if (const auto* multipath = dynamic_cast<const ExtrusionMultiPath*>(&entity)) {
        if (!multipath->paths.empty() &&
            std::all_of(multipath->paths.begin(), multipath->paths.end(), [](const ExtrusionPath& path) {
                return is_perimeter(path.role()) && path.inset_idx > 0;
            }))
            polylines.emplace_back(multipath->as_polyline());
    } else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(&entity)) {
        if (!loop->paths.empty() && std::all_of(loop->paths.begin(), loop->paths.end(), [](const ExtrusionPath& path) {
                return is_perimeter(path.role()) && path.inset_idx > 0;
            }))
            polylines.emplace_back(loop->as_polyline());
    } else if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        for (const ExtrusionEntity* child : collection->entities)
            if (child != nullptr)
                collect_inner_perimeter_polylines(*child, polylines);
    }
}

void collect_perimeter_inset_indices(const ExtrusionEntity& entity, std::set<size_t>& inset_indices)
{
    auto collect_path = [&inset_indices](const ExtrusionPath& path) {
        if (is_perimeter(path.role()) && path.inset_idx >= 0)
            inset_indices.emplace(size_t(path.inset_idx));
    };
    if (const auto* path = dynamic_cast<const ExtrusionPath*>(&entity)) {
        collect_path(*path);
    } else if (const auto* multipath = dynamic_cast<const ExtrusionMultiPath*>(&entity)) {
        if (multipath->inset_idx >= 0)
            inset_indices.emplace(size_t(multipath->inset_idx));
        else
            for (const ExtrusionPath& path : multipath->paths)
                collect_path(path);
    } else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(&entity)) {
        if (loop->inset_idx >= 0)
            inset_indices.emplace(size_t(loop->inset_idx));
        else
            for (const ExtrusionPath& path : loop->paths)
                collect_path(path);
    } else if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        for (const ExtrusionEntity* child : collection->entities)
            if (child != nullptr)
                collect_perimeter_inset_indices(*child, inset_indices);
    }
}

bool perimeter_polyline_is_covered_by(const Polyline& candidate, const Polyline& reference, double tolerance)
{
    const Lines reference_lines = reference.lines();
    if (candidate.points.size() < 2 || reference_lines.empty())
        return false;
    Points samples = candidate.equally_spaced_points(scale_(0.05));
    if (samples.empty() || samples.back() != candidate.points.back())
        samples.emplace_back(candidate.points.back());
    const double tolerance_squared = tolerance * tolerance;
    return std::all_of(samples.begin(), samples.end(), [&reference_lines, tolerance_squared](const Point& sample) {
        return std::any_of(reference_lines.begin(), reference_lines.end(), [&sample, tolerance_squared](const Line& line) {
            return line.distance_to_squared(sample) <= tolerance_squared;
        });
    });
}

size_t duplicate_inner_perimeter_count(const ExtrusionEntity& entity)
{
    Polylines inner_perimeters;
    collect_inner_perimeter_polylines(entity, inner_perimeters);
    size_t duplicate_count = 0;
    const double tolerance = scale_(0.05);
    for (size_t first_idx = 0; first_idx < inner_perimeters.size(); ++first_idx) {
        for (size_t second_idx = first_idx + 1; second_idx < inner_perimeters.size(); ++second_idx) {
            const Polyline& first  = inner_perimeters[first_idx];
            const Polyline& second = inner_perimeters[second_idx];
            if (perimeter_polyline_is_covered_by(first, second, tolerance) &&
                perimeter_polyline_is_covered_by(second, first, tolerance))
                ++duplicate_count;
        }
    }
    return duplicate_count;
}

using EndpointCounts = std::map<std::pair<coord_t, coord_t>, size_t>;

void collect_external_perimeter_endpoints(const ExtrusionEntity& entity, EndpointCounts& counts)
{
    auto collect_path = [&counts](const ExtrusionPath& path) {
        // A closed island may split one loop into external, overhang and
        // regular perimeter roles. Count every role belonging to the outer
        // inset, while excluding the independently generated inner loop.
        if (!is_perimeter(path.role()) || path.inset_idx != 0 || path.polyline.points.size() < 2)
            return;
        const Point& first = path.polyline.points.front();
        const Point& last  = path.polyline.points.back();
        ++counts[{first.x(), first.y()}];
        ++counts[{last.x(), last.y()}];
    };

    if (const auto* path = dynamic_cast<const ExtrusionPath*>(&entity)) {
        collect_path(*path);
    } else if (const auto* multipath = dynamic_cast<const ExtrusionMultiPath*>(&entity)) {
        for (const ExtrusionPath& path : multipath->paths)
            collect_path(path);
    } else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(&entity)) {
        for (const ExtrusionPath& path : loop->paths)
            collect_path(path);
    } else if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        for (const ExtrusionEntity* child : collection->entities)
            if (child != nullptr)
                collect_external_perimeter_endpoints(*child, counts);
    }
}

void collect_external_perimeter_lines(const ExtrusionEntity& entity, Lines& lines)
{
    if (const auto* path = dynamic_cast<const ExtrusionPath*>(&entity)) {
        if (is_image_map_outer_wall(path->role()))
            append(lines, path->polyline.lines());
    } else if (const auto* multipath = dynamic_cast<const ExtrusionMultiPath*>(&entity)) {
        for (const ExtrusionPath& path : multipath->paths)
            if (is_image_map_outer_wall(path.role()))
                append(lines, path.polyline.lines());
    } else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(&entity)) {
        for (const ExtrusionPath& path : loop->paths)
            if (is_image_map_outer_wall(path.role()))
                append(lines, path.polyline.lines());
    } else if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        for (const ExtrusionEntity* child : collection->entities)
            if (child != nullptr)
                collect_external_perimeter_lines(*child, lines);
    }
}

void collect_external_loop_join_gaps(const ExtrusionEntity& entity, size_t& join_count, double& max_gap_mm)
{
    if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(&entity)) {
        const bool has_external_perimeter = std::any_of(loop->paths.begin(), loop->paths.end(),
                                                        [](const ExtrusionPath& path) { return is_image_map_outer_wall(path.role()); });
        if (!has_external_perimeter || loop->paths.size() < 2)
            return;
        for (size_t path_index = 0; path_index < loop->paths.size(); ++path_index) {
            const ExtrusionPath& previous = loop->paths[path_index];
            const ExtrusionPath& next     = loop->paths[(path_index + 1) % loop->paths.size()];
            const double         gap_mm   = unscale<double>((previous.last_point() - next.first_point()).cast<double>().norm());
            max_gap_mm                    = std::max(max_gap_mm, gap_mm);
            ++join_count;
        }
    } else if (const auto* multipath = dynamic_cast<const ExtrusionMultiPath*>(&entity)) {
        if (multipath->paths.size() < 2)
            return;
        for (size_t path_index = 0; path_index + 1 < multipath->paths.size(); ++path_index) {
            const ExtrusionPath& previous = multipath->paths[path_index];
            const ExtrusionPath& next     = multipath->paths[path_index + 1];
            if (!is_image_map_outer_wall(previous.role()) && !is_image_map_outer_wall(next.role()))
                continue;
            const double gap_mm = unscale<double>((previous.last_point() - next.first_point()).cast<double>().norm());
            max_gap_mm          = std::max(max_gap_mm, gap_mm);
            ++join_count;
        }
    } else if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        for (const ExtrusionEntity* child : collection->entities)
            if (child != nullptr)
                collect_external_loop_join_gaps(*child, join_count, max_gap_mm);
    }
}

void update_polyline_turn_stats(const Polyline& polyline, size_t& turn_count, double& minimum_cosine)
{
    for (size_t point_index = 1; point_index + 1 < polyline.points.size(); ++point_index) {
        Vec2d        incoming        = (polyline.points[point_index] - polyline.points[point_index - 1]).cast<double>();
        Vec2d        outgoing        = (polyline.points[point_index + 1] - polyline.points[point_index]).cast<double>();
        const double incoming_length = incoming.norm();
        const double outgoing_length = outgoing.norm();
        if (incoming_length <= EPSILON || outgoing_length <= EPSILON)
            continue;
        const double cosine = incoming.dot(outgoing) / (incoming_length * outgoing_length);
        minimum_cosine      = std::min(minimum_cosine, cosine);
        ++turn_count;
    }
}

void collect_external_turn_stats(const ExtrusionEntity& entity, size_t& turn_count, double& minimum_cosine)
{
    if (const auto* path = dynamic_cast<const ExtrusionPath*>(&entity)) {
        if (is_image_map_outer_wall(path->role()) && std::abs(path->width - 0.45f) > 0.01f)
            update_polyline_turn_stats(path->polyline, turn_count, minimum_cosine);
    } else if (const auto* multipath = dynamic_cast<const ExtrusionMultiPath*>(&entity)) {
        for (const ExtrusionPath& path : multipath->paths)
            if (is_image_map_outer_wall(path.role()) && std::abs(path.width - 0.45f) > 0.01f)
                update_polyline_turn_stats(path.polyline, turn_count, minimum_cosine);
    } else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(&entity)) {
        for (const ExtrusionPath& path : loop->paths)
            if (is_image_map_outer_wall(path.role()) && std::abs(path.width - 0.45f) > 0.01f)
                update_polyline_turn_stats(path.polyline, turn_count, minimum_cosine);
    } else if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        for (const ExtrusionEntity* child : collection->entities)
            if (child != nullptr)
                collect_external_turn_stats(*child, turn_count, minimum_cosine);
    }
}

double cross_2d(const Vec2d& lhs, const Vec2d& rhs) { return lhs.x() * rhs.y() - lhs.y() * rhs.x(); }

bool properly_intersects(const Line& first, const Line& second)
{
    const Vec2d  p           = first.a.cast<double>();
    const Vec2d  r           = (first.b - first.a).cast<double>();
    const Vec2d  q           = second.a.cast<double>();
    const Vec2d  s           = (second.b - second.a).cast<double>();
    const double denominator = cross_2d(r, s);
    if (!std::isfinite(denominator) || std::abs(denominator) <= 1e-9)
        return false;
    const double     first_parameter  = cross_2d(q - p, s) / denominator;
    const double     second_parameter = cross_2d(q - p, r) / denominator;
    constexpr double endpoint_epsilon = 1e-6;
    return first_parameter > endpoint_epsilon && first_parameter < 1. - endpoint_epsilon && second_parameter > endpoint_epsilon &&
           second_parameter < 1. - endpoint_epsilon;
}

size_t polyline_self_intersection_count(Points points)
{
    const bool closed = points.size() > 2 && points.front() == points.back();
    if (closed)
        points.pop_back();
    if (points.size() < 4)
        return 0;

    size_t       count         = 0;
    const size_t segment_count = closed ? points.size() : points.size() - 1;
    for (size_t first = 0; first < segment_count; ++first) {
        const size_t first_next = (first + 1) % points.size();
        for (size_t second = first + 1; second < segment_count; ++second) {
            const size_t second_next = (second + 1) % points.size();
            if (first_next == second || second_next == first)
                continue;
            count += properly_intersects(Line{points[first], points[first_next]}, Line{points[second], points[second_next]});
        }
    }
    return count;
}

size_t path_chain_self_intersection_count(const ExtrusionPaths& paths, bool loop)
{
    size_t count = 0;
    Points chain;
    auto   flush = [&]() {
        count += polyline_self_intersection_count(std::move(chain));
        chain.clear();
    };
    for (const ExtrusionPath& path : paths) {
        if (!is_image_map_outer_wall(path.role()) || path.polyline.points.size() < 2)
            continue;
        if (!chain.empty() && chain.back() != path.first_point())
            flush();
        if (chain.empty())
            chain = path.polyline.points;
        else
            chain.insert(chain.end(), path.polyline.points.begin() + 1, path.polyline.points.end());
    }
    if (loop && !chain.empty() && chain.front() != chain.back())
        chain.emplace_back(chain.front());
    flush();
    return count;
}

void collect_external_self_intersections(const ExtrusionEntity& entity, size_t& count)
{
    if (const auto* path = dynamic_cast<const ExtrusionPath*>(&entity)) {
        if (is_image_map_outer_wall(path->role()))
            count += polyline_self_intersection_count(path->polyline.points);
    } else if (const auto* multipath = dynamic_cast<const ExtrusionMultiPath*>(&entity)) {
        count += path_chain_self_intersection_count(multipath->paths, false);
    } else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(&entity)) {
        count += path_chain_self_intersection_count(loop->paths, true);
    } else if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        for (const ExtrusionEntity* child : collection->entities)
            if (child != nullptr)
                collect_external_self_intersections(*child, count);
    }
}

} // namespace

TEST_CASE("Persistent image maps generate geometry-first perimeter path modulation V2", "[PrintObject][ImageMap]")
{
    ImageMap::RenderMode                        render_mode;
    ImageMapPerimeterModulationMode             modulation_mode = ImageMapPerimeterModulationMode::ReferenceWidePath;
    bool                                        thin_shell      = false;
    bool                                        textured_corner = false;
    bool                                        adaptive_single_palette = false;
    bool                                        synchronize_whole_object_cadence = false;
    int                                         requested_wall_loops = 2;
    std::vector<std::string>                    physical_colors{"#FF0000", "#00FF00"};
    std::vector<MixedFilamentWeightedComponent> cadence_components{{{1}, 50}, {{2}, 50}};
    SECTION("normal mixed filaments are assigned without changing the envelope") { render_mode = ImageMap::RenderMode::NormalMix; }
    SECTION("V2 modulation regenerates walls from the sampled boundary") { render_mode = ImageMap::RenderMode::PerimeterModulationV2; }
    SECTION("V2 printable path keeps a practical fixed-width carrier")
    {
        render_mode     = ImageMap::RenderMode::PerimeterModulationV2;
        modulation_mode = ImageMapPerimeterModulationMode::PrintablePath;
    }
    SECTION("V2 hybrid shares exposure between printable width and path displacement")
    {
        render_mode     = ImageMap::RenderMode::PerimeterModulationV2;
        modulation_mode = ImageMapPerimeterModulationMode::HybridPathWidth;
    }
    SECTION("V2 image-controlled width uses the maximum carrier only where needed")
    {
        render_mode     = ImageMap::RenderMode::PerimeterModulationV2;
        modulation_mode = ImageMapPerimeterModulationMode::ImageControlledWidth;
    }
    SECTION("V2 modulation keeps discontinuous textured corners free of loops")
    {
        render_mode     = ImageMap::RenderMode::PerimeterModulationV2;
        textured_corner = true;
    }
    SECTION("V2 modulation retains thin-shell walls at every instance scale")
    {
        render_mode = ImageMap::RenderMode::PerimeterModulationV2;
        thin_shell  = true;
    }
    SECTION("adaptive localized cycles use zone-local ordinary-layer recipes")
    {
        render_mode = ImageMap::RenderMode::AdaptiveLocalizedCycles;
    }
    SECTION("adaptive localized cycles honor the hybrid carrier selection")
    {
        render_mode     = ImageMap::RenderMode::AdaptiveLocalizedCycles;
        modulation_mode = ImageMapPerimeterModulationMode::HybridPathWidth;
    }
    SECTION("adaptive localized cycles derive local recipes for legacy single-palette maps")
    {
        render_mode             = ImageMap::RenderMode::AdaptiveLocalizedCycles;
        adaptive_single_palette = true;
    }
    SECTION("V2 modulation supports a selected subset of the loaded physical filaments")
    {
        render_mode        = ImageMap::RenderMode::PerimeterModulationV2;
        physical_colors    = {"#FF0000", "#FFFFFF", "#00FF00", "#0000FF"};
        cadence_components = {{{1}, 50}, {{3}, 50}};
    }
    SECTION("V2 modulation can synchronize every object extrusion to its image cadence")
    {
        render_mode                        = ImageMap::RenderMode::PerimeterModulationV2;
        synchronize_whole_object_cadence = true;
        requested_wall_loops              = 4;
    }

    if (render_mode == ImageMap::RenderMode::AdaptiveLocalizedCycles)
        physical_colors = {"#FF0000", "#00FF00", "#0000FF", "#FFFFFF"};

    const bool exercise_instance_scales = render_mode == ImageMap::RenderMode::PerimeterModulationV2 &&
                                          modulation_mode == ImageMapPerimeterModulationMode::ReferenceWidePath &&
                                          physical_colors.size() == 2 && !textured_corner;
    for (const double layer_height : {0.08, 0.2}) {
        CAPTURE(layer_height);
        const std::vector<double> instance_scales = exercise_instance_scales && layer_height == 0.2 ?
                                                        std::vector<double>{1.0, 1.5, 2.0, 3.0} :
                                                        std::vector<double>{1.0};
        for (const double instance_scale : instance_scales) {
            CAPTURE(instance_scale);
            CAPTURE(thin_shell, textured_corner);
            Model        model;
            ModelObject* model_object = model.add_object();
            ModelVolume* volume = model_object->add_volume(Test::mesh(thin_shell ? TestMesh::two_hollow_squares : TestMesh::cube_20x20x20));
            const Vec3d  source_size = volume->mesh().bounding_box().size();
            model_object->add_instance()->set_scaling_factor(Vec3d::Constant(instance_scale));
            model_object->ensure_on_bed();

            MixedFilamentDefinition definition;
            definition.identity.stable_id                         = 424242;
            definition.source.kind                                = MixedFilamentSourceKind::Custom;
            definition.recipe.kind                                = MixedFilamentRecipeKind::WeightedBlend;
            definition.recipe.blend.components                    = cadence_components;
            definition.behavior.distribution                      = MixedFilamentDistributionMode::LayerCycle;
            definition.behavior.surface_bias.perimeter_modulation = true;
            set_mixed_filament_component_surface_offsets(definition, {0.2f, 0.2f});
            definition.presentation.display_color = "#808000";
            MixedFilamentManager definitions;
            REQUIRE(definitions.add_custom_filament_definition(definition, physical_colors));
            const unsigned int mixed_filament_id = unsigned(physical_colors.size() + 1);
            unsigned int       secondary_mixed_filament_id = 0;
            if (render_mode == ImageMap::RenderMode::AdaptiveLocalizedCycles && !adaptive_single_palette) {
                MixedFilamentDefinition secondary_definition = definition;
                secondary_definition.identity.stable_id       = 424243;
                secondary_definition.recipe.blend.components  = {{{3}, 75}, {{4}, 25}};
                set_mixed_filament_component_surface_offsets(secondary_definition, {0.2f, 0.2f});
                REQUIRE(definitions.add_custom_filament_definition(secondary_definition, physical_colors));
                secondary_mixed_filament_id = unsigned(physical_colors.size() + 2);
            }

            ImageMap::VolumeData image_map;
            image_map.topology_fingerprint = ImageMap::topology_fingerprint(volume->mesh());
            ImageMap::Zone zone;
            zone.stable_id                    = "image-map-zone";
            zone.render_mode                  = render_mode;
            // Preserve this geometry regression's physical-model expectations.
            zone.color_mix_model              = ImageMap::ColorMixModel::FullSpectrumKmKs;
            zone.synchronize_whole_object_cadence = synchronize_whole_object_cadence;
            zone.modulation_sample_spacing_mm = 0.25f;
            zone.corner_smoothing_radius_mm   = 0.6f;
            zone.palette.push_back({RGBA{1.f, 0.f, 0.f, 1.f}, 424242, mixed_filament_id});
            image_map.zones.push_back(zone);
            RGBA adaptive_face_color{1.f, 0.f, 0.f, 1.f};
            RGBA adaptive_secondary_face_color = adaptive_face_color;
            if (render_mode == ImageMap::RenderMode::AdaptiveLocalizedCycles) {
                const std::optional<std::string> blended =
                    full_spectrum_ks_blend_color_multi(std::vector<std::pair<std::string, int>>{{"#FF0000", 75}, {"#00FF00", 25}});
                REQUIRE(blended);
                ColorRGB blended_rgb;
                REQUIRE(decode_color(*blended, blended_rgb));
                adaptive_face_color = RGBA{blended_rgb.r(), blended_rgb.g(), blended_rgb.b(), 1.f};
                image_map.zones.front().palette.front().target_color = adaptive_face_color;
                const std::optional<std::string> secondary_blended =
                    full_spectrum_ks_blend_color_multi(std::vector<std::pair<std::string, int>>{{"#0000FF", 75}, {"#FFFFFF", 25}});
                REQUIRE(secondary_blended);
                ColorRGB secondary_rgb;
                REQUIRE(decode_color(*secondary_blended, secondary_rgb));
                adaptive_secondary_face_color = RGBA{secondary_rgb.r(), secondary_rgb.g(), secondary_rgb.b(), 1.f};
                if (!adaptive_single_palette)
                    image_map.zones.front().palette.push_back({adaptive_secondary_face_color, 424243, secondary_mixed_filament_id});
            }
            for (size_t triangle_index = 0; triangle_index < volume->mesh().its.indices.size(); ++triangle_index) {
                ImageMap::TriangleBinding binding;
                binding.triangle_index = uint32_t(triangle_index);
                binding.source.kind    = ImageMap::SourceKind::FaceColor;
                const auto& triangle = volume->mesh().its.indices[triangle_index];
                const Vec3d a        = volume->mesh().its.vertices[size_t(triangle[0])].cast<double>();
                const Vec3d b        = volume->mesh().its.vertices[size_t(triangle[1])].cast<double>();
                const Vec3d c        = volume->mesh().its.vertices[size_t(triangle[2])].cast<double>();
                const Vec3d normal   = (b - a).cross(c - a);
                RGBA        face_color = adaptive_face_color;
                // Keep both triangles of a planar cube face in the same
                // recipe. Alternating by triangle would manufacture a
                // diagonal recipe seam that no authored texture contains.
                if (render_mode == ImageMap::RenderMode::AdaptiveLocalizedCycles && std::abs(normal.y()) > std::abs(normal.x()))
                    face_color = adaptive_secondary_face_color;
                if (textured_corner) {
                    if (std::abs(normal.y()) > std::abs(normal.x()))
                        face_color = RGBA{0.f, 1.f, 0.f, 1.f};
                }
                binding.source.corner_colors = {face_color, face_color, face_color};
                image_map.triangle_bindings.push_back(binding);
            }
            REQUIRE(image_map.validate(volume->mesh()).valid);
            REQUIRE(volume->set_image_map_data(std::move(image_map)));

            DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
            config.set_num_extruders(unsigned(physical_colors.size()));
            config.set_num_filaments(unsigned(physical_colors.size()));
            for (const std::string& key : config.keys()) {
                const ConfigOption* option = config.option(key);
                if (option->type() != coEnums)
                    continue;
                ConfigOption* replacement = print_config_def.get(key)->create_default_option();
                replacement->set(option);
                config.set_key_value(key, replacement);
            }
            config.set_key_value("filament_colour", new ConfigOptionStrings(physical_colors));
            config.set_key_value("filament_diameter", new ConfigOptionFloats(std::vector<double>(physical_colors.size(), 1.75)));
            config.set_key_value("nozzle_diameter", new ConfigOptionFloats(std::vector<double>(physical_colors.size(), 0.4)));
            config.set_key_value("min_layer_height", new ConfigOptionFloats(std::vector<double>(physical_colors.size(), 0.0)));
            config.set_key_value("max_layer_height", new ConfigOptionFloats(std::vector<double>(physical_colors.size(), 0.0)));
            config.set_key_value("layer_height", new ConfigOptionFloat(layer_height));
            config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(layer_height));
            config.set_key_value("line_width", new ConfigOptionFloatOrPercent(0.45, false));
            config.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(0.45, false));
            config.set_key_value("inner_wall_line_width", new ConfigOptionFloatOrPercent(0.45, false));
            config.set_key_value("wall_loops", new ConfigOptionInt(requested_wall_loops));
            config.set_key_value("detect_overhang_wall", new ConfigOptionBool(true));
            config.set_key_value("elefant_foot_compensation", new ConfigOptionFloat(0.2));
            config.set_key_value("elefant_foot_compensation_layers", new ConfigOptionInt(1));
            config.set_key_value("mixed_filament_component_bias_enabled", new ConfigOptionBool(true));
            config.set_key_value("dithering_local_z_mode", new ConfigOptionBool(false));
            config.set_key_value("dithering_local_z_direct_multicolor", new ConfigOptionBool(false));
            config.set_key_value("dithering_local_z_independent_layer_height", new ConfigOptionBool(false));
            config.set_key_value("image_map_perimeter_modulation_mode",
                                 new ConfigOptionEnum<ImageMapPerimeterModulationMode>(modulation_mode));
            config.set_key_value("image_map_perimeter_printable_width", new ConfigOptionFloat(0.55));
            config.set_key_value("mixed_filament_definitions", new ConfigOptionString(definitions.serialize_custom_entries()));
            Print print;
            print.auto_assign_extruders(model_object);
            print.apply(model, config);
            print.validate();
            print.set_status_silent();
            REQUIRE(print.objects().size() == 1);
            print.get_object(0)->slice();

            const PrintObject& print_object = *print.objects().front();
            REQUIRE(print_object.layer_count() > 2);
            if (render_mode == ImageMap::RenderMode::AdaptiveLocalizedCycles) {
                CHECK(print_object.local_z_intervals().empty());
                CHECK(print_object.local_z_sublayer_plan().empty());
            } else {
                CHECK(print_object.local_z_intervals().empty());
                CHECK(print_object.local_z_sublayer_plan().empty());
            }
            const Layer* layer = print_object.get_layer(1);
            REQUIRE(layer != nullptr);
            REQUIRE_FALSE(layer->lslices.empty());
            const BoundingBox bounds          = get_extents(layer->lslices);
            const double      slice_margin_mm = thin_shell ? 0.20 : 0.04;
            CHECK(unscale<double>(bounds.size().x()) == Approx(source_size.x() * instance_scale).margin(slice_margin_mm));
            CHECK(unscale<double>(bounds.size().y()) == Approx(source_size.y() * instance_scale).margin(slice_margin_mm));

            const Layer* first_layer = print_object.get_layer(0);
            REQUIRE(first_layer != nullptr);
            REQUIRE_FALSE(first_layer->lslices.empty());
            const BoundingBox first_layer_bounds = get_extents(first_layer->lslices);
            CHECK(unscale<double>(first_layer_bounds.size().x()) == Approx(source_size.x() * instance_scale).margin(slice_margin_mm));
            CHECK(unscale<double>(first_layer_bounds.size().y()) == Approx(source_size.y() * instance_scale).margin(slice_margin_mm));

            ExPolygons region_geometry;
            bool       mapped_to_mixed_filament = false;
            size_t     nonempty_region_count    = 0;
            for (const LayerRegion* region : layer->regions()) {
                append(region_geometry, to_expolygons(region->slices.surfaces));
                nonempty_region_count += region->slices.empty() ? 0u : 1u;
                mapped_to_mixed_filament |= !region->slices.empty() &&
                                            region->region().config().wall_filament.value == int(mixed_filament_id);
            }
            if (render_mode == ImageMap::RenderMode::PerimeterModulationV2 && synchronize_whole_object_cadence) {
                // Whole-object cadence already selects the physical tool for
                // every extrusion on this layer. The image must therefore
                // modulate the authored shell without manufacturing a second
                // closed material region at the texture boundary.
                CHECK_FALSE(mapped_to_mixed_filament);
                CHECK(nonempty_region_count == 1);
            } else {
                CHECK(mapped_to_mixed_filament);
            }
            REQUIRE_FALSE(region_geometry.empty());
            const BoundingBox region_bounds = get_extents(union_ex(std::move(region_geometry)));
            CHECK(unscale<double>(region_bounds.min.x()) == Approx(unscale<double>(bounds.min.x())).margin(0.002));
            CHECK(unscale<double>(region_bounds.min.y()) == Approx(unscale<double>(bounds.min.y())).margin(0.002));
            CHECK(unscale<double>(region_bounds.max.x()) == Approx(unscale<double>(bounds.max.x())).margin(0.002));
            CHECK(unscale<double>(region_bounds.max.y()) == Approx(unscale<double>(bounds.max.y())).margin(0.002));

            print.process();
            if (synchronize_whole_object_cadence) {
                const Layer* wall_count_layer = print_object.get_layer(1);
                REQUIRE(wall_count_layer != nullptr);
                std::set<size_t> inset_indices;
                for (const LayerRegion* region : wall_count_layer->regions())
                    collect_perimeter_inset_indices(region->perimeters, inset_indices);
                CHECK(inset_indices == std::set<size_t>{0, 1, 2, 3});
            }
            if (synchronize_whole_object_cadence && layer_height == 0.2) {
                const boost::filesystem::path gcode_path = boost::filesystem::temp_directory_path() /
                                                           boost::filesystem::unique_path("whole-object-cadence-%%%%-%%%%.gcode");
                GCodeProcessorResult processor_result;
                print.export_gcode(gcode_path.string(), &processor_result, nullptr);
                boost::filesystem::remove(gcode_path);

                std::map<int, std::set<unsigned int>> object_tools_by_layer;
                for (const GCodeProcessorResult::MoveVertex& move : processor_result.moves) {
                    if (move.type != EMoveType::Extrude || move.delta_extruder <= 0.f ||
                        (!is_perimeter(move.extrusion_role) && !is_infill(move.extrusion_role)))
                        continue;
                    const int layer_key = int(std::lround(move.position.z() / float(layer_height)));
                    object_tools_by_layer[layer_key].insert(move.extruder_id);
                }
                size_t verified_layers = 0;
                for (const auto& [layer_key, tools] : object_tools_by_layer) {
                    if (layer_key <= 1 || size_t(layer_key + 1) >= print_object.layer_count())
                        continue;
                    CAPTURE(layer_key, tools);
                    REQUIRE(tools.size() == 1);
                    CHECK(*tools.begin() == unsigned((layer_key - 1) % 2));
                    ++verified_layers;
                }
                CHECK(verified_layers > 10);
            }
            if (modulation_mode == ImageMapPerimeterModulationMode::ImageControlledWidth && layer_height == 0.2) {
                const boost::filesystem::path gcode_path = boost::filesystem::temp_directory_path() /
                                                           boost::filesystem::unique_path("image-controlled-width-%%%%-%%%%.gcode");
                GCodeProcessorResult processor_result;
                print.export_gcode(gcode_path.string(), &processor_result, nullptr);
                boost::filesystem::remove(gcode_path);

                float         minimum_gcode_width = std::numeric_limits<float>::max();
                float         maximum_gcode_width = 0.f;
                float         minimum_extrusion_per_mm = std::numeric_limits<float>::max();
                float         maximum_extrusion_per_mm = 0.f;
                std::set<int>  emitted_extrusion_per_mm;
                std::optional<Vec3f> previous_position;
                for (const GCodeProcessorResult::MoveVertex& move : processor_result.moves) {
                    const Vec3f current_position = move.position;
                    if (move.type != EMoveType::Extrude || move.delta_extruder <= 0.f || move.extrusion_role != erExternalPerimeter ||
                        !previous_position) {
                        previous_position = current_position;
                        continue;
                    }
                    minimum_gcode_width = std::min(minimum_gcode_width, move.width);
                    maximum_gcode_width = std::max(maximum_gcode_width, move.width);
                    const float move_length_mm = (current_position.head<2>() - previous_position->head<2>()).norm();
                    if (move_length_mm > 0.01f) {
                        const float extrusion_per_mm = move.delta_extruder / move_length_mm;
                        minimum_extrusion_per_mm = std::min(minimum_extrusion_per_mm, extrusion_per_mm);
                        maximum_extrusion_per_mm = std::max(maximum_extrusion_per_mm, extrusion_per_mm);
                        emitted_extrusion_per_mm.insert(int(std::lround(1'000'000.f * extrusion_per_mm)));
                    }
                    previous_position = current_position;
                }
                CHECK(minimum_gcode_width < 0.60f);
                CHECK(maximum_gcode_width == Approx(0.95f).margin(0.01f));
                CHECK(maximum_extrusion_per_mm > 1.5f * minimum_extrusion_per_mm);
                CHECK(emitted_extrusion_per_mm.size() >= 2);
            }
            if (render_mode == ImageMap::RenderMode::AdaptiveLocalizedCycles) {
                // The X faces use only red/green while the Y faces use only
                // blue/white. An adaptive ordinary layer must therefore have
                // exactly one owner from each disjoint local recipe. The sum
                // must remain one complete cube perimeter: routing may split
                // the loop at zone boundaries, but must not duplicate it.
                unsigned int previous_first_recipe_component  = 0;
                unsigned int previous_second_recipe_component = 0;
                for (size_t layer_idx = 1; layer_idx + 1 < print_object.layer_count(); ++layer_idx) {
                    const Layer* adaptive_layer = print_object.get_layer(layer_idx);
                    REQUIRE(adaptive_layer != nullptr);
                    std::vector<double> length_by_component(physical_colors.size() + 1, 0.);
                    double              complete_perimeter_length = 0.;
                    double              inner_perimeter_length    = 0.;
                    EndpointCounts      external_endpoint_counts;
                    for (const LayerRegion* region : adaptive_layer->regions()) {
                        const unsigned int configured_filament = unsigned(std::max(0, region->region().config().wall_filament.value));
                        const unsigned int component = print.mixed_filament_manager().resolve(configured_filament,
                                                                                              physical_colors.size(),
                                                                                              int(adaptive_layer->id()),
                                                                                              float(adaptive_layer->print_z),
                                                                                              float(adaptive_layer->height));
                        const bool adaptive_recipe_region =
                            configured_filament == mixed_filament_id ||
                            (secondary_mixed_filament_id != 0 && configured_filament == secondary_mixed_filament_id);
                        if (adaptive_recipe_region) {
                            CHECK(region->image_map_adaptive_perimeter_island);
                            const float expected_carrier_width =
                                modulation_mode == ImageMapPerimeterModulationMode::ReferenceWidePath ? 0.95f : 0.55f;
                            CHECK(region->image_map_external_perimeter_width_mm == Approx(expected_carrier_width).margin(0.01f));
                        }
                        for (const ExtrusionEntity* island : region->perimeters.entities) {
                            // Adaptive ownership is represented by the
                            // containing physical-filament region. Keep paths
                            // nested in printable island collections so a
                            // single entity can never acquire a second owner.
                            REQUIRE(dynamic_cast<const ExtrusionEntityCollection*>(island) != nullptr);
                        }
                        if (adaptive_recipe_region && component < length_by_component.size())
                            length_by_component[component] += texture_external_perimeter_length(region->perimeters);
                        complete_perimeter_length += perimeter_length(region->perimeters);
                        if (adaptive_recipe_region) {
                            inner_perimeter_length += regular_perimeter_length(region->perimeters);
                            collect_external_perimeter_endpoints(region->perimeters, external_endpoint_counts);
                            CHECK(duplicate_inner_perimeter_count(region->perimeters) == 0);
                        }
                    }
                    const size_t active_components = size_t(std::count_if(length_by_component.begin() + 1,
                                                                          length_by_component.end(),
                                                                          [](double length) { return length > 0.01; }));
                    const double total_length = std::accumulate(length_by_component.begin(), length_by_component.end(), 0.);
                    CAPTURE(layer_idx, active_components, total_length, complete_perimeter_length, inner_perimeter_length);
                    REQUIRE(length_by_component.size() == 5);
                    CHECK(active_components == (adaptive_single_palette ? 1 : 2));
                    CHECK((length_by_component[1] > 0.01) != (length_by_component[2] > 0.01));
                    const unsigned int first_recipe_component  = length_by_component[1] > 0.01 ? 1u : 2u;
                    if (previous_first_recipe_component != 0)
                        CHECK(first_recipe_component != previous_first_recipe_component);
                    previous_first_recipe_component  = first_recipe_component;
                    if (!adaptive_single_palette) {
                        CHECK((length_by_component[3] > 0.01) != (length_by_component[4] > 0.01));
                        const unsigned int second_recipe_component = length_by_component[3] > 0.01 ? 3u : 4u;
                        if (previous_second_recipe_component != 0)
                            CHECK(second_recipe_component != previous_second_recipe_component);
                        previous_second_recipe_component = second_recipe_component;
                    }
                    // Each recipe owns a closed perimeter island: its external
                    // loop contains both the visible modulated span and the
                    // inward return. Two disjoint cube-face recipes therefore
                    // have substantially more external length than the old
                    // open-run implementation.
                    CHECK(total_length > 70.);
                    CHECK(total_length < 190.);
                    const size_t unpaired_endpoints = size_t(std::count_if(external_endpoint_counts.begin(),
                                                                           external_endpoint_counts.end(),
                                                                           [](const auto& endpoint) {
                                                                               return endpoint.second % 2 != 0;
                                                                           }));
                    CHECK(unpaired_endpoints == 0);
                    if (layer_idx > 1 && layer_idx + 2 < print_object.layer_count())
                        CHECK(inner_perimeter_length > 50.);
                    CHECK(complete_perimeter_length > 250.);
                }
            }
            if (render_mode == ImageMap::RenderMode::AdaptiveLocalizedCycles && layer_height == 0.2) {
                const boost::filesystem::path gcode_path =
                    boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("adaptive-zone-cadence-%%%%-%%%%.gcode");
                GCodeProcessorResult processor_result;
                print.export_gcode(gcode_path.string(), &processor_result, nullptr);
                std::ifstream gcode_stream(gcode_path.string());
                const std::string gcode((std::istreambuf_iterator<char>(gcode_stream)), std::istreambuf_iterator<char>());
                gcode_stream.close();
                boost::filesystem::remove(gcode_path);

                std::vector<size_t>      move_counts(physical_colors.size(), 0);
                std::map<int, std::set<unsigned int>> external_tools_by_layer;
                std::map<int, std::vector<unsigned int>> external_tool_runs_by_layer;
                float                    maximum_external_width = 0.f;
                for (const GCodeProcessorResult::MoveVertex& move : processor_result.moves) {
                    if (move.type != EMoveType::Extrude || move.delta_extruder <= 0.f || !is_perimeter(move.extrusion_role))
                        continue;
                    if (move.extrusion_role == erExternalPerimeter)
                        maximum_external_width = std::max(maximum_external_width, move.width);
                    if (move.extruder_id >= move_counts.size())
                        continue;
                    ++move_counts[move.extruder_id];
                    const int layer_key = int(std::lround(move.position.z() / float(layer_height)));
                    external_tools_by_layer[layer_key].insert(move.extruder_id);
                    std::vector<unsigned int>& tool_runs = external_tool_runs_by_layer[layer_key];
                    if (tool_runs.empty() || tool_runs.back() != move.extruder_id)
                        tool_runs.emplace_back(move.extruder_id);
                }
                if (modulation_mode == ImageMapPerimeterModulationMode::ReferenceWidePath)
                    CHECK(maximum_external_width == Approx(0.95f).margin(0.01f));
                else
                    CHECK(maximum_external_width < 0.59f);
                for (size_t component_idx = 0; component_idx < move_counts.size(); ++component_idx) {
                    CAPTURE(component_idx, move_counts[component_idx]);
                    if (!adaptive_single_palette || component_idx < 2)
                        CHECK(move_counts[component_idx] > 20);
                }
                size_t verified_zone_local_layers = 0;
                for (const auto& [layer_key, tools] : external_tools_by_layer) {
                    if (layer_key <= 1 || size_t(layer_key + 1) >= print_object.layer_count())
                        continue;
                    CAPTURE(layer_key, tools.size());
                    // The two adaptive islands select one owner each. The
                    // structural core may add its base tool on layers where
                    // the first recipe selected its other component.
                    CHECK(tools.size() >= (adaptive_single_palette ? 1 : 2));
                    CHECK(tools.size() <= (adaptive_single_palette ? 2 : 3));
                    CHECK((tools.count(0) != 0 || tools.count(1) != 0));
                    if (!adaptive_single_palette)
                        CHECK((tools.count(2) == 1) != (tools.count(3) == 1));
                    const auto tool_runs = external_tool_runs_by_layer.find(layer_key);
                    REQUIRE(tool_runs != external_tool_runs_by_layer.end());
                    // Both zone owners are emitted as distinct tool runs.
                    // A preview color transition therefore corresponds to a
                    // real ownership boundary/toolchange, never a color
                    // mutation inside one extrusion path.
                    CHECK(tool_runs->second.size() >= (adaptive_single_palette ? 1 : 2));
                    ++verified_zone_local_layers;
                }
                CHECK(verified_zone_local_layers > 10);
                CHECK(gcode.find("; local-z phase-b path passes begin") == std::string::npos);
                CHECK(gcode.find("; local-z phase-b path passes end") == std::string::npos);
            }
            double             max_surface_distance_mm          = 0.;
            double             min_surface_distance_mm          = std::numeric_limits<double>::infinity();
            double             max_boundary_to_wall_distance_mm = 0.;
            double             max_original_to_wall_distance_mm = 0.;
            double             max_geometry_inset_mm             = 0.;
            double             max_geometry_outset_mm            = 0.;
            double             max_external_loop_join_gap_mm    = 0.;
            double             minimum_external_turn_cosine     = 1.;
            double             maximum_regular_layer_width_span = 0.;
            size_t             external_point_count             = 0;
            size_t             external_loop_join_count         = 0;
            size_t             external_turn_count              = 0;
            size_t             external_self_intersection_count = 0;
            std::vector<float> external_widths;
            std::vector<float> texture_external_widths;
            for (size_t layer_idx = 0; layer_idx < print_object.layer_count(); ++layer_idx) {
                CAPTURE(layer_idx);
                const Layer* perimeter_layer = print_object.get_layer(layer_idx);
                REQUIRE(perimeter_layer != nullptr);
                Points             external_points;
                Lines              external_lines;
                std::vector<float> layer_external_widths;
                size_t             perimeter_entity_count = 0;
                size_t             thin_fill_entity_count = 0;
                for (const LayerRegion* region : perimeter_layer->regions()) {
                    perimeter_entity_count += region->perimeters.entities.size();
                    thin_fill_entity_count += region->thin_fills.entities.size();
                }
                CAPTURE(perimeter_entity_count, thin_fill_entity_count);
                for (const LayerRegion* region : perimeter_layer->regions())
                    collect_external_perimeter_points(region->perimeters, external_points);
                for (const LayerRegion* region : perimeter_layer->regions()) {
                    const unsigned int configured_filament = unsigned(std::max(0, region->region().config().wall_filament.value));
                    const bool adaptive_recipe_region = render_mode != ImageMap::RenderMode::AdaptiveLocalizedCycles ||
                                                        configured_filament == mixed_filament_id ||
                                                        (secondary_mixed_filament_id != 0 &&
                                                         configured_filament == secondary_mixed_filament_id);
                    if (adaptive_recipe_region) {
                        collect_external_perimeter_widths(region->perimeters, layer_external_widths);
                        collect_texture_external_perimeter_widths(region->perimeters, texture_external_widths);
                    }
                }
                for (const LayerRegion* region : perimeter_layer->regions())
                    collect_external_perimeter_lines(region->perimeters, external_lines);
                for (const LayerRegion* region : perimeter_layer->regions())
                    collect_external_loop_join_gaps(region->perimeters, external_loop_join_count, max_external_loop_join_gap_mm);
                for (const LayerRegion* region : perimeter_layer->regions())
                    collect_external_turn_stats(region->perimeters, external_turn_count, minimum_external_turn_cosine);
                for (const LayerRegion* region : perimeter_layer->regions())
                    collect_external_self_intersections(region->perimeters, external_self_intersection_count);
                REQUIRE_FALSE(external_points.empty());
                REQUIRE_FALSE(external_lines.empty());
                external_widths.insert(external_widths.end(), layer_external_widths.begin(), layer_external_widths.end());
                if (!thin_shell && !textured_corner && !layer_external_widths.empty()) {
                    std::vector<float> layer_texture_widths;
                    std::copy_if(layer_external_widths.begin(), layer_external_widths.end(), std::back_inserter(layer_texture_widths),
                                 [](float width) { return width > 0.80f; });
                    if (!layer_texture_widths.empty()) {
                        const auto [minimum_width, maximum_width] = std::minmax_element(layer_texture_widths.begin(),
                                                                                        layer_texture_widths.end());
                        maximum_regular_layer_width_span =
                            std::max(maximum_regular_layer_width_span, double(*maximum_width - *minimum_width));
                    }
                }
                external_point_count += external_points.size();
                for (const Point& point : external_points) {
                    const Point  boundary    = projection_onto(perimeter_layer->lslices, point);
                    const double distance_mm = unscale<double>((boundary - point).cast<double>().norm());
                    max_surface_distance_mm  = std::max(max_surface_distance_mm, distance_mm);
                    min_surface_distance_mm  = std::min(min_surface_distance_mm, distance_mm);
                }
                AABBTreeLines::LinesDistancer<Line> wall_distancer(std::move(external_lines));
                for (const Line& boundary_line : to_lines(perimeter_layer->lslices)) {
                    const Vec2d  delta   = (boundary_line.b - boundary_line.a).cast<double>();
                    const size_t samples = std::max<size_t>(1, size_t(std::ceil(unscale<double>(delta.norm()) / 0.5)));
                    for (size_t sample_idx = 0; sample_idx <= samples; ++sample_idx) {
                        const double t               = double(sample_idx) / double(samples);
                        const Vec2d  sample_position = boundary_line.a.cast<double>() + delta * t;
                        const Point  sample_point(coord_t(std::llround(sample_position.x())), coord_t(std::llround(sample_position.y())));
                        max_boundary_to_wall_distance_mm = std::max(max_boundary_to_wall_distance_mm,
                                                                    unscale<double>(
                                                                        wall_distancer.distance_from_lines<false>(sample_point)));
                    }
                }

                ExPolygons original_geometry;
                for (const LayerRegion* region : perimeter_layer->regions())
                    append(original_geometry, region->image_map_unmodulated_raw_slices);
                if (!original_geometry.empty()) {
                    original_geometry = union_ex(std::move(original_geometry));
                    for (const Point& point : external_points) {
                        const Point original_boundary = projection_onto(original_geometry, point);
                        max_original_to_wall_distance_mm =
                            std::max(max_original_to_wall_distance_mm,
                                     unscale<double>((original_boundary - point).cast<double>().norm()));
                    }
                    for (const Line& boundary_line : to_lines(perimeter_layer->lslices)) {
                        for (const Point& point : {boundary_line.a, boundary_line.b}) {
                            const Point  original_boundary    = projection_onto(original_geometry, point);
                            const double boundary_distance_mm = unscale<double>((original_boundary - point).cast<double>().norm());
                            const bool   outside_original     = std::none_of(original_geometry.begin(), original_geometry.end(),
                                                                             [&point](const ExPolygon& polygon) {
                                                                           return polygon.contains(point, true);
                                                                       });
                            if (outside_original)
                                max_geometry_outset_mm = std::max(max_geometry_outset_mm, boundary_distance_mm);
                            else
                                max_geometry_inset_mm = std::max(max_geometry_inset_mm, boundary_distance_mm);
                        }
                    }
                }
            }
            CHECK(external_point_count > 100);
            if (render_mode == ImageMap::RenderMode::PerimeterModulationV2 || render_mode == ImageMap::RenderMode::AdaptiveLocalizedCycles) {
                REQUIRE_FALSE(external_widths.empty());
                const auto [minimum_width, maximum_width] = std::minmax_element(external_widths.begin(), external_widths.end());
                if (thin_shell) {
                    // Arachne may reduce a bead which cannot physically fit,
                    // but it must retain every thin island at every tested
                    // scale and layer height.
                    CHECK(*minimum_width > 0.35f);
                    CHECK(*maximum_width > 0.44f);
                } else {
                    REQUIRE_FALSE(texture_external_widths.empty());
                    const auto [minimum_external_width, maximum_external_width] = std::minmax_element(texture_external_widths.begin(),
                                                                                                      texture_external_widths.end());
                    if (modulation_mode == ImageMapPerimeterModulationMode::ReferenceWidePath) {
                        // The reference path keeps the configured maximum-width
                        // carrier and expresses texture strength by position.
                        // Adaptive needs the same carrier spacing so its inner
                        // perimeter cannot become the visible singleton when
                        // Local-Z moves the outer wall inward.
                        CHECK(*minimum_external_width > 0.90f);
                        CHECK(*maximum_external_width < 1.00f);
                        CHECK(*maximum_width < 1.00f);
                        CHECK(maximum_regular_layer_width_span < 0.01);
                    } else if (modulation_mode == ImageMapPerimeterModulationMode::PrintablePath) {
                        CHECK(*minimum_external_width == Approx(0.55f).margin(0.01f));
                        CHECK(*maximum_external_width == Approx(0.55f).margin(0.01f));
                    } else if (modulation_mode == ImageMapPerimeterModulationMode::HybridPathWidth) {
                        // The hybrid fixture uses two inverse 75/25 recipes,
                        // so neither face reaches a pure-component width
                        // extreme. It must still exercise a substantial
                        // printable width range while path displacement
                        // supplies the remaining exposure difference.
                        // The ImageMap-style isotropic component field blends
                        // the final few hundredths of a millimetre around a
                        // face/corner neighborhood before compact exposure.
                        CHECK(*minimum_external_width < 0.41f);
                        CHECK(*maximum_external_width > 0.48f);
                        CHECK(*maximum_external_width < 0.59f);
                        CHECK(*maximum_external_width - *minimum_external_width > 0.11f);
                    } else {
                        // Image-controlled width reserves the 0.95 mm maximum
                        // carrier but emits that width only where exposure
                        // demands it. The inverse 75/25 fixture must therefore
                        // produce a much wider range than the hybrid's 0.55 mm
                        // carrier without becoming a fixed 0.95 mm wall.
                        CHECK(*minimum_external_width < 0.60f);
                        CHECK(*maximum_external_width > 0.70f);
                        CHECK(*maximum_external_width == Approx(0.95f).margin(0.01f));
                        CHECK(*maximum_external_width - *minimum_external_width > 0.25f);
                    }
                }
                if (!thin_shell && modulation_mode != ImageMapPerimeterModulationMode::ImageControlledWidth)
                    CHECK(max_geometry_inset_mm > 0.25);
                if (!thin_shell && modulation_mode == ImageMapPerimeterModulationMode::ImageControlledWidth)
                    CHECK(max_geometry_inset_mm < 0.01);
                // V2 keeps a stable authored outer envelope and only recesses
                // weaker cadence layers. Integer polygon rounding may leave a
                // few micrometres, but a visible outward half-range is a
                // centered-modulation regression.
                CHECK(max_geometry_outset_mm < 0.01);
                CHECK(min_surface_distance_mm < 0.65);
                // The 0.95 mm classic bead rounds an authored 90-degree
                // vertex farther from the exact polygon corner than a normal
                // wall. Missing-island regressions are several millimetres;
                // 1.60 mm retains the corner allowance while catching them.
                CHECK(max_boundary_to_wall_distance_mm < 1.60);
                // Adaptive zones retain enough depth for the 0.95 mm carrier,
                // its normal inner wall, and the 0.63 mm maximum recess. The
                // inward return remains below this allowance, including the
                // diagonal distance at a rounded 90-degree corner.
                if (render_mode == ImageMap::RenderMode::AdaptiveLocalizedCycles &&
                    modulation_mode == ImageMapPerimeterModulationMode::ReferenceWidePath)
                    CHECK(max_original_to_wall_distance_mm < 3.80);
                if (!thin_shell && !textured_corner &&
                    !(render_mode == ImageMap::RenderMode::AdaptiveLocalizedCycles &&
                      (modulation_mode == ImageMapPerimeterModulationMode::HybridPathWidth ||
                       modulation_mode == ImageMapPerimeterModulationMode::ReferenceWidePath)))
                    CHECK(external_turn_count > 0);
                // The synthetic thin-shell fixture contains Arachne hairpins
                // in its unmodulated source path. A regular authored corner
                // must never acquire such a reversal from modulation.
                if (!thin_shell && !textured_corner && render_mode != ImageMap::RenderMode::AdaptiveLocalizedCycles)
                    CHECK(minimum_external_turn_cosine > -0.25);
                if (!thin_shell && render_mode == ImageMap::RenderMode::PerimeterModulationV2 &&
                    modulation_mode == ImageMapPerimeterModulationMode::ReferenceWidePath) {
                    // Match ImageMap Path V2: keep the 0.95 mm carrier on
                    // supported stretches, but classify displaced unsupported
                    // stretches as overhang paths so they use printable bridge
                    // flow, speed and cooling. The G-code preview stitches
                    // these role boundaries without adding flat end caps.
                    // A fully supported fixture may have no role boundaries.
                    // Whenever classification does split a loop, every path
                    // must still meet at the exact authored centerline.
                    if (external_loop_join_count > 0)
                        CHECK(max_external_loop_join_gap_mm < 0.002);
                } else if (external_loop_join_count > 0) {
                    CHECK(max_external_loop_join_gap_mm < 0.002);
                }
                if (!thin_shell)
                    CHECK(external_self_intersection_count == 0);
            } else {
                CHECK(max_surface_distance_mm < 1.00);
            }

            CHECK(print.apply(model, config) == PrintBase::APPLY_STATUS_UNCHANGED);

            ImageMap::VolumeData equivalent_image_map = *volume->image_map_data();
            REQUIRE(volume->set_image_map_data(std::move(equivalent_image_map)));
            CHECK(print.apply(model, config) == PrintBase::APPLY_STATUS_UNCHANGED);

            ImageMap::VolumeData changed_image_map                          = *volume->image_map_data();
            changed_image_map.zones.front().palette.front().target_color[0] = 0.5f;
            REQUIRE(volume->set_image_map_data(std::move(changed_image_map)));
            CHECK(print.apply(model, config) == PrintBase::APPLY_STATUS_INVALIDATED);
        }
    }
}

TEST_CASE("Adaptive Local-Z height modulation keeps XY and emits smooth non-planar extrusion",
          "[PrintObject][ImageMap][LocalZ]")
{
    const auto check_capped_allocation = [](size_t component_count, double expected_sibling_height) {
        std::vector<double> weights(component_count, 0.0);
        weights.front() = 1.0;
        const std::vector<double> heights =
            ImageMap::allocate_adaptive_local_z_heights(weights, 0.20 * double(component_count), 0.06);
        REQUIRE(heights.size() == component_count);
        CHECK(heights.front() == Approx(0.32).margin(1e-8));
        CHECK(std::all_of(heights.begin() + 1, heights.end(), [expected_sibling_height](double height) {
            return height == Approx(expected_sibling_height).margin(1e-8);
        }));
        CHECK(std::accumulate(heights.begin(), heights.end(), 0.0) == Approx(0.20 * double(component_count)).margin(1e-8));
    };
    check_capped_allocation(2, 0.08);
    check_capped_allocation(3, 0.14);
    check_capped_allocation(4, 0.16);

    const std::vector<std::string> colors{"#FF0000", "#00FF00", "#0000FF", "#FFFFFF"};
    MixedFilamentDefinition definition;
    definition.identity.stable_id                         = 515151;
    definition.source.kind                                = MixedFilamentSourceKind::Custom;
    definition.recipe.kind                                = MixedFilamentRecipeKind::WeightedBlend;
    definition.recipe.blend.components                    = {{{1}, 70}, {{2}, 10}, {{3}, 10}, {{4}, 10}};
    definition.behavior.distribution                      = MixedFilamentDistributionMode::LayerCycle;
    definition.behavior.surface_bias.perimeter_modulation = true;
    definition.presentation.display_color                 = "#808080";

    MixedFilamentDefinition pair_definition;
    pair_definition.identity.stable_id                         = 616161;
    pair_definition.source.kind                                = MixedFilamentSourceKind::Custom;
    pair_definition.recipe.kind                                = MixedFilamentRecipeKind::WeightedBlend;
    pair_definition.recipe.blend.components                    = {{{1}, 95}, {{2}, 5}};
    pair_definition.behavior.distribution                      = MixedFilamentDistributionMode::LayerCycle;
    pair_definition.behavior.surface_bias.perimeter_modulation = true;
    pair_definition.presentation.display_color                 = "#808000";

    MixedFilamentManager definitions;
    REQUIRE(definitions.add_custom_filament_definition(definition, colors));
    REQUIRE(definitions.add_custom_filament_definition(pair_definition, colors));

    Model        model;
    ModelObject* model_object = model.add_object();
    TriangleMesh quad_mesh = make_cube(8., 20., 2.4);
    quad_mesh.translate(Vec3f(-6.f, 0.f, 0.f));
    ModelVolume* quad_volume = model_object->add_volume(std::move(quad_mesh));
    TriangleMesh pair_mesh = make_cube(8., 20., 2.4);
    pair_mesh.translate(Vec3f(6.f, 0.f, 0.f));
    ModelVolume* pair_volume = model_object->add_volume(std::move(pair_mesh));
    model_object->add_instance();
    model_object->ensure_on_bed();

    auto attach_adaptive_local_z_image_map = [](ModelVolume* volume,
                                                const char*  stable_zone_id,
                                                const RGBA&  target_color,
                                                uint64_t     mixed_stable_id,
                                                unsigned int fallback_filament_id,
                                                bool         horizontal_gradient = false) {
        ImageMap::VolumeData image_map;
        image_map.topology_fingerprint = ImageMap::topology_fingerprint(volume->mesh());
        ImageMap::Zone zone;
        zone.stable_id                = stable_zone_id;
        zone.render_mode              = ImageMap::RenderMode::AdaptiveLocalizedCycles;
        zone.adaptive_modulation_mode = ImageMap::AdaptiveModulationMode::LocalZHeight;
        zone.palette.push_back({target_color, mixed_stable_id, fallback_filament_id});
        image_map.zones.push_back(zone);
        float min_x = std::numeric_limits<float>::max();
        float max_x = std::numeric_limits<float>::lowest();
        for (const Vec3f& vertex : volume->mesh().its.vertices) {
            min_x = std::min(min_x, vertex.x());
            max_x = std::max(max_x, vertex.x());
        }
        for (size_t triangle_index = 0; triangle_index < volume->mesh().its.indices.size(); ++triangle_index) {
            ImageMap::TriangleBinding binding;
            binding.triangle_index       = uint32_t(triangle_index);
            binding.source.kind          = ImageMap::SourceKind::FaceColor;
            binding.source.corner_colors = {target_color, target_color, target_color};
            if (horizontal_gradient) {
                const auto& triangle = volume->mesh().its.indices[triangle_index];
                for (size_t corner_idx = 0; corner_idx < 3; ++corner_idx) {
                    const float local_x = volume->mesh().its.vertices[size_t(triangle[int(corner_idx)])].x();
                    const float progress = max_x > min_x + EPSILON ? (local_x - min_x) / (max_x - min_x) : 0.5f;
                    const float green   = 0.25f + 0.50f * std::clamp(progress, 0.f, 1.f);
                    binding.source.corner_colors[corner_idx] = RGBA{1.f - green, green, 0.f, 1.f};
                }
            }
            image_map.triangle_bindings.push_back(binding);
        }
        return volume->set_image_map_data(std::move(image_map));
    };
    REQUIRE(attach_adaptive_local_z_image_map(
        quad_volume, "adaptive-local-z-quad-zone", RGBA{0.5f, 0.5f, 0.5f, 1.f}, 515151, 5));
    REQUIRE(attach_adaptive_local_z_image_map(
        pair_volume, "adaptive-local-z-pair-zone", RGBA{0.5f, 0.5f, 0.f, 1.f}, 616161, 6, true));

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(4);
    config.set_num_filaments(4);
    for (const std::string &key : config.keys()) {
        const ConfigOption *option = config.option(key);
        if (option->type() != coEnums)
            continue;
        ConfigOption *replacement = print_config_def.get(key)->create_default_option();
        replacement->set(option);
        config.set_key_value(key, replacement);
    }
    config.set_key_value("filament_colour", new ConfigOptionStrings(colors));
    config.set_key_value("filament_diameter", new ConfigOptionFloats(std::vector<double>(4, 1.75)));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats(std::vector<double>(4, 0.4)));
    config.set_key_value("min_layer_height", new ConfigOptionFloats(std::vector<double>(4, 0.0)));
    config.set_key_value("max_layer_height", new ConfigOptionFloats(std::vector<double>(4, 0.0)));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.20));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.20));
    config.set_key_value("line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("inner_wall_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("mixed_filament_height_lower_bound", new ConfigOptionFloat(0.06));
    config.set_key_value("dithering_local_z_mode", new ConfigOptionBool(false));
    config.set_key_value("dithering_local_z_direct_multicolor", new ConfigOptionBool(false));
    config.set_key_value("dithering_local_z_independent_layer_height", new ConfigOptionBool(false));
    config.set_key_value("mixed_filament_component_bias_enabled", new ConfigOptionBool(true));
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(definitions.serialize_custom_entries()));

    Print print;
    print.auto_assign_extruders(model_object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    REQUIRE(print.objects().size() == 1);
    print.get_object(0)->slice();
    const PrintObject &print_object = *print.objects().front();

    const std::vector<SubLayerPlan> &plans = print_object.local_z_sublayer_plan();
    REQUIRE_FALSE(plans.empty());
    std::array<bool, 4> component_seen{false, false, false, false};
    std::map<size_t, std::vector<unsigned int>> adaptive_component_sequences;
    std::map<size_t, std::vector<double>> adaptive_component_heights;
    std::map<std::pair<size_t, size_t>, size_t> adaptive_passes_per_layer_and_zone;
    size_t adaptive_pass_count = 0;
    for (const SubLayerPlan &plan : plans) {
        if (!plan.external_perimeters_only)
            continue;
        ++adaptive_pass_count;
        CHECK(plan.adaptive_nonplanar);
        CHECK(plan.flow_height > EPSILON);
        CHECK(plan.flow_height <= ImageMap::ADAPTIVE_LOCAL_Z_MAX_HEIGHT_MM + EPSILON);
        CHECK(plan.z_hi - plan.z_lo == Approx(0.20).margin(1e-5));
        CHECK(plan.adaptive_cycle_z_hi > plan.adaptive_cycle_z_lo + EPSILON);
        CHECK(plan.adaptive_sample_z > plan.adaptive_cycle_z_lo - EPSILON);
        ++adaptive_passes_per_layer_and_zone[{plan.layer_id, plan.dependency_group}];
        REQUIRE(plan.adaptive_component_index < plan.adaptive_component_ids.size());
        unsigned int pass_component = 0;
        for (size_t component_idx = 0; component_idx < plan.painted_masks_by_extruder.size() && component_idx < component_seen.size();
             ++component_idx) {
            if (!plan.painted_masks_by_extruder[component_idx].empty()) {
                component_seen[component_idx] = true;
                pass_component = unsigned(component_idx + 1);
            }
        }
        if (pass_component != 0) {
            CHECK(plan.adaptive_component_ids[plan.adaptive_component_index] == pass_component);
            adaptive_component_sequences[plan.dependency_group].emplace_back(pass_component);
            adaptive_component_heights[plan.dependency_group].emplace_back(plan.flow_height);
        }
    }
    CHECK(adaptive_pass_count > 4);
    CHECK(adaptive_pass_count == print_object.layers().size() * 2);
    CHECK(std::all_of(adaptive_passes_per_layer_and_zone.begin(),
                      adaptive_passes_per_layer_and_zone.end(),
                      [](const auto &entry) { return entry.second == 1; }));
    CHECK(std::all_of(component_seen.begin(), component_seen.end(), [](bool seen) { return seen; }));
    REQUIRE(adaptive_component_sequences.size() == 2);
    bool pair_sequence_seen = false;
    bool quad_sequence_seen = false;
    for (const auto &[dependency_group, sequence] : adaptive_component_sequences) {
        (void) dependency_group;
        REQUIRE(sequence.size() > 4);
        const bool quad_sequence = std::find(sequence.begin(), sequence.end(), 3u) != sequence.end();
        const size_t component_count = quad_sequence ? 4 : 2;
        REQUIRE(sequence.size() % component_count == 0);
        for (size_t pass_idx = 0; pass_idx < sequence.size(); ++pass_idx) {
            CHECK(sequence[pass_idx] == unsigned(pass_idx % component_count + 1));
            if ((pass_idx + 1) % component_count == 0) {
                const std::vector<double>& heights = adaptive_component_heights[dependency_group];
                const double cycle_height = std::accumulate(heights.begin() + (pass_idx + 1 - component_count),
                                                            heights.begin() + (pass_idx + 1),
                                                            0.0);
                CHECK(cycle_height == Approx(0.20 * double(component_count)).margin(1e-5));
            }
        }
        pair_sequence_seen |= !quad_sequence;
        quad_sequence_seen |= quad_sequence;
    }
    CHECK(pair_sequence_seen);
    CHECK(quad_sequence_seen);

    const PrintStateBase::StateWithWarnings slice_state = print_object.step_state_with_warnings(posSlice);
    CHECK(std::any_of(slice_state.warnings.begin(), slice_state.warnings.end(), [](const PrintStateBase::Warning &warning) {
        return warning.message.find("Adaptive Local-Z height modulation was clipped") != std::string::npos &&
               warning.message.find("0.02 mm or lower") != std::string::npos;
    }));

    print.process();
    for (const Layer *layer : print_object.layers())
        for (const LayerRegion *region : layer->regions())
            CHECK_FALSE(region->image_map_adaptive_perimeter_island);

    const boost::filesystem::path gcode_path =
        boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("adaptive-local-z-%%%%-%%%%.gcode");
    GCodeProcessorResult processor_result;
    print.export_gcode(gcode_path.string(), &processor_result, nullptr);
    std::ifstream gcode_stream(gcode_path.string());
    const std::string gcode((std::istreambuf_iterator<char>(gcode_stream)), std::istreambuf_iterator<char>());
    gcode_stream.close();
    boost::filesystem::remove(gcode_path);

    std::array<bool, 4> gcode_component_seen{false, false, false, false};
    std::array<float, 4> minimum_height_by_component;
    std::array<float, 4> maximum_height_by_component{};
    minimum_height_by_component.fill(std::numeric_limits<float>::max());
    float maximum_external_width = 0.f;
    float minimum_external_height = std::numeric_limits<float>::max();
    bool  external_move_at_local_z = false;
    for (const GCodeProcessorResult::MoveVertex &move : processor_result.moves) {
        const bool is_external_extrusion =
            move.type == EMoveType::Extrude && move.delta_extruder > 0.f && move.extrusion_role == erExternalPerimeter;
        if (is_external_extrusion) {
            maximum_external_width = std::max(maximum_external_width, move.width);
            minimum_external_height = std::min(minimum_external_height, move.height);
            const float nominal_layer_index = move.position.z() / 0.20f;
            external_move_at_local_z |= std::abs(nominal_layer_index - std::round(nominal_layer_index)) > 0.05f;
            if (move.extruder_id < gcode_component_seen.size()) {
                gcode_component_seen[move.extruder_id] = true;
                minimum_height_by_component[move.extruder_id] = std::min(minimum_height_by_component[move.extruder_id], move.height);
                maximum_height_by_component[move.extruder_id] = std::max(maximum_height_by_component[move.extruder_id], move.height);
            }
        }
    }

    auto axis_value = [](const std::string& line, char axis) -> std::optional<float> {
        const size_t axis_pos = line.find(std::string(" ") + axis);
        if (axis_pos == std::string::npos)
            return std::nullopt;
        char* end = nullptr;
        const float value = std::strtof(line.c_str() + axis_pos + 2, &end);
        return end != line.c_str() + axis_pos + 2 ? std::optional<float>(value) : std::nullopt;
    };
    bool in_local_z_phase = false;
    bool xyz_extrusion_in_local_z_phase = false;
    bool actual_z_change_during_extrusion = false;
    float maximum_observed_z_slope = 0.f;
    Vec3f gcode_position = Vec3f::Zero();
    std::istringstream gcode_lines(gcode);
    std::string gcode_line;
    while (std::getline(gcode_lines, gcode_line)) {
        if (gcode_line.find("; local-z phase-b path passes begin") != std::string::npos)
            in_local_z_phase = true;
        if (gcode_line.find("; local-z phase-b path passes end") != std::string::npos)
            in_local_z_phase = false;
        if (!(gcode_line.rfind("G0 ", 0) == 0 || gcode_line.rfind("G1 ", 0) == 0))
            continue;
        const Vec3f before = gcode_position;
        const std::optional<float> x = axis_value(gcode_line, 'X');
        const std::optional<float> y = axis_value(gcode_line, 'Y');
        const std::optional<float> z = axis_value(gcode_line, 'Z');
        const std::optional<float> e = axis_value(gcode_line, 'E');
        if (x) gcode_position.x() = *x;
        if (y) gcode_position.y() = *y;
        if (z) gcode_position.z() = *z;
        if (!in_local_z_phase || gcode_line.rfind("G1 ", 0) != 0 || !x || !y || !z || !e)
            continue;
        xyz_extrusion_in_local_z_phase = true;
        const Vec3f delta = gcode_position - before;
        const float xy_distance = delta.head<2>().norm();
        if (xy_distance > 1e-4f && std::abs(delta.z()) > 1e-5f) {
            actual_z_change_during_extrusion = true;
            maximum_observed_z_slope = std::max(maximum_observed_z_slope, std::abs(delta.z()) / xy_distance);
        }
    }
    CHECK(std::all_of(gcode_component_seen.begin(), gcode_component_seen.end(), [](bool seen) { return seen; }));
    CHECK(external_move_at_local_z);
    CHECK(xyz_extrusion_in_local_z_phase);
    CHECK(actual_z_change_during_extrusion);
    CHECK(maximum_height_by_component[0] - minimum_height_by_component[0] > 0.10f);
    CHECK(maximum_height_by_component[1] - minimum_height_by_component[1] > 0.10f);
    CHECK(std::all_of(maximum_height_by_component.begin(), maximum_height_by_component.end(), [](float height) {
        return height <= float(ImageMap::ADAPTIVE_LOCAL_Z_MAX_HEIGHT_MM) + 1e-5f;
    }));
    // Three-decimal G-code coordinates can add roughly 0.006 to the measured
    // slope on the shortest resampled segments.
    CHECK(maximum_observed_z_slope <= 0.13f);
    CHECK(gcode.find("; local-z phase-b path passes begin") != std::string::npos);
    CHECK(maximum_external_width < 0.60f);
    CHECK(minimum_external_height >= 0.01f - 1e-5f);
    CHECK(minimum_external_height < 0.10f);
}

TEST_CASE("Local-Z simple multicolor mixes subdivide walls and infill with every component", "[PrintObject][MixedFilament][LocalZ]")
{
    MixedFilamentDefinition definition;
    definition.identity.stable_id         = 737373;
    definition.source.kind                = MixedFilamentSourceKind::Custom;
    definition.recipe.kind                = MixedFilamentRecipeKind::WeightedBlend;
    definition.recipe.blend.components    = {{{1}, 50}, {{2}, 25}, {{3}, 25}};
    definition.behavior.distribution      = MixedFilamentDistributionMode::Simple;
    definition.presentation.display_color = "#806040";

    MixedFilamentManager           definitions;
    const std::vector<std::string> colors{"#FF0000", "#0000FF", "#FFFF00"};
    REQUIRE(definitions.add_custom_filament_definition(definition, colors));

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(3);
    config.set_num_filaments(3);
    // G-code config serialization needs the enum key maps supplied by dynamic preset options.
    for (const std::string &key : config.keys()) {
        const ConfigOption *option = config.option(key);
        if (option->type() != coEnums)
            continue;
        ConfigOption *replacement = print_config_def.get(key)->create_default_option();
        replacement->set(option);
        config.set_key_value(key, replacement);
    }
    config.set_key_value("filament_colour", new ConfigOptionStrings(colors));
    config.set_key_value("filament_diameter", new ConfigOptionFloats({1.75, 1.75, 1.75}));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.4, 0.4}));
    config.set_key_value("wall_filament", new ConfigOptionInt(4));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
    config.set_key_value("sparse_infill_density", new ConfigOptionPercent(20.0));
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(definitions.serialize_custom_entries()));
    config.set_key_value("mixed_filament_height_lower_bound", new ConfigOptionFloat(0.04));
    config.set_key_value("dithering_local_z_mode", new ConfigOptionBool(true));
    config.set_key_value("dithering_local_z_whole_objects", new ConfigOptionBool(true));
    config.set_key_value("dithering_local_z_preserve_first_layer", new ConfigOptionBool(false));
    config.set_key_value("dithering_local_z_infill", new ConfigOptionBool(true));
    config.set_key_value("dithering_local_z_direct_multicolor", new ConfigOptionBool(false));
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
    config.set_key_value("gcode_comments", new ConfigOptionBool(true));

    Model model;
    ModelObject *model_object = model.add_object();
    model_object->add_volume(make_cube(20., 20., 4.));
    model_object->add_instance();
    model_object->ensure_on_bed();

    Print print;
    print.auto_assign_extruders(model_object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();
    const boost::filesystem::path gcode_path =
        boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("local-z-%%%%-%%%%-%%%%.gcode");
    GCodeProcessorResult processor_result;
    print.export_gcode(gcode_path.string(), &processor_result, nullptr);
    std::ifstream gcode_stream(gcode_path.string());
    const std::string gcode((std::istreambuf_iterator<char>(gcode_stream)), std::istreambuf_iterator<char>());
    gcode_stream.close();
    boost::filesystem::remove(gcode_path);

    REQUIRE(print.objects().size() == 1);
    const std::vector<SubLayerPlan> &plans = print.objects().front()->local_z_sublayer_plan();
    REQUIRE_FALSE(plans.empty());

    std::array<bool, 3> component_seen{false, false, false};
    for (const SubLayerPlan &plan : plans) {
        CHECK_FALSE(plan.external_perimeters_only);
        for (size_t component_idx = 0;
             component_idx < component_seen.size() && component_idx < plan.painted_masks_by_extruder.size();
             ++component_idx)
            component_seen[component_idx] = component_seen[component_idx] ||
                                             !plan.painted_masks_by_extruder[component_idx].empty();
    }
    CHECK(component_seen[0]);
    CHECK(component_seen[1]);
    CHECK(component_seen[2]);

    CHECK(std::any_of(processor_result.moves.begin(), processor_result.moves.end(), [](const GCodeProcessorResult::MoveVertex& move) {
        return move.type == EMoveType::Extrude && move.extrusion_role == erPerimeter && move.delta_extruder > 0.f && move.height < 0.19f;
    }));

    bool   local_z_infill_seen = false;
    size_t section_begin       = 0;
    while ((section_begin = gcode.find("; local-z phase-b path passes begin", section_begin)) != std::string::npos) {
        const size_t section_end = gcode.find("; local-z phase-b path passes end", section_begin);
        REQUIRE(section_end != std::string::npos);
        if (gcode.find("; infill", section_begin) < section_end) {
            local_z_infill_seen = true;
            break;
        }
        section_begin = section_end + 1;
    }
    CHECK(local_z_infill_seen);
}

TEST_CASE("Gradient Local-Z preserves configured and painted seam placement", "[PrintObject][MixedFilament][LocalZ][Seam]")
{
    bool paint_front = false;
    SECTION("configured rear seam") { paint_front = false; }
    SECTION("painted front seam overrides the configured rear seam")
    {
        paint_front = true;
    }

    MixedFilamentDefinition definition;
    definition.identity.stable_id                  = 737374;
    definition.source.kind                         = MixedFilamentSourceKind::Custom;
    definition.recipe.kind                         = MixedFilamentRecipeKind::WeightedBlend;
    definition.recipe.blend.components             = {{{1}, 50}, {{2}, 50}};
    definition.behavior.distribution               = MixedFilamentDistributionMode::Simple;
    definition.behavior.gradient.enabled           = true;
    definition.behavior.gradient.component_a_start = 0.8f;
    definition.behavior.gradient.component_a_end   = 0.2f;
    definition.presentation.display_color          = "#808000";

    MixedFilamentManager           definitions;
    const std::vector<std::string> colors{"#FF0000", "#00FF00"};
    REQUIRE(definitions.add_custom_filament_definition(definition, colors));

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    for (const std::string& key : config.keys()) {
        const ConfigOption* option = config.option(key);
        if (option->type() != coEnums)
            continue;
        ConfigOption* replacement = print_config_def.get(key)->create_default_option();
        replacement->set(option);
        config.set_key_value(key, replacement);
    }
    config.set_key_value("filament_colour", new ConfigOptionStrings(colors));
    config.set_key_value("filament_diameter", new ConfigOptionFloats({1.75, 1.75}));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.4}));
    config.set_key_value("wall_filament", new ConfigOptionInt(3));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
    config.set_key_value("sparse_infill_density", new ConfigOptionPercent(0.0));
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(definitions.serialize_custom_entries()));
    config.set_key_value("mixed_filament_height_lower_bound", new ConfigOptionFloat(0.04));
    config.set_key_value("dithering_local_z_mode", new ConfigOptionBool(false));
    config.set_key_value("dithering_local_z_whole_objects", new ConfigOptionBool(false));
    config.set_key_value("dithering_local_z_preserve_first_layer", new ConfigOptionBool(false));
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
    config.set_key_value("seam_position", new ConfigOptionEnum<SeamPosition>(spRear));

    Model        model;
    ModelObject* model_object = model.add_object();
    ModelVolume* model_volume = model_object->add_volume(make_cube(20., 20., 2.));
    if (paint_front) {
        TriangleSelector selector(model_volume->mesh());
        // its_make_cube() facets 6 and 7 cover the Y-min vertical face.
        selector.set_facet(6, EnforcerBlockerType::ENFORCER);
        selector.set_facet(7, EnforcerBlockerType::ENFORCER);
        REQUIRE(model_volume->seam_facets.set(selector));
    }
    model_object->add_instance();
    model_object->ensure_on_bed();

    Print print;
    print.auto_assign_extruders(model_object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();

    REQUIRE(print.objects().size() == 1);
    REQUIRE_FALSE(print.objects().front()->local_z_sublayer_plan().empty());

    const boost::filesystem::path gcode_path = boost::filesystem::temp_directory_path() /
                                               boost::filesystem::unique_path("local-z-seam-%%%%-%%%%-%%%%.gcode");
    GCodeProcessorResult processor_result;
    print.export_gcode(gcode_path.string(), &processor_result, nullptr);
    boost::filesystem::remove(gcode_path);

    float min_external_y = std::numeric_limits<float>::max();
    float max_external_y = std::numeric_limits<float>::lowest();
    for (const GCodeProcessorResult::MoveVertex& move : processor_result.moves) {
        if (move.type != EMoveType::Extrude || move.extrusion_role != erExternalPerimeter)
            continue;
        min_external_y = std::min(min_external_y, move.position.y());
        max_external_y = std::max(max_external_y, move.position.y());
    }
    REQUIRE(min_external_y < max_external_y);

    size_t seam_count = 0;
    for (const GCodeProcessorResult::MoveVertex& move : processor_result.moves) {
        if (move.type != EMoveType::Seam)
            continue;
        ++seam_count;
        if (paint_front)
            CHECK(move.position.y() == Approx(min_external_y).margin(0.5));
        else
            CHECK(move.position.y() == Approx(max_external_y).margin(0.5));
    }
    CHECK(seam_count > 0);
}

TEST_CASE("Gradient Local-Z uses its configured nominal height independently of process layers",
          "[PrintObject][MixedFilament][LocalZ][Gradient]")
{
    bool painted_gradient = false;
    SECTION("whole-object gradient assignment") { painted_gradient = false; }
    SECTION("painted gradient assignment") { painted_gradient = true; }

    MixedFilamentDefinition definition;
    definition.identity.stable_id                  = 737375;
    definition.source.kind                         = MixedFilamentSourceKind::Custom;
    definition.recipe.kind                         = MixedFilamentRecipeKind::WeightedBlend;
    definition.recipe.blend.components             = {{{1}, 50}, {{2}, 50}};
    definition.behavior.distribution               = MixedFilamentDistributionMode::Simple;
    definition.behavior.gradient.enabled           = true;
    definition.behavior.gradient.component_a_start = 0.75f;
    definition.behavior.gradient.component_a_end   = 0.25f;
    definition.presentation.display_color          = "#808000";

    MixedFilamentManager           definitions;
    const std::vector<std::string> colors{"#FF0000", "#00FF00"};
    REQUIRE(definitions.add_custom_filament_definition(definition, colors));

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    for (const std::string &key : config.keys()) {
        const ConfigOption *option = config.option(key);
        if (option->type() != coEnums)
            continue;
        ConfigOption *replacement = print_config_def.get(key)->create_default_option();
        replacement->set(option);
        config.set_key_value(key, replacement);
    }
    config.set_key_value("filament_colour", new ConfigOptionStrings(colors));
    config.set_key_value("filament_diameter", new ConfigOptionFloats({1.75, 1.75}));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.4}));
    config.set_key_value("max_layer_height", new ConfigOptionFloats({0.2, 0.2}));
    config.set_key_value("line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("initial_layer_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("inner_wall_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("top_surface_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("wall_filament", new ConfigOptionInt(painted_gradient ? 1 : 3));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.08));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.08));
    config.set_key_value("sparse_infill_density", new ConfigOptionPercent(0.0));
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(definitions.serialize_custom_entries()));
    config.set_key_value("mixed_filament_height_lower_bound", new ConfigOptionFloat(0.06));
    config.set_key_value("mixed_color_layer_height_a", new ConfigOptionFloat(0.03));
    config.set_key_value("mixed_color_layer_height_b", new ConfigOptionFloat(0.03));
    config.set_key_value("dithering_local_z_gradient_layer_height", new ConfigOptionFloat(0.20));
    config.set_key_value("dithering_local_z_mode", new ConfigOptionBool(false));
    config.set_key_value("dithering_local_z_whole_objects", new ConfigOptionBool(false));
    config.set_key_value("dithering_local_z_preserve_first_layer", new ConfigOptionBool(false));
    config.set_key_value("dithering_local_z_direct_multicolor", new ConfigOptionBool(false));
    config.set_key_value("dithering_local_z_independent_layer_height", new ConfigOptionBool(false));
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));

    Model        model;
    ModelObject *model_object = model.add_object();
    ModelVolume *model_volume = model_object->add_volume(make_cube(20., 20., 1.0));
    if (painted_gradient) {
        TriangleSelector selector(model_volume->mesh());
        for (int facet_idx = 0; facet_idx < 12; ++facet_idx)
            selector.set_facet(facet_idx, EnforcerBlockerType::Extruder3);
        REQUIRE(model_volume->mmu_segmentation_facets.set(selector));
    }
    model_object->add_instance();
    model_object->ensure_on_bed();

    Print print;
    print.auto_assign_extruders(model_object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();

    REQUIRE(print.objects().size() == 1);
    const PrintObject &print_object = *print.objects().front();
    REQUIRE(print_object.layer_count() >= 3);
    CHECK(print_object.get_layer(0)->height == Approx(0.08).margin(1e-6));
    CHECK(print_object.get_layer(1)->height == Approx(0.08).margin(1e-6));
    CHECK(print_object.get_layer(2)->height == Approx(0.08).margin(1e-6));

    const std::vector<LocalZInterval> &intervals = print_object.local_z_intervals();
    REQUIRE_FALSE(intervals.empty());
    CHECK(std::any_of(intervals.begin(), intervals.end(), [](const LocalZInterval &interval) {
        return interval.independent_layer_height && !interval.managed_masks.empty();
    }));

    const std::vector<SubLayerPlan> &plans = print_object.local_z_sublayer_plan();
    std::vector<unsigned int> gradient_components;
    std::vector<double>       gradient_heights;
    std::vector<double>       gradient_print_zs;
    for (const SubLayerPlan &plan : plans) {
        if (plan.dependency_group != 1)
            continue;
        for (size_t component_idx = 0; component_idx < plan.painted_masks_by_extruder.size(); ++component_idx) {
            if (plan.painted_masks_by_extruder[component_idx].empty())
                continue;
            gradient_components.push_back(unsigned(component_idx + 1));
            gradient_heights.push_back(plan.flow_height);
            gradient_print_zs.push_back(plan.print_z);
            break;
        }
    }

    REQUIRE(gradient_components.size() >= 2);
    REQUIRE(gradient_heights.size() >= 2);
    REQUIRE(gradient_print_zs.size() >= 2);
    CHECK(gradient_components[0] == 2);
    CHECK(gradient_components[1] == 1);
    CHECK(gradient_heights[0] == Approx(0.06).margin(1e-6));
    CHECK(gradient_heights[1] == Approx(0.14).margin(1e-6));
    CHECK(gradient_print_zs[0] == Approx(0.06).margin(1e-6));
    CHECK(gradient_print_zs[1] == Approx(0.20).margin(1e-6));
    CHECK(*std::max_element(gradient_heights.begin(), gradient_heights.end()) > 0.08 + EPSILON);
}

TEST_CASE("Multi-filament Gradient Local-Z reserves a centered window for the middle filament",
          "[PrintObject][MixedFilament][LocalZ][Gradient]")
{
    MixedFilamentDefinition definition;
    definition.identity.stable_id                  = 737376;
    definition.source.kind                         = MixedFilamentSourceKind::Custom;
    definition.recipe.kind                         = MixedFilamentRecipeKind::WeightedBlend;
    definition.recipe.blend.components             = {{{1}, 34}, {{2}, 33}, {{3}, 33}};
    definition.behavior.distribution               = MixedFilamentDistributionMode::LayerCycle;
    definition.behavior.gradient.enabled           = true;
    definition.behavior.gradient.stop_positions    = {0.0f, 0.25f, 0.50f, 0.75f, 1.0f};
    definition.presentation.display_color          = "#808080";

    MixedFilamentManager           definitions;
    const std::vector<std::string> colors{"#FF0000", "#00FF00", "#0000FF"};
    REQUIRE(definitions.add_custom_filament_definition(definition, colors));

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(3);
    config.set_num_filaments(3);
    for (const std::string &key : config.keys()) {
        const ConfigOption *option = config.option(key);
        if (option->type() != coEnums)
            continue;
        ConfigOption *replacement = print_config_def.get(key)->create_default_option();
        replacement->set(option);
        config.set_key_value(key, replacement);
    }
    config.set_key_value("filament_colour", new ConfigOptionStrings(colors));
    config.set_key_value("filament_diameter", new ConfigOptionFloats({1.75, 1.75, 1.75}));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.4, 0.4}));
    config.set_key_value("max_layer_height", new ConfigOptionFloats({0.2, 0.2, 0.2}));
    config.set_key_value("wall_filament", new ConfigOptionInt(4));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.10));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.10));
    config.set_key_value("sparse_infill_density", new ConfigOptionPercent(0.0));
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(definitions.serialize_custom_entries()));
    config.set_key_value("mixed_filament_height_lower_bound", new ConfigOptionFloat(0.02));
    config.set_key_value("dithering_local_z_gradient_layer_height", new ConfigOptionFloat(0.20));
    config.set_key_value("dithering_local_z_gradient_middle_filament_window", new ConfigOptionPercent(40));
    config.set_key_value("dithering_local_z_mode", new ConfigOptionBool(false));
    config.set_key_value("dithering_local_z_whole_objects", new ConfigOptionBool(false));
    config.set_key_value("dithering_local_z_preserve_first_layer", new ConfigOptionBool(false));
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));

    Model        model;
    ModelObject *model_object = model.add_object();
    model_object->add_volume(make_cube(20., 20., 2.0));
    model_object->add_instance();
    model_object->ensure_on_bed();

    Print print;
    print.auto_assign_extruders(model_object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();

    REQUIRE(print.objects().size() == 1);
    const std::vector<SubLayerPlan> &plans = print.objects().front()->local_z_sublayer_plan();
    REQUIRE_FALSE(plans.empty());

    std::map<int, std::vector<unsigned int>> components_by_cycle;
    for (const SubLayerPlan &plan : plans) {
        if (plan.dependency_group != 1)
            continue;

        unsigned int component_id = 0;
        for (size_t component_idx = 0; component_idx < plan.painted_masks_by_extruder.size(); ++component_idx) {
            if (!plan.painted_masks_by_extruder[component_idx].empty()) {
                component_id = unsigned(component_idx + 1);
                break;
            }
        }
        REQUIRE(component_id != 0);
        const int cycle_idx = int(std::floor((plan.z_lo + 1e-6) / 0.20));
        components_by_cycle[cycle_idx].emplace_back(component_id);
    }

    // The 40% window is [0.30, 0.70]. With ten 0.20 mm cadence cycles,
    // samples 0.35, 0.45, 0.55, and 0.65 must be solid middle filament.
    for (const int cycle_idx : {3, 4, 5, 6}) {
        REQUIRE(components_by_cycle.count(cycle_idx) == 1);
        CHECK(components_by_cycle[cycle_idx] == std::vector<unsigned int>{2});
    }

    REQUIRE(components_by_cycle.count(2) == 1);
    CHECK(std::find(components_by_cycle[2].begin(), components_by_cycle[2].end(), 1u) != components_by_cycle[2].end());
    CHECK(std::find(components_by_cycle[2].begin(), components_by_cycle[2].end(), 2u) != components_by_cycle[2].end());
    REQUIRE(components_by_cycle.count(7) == 1);
    CHECK(std::find(components_by_cycle[7].begin(), components_by_cycle[7].end(), 2u) != components_by_cycle[7].end());
    CHECK(std::find(components_by_cycle[7].begin(), components_by_cycle[7].end(), 3u) != components_by_cycle[7].end());
}

TEST_CASE("Independent direct multicolor Local-Z preserves ratios within printer height limits",
          "[PrintObject][MixedFilament][LocalZ]")
{
    MixedFilamentDefinition definition;
    definition.identity.stable_id         = 747474;
    definition.source.kind                = MixedFilamentSourceKind::Custom;
    definition.recipe.kind                = MixedFilamentRecipeKind::WeightedBlend;
    definition.recipe.blend.components    = {{{1}, 20}, {{2}, 20}, {{3}, 60}};
    definition.behavior.distribution      = MixedFilamentDistributionMode::Simple;
    definition.presentation.display_color = "#808040";

    MixedFilamentDefinition two_component_definition;
    two_component_definition.identity.stable_id         = 747475;
    two_component_definition.source.kind                = MixedFilamentSourceKind::Custom;
    two_component_definition.recipe.kind                = MixedFilamentRecipeKind::WeightedBlend;
    two_component_definition.recipe.blend.components    = {{{1}, 25}, {{3}, 75}};
    two_component_definition.behavior.distribution      = MixedFilamentDistributionMode::Simple;
    two_component_definition.presentation.display_color = "#BF4000";

    MixedFilamentDefinition extreme_ratio_definition;
    extreme_ratio_definition.identity.stable_id         = 747476;
    extreme_ratio_definition.source.kind                = MixedFilamentSourceKind::Custom;
    extreme_ratio_definition.recipe.kind                = MixedFilamentRecipeKind::WeightedBlend;
    extreme_ratio_definition.recipe.blend.components    = {{{1}, 3}, {{3}, 97}};
    extreme_ratio_definition.behavior.distribution      = MixedFilamentDistributionMode::Simple;
    extreme_ratio_definition.presentation.display_color = "#F7E000";

    MixedFilamentManager definitions;
    const std::vector<std::string> colors{"#FF0000", "#0000FF", "#FFFF00"};
    REQUIRE(definitions.add_custom_filament_definition(definition, colors));
    REQUIRE(definitions.add_custom_filament_definition(two_component_definition, colors));
    REQUIRE(definitions.add_custom_filament_definition(extreme_ratio_definition, colors));

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(3);
    config.set_num_filaments(3);
    for (const std::string &key : config.keys()) {
        const ConfigOption *option = config.option(key);
        if (option->type() != coEnums)
            continue;
        ConfigOption *replacement = print_config_def.get(key)->create_default_option();
        replacement->set(option);
        config.set_key_value(key, replacement);
    }
    config.set_key_value("filament_colour", new ConfigOptionStrings(colors));
    config.set_key_value("filament_diameter", new ConfigOptionFloats({1.75, 1.75, 1.75}));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.4, 0.4}));
    config.set_key_value("max_layer_height", new ConfigOptionFloats({0.2, 0.2, 0.2}));
    config.set_key_value("line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("initial_layer_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("inner_wall_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("sparse_infill_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("internal_solid_infill_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("top_surface_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("wall_filament", new ConfigOptionInt(4));
    config.set_key_value("enable_infill_filament_override", new ConfigOptionBool(true));
    config.set_key_value("sparse_infill_filament", new ConfigOptionInt(2));
    config.set_key_value("solid_infill_filament", new ConfigOptionInt(3));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.08));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.08));
    config.set_key_value("sparse_infill_density", new ConfigOptionPercent(20.0));
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(definitions.serialize_custom_entries()));
    config.set_key_value("mixed_filament_height_lower_bound", new ConfigOptionFloat(0.04));
    config.set_key_value("dithering_local_z_mode", new ConfigOptionBool(true));
    config.set_key_value("dithering_local_z_whole_objects", new ConfigOptionBool(true));
    config.set_key_value("dithering_local_z_preserve_first_layer", new ConfigOptionBool(false));
    config.set_key_value("dithering_local_z_infill", new ConfigOptionBool(true));
    config.set_key_value("dithering_local_z_direct_multicolor", new ConfigOptionBool(true));
    config.set_key_value("dithering_local_z_independent_layer_height", new ConfigOptionBool(true));
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(true));
    config.set_key_value("flush_volumes_matrix",
                         new ConfigOptionFloats({0.0, 100.0, 100.0,
                                                 100.0, 0.0, 100.0,
                                                 100.0, 100.0, 0.0}));
    config.set_key_value("flush_volumes_vector", new ConfigOptionFloats({100.0, 100.0, 100.0, 100.0, 100.0, 100.0}));

    Model model;
    ModelObject *model_object = model.add_object();
    ModelVolume *model_volume = model_object->add_volume(make_cube(20., 20., 4.0));
    TriangleSelector selector(model_volume->mesh());
    selector.set_facet(0, EnforcerBlockerType::Extruder2);
    for (int facet_idx = 2; facet_idx <= 7; ++facet_idx)
        selector.set_facet(facet_idx, EnforcerBlockerType::Extruder5);
    selector.set_facet(8, EnforcerBlockerType::Extruder6);
    selector.set_facet(9, EnforcerBlockerType::Extruder6);
    REQUIRE(model_volume->mmu_segmentation_facets.set(selector));
    model_object->add_instance();
    model_object->ensure_on_bed();

    Print print;
    print.is_BBL_printer() = false;
    print.auto_assign_extruders(model_object);
    print.apply(model, config);
    print.validate();
    REQUIRE(print.has_wipe_tower());
    print.set_status_silent();
    print.process();

    REQUIRE(print.objects().size() == 1);
    const PrintObject &print_object = *print.objects().front();
    const std::vector<LocalZInterval> &intervals = print_object.local_z_intervals();
    const std::vector<SubLayerPlan> &plans = print_object.local_z_sublayer_plan();
    REQUIRE(intervals.size() >= 3);
    REQUIRE(plans.size() >= 3);
    CHECK(intervals[0].independent_layer_height);
    CHECK(intervals[1].independent_layer_height);
    CHECK(intervals[2].independent_layer_height);
    CHECK_FALSE(intervals[1].managed_masks.empty());

    std::vector<unsigned int> component_sequence;
    std::vector<double>       height_sequence;
    std::vector<double>       print_z_sequence;
    for (const SubLayerPlan &plan : plans) {
        if (plan.dependency_group != 1)
            continue;
        for (size_t component_idx = 0; component_idx < plan.painted_masks_by_extruder.size(); ++component_idx) {
            if (plan.painted_masks_by_extruder[component_idx].empty())
                continue;
            component_sequence.push_back(unsigned(component_idx + 1));
            height_sequence.push_back(plan.flow_height);
            print_z_sequence.push_back(plan.print_z);
            break;
        }
    }
    REQUIRE(component_sequence.size() >= 3);
    REQUIRE(height_sequence.size() >= 3);
    REQUIRE(print_z_sequence.size() >= 3);
    CHECK(component_sequence[0] == 1);
    CHECK(component_sequence[1] == 2);
    CHECK(component_sequence[2] == 3);
    CHECK(height_sequence[0] == Approx(0.04).margin(1e-6));
    CHECK(height_sequence[1] == Approx(0.04).margin(1e-6));
    CHECK(height_sequence[2] == Approx(0.12).margin(1e-6));
    CHECK(print_z_sequence[0] == Approx(0.04).margin(1e-6));
    CHECK(print_z_sequence[1] == Approx(0.08).margin(1e-6));
    CHECK(print_z_sequence[2] == Approx(0.20).margin(1e-6));

    std::vector<unsigned int> two_component_sequence;
    std::vector<double>       two_component_heights;
    for (const SubLayerPlan &plan : plans) {
        if (plan.dependency_group != 2)
            continue;
        for (size_t component_idx = 0; component_idx < plan.painted_masks_by_extruder.size(); ++component_idx) {
            if (plan.painted_masks_by_extruder[component_idx].empty())
                continue;
            two_component_sequence.push_back(unsigned(component_idx + 1));
            two_component_heights.push_back(plan.flow_height);
            break;
        }
    }
    REQUIRE(two_component_sequence.size() >= 2);
    REQUIRE(two_component_heights.size() >= 2);
    CHECK(two_component_sequence[0] == 1);
    CHECK(two_component_sequence[1] == 3);
    CHECK(two_component_heights[0] == Approx(0.04).margin(1e-6));
    CHECK(two_component_heights[1] == Approx(0.12).margin(1e-6));

    std::vector<unsigned int> extreme_component_sequence;
    std::vector<double>       extreme_height_sequence;
    for (const SubLayerPlan &plan : plans) {
        if (plan.dependency_group != 3)
            continue;
        for (size_t component_idx = 0; component_idx < plan.painted_masks_by_extruder.size(); ++component_idx) {
            if (plan.painted_masks_by_extruder[component_idx].empty())
                continue;
            extreme_component_sequence.push_back(unsigned(component_idx + 1));
            extreme_height_sequence.push_back(plan.flow_height);
            break;
        }
    }
    REQUIRE(extreme_component_sequence.size() >= 9);
    REQUIRE(extreme_height_sequence.size() == extreme_component_sequence.size());
    CHECK(extreme_component_sequence.front() == 1);
    CHECK(extreme_height_sequence.front() == Approx(0.04).margin(1e-6));

    const auto next_small_component =
        std::find(extreme_component_sequence.begin() + 1, extreme_component_sequence.end(), 1);
    REQUIRE(next_small_component != extreme_component_sequence.end());
    const size_t next_small_component_idx =
        size_t(std::distance(extreme_component_sequence.begin(), next_small_component));
    CHECK(next_small_component_idx == 8);

    const double expected_dominant_total_height = 0.04 * 97.0 / 3.0;
    double       dominant_total_height          = 0.0;
    for (size_t pass_idx = 1; pass_idx < next_small_component_idx; ++pass_idx) {
        CHECK(extreme_component_sequence[pass_idx] == 3);
        CHECK(extreme_height_sequence[pass_idx] <= 0.2 + EPSILON);
        CHECK(extreme_height_sequence[pass_idx] ==
              Approx(expected_dominant_total_height / 7.0).margin(1e-6));
        dominant_total_height += extreme_height_sequence[pass_idx];
    }
    CHECK(dominant_total_height == Approx(expected_dominant_total_height).margin(1e-6));

    const boost::filesystem::path gcode_path =
        boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("local-z-independent-%%%%-%%%%.gcode");
    GCodeProcessorResult processor_result;
    print.export_gcode(gcode_path.string(), &processor_result, nullptr);
    std::ifstream gcode_stream(gcode_path.string());
    const std::string gcode((std::istreambuf_iterator<char>(gcode_stream)), std::istreambuf_iterator<char>());
    gcode_stream.close();
    boost::filesystem::remove(gcode_path);
    const WipeTowerData &wipe_tower_data = print.wipe_tower_data();
    REQUIRE_FALSE(wipe_tower_data.tool_changes.empty());
    REQUIRE(wipe_tower_data.local_z_tool_changes.size() >= 2);
    bool planned_single_pass_interval = false;
    for (const std::vector<WipeTower::ToolChangeResult> &layer_toolchanges : wipe_tower_data.local_z_tool_changes) {
        for (const WipeTower::ToolChangeResult &toolchange : layer_toolchanges) {
            for (const LocalZInterval &interval : intervals) {
                if (interval.sublayer_count != 1 || interval.layer_id >= print_object.layers().size())
                    continue;
                if (std::abs(toolchange.print_z - float(print_object.layers()[interval.layer_id]->print_z)) <= float(EPSILON)) {
                    planned_single_pass_interval = true;
                    break;
                }
            }
            if (planned_single_pass_interval)
                break;
        }
        if (planned_single_pass_interval)
            break;
    }
    CHECK(planned_single_pass_interval);
    CHECK(gcode.find("; local-z phase-b path passes begin") != std::string::npos);

    std::array<bool, 3> exact_ratio_height_seen{false, false, false};
    const std::array<double, 3> exact_ratio_heights{0.04, 0.04, 0.12};
    for (const GCodeProcessorResult::MoveVertex &move : processor_result.moves) {
        if (move.type != EMoveType::Extrude || move.extruder_id >= exact_ratio_height_seen.size())
            continue;
        if (std::abs(double(move.height) - exact_ratio_heights[move.extruder_id]) <= 1e-5)
            exact_ratio_height_seen[move.extruder_id] = true;
    }
    CHECK(exact_ratio_height_seen[0]);
    CHECK(exact_ratio_height_seen[1]);
    CHECK(exact_ratio_height_seen[2]);

    size_t section_begin = 0;
    while ((section_begin = gcode.find("; local-z phase-b path passes begin", section_begin)) != std::string::npos) {
        const size_t section_end = gcode.find("; local-z phase-b path passes end", section_begin);
        REQUIRE(section_end != std::string::npos);

        double previous_pass_z = -std::numeric_limits<double>::infinity();
        size_t line_begin = section_begin;
        while (line_begin < section_end) {
            const size_t line_end = std::min(gcode.find('\n', line_begin), section_end);
            const std::string line = gcode.substr(line_begin, line_end - line_begin);
            if (line.find("Local-Z path pass") != std::string::npos) {
                const size_t z_pos = line.find(" Z");
                REQUIRE(z_pos != std::string::npos);
                const double pass_z = std::stod(line.substr(z_pos + 2));
                CHECK(pass_z + EPSILON >= previous_pass_z);
                previous_pass_z = pass_z;
            }
            line_begin = line_end + 1;
        }
        section_begin = section_end + 1;
    }
}

SCENARIO("PrintObject: Perimeter generation", "[PrintObject]") {
    GIVEN("20mm cube and default config") {
        WHEN("make_perimeters() is called")  {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, { { "sparse_infill_density", 0 } });
			const PrintObject &object = *print.objects().front();
			THEN("100 layers exist in the model") {
                REQUIRE(object.layers().size() == 100);
            }
            THEN("Every layer in region 0 has 1 island of perimeters") {
                for (const Layer *layer : object.layers())
                    REQUIRE(layer->regions().front()->perimeters.entities.size() == 1);
            }
			THEN("Every layer in region 0 has 2 paths in its perimeters list.") {
                for (const Layer *layer : object.layers())
                    REQUIRE(layer->regions().front()->perimeters.items_count() == 2);
            }
        }
    }
}

SCENARIO("Print: Skirt generation", "[Print]") {
    GIVEN("20mm cube and default config") {
        WHEN("Skirts is set to 2 loops")  {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
            	{ "skirt_height", 	1 },
        		{ "skirt_distance", 1 },
                { "skirt_loops",   2 }
            });
            THEN("Skirt Extrusion collection has 2 loops in it") {
                REQUIRE(print.skirt().items_count() == 2);
                REQUIRE(print.skirt().flatten().entities.size() == 2);
            }
        }
    }
}

SCENARIO("Print: Changing number of solid surfaces does not cause all surfaces to become internal.", "[Print]") {
    GIVEN("sliced 20mm cube and config with top_solid_surfaces = 2 and bottom_solid_surfaces = 1") {
        Slic3r::DynamicPrintConfig config = Slic3r::Test::default_print_config();
		config.set_deserialize_strict({
			{ "top_shell_layers",		2 },
			{ "bottom_shell_layers",	1 },
			{ "layer_height",			0.25 }, // get a known number of layers
			{ "initial_layer_print_height", 0.25 }
			});
        Slic3r::Print print;
        Slic3r::Model model;
        Slic3r::Test::init_print({TestMesh::cube_20x20x20}, print, model, config);
        // Precondition: Ensure that the model has 2 solid top layers (39, 38)
        // and one solid bottom layer (0).
		auto test_is_solid_infill = [&print](size_t obj_id, size_t layer_id) {
		    const Layer &layer = *(print.objects().at(obj_id)->get_layer((int)layer_id));
		    // iterate over all of the regions in the layer
		    for (const LayerRegion *region : layer.regions()) {
		        // for each region, iterate over the fill surfaces
		        for (const Surface &surface : region->fill_surfaces.surfaces)
		            CHECK(surface.is_solid());
		    }
		};
        print.process();
        test_is_solid_infill(0,  0); // should be solid
        test_is_solid_infill(0, 79); // should be solid
        test_is_solid_infill(0, 78); // should be solid
        WHEN("Model is re-sliced with top_solid_layers == 3") {
			config.set_deserialize_strict("top_shell_layers", "3");
			print.apply(model, config);
            print.process();
            THEN("Print object does not have 0 solid bottom layers.") {
                test_is_solid_infill(0, 0);
            }
            AND_THEN("Print object has 3 top solid layers") {
                test_is_solid_infill(0, 79);
                test_is_solid_infill(0, 78);
                test_is_solid_infill(0, 77);
            }
        }
    }
}

SCENARIO("Print: Brim generation", "[Print]") {
    GIVEN("20mm cube and default config, 1mm first layer width") {
        WHEN("Brim is set to 3mm")  {
	        Slic3r::Print print;
	        Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
	        	{ "initial_layer_line_width", 	1 },
                { "brim_type",                     "outer_only" },
	        	{ "brim_width", 					3 }
	        });
            THEN("Brim Extrusion collection has 2 loops in it") {
                size_t total_items = 0;
                for (const auto& pair : print.get_brimMap()) {
                    total_items += pair.second.items_count();
                }
                REQUIRE(total_items == 2);
            }
        }
        WHEN("Brim is set to 6mm")  {
	        Slic3r::Print print;
	        Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
	        	{ "initial_layer_line_width", 	1 },
                { "brim_type",                     "outer_only" },
	        	{ "brim_width", 					6 }
	        });
            THEN("Brim Extrusion collection has 6 loops in it") {
                size_t total_items = 0;
                for (const auto& pair : print.get_brimMap()) {
                    total_items += pair.second.items_count();
                }
                REQUIRE(total_items == 6);
            }
        }
        WHEN("Brim is set to 6mm, extrusion width 0.5mm")  {
	        Slic3r::Print print;
	        Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
	        	{ "initial_layer_line_width", 	1 },
                { "brim_type",                     "outer_only" },
	        	{ "brim_width", 					6 },
	        	{ "initial_layer_line_width", 	0.5 }
	        });
			print.process();
            THEN("Brim Extrusion collection has 12 loops in it") {
                size_t total_items = 0;
                for (const auto& pair : print.get_brimMap()) {
                    total_items += pair.second.items_count();
                }
                REQUIRE(total_items == 12);
            }
        }
    }
}
