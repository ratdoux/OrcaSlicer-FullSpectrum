#ifndef slic3r_BoundaryValidator_hpp_
#define slic3r_BoundaryValidator_hpp_

#include "Point.hpp"
#include "Polygon.hpp"
#include "BuildVolume.hpp"
#include <vector>
#include <string>

namespace Slic3r {

/**
 * @brief Abstract interface for validating geometric elements against print boundaries
 *
 * This class provides a unified interface for boundary validation across different
 * parts of the slicing pipeline. Implementations can validate points, lines, arcs,
 * and polygons against the build volume.
 */
class BoundaryValidator {
public:
    /**
     * @brief Types of boundary violations that can occur
     */
    enum class ViolationType {
        Unknown = 0,
        TravelMove,              // Travel move exceeds boundaries
        ExtrudeMove,             // Extrude move exceeds boundaries
        SpiralLift,              // Spiral lift arc exceeds boundaries
        LazyLift,                // Lazy lift slope exceeds boundaries
        WipeTower,               // Wipe tower position exceeds boundaries
        Skirt,                   // Skirt exceeds boundaries
        Brim,                    // Brim exceeds boundaries
        Support,                 // Support material exceeds boundaries
        ArcMove,                 // G2/G3 arc move exceeds boundaries
        Count                    // Sentinel value
    };

    /**
     * @brief Direction of boundary violation
     */
    enum class BoundaryDirection {
        Unknown = 0,
        X_Min,                   // Beyond X minimum boundary
        X_Max,                   // Beyond X maximum boundary
        Y_Min,                   // Beyond Y minimum boundary
        Y_Max,                   // Beyond Y maximum boundary
        Z_Max,                   // Above Z maximum boundary
        Radius,                  // Beyond circular bed radius
        Count                    // Sentinel value
    };

    /**
     * @brief Describes a single boundary violation
     */
    struct BoundaryViolation {
        ViolationType type;         // Type of violation
        BoundaryDirection direction; // Direction of violation
        std::string description;    // Human-readable description
        Vec3d position;             // Position where violation occurs (unscaled)
        double distance_out;        // How far outside boundaries (unscaled)
        double layer_z;             // Z height of the layer (unscaled)
        std::string object_name;    // Name of related object (if applicable)

        BoundaryViolation(ViolationType t, const std::string& desc,
                         const Vec3d& pos, double z, const std::string& obj = "",
                         BoundaryDirection dir = BoundaryDirection::Unknown, double dist = 0.0)
            : type(t), direction(dir), description(desc), position(pos), distance_out(dist), layer_z(z), object_name(obj) {}
    };

    using BoundaryViolations = std::vector<BoundaryViolation>;

    virtual ~BoundaryValidator() = default;

    /**
     * @brief Validate a single point against boundaries
     * @param point Point to validate (unscaled coordinates)
     * @return true if point is within boundaries, false otherwise
     */
    virtual bool validate_point(const Vec3d& point) const = 0;

    /**
     * @brief Validate a line segment against boundaries
     * @param from Start point (unscaled coordinates)
     * @param to End point (unscaled coordinates)
     * @return true if entire line is within boundaries, false otherwise
     */
    virtual bool validate_line(const Vec3d& from, const Vec3d& to) const = 0;

    /**
     * @brief Validate an arc path against boundaries
     * @param center Arc center point (unscaled coordinates)
     * @param radius Arc radius (unscaled)
     * @param start_angle Start angle in radians
     * @param end_angle End angle in radians
     * @param z_height Z height of the arc (unscaled)
     * @return true if entire arc is within boundaries, false otherwise
     */
    virtual bool validate_arc(const Vec3d& center, double radius,
                             double start_angle, double end_angle,
                             double z_height) const = 0;

    /**
     * @brief Validate a polygon against boundaries
     * @param poly Polygon to validate (scaled coordinates)
     * @param z_height Z height of the polygon (unscaled)
     * @return true if entire polygon is within boundaries, false otherwise
     */
    virtual bool validate_polygon(const Polygon& poly, double z_height = 0.0) const = 0;

    /**
     * @brief Get human-readable name for violation type
     */
    static std::string violation_type_name(ViolationType type);
};

/**
 * @brief Concrete implementation of BoundaryValidator based on BuildVolume
 *
 * This validator uses the BuildVolume class to perform boundary checks.
 * It supports all build volume types (Rectangle, Circle, Convex, Custom).
 */
class BuildVolumeBoundaryValidator : public BoundaryValidator {
public:
    /**
     * @brief Construct validator from BuildVolume
     * @param build_volume Reference to the build volume
     * @param epsilon Tolerance for boundary checks (default: BedEpsilon)
     */
    explicit BuildVolumeBoundaryValidator(const BuildVolume& build_volume,
                                         double epsilon = BuildVolume::BedEpsilon);

    bool validate_point(const Vec3d& point) const override;
    bool validate_line(const Vec3d& from, const Vec3d& to) const override;
    bool validate_arc(const Vec3d& center, double radius,
                     double start_angle, double end_angle,
                     double z_height) const override;
    bool validate_polygon(const Polygon& poly, double z_height = 0.0) const override;

private:
    const BuildVolume& m_build_volume;
    double m_epsilon;

    /**
     * @brief Sample points along an arc for validation
     * @param center Arc center (unscaled)
     * @param radius Arc radius (unscaled)
     * @param start_angle Start angle in radians
     * @param end_angle End angle in radians
     * @param z_height Z height (unscaled)
     * @param num_samples Number of sample points (default: 16)
     * @return Vector of sampled points
     */
    std::vector<Vec3d> sample_arc_points(const Vec3d& center, double radius,
                                         double start_angle, double end_angle,
                                         double z_height,
                                         int num_samples = 16) const;

    /**
     * @brief Check if a 2D point is inside the build volume
     * @param point 2D point (unscaled)
     * @return true if inside, false otherwise
     */
    bool is_inside_2d(const Vec2d& point) const;
};

} // namespace Slic3r

#endif // slic3r_BoundaryValidator_hpp_
