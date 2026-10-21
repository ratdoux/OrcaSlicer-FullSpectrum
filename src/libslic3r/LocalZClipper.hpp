#ifndef slic3r_LocalZClipper_hpp_
#define slic3r_LocalZClipper_hpp_

#include "BoundingBox.hpp"
#include "ExPolygon.hpp"
#include <cstdint>
#include <memory>

namespace Slic3r {

class LocalZClipIndex;

// Opt-in expensive reference clipping and base-path leak checks for development.
bool local_z_validate_clipping();

// One export layer's bounded memoization. Bind masks before querying them, and
// keep their geometry and this cache alive until all borrowed clippers expire.
// Queries are synchronized; cached coordinates are copied, never shared with callers.
class LocalZClipCache
{
public:
    explicit LocalZClipCache(size_t byte_budget = 32 * 1024 * 1024);
    ~LocalZClipCache();
    struct Statistics
    {
        size_t hits = 0, misses = 0, entries = 0, bytes = 0, masks = 0;
    };
    Statistics statistics() const;

private:
    friend class LocalZClipper;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Immutable, borrowed mask geometry. The ExPolygons and their contents must stay
// alive and unmodified until this object is discarded. Queries may run concurrently.
class LocalZClipper
{
public:
    LocalZClipper() = default;
    explicit LocalZClipper(const ExPolygons& masks, LocalZClipCache* cache = nullptr);
    LocalZClipper(ExPolygons&&, LocalZClipCache* = nullptr) = delete;

    bool      empty() const { return m_masks == nullptr || m_masks->empty(); }
    Polylines intersection(const Polyline& subject) const;
    Polylines difference(const Polylines& subject) const;
    bool      intersects(const Polyline& subject) const;

    // Geometric ownership only: every segment survives the operation unchanged.
    // Does not promise Clipper's fragment order, direction or point normalization.
    // Boundary touches, unsupported coordinates and degenerate segments decline.
    bool can_reuse_whole_path(const Polyline& subject, bool difference = false) const;

    // Export's intersection-then-difference operation. Optional process-wide
    // timing/capture is enabled by ORCA_LOCAL_Z_PROFILE / ORCA_LOCAL_Z_CAPTURE.
    static Polylines clip_path(const Polyline& subject, const LocalZClipper* include_masks, const LocalZClipper* exclude_masks);

private:
    friend class LocalZClipAcceleration;
    const ExPolygons*                      m_masks = nullptr;
    BoundingBox                            m_bounds;
    std::vector<BoundingBox>               m_polygon_bounds;
    size_t                                 m_path_count = 0;
    std::shared_ptr<const LocalZClipIndex> m_empty_index;
    uint32_t                               m_edge_count = 0;
    LocalZClipCache*                       m_cache      = nullptr;
    size_t                                 m_cache_mask = 0;

    bool                       may_intersect(const BoundingBox& subject_bounds) const;
    bool                       proven_empty(const Points& subject, bool difference) const;
    std::vector<const Points*> candidates(const BoundingBox& subject_bounds) const;
};

} // namespace Slic3r

#endif
