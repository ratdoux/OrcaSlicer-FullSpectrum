#ifndef slic3r_LocalZMaterialGraph_hpp_
#define slic3r_LocalZMaterialGraph_hpp_

#include "BoundingBox.hpp"
#include "ExPolygon.hpp"
#include "LocalZCadence.hpp"

#include <functional>
#include <vector>

namespace Slic3r {

// Immutable sampled geometry. Nominal slices are samples, not recipe cycles.
class LocalZMaterialGraph
{
public:
    struct Region
    {
        unsigned                              source        = 0;
        size_t                                configuration = 0;
        std::vector<LocalZCadence::Component> recipe;
        ExPolygons                            masks;
    };
    struct Slice
    {
        size_t              layer_id = 0;
        double              bottom = 0., top = 0., sample_z = 0.;
        ExPolygons          outline;
        std::vector<Region> regions;
    };
    struct Node
    {
        size_t              slice = 0, region = 0, polygon = 0, domain = 0;
        BoundingBox         bounds;
        std::vector<size_t> below, above;
    };
    struct Domain
    {
        size_t              id = 0, first_slice = 0, last_slice = 0;
        unsigned            source        = 0;
        size_t              configuration = 0;
        std::vector<size_t> nodes;
        // Branches remain part of their connected domain, retaining cadence.
        size_t births = 0, deaths = 0, splits = 0, merges = 0;
    };
    struct Contact
    {
        size_t              a = 0, b = 0;
        std::vector<size_t> slices;
        // Lower sample index for contacts crossing a horizontal sample boundary.
        std::vector<size_t>                    vertical_slices;
        std::vector<std::pair<size_t, size_t>> vertical_nodes;
    };

    static LocalZMaterialGraph build(std::vector<Slice> slices, const std::function<void()>& cancel = {});
    const ExPolygon&           mask(size_t node) const;
    ExPolygons                 footprint(size_t domain, size_t slice) const;
    const Region&              region(size_t node) const;

    std::vector<Slice>               slices;
    std::vector<Node>                nodes;
    std::vector<std::vector<size_t>> nodes_by_slice;
    std::vector<Domain>              domains;
    std::vector<Contact>             contacts;
};
} // namespace Slic3r
#endif
