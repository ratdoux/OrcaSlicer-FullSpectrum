#ifndef slic3r_ImageMap_BoundaryModulation_hpp_
#define slic3r_ImageMap_BoundaryModulation_hpp_

#include "../ExPolygon.hpp"

#include <cstddef>
#include <functional>
#include <optional>

namespace Slic3r::ImageMap {

struct BoundaryDisplacement
{
    // Positive values move into the material. Negative input is clamped to
    // zero: modulation is intentionally one-sided so it cannot create spikes
    // or expand the authored model envelope.
    float inset_mm{0.f};
    float smoothing_radius_mm{0.f};
    float first_smoothing_strength{1.f};
    float second_smoothing_strength{1.f};
};

struct BoundaryModulationOptions
{
    float  sample_spacing_mm{0.16f};
    float  max_abs_displacement_mm{0.63f};
    float  max_slope_mm_per_mm{0.35f};
    float  simplify_tolerance_mm{0.006f};
    size_t max_samples{250000};
    // Interpret sampler values in [0, max] as a full modulation range centered
    // on the authored boundary. This lets printable shells move both outward
    // and inward by max / 2 instead of consuming max millimetres of wall on
    // both faces. The default retains the one-sided inset primitive.
    bool center_displacement_on_boundary{false};
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
