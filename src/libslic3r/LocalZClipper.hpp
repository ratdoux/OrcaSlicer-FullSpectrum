#ifndef slic3r_LocalZClipper_hpp_
#define slic3r_LocalZClipper_hpp_

#include "BoundingBox.hpp"
#include "ExPolygon.hpp"

namespace Slic3r {

// Opt-in expensive reference clipping and base-path leak checks for development.
bool local_z_validate_clipping();

// Immutable, borrowed mask geometry. The ExPolygons and their contents must stay
// alive and unmodified until this object is discarded. Queries have no shared
// scratch state and may run concurrently.
class LocalZClipper
{
public:
    LocalZClipper() = default;
    explicit LocalZClipper(const ExPolygons& masks);
    LocalZClipper(ExPolygons&&) = delete;

    bool      empty() const { return m_masks == nullptr || m_masks->empty(); }
    Polylines intersection(const Polyline& subject) const;
    Polylines difference(const Polylines& subject) const;
    bool      intersects(const Polyline& subject) const;

    // Export's intersection-then-difference operation. Optional process-wide
    // timing/capture is enabled by ORCA_LOCAL_Z_PROFILE / ORCA_LOCAL_Z_CAPTURE.
    static Polylines clip_path(const Polyline& subject, const LocalZClipper* include_masks, const LocalZClipper* exclude_masks);

private:
    friend class LocalZClipAcceleration;
    const ExPolygons*        m_masks = nullptr;
    BoundingBox              m_bounds;
    std::vector<BoundingBox> m_polygon_bounds;
    size_t                   m_path_count = 0;

    bool                       may_intersect(const BoundingBox& subject_bounds) const;
    std::vector<const Points*> candidates(const BoundingBox& subject_bounds) const;
};

} // namespace Slic3r

#endif
