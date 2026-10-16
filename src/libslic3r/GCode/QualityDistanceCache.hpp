#ifndef slic3r_QualityDistanceCache_hpp_
#define slic3r_QualityDistanceCache_hpp_
#include "../AABBTreeLines.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../Layer.hpp"
#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>
#include <algorithm>
#include <functional>
#include <cstdlib>
#include <chrono>
#include <boost/log/trivial.hpp>
#include <unordered_map>

namespace Slic3r {
// Immutable signed distances for known perimeter/bridge vertices of one layer.
// Newly clipped or seam-inserted vertices continue through the original query.
class QualityDistanceCache
{
public:
    void prepare(const Layer& layer, const AABBTreeLines::LinesDistancer<Linef>& boundaries, size_t budget)
    {
        const auto started = profile() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
        auto       finish  = [&] {
            if (profile()) {
                ++m_layers;
                m_prepared += m_values.size();
                m_prepare_ms += milliseconds(started);
            }
        };
        m_values.clear();
        if (budget < 512 || boundaries.get_lines().empty()) {
            decltype(m_values)().swap(m_values);
            finish();
            return;
        }
        budget      = std::min(budget, size_t(65536));
        using Entry = decltype(m_values)::value_type;
        std::vector<Entry*> entries;
        bool                oversized = false;
        auto                path      = [&](const ExtrusionPath& p) {
            if (oversized || !(is_perimeter(p.role()) || is_bridge(p.role())))
                return;
            for (const Point& point : p.polyline.points) {
                auto [it, inserted] = m_values.emplace(point, 0.);
                if (inserted)
                    entries.push_back(&*it);
                if (entries.size() > budget) {
                    oversized = true;
                    return;
                }
            }
        };
        std::function<void(const ExtrusionEntity&)> gather = [&](const ExtrusionEntity& entity) {
            if (oversized)
                return;
            if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity))
                for (const ExtrusionEntity* child : collection->entities)
                    gather(*child);
            else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(&entity))
                for (const ExtrusionPath& p : loop->paths)
                    path(p);
            else if (const auto* multi = dynamic_cast<const ExtrusionMultiPath*>(&entity))
                for (const ExtrusionPath& p : multi->paths)
                    path(p);
            else if (const auto* p = dynamic_cast<const ExtrusionPath*>(&entity))
                path(*p);
        };
        for (const LayerRegion* region : layer.regions()) {
            gather(region->perimeters);
            gather(region->fills);
        }
        if (oversized || entries.size() < 512) {
            decltype(m_values)().swap(m_values);
            finish();
            return;
        }
        if (m_values.bucket_count() > 2 * entries.size())
            m_values.rehash(2 * entries.size());
        tbb::parallel_for(tbb::blocked_range<size_t>(0, entries.size(), 256), [&](const auto& range) {
            for (size_t i = range.begin(); i < range.end(); ++i)
                entries[i]->second = boundaries.distance_from_lines<true>(unscaled(entries[i]->first));
        });
        finish();
    }
    size_t              size() const { return m_values.size(); }
    std::vector<double> query(const Points& points, const AABBTreeLines::LinesDistancer<Linef>& boundaries) const
    {
        if (m_values.empty()) {
            if (profile())
                m_uncached += points.size();
            return {};
        }
        const auto          started = profile() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
        std::vector<double> result;
        result.reserve(points.size());
        for (const Point& point : points) {
            auto it = m_values.find(point);
            if (profile()) {
                if (it == m_values.end())
                    ++m_misses;
                else
                    ++m_hits;
            }
            result.push_back(it == m_values.end() ? boundaries.distance_from_lines<true>(unscaled(point)) : it->second);
        }
        if (profile())
            m_query_ms += milliseconds(started);
        return result;
    }
    void report() const
    {
        if (profile())
            BOOST_LOG_TRIVIAL(info) << "Quality batch profile layers=" << m_layers << " prepared=" << m_prepared << " hits=" << m_hits
                                    << " misses=" << m_misses << " uncached=" << m_uncached << " prepare_ms=" << m_prepare_ms
                                    << " query_ms=" << m_query_ms;
    }

private:
    static bool profile()
    {
        static const bool enabled = std::getenv("ORCA_QUALITY_PROFILE") != nullptr;
        return enabled;
    }
    static double milliseconds(std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
    std::unordered_map<Point, double, PointHash> m_values;
    size_t                                       m_layers = 0, m_prepared = 0;
    double                                       m_prepare_ms = 0;
    mutable size_t                               m_hits = 0, m_misses = 0, m_uncached = 0;
    mutable double                               m_query_ms = 0;
};
} // namespace Slic3r
#endif
