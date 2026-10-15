#include "LocalZClipAcceleration.hpp"
#include "LocalZClipper.hpp"
#include "Compute/LocalZClipDecision.hpp"
#include "ExtrusionEntityCollection.hpp"
#include <boost/log/trivial.hpp>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <unordered_map>

namespace Slic3r {
namespace {
using Clock = std::chrono::steady_clock;
double elapsed_ms(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
bool   bounded(const Point& p) { return p.x() >= -(1 << 29) && p.x() <= (1 << 29) && p.y() >= -(1 << 29) && p.y() <= (1 << 29); }
struct Key
{
    const ExtrusionEntity* entity;
    const LocalZClipper*   mask;
    bool                   difference;
    bool operator==(const Key& rhs) const { return entity == rhs.entity && mask == rhs.mask && difference == rhs.difference; }
};
struct KeyHash
{
    size_t operator()(const Key& key) const
    {
        return std::hash<const void*>{}(key.entity) ^ (std::hash<const void*>{}(key.mask) << 1) ^ size_t(key.difference);
    }
};
} // namespace

struct LocalZClipAcceleration::Impl
{
    static constexpr size_t MAX_EDGES = 1u << 20, MAX_QUERIES = 1u << 20, MAX_ENTITIES = 1u << 18;
    struct Mask
    {
        uint32_t start, count;
        bool     eligible;
    };
    struct Entry
    {
        uint32_t first, count;
        bool     eligible, empty = false;
    };
    std::unordered_map<const LocalZClipper*, Mask> masks;
    std::unordered_map<Key, Entry, KeyHash>        entries;
    std::vector<ClipMaskEdge>                      edges;
    std::vector<ClipSegmentQuery>                  queries;
    std::unique_ptr<VulkanLocalZClipDecision>      backend;
    bool                                           validate, disabled = false, overflow = false, ready = false;
    Clock::time_point                              start;
    double                                         total_ms = 0;
    uint64_t                                       layers = 0, fallback_layers = 0, rejected = 0, tested = 0;
    size_t                                         peak_edges = 0, peak_queries = 0, peak_entries = 0;
    explicit Impl(bool validation) : validate(validation) {}
    void fail(const std::string& error)
    {
        ready    = false;
        disabled = true;
        if (validate)
            throw std::runtime_error("Local-Z Vulkan validation unavailable: " + error);
        BOOST_LOG_TRIVIAL(warning) << "Local-Z Vulkan clipping fallback: " << error;
    }
    void add_path(const Polyline& path, const Mask& mask, bool difference, Entry& entry)
    {
        if (path.points.size() < 2 || path.points.size() - 1 > MAX_QUERIES - queries.size()) {
            entry.eligible = false;
            if (path.points.size() >= 2)
                overflow = true;
            return;
        }
        for (size_t i = 1; i < path.points.size(); ++i) {
            const Point& a = path.points[i - 1];
            const Point& b = path.points[i];
            if (!bounded(a) || !bounded(b)) {
                entry.eligible = false;
                return;
            }
            queries.push_back(
                {{int32_t(a.x()), int32_t(a.y()), int32_t(b.x()), int32_t(b.y())}, mask.start, mask.count, uint32_t(difference), 0});
        }
    }
};

LocalZClipAcceleration::LocalZClipAcceleration(bool validate) : m_impl(std::make_unique<Impl>(validate)) {}
LocalZClipAcceleration::~LocalZClipAcceleration() = default;
std::shared_ptr<LocalZClipAcceleration> LocalZClipAcceleration::from_environment()
{
    const char* mode = std::getenv("ORCA_LOCAL_Z_CLIP");
    if (!mode || std::string(mode) == "cpu")
        return nullptr;
    if (std::string(mode) != "vulkan" && std::string(mode) != "validate")
        throw std::runtime_error("ORCA_LOCAL_Z_CLIP must be cpu, vulkan, or validate");
    return std::shared_ptr<LocalZClipAcceleration>(new LocalZClipAcceleration(std::string(mode) == "validate"));
}
void LocalZClipAcceleration::begin_layer()
{
    auto& p = *m_impl;
    p.start = Clock::now();
    p.entries.clear();
    p.masks.clear();
    p.edges.clear();
    p.queries.clear();
    p.overflow = false;
    p.ready    = false;
}
void LocalZClipAcceleration::add(const ExtrusionEntityCollection& source, const LocalZClipper& masks, bool difference)
{
    auto& p = *m_impl;
    if (p.disabled || p.overflow || masks.empty())
        return;
    auto found = p.masks.find(&masks);
    if (found == p.masks.end()) {
        Impl::Mask mask{uint32_t(p.edges.size()), 0, true};
        auto       ring = [&](const Points& points) {
            if (points.size() > Impl::MAX_EDGES - p.edges.size()) {
                p.overflow = true;
                return;
            }
            for (size_t i = 0; i < points.size(); ++i) {
                const Point& a = points[i];
                const Point& b = points[(i + 1) % points.size()];
                if (!bounded(a) || !bounded(b)) {
                    mask.eligible = false;
                    return;
                }
                p.edges.push_back({int32_t(a.x()), int32_t(a.y()), int32_t(b.x()), int32_t(b.y())});
            }
        };
        for (const ExPolygon& polygon : *masks.m_masks) {
            ring(polygon.contour.points);
            for (const Polygon& hole : polygon.holes)
                ring(hole.points);
        }
        mask.count = uint32_t(p.edges.size()) - mask.start;
        found      = p.masks.emplace(&masks, mask).first;
    }
    if (p.overflow)
        return;
    const auto mask = found->second;
    for (const ExtrusionEntity* entity : source.flatten_refs()) {
        const Key key{entity, &masks, difference};
        if (p.entries.find(key) != p.entries.end())
            continue;
        if (p.entries.size() >= Impl::MAX_ENTITIES) {
            p.overflow = true;
            return;
        }
        Impl::Entry entry{uint32_t(p.queries.size()), 0, mask.eligible};
        if (mask.eligible) {
            if (const auto* path = dynamic_cast<const ExtrusionPath*>(entity))
                p.add_path(path->polyline, mask, difference, entry);
            else if (const auto* multi = dynamic_cast<const ExtrusionMultiPath*>(entity))
                for (const auto& path : multi->paths)
                    p.add_path(path.polyline, mask, difference, entry);
            else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(entity))
                for (const auto& path : loop->paths)
                    p.add_path(path.polyline, mask, difference, entry);
            else
                entry.eligible = false;
        }
        entry.count    = uint32_t(p.queries.size()) - entry.first;
        entry.eligible = entry.eligible && entry.count != 0;
        p.entries.emplace(key, entry);
        if (p.overflow)
            return;
    }
}
void LocalZClipAcceleration::prepare(const std::function<void()>& cancel)
{
    auto& p = *m_impl;
    ++p.layers;
    p.peak_edges   = std::max(p.peak_edges, p.edges.size());
    p.peak_queries = std::max(p.peak_queries, p.queries.size());
    p.peak_entries = std::max(p.peak_entries, p.entries.size());
    if (p.disabled || p.overflow || p.queries.empty() || p.edges.empty()) {
        ++p.fallback_layers;
        p.total_ms += elapsed_ms(p.start);
        return;
    }
    cancel();
    std::string error;
    if (!p.backend) {
        const char* device = std::getenv("ORCA_VULKAN_DEVICE");
        p.backend          = VulkanLocalZClipDecision::create(std::vector<ClipMaskEdge>(Impl::MAX_EDGES), device ? device : "", error);
        if (!p.backend) {
            p.fail(error);
            p.total_ms += elapsed_ms(p.start);
            return;
        }
    }
    if (!p.backend->update_edges(p.edges, error)) {
        p.fail(error);
        p.total_ms += elapsed_ms(p.start);
        return;
    }
    std::vector<uint32_t>         decisions, batch_results;
    std::vector<ClipSegmentQuery> batch;
    decisions.reserve(p.queries.size());
    for (size_t offset = 0; offset < p.queries.size(); offset += VulkanBatch::BATCH_CAPACITY) {
        cancel();
        const size_t end = std::min(p.queries.size(), offset + VulkanBatch::BATCH_CAPACITY);
        batch.assign(p.queries.begin() + offset, p.queries.begin() + end);
        if (!p.backend->compute(batch, batch_results, error)) {
            p.fail(error);
            p.total_ms += elapsed_ms(p.start);
            return;
        }
        decisions.insert(decisions.end(), batch_results.begin(), batch_results.end());
    }
    for (auto& item : p.entries) {
        auto& entry = item.second;
        entry.empty = entry.eligible && std::all_of(decisions.begin() + entry.first, decisions.begin() + entry.first + entry.count,
                                                    [](uint32_t value) { return value == 1; });
    }
    p.ready = true;
    p.total_ms += elapsed_ms(p.start);
}
bool LocalZClipAcceleration::proven_empty(const ExtrusionEntity& entity,
                                          const LocalZClipper*   include_masks,
                                          const LocalZClipper*   exclude_masks)
{
    auto& p = *m_impl;
    if (!p.ready || (include_masks && exclude_masks))
        return false;
    const auto it = p.entries.find({&entity, include_masks ? include_masks : exclude_masks, exclude_masks != nullptr});
    ++p.tested;
    const bool empty = it != p.entries.end() && it->second.empty;
    p.rejected += empty;
    return empty;
}
bool LocalZClipAcceleration::validation() const { return m_impl->validate; }
void LocalZClipAcceleration::report() const
{
    const auto&             p      = *m_impl;
    const VulkanBatchTiming timing = p.backend ? p.backend->timing() : VulkanBatchTiming{};
    BOOST_LOG_TRIVIAL(info) << "Local-Z Vulkan clipping device=" << (p.backend ? p.backend->device_name() : "none")
                            << " validation=" << p.validate << " prepare_ms=" << p.total_ms << " setup_ms=" << timing.setup_ms
                            << " upload_ms=" << timing.upload_ms << " dispatch_ms=" << timing.dispatch_readback_ms << " layers=" << p.layers
                            << " fallback_layers=" << p.fallback_layers << " tested_entities=" << p.tested
                            << " rejected_entities=" << p.rejected << " segments=" << timing.queries << " peak_edges=" << p.peak_edges
                            << " peak_queries=" << p.peak_queries << " peak_entries=" << p.peak_entries;
}
} // namespace Slic3r
