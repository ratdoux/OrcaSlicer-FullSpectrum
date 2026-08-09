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

Vec2d corner_inward(const Polygon& polygon, size_t index)
{
    const size_t count    = polygon.points.size();
    const Vec2d  incoming = left_normal(polygon.points[(index + count - 1) % count], polygon.points[index]);
    const Vec2d  outgoing = left_normal(polygon.points[index], polygon.points[(index + 1) % count]);
    Vec2d        bisector = incoming + outgoing;
    const double length   = bisector.norm();
    if (!std::isfinite(length) || length <= EPSILON)
        return outgoing.squaredNorm() > EPSILON ? outgoing : incoming;
    bisector /= length;

    // A true miter may grow without bound at acute corners. Preserve some of
    // the offset-line geometry, but cap the vector itself to the requested
    // displacement later so a corner can never turn into a spike.
    const double projection = std::abs(bisector.dot(outgoing));
    const double miter      = std::clamp(1.0 / std::max(0.25, projection), 1.0, 1.5);
    return bisector * miter;
}

double distance_mm(const BoundaryPoint& lhs, const BoundaryPoint& rhs)
{
    return unscale<double>((lhs.point - rhs.point).cast<double>().norm());
}

void smooth_displacements(std::vector<BoundaryPoint>& samples, float max_slope)
{
    if (samples.size() < 3)
        return;

    std::vector<float> smoothed(samples.size(), 0.f);
    for (size_t index = 0; index < samples.size(); ++index) {
        const float radius = std::clamp(samples[index].smoothing_radius_mm, 0.f, 2.f);
        if (radius <= EPSILON) {
            smoothed[index] = samples[index].inset_mm;
            continue;
        }

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
        smoothed[index] = weight_sum > EPSILON ? float(weighted_sum / weight_sum) : samples[index].inset_mm;
    }
    for (size_t index = 0; index < samples.size(); ++index)
        samples[index].inset_mm = smoothed[index];

    // Limit the spatial derivative in both directions. This is what prevents
    // a hard palette boundary from becoming a sawtooth or self-intersection.
    max_slope = std::max(0.f, max_slope);
    for (int pass = 0; pass < 4; ++pass) {
        for (size_t index = 0; index < samples.size(); ++index) {
            const size_t previous   = index == 0 ? samples.size() - 1 : index - 1;
            const float  limit      = float(distance_mm(samples[index], samples[previous])) * max_slope + 0.015f;
            samples[index].inset_mm = std::clamp(samples[index].inset_mm, samples[previous].inset_mm - limit,
                                                 samples[previous].inset_mm + limit);
        }
        for (size_t index = samples.size(); index-- > 0;) {
            const size_t next       = (index + 1) % samples.size();
            const float  limit      = float(distance_mm(samples[index], samples[next])) * max_slope + 0.015f;
            samples[index].inset_mm = std::clamp(samples[index].inset_mm, samples[next].inset_mm - limit, samples[next].inset_mm + limit);
        }
    }
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
    const double requested_spacing = std::clamp(double(options.sample_spacing_mm), 0.03, 2.0);
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
            point.point                = Point(coord_t(std::llround(point_scaled.x())), coord_t(std::llround(point_scaled.y())));
            point.inward               = sample_index == 0 ? corner_inward(polygon, index) : edge_inward;
            const double inward_length = point.inward.norm();
            if (!std::isfinite(inward_length) || inward_length <= EPSILON)
                point.inward = edge_inward;
            const Vec2d query_inward =
                point.inward.squaredNorm() > EPSILON ? Vec2d(point.inward.normalized()) : edge_inward;
            const Vec2d point_mm(unscale<double>(point.point.x()), unscale<double>(point.point.y()));
            if (const std::optional<BoundaryDisplacement> sampled = sampler(point_mm, query_inward)) {
                point.inset_mm = std::clamp(sampled->inset_mm, -options.max_abs_displacement_mm, options.max_abs_displacement_mm);
                point.smoothing_radius_mm = sampled->smoothing_radius_mm;
            }
            result.emplace_back(point);
        }
    }
    return result;
}

bool material_contains(const ExPolygon& source, const Point& point) { return source.contains(point, true); }

Polygon moved_polygon(std::vector<BoundaryPoint>&      samples,
                      const ExPolygon&                 source,
                      const BoundaryModulationOptions& options,
                      size_t&                          safety_clamped)
{
    smooth_displacements(samples, options.max_slope_mm_per_mm);
    Polygon moved;
    moved.points.reserve(samples.size());
    for (const BoundaryPoint& sample : samples) {
        const double direction_length = sample.inward.norm();
        Vec2d        direction        = direction_length > EPSILON ? sample.inward : Vec2d::Zero();
        double       inset_mm         = std::clamp(double(sample.inset_mm), -double(options.max_abs_displacement_mm),
                                                   double(options.max_abs_displacement_mm));
        if (direction_length > 1.0)
            inset_mm = std::clamp(inset_mm, -double(options.max_abs_displacement_mm) / direction_length,
                                  double(options.max_abs_displacement_mm) / direction_length);
        auto point_at = [&sample, &direction](double value_mm) {
            const Vec2d shifted = sample.point.cast<double>() + direction * scale_(value_mm);
            return Point(coord_t(std::llround(shifted.x())), coord_t(std::llround(shifted.y())));
        };

        // An inward displacement must remain inside the same material island.
        // Binary clamping protects narrow necks and concave corners without
        // coupling the operator to a particular wall generator.
        if (inset_mm > EPSILON && !material_contains(source, point_at(inset_mm))) {
            double low  = 0.0;
            double high = inset_mm;
            for (int iteration = 0; iteration < 10; ++iteration) {
                const double middle = 0.5 * (low + high);
                if (material_contains(source, point_at(middle)))
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
        requested_displacement |= std::any_of(contour.begin(), contour.end(),
                                              [](const BoundaryPoint& point) { return std::abs(point.inset_mm) > 0.0005f; });
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
            requested_displacement |= std::any_of(hole.begin(), hole.end(),
                                                  [](const BoundaryPoint& point) { return std::abs(point.inset_mm) > 0.0005f; });
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
            ExPolygons   simplified      = moved.simplify(scale_(std::clamp(options.simplify_tolerance_mm, 0.001f, 0.05f)));
            const double original_area   = std::abs(source_expolygon.area());
            const double simplified_area = std::abs(area(simplified));
            valid                        = !simplified.empty() && std::isfinite(simplified_area) && simplified_area > original_area * 0.05;
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
