#include "LocalZClipper.hpp"

#include "ClipperUtils.hpp"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <boost/iterator/indirect_iterator.hpp>

namespace Slic3r {
namespace {

struct BorrowedPaths
{
    const std::vector<const Points*>& paths;
    auto                              begin() const { return boost::make_indirect_iterator(paths.begin()); }
    auto                              end() const { return boost::make_indirect_iterator(paths.end()); }
    size_t                            size() const { return paths.size(); }
};

template<class SubjectProvider>
ClipperLib::PolyTree clip(SubjectProvider subject, const std::vector<const Points*>& masks, ClipperLib::ClipType operation)
{
    ClipperLib::Clipper clipper;
    clipper.AddPaths(subject, ClipperLib::ptSubject, false);
    clipper.AddPaths(BorrowedPaths{masks}, ClipperLib::ptClip, true);
    ClipperLib::PolyTree result;
    clipper.Execute(operation, result, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
    return result;
}

Polylines to_polylines(ClipperLib::PolyTree tree)
{
    Polylines result;
    result.reserve(tree.Total());
    // Same depth-first order as ClipperUtils' PolyTreeToPolylines.
    for (auto* node = tree.GetFirst(); node != nullptr; node = node->GetNext())
        if (!node->Contour.empty())
            result.emplace_back(std::move(node->Contour));
    return result;
}

void validate(const Polylines& result, const Polylines& reference)
{
    if (result.size() != reference.size() || !std::equal(result.begin(), result.end(), reference.begin(),
                                                         [](const Polyline& lhs, const Polyline& rhs) { return lhs.points == rhs.points; }))
        throw std::runtime_error("Local-Z filtered clipping differs from reference clipping");
}

bool has_segment(const Polylines& lines)
{
    return std::any_of(lines.begin(), lines.end(), [](const Polyline& line) { return line.is_valid(); });
}

bool has_scanline_ties(const Polylines& lines)
{
    if (lines.size() < 2)
        return false;
    std::vector<coord_t> maxima;
    maxima.reserve(lines.size());
    for (const Polyline& line : lines)
        maxima.push_back(get_extents<true>(line.points).max.y());
    std::sort(maxima.begin(), maxima.end());
    return std::adjacent_find(maxima.begin(), maxima.end()) != maxima.end();
}

} // namespace

bool local_z_validate_clipping()
{
    static const bool enabled = [] {
        const char* value = std::getenv("ORCA_LOCAL_Z_VALIDATE");
        return value != nullptr && std::string(value) == "1";
    }();
    return enabled;
}

LocalZClipper::LocalZClipper(const ExPolygons& masks) : m_masks(&masks)
{
    m_polygon_bounds.reserve(masks.size());
    for (const ExPolygon& mask : masks) {
        m_path_count += 1 + mask.holes.size();
        BoundingBox bounds = get_extents<true>(mask.contour.points);
        for (const Polygon& hole : mask.holes)
            bounds.merge(get_extents<true>(hole.points));
        m_polygon_bounds.push_back(bounds);
        m_bounds.merge(bounds);
    }
}

bool LocalZClipper::may_intersect(const BoundingBox& subject_bounds) const
{
    return subject_bounds.defined && m_bounds.defined && subject_bounds.overlap(m_bounds) &&
           std::any_of(m_polygon_bounds.begin(), m_polygon_bounds.end(),
                       [&subject_bounds](const BoundingBox& bounds) { return bounds.defined && subject_bounds.overlap(bounds); });
}

std::vector<const Points*> LocalZClipper::candidates(const BoundingBox& subject_bounds) const
{
    std::vector<const Points*> paths;
    const auto                 overlaps_y = [&subject_bounds](const BoundingBox& bounds) {
        return bounds.min.y() <= subject_bounds.max.y() && bounds.max.y() >= subject_bounds.min.y();
    };
    if (!subject_bounds.defined || !m_bounds.defined || !overlaps_y(m_bounds))
        return paths;
    for (size_t i = 0; i < m_polygon_bounds.size(); ++i) {
        // Retain every polygon overlapping the subject's Y range, even when it
        // is disjoint in X: its vertices contribute scanlines, which can affect
        // integer rounding at subject intersections. Include boundary touches
        // and keep complete polygons/holes in the original mask order.
        if (m_polygon_bounds[i].defined && overlaps_y(m_polygon_bounds[i])) {
            const ExPolygon& mask = (*m_masks)[i];
            paths.push_back(&mask.contour.points);
            for (const Polygon& hole : mask.holes)
                paths.push_back(&hole.points);
        }
    }
    return paths;
}

Polylines LocalZClipper::intersection(const Polyline& subject) const
{
    const auto bounds = get_extents<true>(subject.points);
    const auto masks  = may_intersect(bounds) ? candidates(bounds) : std::vector<const Points*>{};
    Polylines  result;
    if (!masks.empty())
        result = to_polylines(clip(ClipperUtils::SinglePathProvider(subject.points), masks, ClipperLib::ctIntersection));
    // Removing unrelated minima can change Clipper's tie ordering. Preserve the
    // old fragment order for equal starting scanlines with a full-mask query.
    if (m_masks != nullptr && masks.size() != m_path_count && has_scanline_ties(result))
        result = intersection_pl(Polylines{subject}, *m_masks);
    if (local_z_validate_clipping() && m_masks != nullptr)
        validate(result, intersection_pl(Polylines{subject}, *m_masks));
    return result;
}

Polylines LocalZClipper::difference(const Polylines& subject) const
{
    BoundingBox bounds;
    for (const Polyline& line : subject)
        bounds.merge(get_extents<true>(line.points));
    // Even with no candidate masks, let Clipper normalize path direction and
    // duplicate points exactly as before; returning the input would differ.
    const auto masks  = candidates(bounds);
    Polylines  result = to_polylines(clip(ClipperUtils::PolylinesProvider(subject), masks, ClipperLib::ctDifference));
    if (m_masks != nullptr && masks.size() != m_path_count && has_scanline_ties(result))
        result = diff_pl(subject, *m_masks);
    if (local_z_validate_clipping() && m_masks != nullptr)
        validate(result, diff_pl(subject, *m_masks));
    return result;
}

bool LocalZClipper::intersects(const Polyline& subject) const
{
    const auto bounds = get_extents<true>(subject.points);
    const auto masks  = may_intersect(bounds) ? candidates(bounds) : std::vector<const Points*>{};
    bool       result = false;
    if (!masks.empty()) {
        const auto tree = clip(ClipperUtils::SinglePathProvider(subject.points), masks, ClipperLib::ctIntersection);
        for (const auto* node = tree.GetFirst(); node != nullptr; node = node->GetNext()) {
            if (node->Contour.size() >= 2) {
                result = true;
                break;
            }
        }
    }
    if (local_z_validate_clipping() && m_masks != nullptr && result != has_segment(intersection_pl(Polylines{subject}, *m_masks)))
        throw std::runtime_error("Local-Z filtered intersection query differs from reference clipping");
    return result;
}

} // namespace Slic3r
