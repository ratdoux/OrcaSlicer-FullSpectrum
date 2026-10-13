#ifndef slic3r_MixedFilamentLayerTracker_hpp_
#define slic3r_MixedFilamentLayerTracker_hpp_

#include "BoundingBox.hpp"
#include "ExPolygon.hpp"

#include <algorithm>
#include <vector>

namespace Slic3r {

// Tracks the vertical activation run of each connected painted component in one
// filament channel. Layers must be visited in order. Masks are borrowed: their
// storage and geometry must remain unchanged until the next layer is processed.
class MixedFilamentLayerTracker
{
public:
    void track_layer(const ExPolygons& masks, int layer_id, std::vector<int>& local_layer_indices)
    {
        local_layer_indices.assign(masks.size(), -1);
        m_next.clear();
        m_next.reserve(masks.size());
        for (size_t component_idx = 0; component_idx < masks.size(); ++component_idx) {
            const ExPolygon& component = masks[component_idx];
            if (component.empty())
                continue;

            const BoundingBox bbox        = get_extents(component);
            int               start_layer = layer_id;
            for (const Component& active : m_active) {
                if (active.bbox.overlap(bbox) && active.mask->overlaps(component))
                    start_layer = std::min(start_layer, active.start_layer);
            }
            local_layer_indices[component_idx] = layer_id - start_layer;
            m_next.push_back({&component, bbox, start_layer});
        }
        m_active.swap(m_next);
    }

private:
    struct Component
    {
        const ExPolygon* mask;
        BoundingBox      bbox;
        int              start_layer;
    };
    std::vector<Component> m_active;
    std::vector<Component> m_next;
};

} // namespace Slic3r

#endif
