#ifndef slic3r_LocalZMixBridge_hpp_
#define slic3r_LocalZMixBridge_hpp_

#include "ExPolygon.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include <memory>

#include <vector>

namespace Slic3r {

class Layer;
struct LocalZMixBridge;
namespace FillAdaptive {
struct Octree;
}
namespace FillLightning {
class Generator;
}

// Generated from one physical-material region, not clipped from another
// material's perimeter network. Shared by export and prime-tower prediction.
struct LocalZMixBridgePaths
{
    size_t                                             region_id = 0;
    ExtrusionEntityCollection                          perimeters;
    ExPolygons                                         fill_masks;
    ExtrusionEntityCollection                          fills;
    ExPolygons                                         fill_no_overlap;
    static std::shared_ptr<const LocalZMixBridgePaths> with_fills(const LocalZMixBridgePaths& paths,
                                                                  Layer&                      layer,
                                                                  const LocalZMixBridge&      bridge,
                                                                  unsigned                    filament_id,
                                                                  FillAdaptive::Octree*       adaptive,
                                                                  FillAdaptive::Octree*       support,
                                                                  FillLightning::Generator*   lightning);
    static std::shared_ptr<const LocalZMixBridgePaths> build(Layer& layer, const ExPolygons& masks, unsigned filament_id);
};

// Shared spatial interfaces, independent of path seams and travel direction.
// Equal component sets share the whole stack. Different sets share a common
// bottom pass and finish with separate, supported residual stacks.
struct LocalZMixBridge
{
    struct Zone
    {
        ExPolygons                masks;
        std::vector<double>       heights;
        std::vector<unsigned int> component_ids;
        unsigned int              source_filament_id = 0;
        // Pure zones retain nominal passes, optionally sharing the final one.
        bool preserve_nominal_layers = false;
        bool join_terminal_pass      = false;
    };
    struct Pass
    {
        std::vector<size_t> zones;
        size_t              component   = 0;
        unsigned int        filament_id = 0;
        double              ceiling     = 0.;
        double              flat_height = 0.;
        int                 tongue      = -1;
        ExPolygons          masks;
    };

    double z_lo          = 0.;
    double z_hi          = 0.;
    double maximum_slope = 0.12;
    bool   shared_stack  = true;
    // Same-color merging tips may taper below the normal bead minimum.
    double                                 tongue_tip_height = .01;
    std::vector<double>                    tongue_planes;
    std::vector<ExPolygons>                tongue_masks;
    std::vector<Zone>                      zones;
    std::vector<Pass>                      passes;
    std::vector<unsigned int>              source_filament_ids;
    std::vector<std::pair<double, double>> anchor_limits;

    static bool                         enabled_from_environment();
    static std::vector<LocalZMixBridge> build(double                     z_lo,
                                              double                     z_hi,
                                              double                     minimum_height,
                                              const std::vector<double>& maximum_heights,
                                              std::vector<Zone>          zones,
                                              const std::vector<double>& nominal_heights = {});
    ExPolygons                          masks(const Pass& pass) const;
    double                              pure_distance(const Point& point) const;
    double                              terminal_interface(const Point& point, size_t tongue_count) const;
    double                              schedule_z(const Pass& pass) const;
    double                              interface_z(const Point& point, size_t component = 0) const;
    std::pair<double, double>           surfaces(const Point& point, size_t pass) const;
    std::vector<ExtrusionPathSloped>    modulate(const ExtrusionPath& path, size_t component, double z_offset) const;
};

} // namespace Slic3r
#endif
