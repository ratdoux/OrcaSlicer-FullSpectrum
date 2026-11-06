#ifndef slic3r_LocalZBridgePaths_hpp_
#define slic3r_LocalZBridgePaths_hpp_
#include "ExPolygon.hpp"
#include "ExtrusionEntityCollection.hpp"
#include <memory>
namespace Slic3r {
class Layer;
class LayerRegion;
struct SubLayerPlan;
struct LocalZBridgeIsland
{
    std::shared_ptr<ExtrusionEntityCollection> walls;
    ExPolygons                                 infill_area;
    const LayerRegion*                         region{nullptr};
    bool                                       has_extrusions{false};
};
// Prepare bridge geometry independently of machine state and G-code emission.
LocalZBridgeIsland make_local_z_bridge_island(const Layer& layer, ExPolygons island);
// Both export and wipe-tower planning use this geometry-based eligibility.
LocalZBridgeIsland                                      prepare_local_z_bridge_pass(const Layer& layer, const SubLayerPlan& plan);
std::vector<std::unique_ptr<ExtrusionEntityCollection>> split_local_z_collection_into_runs(
    std::unique_ptr<ExtrusionEntityCollection> collection);
} // namespace Slic3r
#endif
