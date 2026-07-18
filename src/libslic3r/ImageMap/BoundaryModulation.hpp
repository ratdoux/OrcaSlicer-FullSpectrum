#ifndef slic3r_ImageMap_BoundaryModulation_hpp_
#define slic3r_ImageMap_BoundaryModulation_hpp_

#include "../ExPolygon.hpp"

#include <cstddef>
#include <functional>
#include <optional>

namespace Slic3r::ImageMap {

struct BoundaryDisplacement
{
    // Positive values move into the material; negative values move outwards.
    float inset_mm{0.f};
    float smoothing_radius_mm{0.f};
};

struct BoundaryModulationOptions
{
    float  sample_spacing_mm{0.25f};
    float  max_abs_displacement_mm{0.35f};
    float  max_slope_mm_per_mm{0.35f};
    float  simplify_tolerance_mm{0.006f};
    size_t max_samples{250000};
};

struct BoundaryModulationResult
{
    ExPolygons geometry;
    size_t     sampled_points{0};
    size_t     safety_clamped_points{0};
    size_t     fallback_polygons{0};
    bool       changed{false};
};

using BoundaryDisplacementSampler = std::function<std::optional<BoundaryDisplacement>(const Vec2d& point_mm, const Vec2d& inward)>;

// Resample and displace the complete material envelope. This operation knows
// nothing about textures, filaments, layers, or G-code; those concerns are
// supplied by the sampler and remain independently testable.
BoundaryModulationResult modulate_boundary(const ExPolygons&                  source,
                                           const BoundaryModulationOptions&   options,
                                           const BoundaryDisplacementSampler& sample_displacement);

} // namespace Slic3r::ImageMap

#endif
