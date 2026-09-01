#include "BoundaryModulation.hpp"

#include "../ClipperUtils.hpp"
#include "../libslic3r.h"

#include <algorithm>
#include <cmath>

namespace Slic3r::ImageMap {
namespace {

struct BoundaryPoint
{
    Point point;
    Vec2d inward{Vec2d::Zero()};
    float inset_mm{0.f};
    float smoothing_radius_mm{0.f};
    float first_smoothing_strength{1.f};
    float second_smoothing_strength{1.f};
};

Vec2d left_normal(const Point& from, const Point& to)
{
    Vec2d        tangent = (to - from).cast<double>();
    const double length  = tangent.norm();
    if (!std::isfinite(length) || length <= EPSILON)
        return Vec2d::Zero();
    tangent /= length;
    return Vec2d(-tangent.y(), tangent.x());
}

double distance_mm(const BoundaryPoint& lhs, const BoundaryPoint& rhs)
{
    return unscale<double>((lhs.point - rhs.point).cast<double>().norm());
}

void smooth_displacements_once(std::vector<BoundaryPoint>& samples, float radius_scale, float minimum_radius, bool first_pass)
{
    if (samples.size() < 3)
        return;

    std::vector<float> smoothed(samples.size(), 0.f);
    for (size_t index = 0; index < samples.size(); ++index) {
        const float strength = std::clamp(first_pass ? samples[index].first_smoothing_strength : samples[index].second_smoothing_strength,
                                          0.f, 4.f);
        if (samples[index].smoothing_radius_mm <= EPSILON || strength <= EPSILON) {
            smoothed[index] = samples[index].inset_mm;
            continue;
        }
        const float radius = std::clamp(std::max(minimum_radius, samples[index].smoothing_radius_mm * radius_scale) * strength, 0.f, 2.f);

        double weighted_sum = samples[index].inset_mm;
        double weight_sum   = 1.0;
        double distance     = 0.0;
        for (size_t step = 1; step < samples.size(); ++step) {
            const size_t previous      = (index + samples.size() - step) % samples.size();
            const size_t next_previous = (previous + 1) % samples.size();
            distance += distance_mm(samples[previous], samples[next_previous]);
            if (distance > radius)
                break;
            const double weight = 1.0 - distance / radius;
            weighted_sum += double(samples[previous].inset_mm) * weight;
            weight_sum += weight;
        }
        distance = 0.0;
        for (size_t step = 1; step < samples.size(); ++step) {
            const size_t next          = (index + step) % samples.size();
            const size_t previous_next = next == 0 ? samples.size() - 1 : next - 1;
            distance += distance_mm(samples[previous_next], samples[next]);
            if (distance > radius)
                break;
            const double weight = 1.0 - distance / radius;
            weighted_sum += double(samples[next].inset_mm) * weight;
            weight_sum += weight;
        }
        // Never raise a low point while smoothing. Raising it spreads a color
        // transition into neighboring texels and is the main source of long,
        // false striations at hard image edges.
        smoothed[index] = weight_sum > EPSILON ? std::min(samples[index].inset_mm, float(weighted_sum / weight_sum)) :
                                                 samples[index].inset_mm;
    }
    for (size_t index = 0; index < samples.size(); ++index)
        samples[index].inset_mm = smoothed[index];
}

void smooth_displacements(std::vector<BoundaryPoint>& samples, float max_slope)
{
    if (samples.size() < 3)
        return;

    smooth_displacements_once(samples, 1.15f, 0.30f, true);

    // Only reduce high peaks. Pulling low points upwards changes the sampled
    // image, while lowering peaks merely makes a transition more printable.
    max_slope = std::max(0.f, max_slope);
    for (int pass = 0; pass < 4; ++pass) {
        for (size_t index = 0; index < samples.size(); ++index) {
            const size_t previous = index == 0 ? samples.size() - 1 : index - 1;
            const float  limit    = float(distance_mm(samples[index], samples[previous])) * max_slope + 0.015f;
            if (samples[index].inset_mm > samples[previous].inset_mm + limit)
                samples[index].inset_mm = samples[previous].inset_mm + limit;
        }
        for (size_t index = samples.size(); index-- > 0;) {
            const size_t next  = (index + 1) % samples.size();
            const float  limit = float(distance_mm(samples[index], samples[next])) * max_slope + 0.015f;
            if (samples[index].inset_mm > samples[next].inset_mm + limit)
                samples[index].inset_mm = samples[next].inset_mm + limit;
        }
    }

    smooth_displacements_once(samples, 0.45f, 0.20f, false);
}

std::vector<BoundaryPoint> sample_polygon(const Polygon&                     polygon,
                                          const BoundaryModulationOptions&   options,
                                          const BoundaryDisplacementSampler& sampler)
{
    std::vector<BoundaryPoint> result;
    // Closing a partially sampled polygon would create a synthetic edge across
    // the model. Fall back as a unit when the original contour alone exceeds
    // the safety budget.
    if (polygon.points.size() < 3 || polygon.points.size() > options.max_samples || !sampler)
        return result;

    double perimeter_mm = 0.0;
    for (size_t index = 0; index < polygon.points.size(); ++index)
        perimeter_mm += unscale<double>((polygon.points[(index + 1) % polygon.points.size()] - polygon.points[index]).cast<double>().norm());
    const double requested_spacing = std::clamp(double(options.sample_spacing_mm), 0.02, 2.0);
    const double bounded_spacing   = std::max(requested_spacing,
                                              perimeter_mm / double(std::max<size_t>(polygon.points.size(), options.max_samples)));
    result.reserve(std::min<size_t>(options.max_samples, size_t(std::ceil(perimeter_mm / bounded_spacing)) + polygon.points.size()));

    for (size_t index = 0; index < polygon.points.size() && result.size() < options.max_samples; ++index) {
        const Point& a             = polygon.points[index];
        const Point& b             = polygon.points[(index + 1) % polygon.points.size()];
        const Vec2d  edge          = (b - a).cast<double>();
        const double length_scaled = edge.norm();
        const double length_mm     = unscale<double>(length_scaled);
        if (!std::isfinite(length_mm) || length_mm <= EPSILON)
            continue;
        const size_t count       = std::max<size_t>(1, size_t(std::ceil(length_mm / bounded_spacing)));
        const Vec2d  edge_inward = left_normal(a, b);
        for (size_t sample_index = 0; sample_index < count && result.size() < options.max_samples; ++sample_index) {
            const double  t            = double(sample_index) / double(count);
            const Vec2d   point_scaled = a.cast<double>() + edge * t;
            BoundaryPoint point;
            point.point = Point(coord_t(std::llround(point_scaled.x())), coord_t(std::llround(point_scaled.y())));
            // Follow the reference V2 construction at vertices as well as on
            // edge interiors. A bisector/miter displacement looks natural for
            // a constant offset, but with independently sampled texture values
            // it can reverse the local point order and split one solid slice
            // into many islands. The outgoing edge normal creates a short,
            // printable corner transition without changing topology.
            point.inward             = edge_inward;
            const Vec2d query_inward = edge_inward;
            const Vec2d point_mm(unscale<double>(point.point.x()), unscale<double>(point.point.y()));
            if (const std::optional<BoundaryDisplacement> sampled = sampler(point_mm, query_inward)) {
                point.inset_mm                  = std::clamp(sampled->inset_mm, 0.f, options.max_abs_displacement_mm);
                point.smoothing_radius_mm       = sampled->smoothing_radius_mm;
                point.first_smoothing_strength  = sampled->first_smoothing_strength;
                point.second_smoothing_strength = sampled->second_smoothing_strength;
            }
            result.emplace_back(point);
        }
    }
    return result;
}

bool material_contains(const ExPolygons& source, const Point& point)
{
    return std::any_of(source.begin(), source.end(), [&point](const ExPolygon& polygon) { return polygon.contains(point, true); });
}

Polygon moved_polygon(std::vector<BoundaryPoint>&      samples,
                      const ExPolygon&                 source,
                      const BoundaryModulationOptions& options,
                      size_t&                          safety_clamped)
{
    smooth_displacements(samples, options.max_slope_mm_per_mm);
    const float             max_inset_mm  = std::max(0.f, options.max_abs_displacement_mm);
    const float             erode_step_mm = std::clamp(max_inset_mm / 10.f, 0.025f, 0.08f);
    const int               level_count   = std::clamp(int(std::ceil(max_inset_mm / std::max(erode_step_mm, 1e-4f))) + 1, 1, 96);
    std::vector<ExPolygons> erode_ladder(static_cast<size_t>(level_count));
    erode_ladder.front().emplace_back(source);
    for (int level = 1; level < level_count; ++level) {
        erode_ladder[size_t(level)] = offset_ex(source, -float(scale_(double(erode_step_mm) * double(level))));
        if (erode_ladder[size_t(level)].empty())
            break;
    }
    Polygon moved;
    moved.points.reserve(samples.size());
    for (const BoundaryPoint& sample : samples) {
        const double direction_length = sample.inward.norm();
        Vec2d        direction        = direction_length > EPSILON ? sample.inward : Vec2d::Zero();
        double       inset_mm         = std::clamp(double(sample.inset_mm), 0.0, double(max_inset_mm));
        if (options.center_displacement_on_boundary)
            inset_mm -= 0.5 * double(max_inset_mm);
        if (direction_length > 1.0)
            inset_mm = std::clamp(inset_mm, -double(max_inset_mm) / direction_length, double(max_inset_mm) / direction_length);
        auto point_at = [&sample, &direction](double value_mm) {
            const Vec2d shifted = sample.point.cast<double>() + direction * scale_(value_mm);
            return Point(coord_t(std::llround(shifted.x())), coord_t(std::llround(shifted.y())));
        };

        auto inset_allowed = [&](double candidate_mm) {
            const Point candidate = point_at(candidate_mm);
            if (!source.contains(candidate, true))
                return false;
            const size_t level = std::min(size_t(std::floor(std::max(0.0, candidate_mm - 0.01) / std::max(double(erode_step_mm), 1e-4))),
                                          erode_ladder.size() - 1);
            return level == 0 || (!erode_ladder[level].empty() && material_contains(erode_ladder[level], candidate));
        };

        // The erosion ladder protects thin necks and concave corners. A point
        // must not only remain within the island, but also within the material
        // that survives an erosion proportional to its requested inset.
        if (inset_mm > EPSILON && !inset_allowed(inset_mm)) {
            double low  = 0.0;
            double high = inset_mm;
            for (int iteration = 0; iteration < 10; ++iteration) {
                const double middle = 0.5 * (low + high);
                if (inset_allowed(middle))
                    low = middle;
                else
                    high = middle;
            }
            inset_mm = low;
            ++safety_clamped;
        }
        moved.points.emplace_back(point_at(inset_mm));
    }
    remove_same_neighbor(moved);
    return moved;
}

} // namespace

BoundaryModulationResult modulate_boundary(const ExPolygons&                  source,
                                           const BoundaryModulationOptions&   options,
                                           const BoundaryDisplacementSampler& sample_displacement)
{
    BoundaryModulationResult result;
    if (source.empty() || !sample_displacement || !std::isfinite(options.max_abs_displacement_mm) ||
        options.max_abs_displacement_mm <= EPSILON || options.max_samples < 3) {
        result.geometry = source;
        return result;
    }

    ExPolygons moved_geometry;
    bool       requested_displacement = false;
    for (const ExPolygon& source_expolygon : source) {
        if (source_expolygon.empty() || source_expolygon.contour.points.size() < 3) {
            moved_geometry.emplace_back(source_expolygon);
            ++result.fallback_polygons;
            continue;
        }

        std::vector<BoundaryPoint> contour = sample_polygon(source_expolygon.contour, options, sample_displacement);
        result.sampled_points += contour.size();
        const float neutral_displacement = options.center_displacement_on_boundary ? 0.5f * options.max_abs_displacement_mm : 0.f;
        requested_displacement |= std::any_of(contour.begin(), contour.end(), [neutral_displacement](const BoundaryPoint& point) {
            return std::abs(point.inset_mm - neutral_displacement) > 0.0005f;
        });
        if (contour.size() < 3) {
            moved_geometry.emplace_back(source_expolygon);
            ++result.fallback_polygons;
            continue;
        }
        ExPolygon moved;
        moved.contour = moved_polygon(contour, source_expolygon, options, result.safety_clamped_points);
        moved.holes.reserve(source_expolygon.holes.size());
        bool valid = moved.contour.points.size() >= 3;
        for (const Polygon& source_hole : source_expolygon.holes) {
            std::vector<BoundaryPoint> hole = sample_polygon(source_hole, options, sample_displacement);
            result.sampled_points += hole.size();
            requested_displacement |= std::any_of(hole.begin(), hole.end(), [neutral_displacement](const BoundaryPoint& point) {
                return std::abs(point.inset_mm - neutral_displacement) > 0.0005f;
            });
            if (hole.size() < 3) {
                valid = false;
                break;
            }
            Polygon moved_hole = moved_polygon(hole, source_expolygon, options, result.safety_clamped_points);
            if (moved_hole.points.size() < 3) {
                valid = false;
                break;
            }
            moved.holes.emplace_back(std::move(moved_hole));
        }
        if (valid) {
            moved.contour.make_counter_clockwise();
            for (Polygon& hole : moved.holes)
                hole.make_clockwise();
            ExPolygons simplified = moved.simplify(scale_(std::clamp(options.simplify_tolerance_mm, 0.001f, 0.05f)));
            if (options.center_displacement_on_boundary) {
                // A centered displacement can never legitimately remove the
                // material farther than half the configured range from every
                // source boundary. Preserve that medial core explicitly. It
                // repairs tiny transition self-intersections and prevents a
                // printable hollow wall from splitting into separate islands,
                // while changing the requested envelope by at most 0.01 mm.
                const double core_inset_mm = std::max(0.0, 0.5 * double(options.max_abs_displacement_mm) - 0.01);
                ExPolygons   core          = offset_ex(source_expolygon, -float(scale_(core_inset_mm)));
                if (!core.empty()) {
                    append(simplified, std::move(core));
                    simplified = union_ex(std::move(simplified));
                }
                if (simplified.size() > 1) {
                    for (const double repair_radius_mm : {0.02, 0.04, 0.08, 0.12}) {
                        ExPolygons repaired = offset_ex(simplified, float(scale_(repair_radius_mm)));
                        repaired            = offset_ex(repaired, -float(scale_(repair_radius_mm)));
                        if (!repaired.empty() && repaired.size() < simplified.size()) {
                            simplified = std::move(repaired);
                            if (simplified.size() == 1)
                                break;
                        }
                    }
                }
            } else {
                simplified = intersection_ex(simplified, ExPolygons{source_expolygon});
                if (simplified.size() > 1) {
                    // Clipper may detach tiny triangles at a convex vertex
                    // when the two incident edge samples use different inward
                    // normals. They are numerical corner artifacts, not
                    // printable islands. Keep the dominant body only when it
                    // accounts for virtually all material; a genuine split
                    // instead falls back to the intact source below.
                    size_t largest_index = 0;
                    double total_area    = 0.;
                    double largest_area  = 0.;
                    for (size_t index = 0; index < simplified.size(); ++index) {
                        const double polygon_area = std::abs(simplified[index].area());
                        total_area += polygon_area;
                        if (polygon_area > largest_area) {
                            largest_area  = polygon_area;
                            largest_index = index;
                        }
                    }
                    if (total_area > EPSILON && largest_area >= 0.98 * total_area) {
                        ExPolygon dominant = std::move(simplified[largest_index]);
                        simplified.clear();
                        simplified.emplace_back(std::move(dominant));
                    }
                }
            }
            const double original_area   = std::abs(source_expolygon.area());
            const double simplified_area = std::abs(area(simplified));
            valid = !simplified.empty() && std::isfinite(simplified_area) && simplified_area > original_area * 0.10 &&
                    (options.center_displacement_on_boundary ||
                     (simplified.size() == 1 && simplified_area <= original_area * 1.002));
            if (valid)
                append(moved_geometry, std::move(simplified));
        }
        if (!valid) {
            moved_geometry.emplace_back(source_expolygon);
            ++result.fallback_polygons;
        }
    }

    if (!moved_geometry.empty())
        moved_geometry = union_ex(std::move(moved_geometry));
    if (moved_geometry.empty()) {
        result.geometry = source;
        ++result.fallback_polygons;
        return result;
    }
    result.changed  = requested_displacement;
    result.geometry = std::move(moved_geometry);
    return result;
}

} // namespace Slic3r::ImageMap
