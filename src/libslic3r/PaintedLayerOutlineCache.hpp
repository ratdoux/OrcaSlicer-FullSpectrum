#ifndef slic3r_PaintedLayerOutlineCache_hpp_
#define slic3r_PaintedLayerOutlineCache_hpp_

#include "ClipperUtils.hpp"
#include <array>
#include <cassert>

namespace Slic3r {

// Cumulative outline intersections depend on the source layer and direction,
// not the filament color. Own a bounded prefix; continue deeper/large chains
// in the caller's per-color scratch without retaining more geometry.
class PaintedLayerOutlineCache
{
public:
    PaintedLayerOutlineCache(const std::vector<ExPolygons>& layers, size_t source, size_t point_limit = 32768, size_t depth_limit = 16)
        : m_layers(layers), m_source(source), m_point_limit(point_limit), m_depth_limit(depth_limit)
    {}

    const ExPolygons& step(bool top, size_t distance, ExPolygons& scratch)
    {
        assert(distance > 0 && (top ? distance <= m_source : distance < m_layers.size() - m_source));
        auto& cache = m_cache[top ? 0 : 1];
        if (distance <= cache.size())
            return cache[distance - 1];
        const ExPolygons& previous = distance == 1 ? m_layers[m_source] : distance - 1 <= cache.size() ? cache[distance - 2] : scratch;
        ExPolygons        next     = intersection_ex(previous, m_layers[top ? m_source - distance : m_source + distance]);
        const size_t      points   = count_points(next);
        if (distance == cache.size() + 1 && cache.size() < m_depth_limit && points <= m_point_limit - m_cached_points) {
            m_cached_points += points;
            cache.emplace_back(std::move(next));
            return cache.back();
        }
        scratch = std::move(next);
        return scratch;
    }

private:
    const std::vector<ExPolygons>&         m_layers;
    size_t                                 m_source, m_point_limit, m_depth_limit;
    size_t                                 m_cached_points = 0;
    std::array<std::vector<ExPolygons>, 2> m_cache;
};

} // namespace Slic3r
#endif
