#include "../ClipperUtils.hpp"
#include "../Layer.hpp"
#include "../Polyline.hpp"
#include "../SlicingAcceleration.hpp"
#include <stdexcept>

#include "RetractWhenCrossingPerimeters.hpp"

namespace Slic3r {

bool RetractWhenCrossingPerimeters::travel_inside_internal_regions(const Layer &layer, const Polyline &travel)
{
    if (m_layer != &layer) {
        // Update cache.
        m_layer = &layer;
        m_internal_islands.clear();
        m_aabbtree_internal_islands.clear();
        // Collect expolygons of internal slices.
        for (const LayerRegion *layerm : layer.regions())
            for (const Surface &surface : layerm->get_slices().surfaces)
                if (surface.is_internal())
                    m_internal_islands.emplace_back(&surface.expolygon);
        // Calculate bounding boxes of internal slices.
        std::vector<AABBTreeIndirect::BoundingBoxWrapper> bboxes;
        bboxes.reserve(m_internal_islands.size());
        for (size_t i = 0; i < m_internal_islands.size(); ++ i)
            bboxes.emplace_back(i, get_extents(*m_internal_islands[i]));
        // Build AABB tree over bounding boxes of internal slices.
        m_aabbtree_internal_islands.build_modify_input(bboxes);
        m_boundary_indices.clear();
        m_boundary_indices.resize(m_internal_islands.size());
        m_boundary_edge_counts.assign(m_internal_islands.size(), 0);
    }

    BoundingBox           bbox_travel = get_extents(travel);
    AABBTree::BoundingBox bbox_travel_eigen{ bbox_travel.min, bbox_travel.max };
    int result = -1;
    bbox_travel.offset(SCALED_EPSILON);
    AABBTreeIndirect::traverse(
        m_aabbtree_internal_islands, [&bbox_travel_eigen](const AABBTree::Node& node) { return bbox_travel_eigen.intersects(node.bbox); },
        [this, &travel, &bbox_travel, &result, &islands = m_internal_islands](const AABBTree::Node& node) {
            assert(node.is_leaf());
            assert(node.is_valid());
            const auto reference = [&] {
                Polygons clipped = ClipperUtils::clip_clipper_polygons_with_subject_bbox(*islands[node.idx], bbox_travel);
                return diff_pl(travel, clipped).empty();
            };
            static const bool accelerate = slicing_acceleration("travel");
            uint32_t          decision   = 0;
            const auto        eligible   = [](const Point& p) {
                return p.x() >= -(1 << 29) && p.x() <= (1 << 29) && p.y() >= -(1 << 29) && p.y() <= (1 << 29);
            };
            if (accelerate && travel.points.size() > 1 && std::all_of(travel.points.begin(), travel.points.end(), eligible)) {
                auto& index = m_boundary_indices[node.idx];
                auto& count = m_boundary_edge_counts[node.idx];
                if (!index && count != UINT32_MAX) {
                    std::vector<ClipMaskEdge> edges;
                    for (const Polygon& ring : to_polygons(*islands[node.idx]))
                        for (const Line& edge : ring.lines()) {
                            if (!eligible(edge.a) || !eligible(edge.b)) {
                                count = UINT32_MAX;
                                break;
                            }
                            edges.push_back({int32_t(edge.a.x()), int32_t(edge.a.y()), int32_t(edge.b.x()), int32_t(edge.b.y())});
                        }
                    if (count != UINT32_MAX) {
                        count = uint32_t(edges.size());
                        index = std::make_unique<LocalZClipIndex>(edges, 0, count);
                    }
                }
                decision = index ? uint32_t(ClipDecision::Empty) : 0;
                for (size_t i = 1; index && i < travel.points.size(); ++i) {
                    const Point&   a       = travel.points[i - 1];
                    const Point&   b       = travel.points[i];
                    const uint32_t segment = index->classify(
                        {{int32_t(a.x()), int32_t(a.y()), int32_t(b.x()), int32_t(b.y())}, 0, count, 1, 1});
                    if (segment != uint32_t(ClipDecision::Empty)) {
                        decision = segment;
                        break;
                    }
                }
            }
            const bool inside = decision ? decision == uint32_t(ClipDecision::Empty) : reference();
            if (decision && slicing_acceleration_validate() && inside != reference())
                throw std::runtime_error("Travel containment differs from clipping reference");
            if (inside) {
                // Travel path is completely inside an "internal" island. Don't retract.
                result = int(node.idx);
                // Stop traversal.
                return false;
            }
            // Continue traversal.
            return true;
        });
    return result != -1;
}

} // namespace Slic3r
