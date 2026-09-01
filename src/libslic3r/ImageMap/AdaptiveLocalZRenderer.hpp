#ifndef slic3r_ImageMap_AdaptiveLocalZRenderer_hpp_
#define slic3r_ImageMap_AdaptiveLocalZRenderer_hpp_

#include "../ExtrusionEntity.hpp"

#include <memory>
#include <vector>

namespace Slic3r {

class Layer;
class PrintObject;

namespace ImageMap {

inline constexpr double ADAPTIVE_LOCAL_Z_MAX_HEIGHT_MM = 0.32;

// Projects a requested component ratio onto a fixed-height cadence while
// enforcing the Local-Z minimum and hard maximum for every component.
std::vector<double> allocate_adaptive_local_z_heights(const std::vector<double>& weights,
                                                      double                     total_height,
                                                      double                     configured_minimum);

// G-code-time renderer for adaptive Local-Z. It never moves XY off the
// generated external perimeter; it only resamples that centerline and assigns
// a cumulative physical Z plus a local bead height to each short segment.
class AdaptiveLocalZRenderer
{
public:
    static std::unique_ptr<AdaptiveLocalZRenderer> create(const PrintObject& print_object);

    ~AdaptiveLocalZRenderer();
    AdaptiveLocalZRenderer(AdaptiveLocalZRenderer&&) noexcept;
    AdaptiveLocalZRenderer& operator=(AdaptiveLocalZRenderer&&) noexcept;
    AdaptiveLocalZRenderer(const AdaptiveLocalZRenderer&)            = delete;
    AdaptiveLocalZRenderer& operator=(const AdaptiveLocalZRenderer&) = delete;

    std::vector<ExtrusionPathSloped> modulate_path(const ExtrusionPath&             path,
                                                   const Layer&                     layer,
                                                   const ExPolygons&                layer_slices,
                                                   const std::vector<unsigned int>& component_ids,
                                                   const std::vector<int>&          fallback_component_weights,
                                                   size_t                           component_index,
                                                   double                           nominal_z_lo,
                                                   double                           nominal_z_hi,
                                                   double                           sample_z,
                                                   double                           minimum_height) const;

private:
    struct Impl;
    explicit AdaptiveLocalZRenderer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace ImageMap
} // namespace Slic3r

#endif
