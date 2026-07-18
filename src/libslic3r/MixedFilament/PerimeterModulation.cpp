#include "PerimeterModulation.hpp"

#include "../libslic3r.h"

#include <algorithm>
#include <cmath>

namespace Slic3r {

namespace {

bool layer_slices_contain(const ExPolygons& layer_slices, const Point& point)
{
    return std::any_of(layer_slices.begin(), layer_slices.end(), [&point](const ExPolygon& slice) { return slice.contains(point, false); });
}

Vec2d fallback_outward_direction(const Points& points, size_t point_index, const ExPolygons& layer_slices, double probe_scaled)
{
    if (points.size() < 2)
        return Vec2d::Zero();

    size_t previous = point_index;
    while (previous > 0 && points[previous - 1] == points[point_index])
        --previous;
    size_t next = point_index;
    while (next + 1 < points.size() && points[next + 1] == points[point_index])
        ++next;

    const Point& before         = previous > 0 ? points[previous - 1] : points[point_index];
    const Point& after          = next + 1 < points.size() ? points[next + 1] : points[point_index];
    Vec2d        tangent        = (after - before).cast<double>();
    const double tangent_length = tangent.norm();
    if (!std::isfinite(tangent_length) || tangent_length <= EPSILON)
        return Vec2d::Zero();
    tangent /= tangent_length;

    const Vec2d normal(-tangent.y(), tangent.x());
    auto        probe = [&points, point_index, probe_scaled](const Vec2d& direction) {
        const Vec2d shifted = points[point_index].cast<double>() + direction * probe_scaled;
        return Point(coord_t(std::llround(shifted.x())), coord_t(std::llround(shifted.y())));
    };
    const bool positive_inside = layer_slices_contain(layer_slices, probe(normal));
    const bool negative_inside = layer_slices_contain(layer_slices, probe(-normal));
    if (positive_inside != negative_inside)
        return positive_inside ? -normal : normal;
    return normal;
}

} // namespace

bool apply_mixed_filament_perimeter_modulation(
    Polyline& polyline, const ExPolygons& layer_slices, float surface_inset_mm, float max_abs_shift_mm, float max_boundary_distance_mm)
{
    if (polyline.points.size() < 2 || layer_slices.empty() || !std::isfinite(surface_inset_mm) || !std::isfinite(max_abs_shift_mm) ||
        max_abs_shift_mm <= 0.f || !std::isfinite(max_boundary_distance_mm) || max_boundary_distance_mm <= 0.f)
        return false;

    const double clamped_inset_mm     = std::clamp(double(surface_inset_mm), -double(max_abs_shift_mm), double(max_abs_shift_mm));
    const double outward_shift_scaled = scale_(-clamped_inset_mm);
    if (std::abs(outward_shift_scaled) <= EPSILON)
        return false;

    const double probe_scaled                 = scale_(std::max(0.05, std::abs(clamped_inset_mm) + 0.05));
    const double max_boundary_distance_scaled = scale_(max_boundary_distance_mm);
    Points       shifted_points;
    shifted_points.reserve(polyline.points.size());
    bool changed = false;
    for (size_t point_index = 0; point_index < polyline.points.size(); ++point_index) {
        const Point& source   = polyline.points[point_index];
        const Point  boundary = projection_onto(layer_slices, source);
        Vec2d        outward  = (boundary - source).cast<double>();
        const double distance = outward.norm();
        if (!std::isfinite(distance) || distance > max_boundary_distance_scaled) {
            shifted_points.emplace_back(source);
            continue;
        }
        if (distance <= EPSILON)
            outward = fallback_outward_direction(polyline.points, point_index, layer_slices, probe_scaled);
        else
            outward /= distance;

        if (!outward.allFinite() || outward.squaredNorm() <= EPSILON * EPSILON) {
            shifted_points.emplace_back(source);
            continue;
        }

        const Vec2d shifted = source.cast<double>() + outward * outward_shift_scaled;
        if (!shifted.allFinite()) {
            shifted_points.emplace_back(source);
            continue;
        }
        const Point quantized(coord_t(std::llround(shifted.x())), coord_t(std::llround(shifted.y())));
        shifted_points.emplace_back(quantized);
        changed |= quantized != source;
    }

    if (changed)
        polyline.points = std::move(shifted_points);
    return changed;
}

} // namespace Slic3r
