#include "LocalZClipper.hpp"

#include "ClipperUtils.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <boost/iterator/indirect_iterator.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r {
namespace {

// Development-only, process-wide export diagnostics. Timed intervals exclude
// statistics/capture serialization. No clocks or geometry copies when disabled.
class ExportClipTrace
{
    using Clock = std::chrono::steady_clock;
    using Json  = nlohmann::json;
    struct Counter
    {
        uint64_t calls = 0, input_points = 0, output_points = 0, empty_results = 0;
        double   elapsed_ms = 0, empty_ms = 0, max_ms = 0;
    };
    std::string                         m_profile_path;
    std::ofstream                       m_capture;
    std::mutex                          m_mutex;
    Counter                             m_counters[2];
    std::map<std::thread::id, uint64_t> m_threads;
    uint64_t                            m_calls = 0, m_captured = 0, m_capture_bytes = 0;
    bool                                m_capture_capped = false;
    static constexpr uint64_t           CAPTURE_STRIDE = 257, CAPTURE_LIMIT = 128 * 1024 * 1024;

    static Json points(const Points& input)
    {
        Json result = Json::array();
        for (const Point& point : input)
            result.push_back({point.x(), point.y()});
        return result;
    }
    static Json lines(const Polylines& input)
    {
        Json result = Json::array();
        for (const Polyline& line : input)
            result.push_back(points(line.points));
        return result;
    }

public:
    ExportClipTrace()
    {
        if (const char* path = std::getenv("ORCA_LOCAL_Z_PROFILE"))
            m_profile_path = path;
        if (const char* path = std::getenv("ORCA_LOCAL_Z_CAPTURE"); path && *path) {
            m_capture.open(std::filesystem::u8path(path), std::ios::out | std::ios::binary);
            if (!m_capture)
                throw std::runtime_error("Cannot create Local-Z clipping capture");
        }
    }
    bool enabled() const { return !m_profile_path.empty() || m_capture.is_open(); }
    ~ExportClipTrace()
    {
        if (m_profile_path.empty())
            return;
        try {
            Json operations = Json::object();
            for (size_t i = 0; i < 2; ++i) {
                const Counter& c                                   = m_counters[i];
                operations[i == 0 ? "intersection" : "difference"] = {{"calls", c.calls},
                                                                      {"elapsed_ms", c.elapsed_ms},
                                                                      {"max_ms", c.max_ms},
                                                                      {"input_points", c.input_points},
                                                                      {"output_points", c.output_points},
                                                                      {"empty_results", c.empty_results},
                                                                      {"empty_ms", c.empty_ms}};
            }
            Json          summary = {{"version", 1},
                                     {"operations", operations},
                                     {"thread_count", m_threads.size()},
                                     {"capture_stride", CAPTURE_STRIDE},
                                     {"captured_calls", m_captured},
                                     {"capture_bytes", m_capture_bytes},
                                     {"capture_capped", m_capture_capped},
                                     {"reference_validation", local_z_validate_clipping()}};
            std::ofstream stream(std::filesystem::u8path(m_profile_path));
            stream << summary.dump(2) << '\n';
            if (!stream)
                std::fprintf(stderr, "Failed to write Local-Z clipping profile\n");
        } catch (...) {
            std::fprintf(stderr, "Failed to write Local-Z clipping profile\n");
        }
    }
    void record(bool              difference,
                Clock::time_point start,
                const Polyline*   line,
                const Polylines*  paths,
                const ExPolygons& masks,
                const Polylines&  result)
    {
        const double                milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        std::lock_guard<std::mutex> lock(m_mutex);
        Counter&                    c = m_counters[difference ? 1 : 0];
        ++c.calls;
        c.elapsed_ms += milliseconds;
        c.max_ms = std::max(c.max_ms, milliseconds);
        if (result.empty()) {
            ++c.empty_results;
            c.empty_ms += milliseconds;
        }
        if (line)
            c.input_points += line->points.size();
        else
            for (const Polyline& path : *paths)
                c.input_points += path.points.size();
        for (const Polyline& path : result)
            c.output_points += path.points.size();
        ++m_threads[std::this_thread::get_id()];
        const uint64_t id = m_calls++;
        if (!m_capture.is_open() || m_capture_capped || id % CAPTURE_STRIDE != 0)
            return;
        Json polygons = Json::array();
        for (const ExPolygon& polygon : masks) {
            Json rings = Json::array();
            rings.push_back(points(polygon.contour.points));
            for (const Polygon& hole : polygon.holes)
                rings.push_back(points(hole.points));
            polygons.push_back(std::move(rings));
        }
        Json              input = line ? Json::array({points(line->points)}) : lines(*paths);
        Json              row   = {{"version", 1},
                                   {"id", id},
                                   {"operation", difference ? "difference" : "intersection"},
                                   {"elapsed_ms", milliseconds},
                                   {"subject", std::move(input)},
                                   {"masks", std::move(polygons)},
                                   {"result", lines(result)}};
        const std::string data  = row.dump();
        if (m_capture_bytes + data.size() + 1 > CAPTURE_LIMIT) {
            m_capture_capped = true;
            return;
        }
        m_capture << data << '\n';
        if (!m_capture)
            throw std::runtime_error("Failed to write Local-Z clipping capture");
        m_capture_bytes += data.size() + 1;
        ++m_captured;
    }
};

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

Polylines LocalZClipper::clip_path(const Polyline& subject, const LocalZClipper* include_masks, const LocalZClipper* exclude_masks)
{
    static ExportClipTrace trace;
    const bool             profile = trace.enabled();
    Polylines              result;
    if (include_masks != nullptr && !include_masks->empty()) {
        const auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        result           = include_masks->intersection(subject);
        if (profile)
            trace.record(false, start, &subject, nullptr, *include_masks->m_masks, result);
    } else {
        result.emplace_back(subject);
    }
    if (exclude_masks != nullptr && !exclude_masks->empty()) {
        const auto start      = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        Polylines  difference = exclude_masks->difference(result);
        if (profile)
            trace.record(true, start, nullptr, &result, *exclude_masks->m_masks, difference);
        result = std::move(difference);
    }
    return result;
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
