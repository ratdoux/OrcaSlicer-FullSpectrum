#include "BoundaryValidator.hpp"
#include "Geometry.hpp"
#include "libslic3r.h"
#include <cmath>

namespace Slic3r {

// ============================================================================
// BoundaryValidator static methods
// ============================================================================

std::string BoundaryValidator::violation_type_name(ViolationType type)
{
    switch (type) {
        case ViolationType::Unknown:
            return "Unknown";
        case ViolationType::TravelMove:
            return "Travel Move";
        case ViolationType::ExtrudeMove:
            return "Extrude Move";
        case ViolationType::SpiralLift:
            return "Spiral Lift";
        case ViolationType::LazyLift:
            return "Lazy Lift";
        case ViolationType::WipeTower:
            return "Wipe Tower";
        case ViolationType::Skirt:
            return "Skirt";
        case ViolationType::Brim:
            return "Brim";
        case ViolationType::Support:
            return "Support";
        case ViolationType::ArcMove:
            return "Arc Move";
        default:
            return "Unknown Violation";
    }
}

// ============================================================================
// BuildVolumeBoundaryValidator implementation
// ============================================================================

BuildVolumeBoundaryValidator::BuildVolumeBoundaryValidator(
    const BuildVolume& build_volume,
    double epsilon)
    : m_build_volume(build_volume), m_epsilon(epsilon)
{
}

bool BuildVolumeBoundaryValidator::validate_point(const Vec3d& point) const
{
    // Validate Z height first (if printable_height is set)
    if (m_build_volume.printable_height() > 0.0) {
        if (point.z() > m_build_volume.printable_height() + m_epsilon) {
            return false;
        }
    }

    // Validate XY position based on build volume type
    return is_inside_2d(Vec2d(point.x(), point.y()));
}

bool BuildVolumeBoundaryValidator::validate_line(const Vec3d& from, const Vec3d& to) const
{
    // For line validation, we sample multiple points along the line
    // to ensure the entire segment is within boundaries
    const int num_samples = 10;

    for (int i = 0; i <= num_samples; ++i) {
        double t = static_cast<double>(i) / num_samples;
        Vec3d sample_point = from + t * (to - from);

        if (!validate_point(sample_point)) {
            return false;
        }
    }

    return true;
}

bool BuildVolumeBoundaryValidator::validate_arc(
    const Vec3d& center,
    double radius,
    double start_angle,
    double end_angle,
    double z_height) const
{
    // Sample points along the arc and validate each
    std::vector<Vec3d> arc_points = sample_arc_points(
        center, radius, start_angle, end_angle, z_height
    );

    for (const Vec3d& point : arc_points) {
        if (!validate_point(point)) {
            return false;
        }
    }

    return true;
}

bool BuildVolumeBoundaryValidator::validate_polygon(const Polygon& poly, double z_height) const
{
    // Check if Z height is valid
    if (m_build_volume.printable_height() > 0.0) {
        if (z_height > m_build_volume.printable_height() + m_epsilon) {
            return false;
        }
    }

    // Check all polygon vertices
    for (const Point& pt : poly.points) {
        Vec2d unscaled_pt = unscale(pt);
        if (!is_inside_2d(unscaled_pt)) {
            return false;
        }
    }

    return true;
}

std::vector<Vec3d> BuildVolumeBoundaryValidator::sample_arc_points(
    const Vec3d& center,
    double radius,
    double start_angle,
    double end_angle,
    double z_height,
    int num_samples) const
{
    std::vector<Vec3d> points;
    points.reserve(num_samples);

    // Handle angle wrapping (e.g., from 350° to 10° should go through 360°/0°)
    double angle_range = end_angle - start_angle;

    // Normalize to handle wrapping
    if (angle_range < 0) {
        angle_range += 2 * PI;
    }

    for (int i = 0; i < num_samples; ++i) {
        double t = static_cast<double>(i) / (num_samples - 1);
        double angle = start_angle + t * angle_range;

        double x = center.x() + radius * std::cos(angle);
        double y = center.y() + radius * std::sin(angle);

        points.emplace_back(x, y, z_height);
    }

    return points;
}

bool BuildVolumeBoundaryValidator::is_inside_2d(const Vec2d& point) const
{
    const BuildVolume_Type type = m_build_volume.type();

    switch (type) {
        case BuildVolume_Type::Rectangle:
        {
            // Get the bounding box of the build volume
            const BoundingBoxf& bbox = m_build_volume.bounding_volume2d();
            BoundingBoxf inflated_bbox = bbox;
            inflated_bbox.min -= Vec2d(m_epsilon, m_epsilon);
            inflated_bbox.max += Vec2d(m_epsilon, m_epsilon);

            return inflated_bbox.contains(point);
        }

        case BuildVolume_Type::Circle:
        {
            // Get circle parameters - circle.center is already in scaled coordinates
            const Geometry::Circled& circle = m_build_volume.circle();
            const Vec2d center_unscaled(unscale<double>(circle.center.x()),
                                       unscale<double>(circle.center.y()));
            const double radius = unscale<double>(circle.radius) + m_epsilon;

            // Check distance from center
            double dist_sq = (point - center_unscaled).squaredNorm();
            return dist_sq <= radius * radius;
        }

        case BuildVolume_Type::Convex:
        case BuildVolume_Type::Custom:
        {
            // For convex/custom volumes, use point-in-polygon test
            // Get the convex hull decomposition - this returns pair<top, bottom>
            const auto& decomp = m_build_volume.top_bottom_convex_hull_decomposition_bed();
            const std::vector<Vec2d>& top_hull = decomp.first;

            if (top_hull.empty()) {
                return false;
            }

            // Check if point is inside the top convex hull
            Point scaled_point = scaled<coord_t>(point);

            // Build polygon from Vec2d points
            Polygon hull_poly;
            for (const Vec2d& pt : top_hull) {
                hull_poly.points.push_back(scaled<coord_t>(pt));
            }

            return hull_poly.contains(scaled_point);
        }

        case BuildVolume_Type::Invalid:
        default:
            // If build volume type is invalid, allow everything (fail-safe)
            return true;
    }
}

} // namespace Slic3r
