#include "LocalZClipper.hpp"

#include "ClipperUtils.hpp"
#include "Compute/LocalZClipDecision.hpp"

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
#include <unordered_map>
#include <boost/iterator/indirect_iterator.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r {
namespace {

const std::string& cpu_mode()
{
    static const std::string mode = [] {
        const char*       value  = std::getenv("ORCA_LOCAL_Z_CPU");
        const std::string result = value ? value : "all";
        if (result != "all" && result != "index" && result != "cache" && result != "legacy")
            throw std::runtime_error("ORCA_LOCAL_Z_CPU must be all, index, cache, or legacy");
        return result;
    }();
    return mode;
}

bool cpu_index_enabled() { return cpu_mode() == "all" || cpu_mode() == "index"; }
bool cpu_cache_enabled() { return cpu_mode() == "all" || cpu_mode() == "cache"; }

bool bounded(const Point& point)
{
    return point.x() >= -(1 << 29) && point.x() <= (1 << 29) && point.y() >= -(1 << 29) && point.y() <= (1 << 29);
}

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

struct LocalZClipCache::Impl
{
    struct Mask
    {
        const ExPolygons* polygons;
        size_t            hash, users;
    };
    struct Entry
    {
        size_t              mask;
        bool                difference;
        std::vector<Points> subject;
        Polylines           result;
    };
    mutable std::mutex                     mutex;
    std::vector<Mask>                      masks;
    std::unordered_multimap<size_t, Entry> entries;
    const size_t                           byte_budget;
    Statistics                             stats;
    explicit Impl(size_t budget) : byte_budget(budget) {}

    static void combine(size_t& hash, size_t value) { hash ^= value + size_t(0x9e3779b9) + (hash << 6) + (hash >> 2); }
    template<class Provider> static size_t hash_paths(Provider paths, size_t seed)
    {
        combine(seed, paths.size());
        for (const Points& points : paths) {
            combine(seed, points.size());
            for (const Point& point : points) {
                combine(seed, std::hash<coord_t>{}(point.x()));
                combine(seed, std::hash<coord_t>{}(point.y()));
            }
        }
        return seed;
    }
    size_t bind(const ExPolygons& polygons)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const size_t                hash = hash_paths(ClipperUtils::ExPolygonsProvider(polygons), 0);
        for (size_t i = 0; i < masks.size(); ++i) {
            // Preserve polygon, hole and vertex order as well as coordinates.
            // Geometric equivalence alone would not preserve Clipper's ties.
            if (masks[i].hash == hash && *masks[i].polygons == polygons) {
                ++masks[i].users;
                return i;
            }
        }
        masks.push_back({&polygons, hash, 1});
        stats.masks = masks.size();
        return masks.size() - 1;
    }
    template<class Provider> static bool matches(const Entry& entry, size_t mask, bool difference, Provider paths)
    {
        return entry.mask == mask && entry.difference == difference && entry.subject.size() == paths.size() &&
               std::equal(entry.subject.begin(), entry.subject.end(), paths.begin());
    }
    template<class Provider> bool find(size_t mask, bool difference, Provider paths, Polylines& result)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (masks[mask].users < 2 || byte_budget == 0)
            return false;
        const auto range = entries.equal_range(hash_paths(paths, mask * 2 + size_t(difference)));
        for (auto it = range.first; it != range.second; ++it) {
            if (matches(it->second, mask, difference, paths)) {
                result = it->second.result;
                ++stats.hits;
                return true;
            }
        }
        ++stats.misses;
        return false;
    }
    template<class Provider> void insert(size_t mask, bool difference, Provider paths, const Polylines& result)
    {
        std::lock_guard<std::mutex> lock(mutex);
        // Only retain masks shared by multiple passes. Bound both retained data
        // (including a per-entry bookkeeping allowance) and hash-table growth.
        if (masks[mask].users < 2 || entries.size() >= 65536 || stats.bytes >= byte_budget)
            return;
        const size_t hash  = hash_paths(paths, mask * 2 + size_t(difference));
        const auto   range = entries.equal_range(hash);
        for (auto it = range.first; it != range.second; ++it)
            if (matches(it->second, mask, difference, paths))
                return;
        size_t bytes = sizeof(Entry) + 64 + paths.size() * sizeof(Points) + result.size() * sizeof(Polyline);
        for (const Points& points : paths)
            bytes += points.size() * sizeof(Point);
        for (const Polyline& line : result)
            bytes += line.points.size() * sizeof(Point) + line.fitting_result.size() * sizeof(PathFittingData);
        if (bytes > byte_budget - stats.bytes)
            return;
        Entry entry{mask, difference, {}, result};
        entry.subject.reserve(paths.size());
        for (const Points& points : paths)
            entry.subject.emplace_back(points);
        entries.emplace(hash, std::move(entry));
        stats.bytes += bytes;
        stats.entries = entries.size();
    }
};

LocalZClipCache::LocalZClipCache(size_t byte_budget) : m_impl(std::make_unique<Impl>(byte_budget)) {}
LocalZClipCache::~LocalZClipCache()
{
    if (const char* profile = std::getenv("ORCA_PREP_PROFILE"); profile && std::string(profile) == "1" && m_impl->stats.masks) {
        const auto& stats = m_impl->stats;
        BOOST_LOG_TRIVIAL(info) << "Local-Z CPU clip cache masks=" << stats.masks << " hits=" << stats.hits << " misses=" << stats.misses
                                << " entries=" << stats.entries << " bytes=" << stats.bytes;
    }
}
LocalZClipCache::Statistics LocalZClipCache::statistics() const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->stats;
}

bool local_z_validate_clipping()
{
    static const bool enabled = [] {
        const char* value = std::getenv("ORCA_LOCAL_Z_VALIDATE");
        return value != nullptr && std::string(value) == "1";
    }();
    return enabled;
}

LocalZClipper::LocalZClipper(const ExPolygons& masks, LocalZClipCache* cache) : m_masks(&masks)
{
    if (cache && !masks.empty() && cpu_cache_enabled()) {
        m_cache      = cache;
        m_cache_mask = cache->m_impl->bind(masks);
    }
    m_polygon_bounds.reserve(masks.size());
    for (const ExPolygon& mask : masks) {
        m_path_count += 1 + mask.holes.size();
        BoundingBox bounds = get_extents<true>(mask.contour.points);
        for (const Polygon& hole : mask.holes)
            bounds.merge(get_extents<true>(hole.points));
        m_polygon_bounds.push_back(bounds);
        m_bounds.merge(bounds);
    }
    // Reuse the conservative integer classifier without packing an entire
    // layer's queries or requiring Vulkan. The exact clipping API retains
    // Clipper for nonempty results; export has a separate whole-path contract.
    size_t edge_count = 0;
    for (const ExPolygon& mask : masks) {
        edge_count += mask.contour.points.size();
        for (const Polygon& hole : mask.holes)
            edge_count += hole.points.size();
    }
    if (!cpu_index_enabled() || edge_count < 3 || edge_count > (1u << 20))
        return;
    std::vector<ClipMaskEdge> edges;
    edges.reserve(edge_count);
    auto append_ring = [&edges](const Points& points) {
        for (size_t i = 0; i < points.size(); ++i) {
            const Point& a = points[i];
            const Point& b = points[(i + 1) % points.size()];
            if (!bounded(a) || !bounded(b))
                return false;
            edges.push_back({int32_t(a.x()), int32_t(a.y()), int32_t(b.x()), int32_t(b.y())});
        }
        return true;
    };
    for (const ExPolygon& mask : masks) {
        if (!append_ring(mask.contour.points))
            return;
        for (const Polygon& hole : mask.holes)
            if (!append_ring(hole.points))
                return;
    }
    m_edge_count  = uint32_t(edges.size());
    m_empty_index = std::make_shared<LocalZClipIndex>(edges, 0, m_edge_count);
}

bool LocalZClipper::proven_empty(const Points& subject, bool difference) const
{
    if (!m_empty_index || subject.size() < 2)
        return false;
    for (size_t i = 1; i < subject.size(); ++i) {
        const Point& a = subject[i - 1];
        const Point& b = subject[i];
        if (!bounded(a) || !bounded(b) ||
            m_empty_index->classify(
                {{int32_t(a.x()), int32_t(a.y()), int32_t(b.x()), int32_t(b.y())}, 0, m_edge_count, uint32_t(difference), 0}) !=
                uint32_t(ClipDecision::Empty))
            return false;
    }
    return true;
}

bool LocalZClipper::can_reuse_whole_path(const Polyline& subject, bool difference) const
{
    if (!m_empty_index || !subject.is_valid())
        return false;
    for (size_t i = 1; i < subject.points.size(); ++i) {
        const Point& a = subject.points[i - 1];
        const Point& b = subject.points[i];
        if (!bounded(a) || !bounded(b) ||
            m_empty_index->classify(
                {{int32_t(a.x()), int32_t(a.y()), int32_t(b.x()), int32_t(b.y())}, 0, m_edge_count, uint32_t(difference), 1}) !=
                uint32_t(ClipDecision::KeepWhole))
            return false;
    }
    if (local_z_validate_clipping()) {
        // Independent full-mask Boolean checks: no discarded geometry and equal
        // deposited length. Ordering may intentionally differ from Clipper.
        const Polylines input{Polyline(subject.points)};
        const Polylines discarded       = difference ? intersection_pl(input, *m_masks) : diff_pl(input, *m_masks);
        const Polylines retained        = difference ? diff_pl(input, *m_masks) : intersection_pl(input, *m_masks);
        double          retained_length = 0.;
        for (const Polyline& line : retained)
            retained_length += line.length();
        if (has_segment(discarded) || std::abs(retained_length - subject.length()) > std::max(1., subject.length() * 1e-10))
            throw std::runtime_error("Local-Z whole-path ownership differs from reference clipping");
    }
    return true;
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
    const auto provider = ClipperUtils::SinglePathProvider(subject.points);
    Polylines  result;
    const bool cached = m_cache && m_cache->m_impl->find(m_cache_mask, false, provider, result);
    if (!cached) {
        const auto bounds = get_extents<true>(subject.points);
        const auto masks  = may_intersect(bounds) ? candidates(bounds) : std::vector<const Points*>{};
        if (!masks.empty() && !proven_empty(subject.points, false))
            result = to_polylines(clip(provider, masks, ClipperLib::ctIntersection));
        // Removing unrelated minima can change Clipper's tie ordering. Preserve
        // fragment order for equal starting scanlines with a full-mask query.
        if (m_masks != nullptr && masks.size() != m_path_count && has_scanline_ties(result))
            result = intersection_pl(Polylines{subject}, *m_masks);
        if (m_cache)
            m_cache->m_impl->insert(m_cache_mask, false, provider, result);
    }
    if (local_z_validate_clipping() && m_masks != nullptr)
        validate(result, intersection_pl(Polylines{subject}, *m_masks));
    return result;
}

Polylines LocalZClipper::difference(const Polylines& subject) const
{
    const auto provider = ClipperUtils::PolylinesProvider(subject);
    Polylines  result;
    const bool cached = m_cache && m_cache->m_impl->find(m_cache_mask, true, provider, result);
    if (!cached) {
        BoundingBox bounds;
        for (const Polyline& line : subject)
            bounds.merge(get_extents<true>(line.points));
        // Even with no candidate masks, let Clipper normalize path direction
        // and duplicate points exactly as before; returning input would differ.
        const auto masks = candidates(bounds);
        if (!std::all_of(subject.begin(), subject.end(), [this](const Polyline& line) { return proven_empty(line.points, true); }))
            result = to_polylines(clip(provider, masks, ClipperLib::ctDifference));
        if (m_masks != nullptr && masks.size() != m_path_count && has_scanline_ties(result))
            result = diff_pl(subject, *m_masks);
        if (m_cache)
            m_cache->m_impl->insert(m_cache_mask, true, provider, result);
    }
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
