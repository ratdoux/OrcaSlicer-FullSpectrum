#include "SeamAlignmentSearch.hpp"
#include "libslic3r/Compute/SeamNeighbors.hpp"
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <list>
#include <unordered_set>
#include <stdexcept>

namespace Slic3r {
namespace {
thread_local SeamAlignmentSearch* current_search = nullptr;
using Clock                                      = std::chrono::steady_clock;
double   elapsed(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
uint32_t bits(float f)
{
    uint32_t value;
    std::memcpy(&value, &f, sizeof(value));
    return value;
}
struct Key
{
    size_t                  layer;
    std::array<uint32_t, 4> query;
    bool                    operator==(const Key& rhs) const { return layer == rhs.layer && query == rhs.query; }
};
struct Hash
{
    size_t operator()(const Key& key) const
    {
        size_t h = key.layer;
        for (auto v : key.query)
            h ^= size_t(v) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};
Key key_for(size_t layer, const SeamNeighborQuery& q) { return {layer, {bits(q[0]), bits(q[1]), bits(q[2]), bits(q[3])}}; }
} // namespace
struct SeamAlignmentSearch::Impl
{
    const std::vector<PrintObjectSeamData::LayerSeams>& layers;
    struct Entry
    {
        std::vector<size_t>      indices;
        std::list<Key>::iterator order;
        bool                     gpu;
    };
    std::unordered_map<Key, Entry, Hash> cache;
    std::list<Key>                       order;
    std::vector<size_t>                  scratch;
    bool                                 detailed_timing = false;
    std::unordered_set<Key, Hash>        prefetched;
    std::unique_ptr<VulkanSeamNeighbors> backend;
    std::string                          mode, prefetch_policy;
    bool                                 disabled = false;
    size_t                               capacity = 0, bytes = 0, peak_bytes = 0;
    uint64_t          calls = 0, hits = 0, gpu_hits = 0, gpu_queries = 0, gpu_fallbacks = 0, cpu_queries = 0, evictions = 0, validated = 0;
    uint64_t          ineligible = 0;
    double            cpu_ms = 0, lookup_ms = 0, prefetch_ms = 0;
    Clock::time_point start = Clock::now();
    explicit Impl(const std::vector<PrintObjectSeamData::LayerSeams>& input) : layers(input)
    {
        const char* value = std::getenv("ORCA_SEAM_ALIGNMENT");
        mode              = value ? value : "cpu";
        if (mode != "cpu" && mode != "profile" && mode != "cache" && mode != "vulkan" && mode != "validate")
            throw std::runtime_error("ORCA_SEAM_ALIGNMENT must be cpu, profile, cache, vulkan, or validate");
        detailed_timing = mode == "profile" || mode == "validate";
        value           = std::getenv("ORCA_SEAM_NEIGHBORS_PREFETCH");
        prefetch_policy = value ? value : "seeds";
        if (prefetch_policy != "seeds" && prefetch_policy != "all")
            throw std::runtime_error("Invalid seam prefetch policy");
        for (const auto& layer : layers)
            if (layer.points_tree && layer.points_tree->node_indices().size() <= (1u << 20))
                capacity = std::max(capacity, layer.points_tree->node_indices().size());
    }
    std::vector<size_t> reference(size_t layer, const SeamNeighborQuery& q)
    {
        auto t      = detailed_timing ? Clock::now() : Clock::time_point{};
        auto result = find_nearby_points(*layers[layer].points_tree, Vec3f(q[0], q[1], q[2]), q[3]);
        if (detailed_timing)
            cpu_ms += elapsed(t);
        ++cpu_queries;
        return result;
    }
    void store(const Key& key, std::vector<size_t> indices, bool gpu)
    {
        if (cache.find(key) != cache.end())
            return;
        const size_t     needed = 192 + indices.capacity() * sizeof(size_t);
        constexpr size_t LIMIT  = 64u * 1024 * 1024;
        if (needed > LIMIT)
            return;
        while (!order.empty() && (bytes + needed > LIMIT || cache.size() >= 131072)) {
            auto it = cache.find(order.front());
            bytes -= 192 + it->second.indices.capacity() * sizeof(size_t);
            cache.erase(it);
            order.pop_front();
            ++evictions;
        }
        order.push_back(key);
        cache.emplace(key, Entry{std::move(indices), std::prev(order.end()), gpu});
        bytes += needed;
        peak_bytes = std::max(peak_bytes, bytes);
    }
    void prefetch(size_t layer, const SeamNeighborQuery& requested)
    {
        if (disabled || mode == "cache" || mode == "profile" || prefetched.size() >= 8192 ||
            layers[layer].points_tree->node_indices().size() > (1u << 20))
            return;
        // Radius/Z belong to the query, not necessarily to this target layer's flow.
        const Key group{layer, {bits(requested[2]), bits(requested[3]), 0, 0}};
        if (!prefetched.insert(group).second)
            return;
        auto        t = Clock::now();
        std::string error;
        auto        fail = [&] {
            disabled = true;
            if (mode == "validate")
                throw std::runtime_error("Seam alignment Vulkan unavailable: " + error);
            BOOST_LOG_TRIVIAL(warning) << "Seam alignment Vulkan fallback: " << error;
        };
        if (!backend) {
            const char* device = std::getenv("ORCA_VULKAN_DEVICE");
            backend            = VulkanSeamNeighbors::create(capacity, device ? device : "", error);
            if (!backend) {
                fail();
                prefetch_ms += elapsed(t);
                return;
            }
        }
        const auto&                   source = layers[layer];
        const auto&                   layout = source.points_tree->node_indices();
        std::vector<SeamNeighborNode> nodes(layout.size());
        for (size_t i = 0; i < layout.size(); ++i)
            if (layout[i] != PrintObjectSeamData::SeamCandidatesTree::npos) {
                const auto& point = source.points[layout[i]].position;
                nodes[i].position = {point.x(), point.y(), point.z(), 0};
                nodes[i].meta[0]  = uint32_t(layout[i]);
            }
        if (!backend->update(nodes, error)) {
            fail();
            prefetch_ms += elapsed(t);
            return;
        }
        std::vector<SeamNeighborQuery> queries{requested};
        for (size_t adjacent : {layer ? layer - 1 : layers.size(), layer + 1})
            if (adjacent < layers.size()) {
                const auto& from = layers[adjacent];
                auto        add  = [&](size_t index) {
                    const auto&       p = from.points[index].position;
                    SeamNeighborQuery q{p.x(), p.y(), requested[2], requested[3]};
                    if (VulkanSeamNeighbors::supports_query(q) && cache.find(key_for(layer, q)) == cache.end())
                        queries.push_back(q);
                };
                if (prefetch_policy == "all")
                    for (size_t i = 0; i < from.points.size(); ++i)
                        add(i);
                else
                    for (const auto& perimeter : from.perimeters)
                        if (perimeter.seam_index < from.points.size())
                            add(perimeter.seam_index);
            }
        std::vector<SeamNeighborQuery>  batch;
        std::vector<SeamNeighborResult> results;
        for (size_t first = 0; first < queries.size(); first += VulkanBatch::BATCH_CAPACITY) {
            batch.assign(queries.begin() + first, queries.begin() + std::min(queries.size(), first + VulkanBatch::BATCH_CAPACITY));
            if (!backend->compute(batch, results, error)) {
                fail();
                break;
            }
            gpu_queries += batch.size();
            for (size_t i = 0; i < results.size(); ++i) {
                const auto&         r = results[i];
                std::vector<size_t> found;
                if (r.fallback) {
                    ++gpu_fallbacks;
                    found = reference(layer, batch[i]);
                } else {
                    found.assign(r.indices.begin(), r.indices.begin() + r.count);
                    for (size_t index : found)
                        if (index >= source.points.size())
                            throw std::runtime_error("Invalid GPU seam index");
                    if (mode == "validate") {
                        if (found != reference(layer, batch[i]))
                            throw std::runtime_error("GPU seam neighbor order/content mismatch");
                        ++validated;
                    }
                }
                store(key_for(layer, batch[i]), std::move(found), !r.fallback);
            }
        }
        prefetch_ms += elapsed(t);
    }
};
SeamAlignmentSearch::SeamAlignmentSearch(const std::vector<PrintObjectSeamData::LayerSeams>& layers)
    : m_impl(std::make_unique<Impl>(layers)), m_previous(current_search)
{
    if (m_impl->mode != "cpu")
        current_search = this;
}
SeamAlignmentSearch* SeamAlignmentSearch::active() { return current_search; }
const std::vector<size_t>& SeamAlignmentSearch::neighbors(size_t layer, const Vec3f& center, float radius)
{
    auto& p = *m_impl;
    const auto started = p.detailed_timing ? Clock::now() : Clock::time_point{};
    auto       finish  = [&](const std::vector<size_t>& result) -> const std::vector<size_t>& {
        if (p.detailed_timing)
            p.lookup_ms += elapsed(started);
        return result;
    };
    ++p.calls;
    const SeamNeighborQuery query{center.x(), center.y(), center.z(), radius};
    const auto              key = key_for(layer, query);
    if (p.layers[layer].points.empty()) {
        p.scratch.clear();
        return finish(p.scratch);
    }
    // Degenerate searches retain the original CPU semantics.
    if (!VulkanSeamNeighbors::supports_query(query)) {
        ++p.ineligible;
        p.scratch = p.reference(layer, query);
        return finish(p.scratch);
    }
    auto it = p.cache.find(key);
    if (it == p.cache.end() && (p.mode == "vulkan" || p.mode == "validate")) {
        p.prefetch(layer, query);
        it = p.cache.find(key);
    }
    if (it != p.cache.end()) {
        ++p.hits;
        p.gpu_hits += it->second.gpu;
        p.order.splice(p.order.end(), p.order, it->second.order);
        if (p.mode == "profile" || p.mode == "validate") {
            const auto expected = p.reference(layer, query);
            if (it->second.indices != expected)
                throw std::runtime_error("Cached seam neighborhood mismatch");
        }
        return finish(it->second.indices);
    }
    p.scratch = p.reference(layer, query);
    p.store(key, p.scratch, false);
    return finish(p.scratch);
}
SeamAlignmentSearch::~SeamAlignmentSearch()
{
    current_search = m_previous;
    try {
        const auto& p = *m_impl;
        // CPU mode only times the whole scope; individual lookups stay uninstrumented.
        nlohmann::json report = {{"mode", p.mode},
                                 {"prefetch_policy", p.prefetch_policy},
                                 {"calls", p.calls},
                                 {"hits", p.hits},
                                 {"gpu_hits", p.gpu_hits},
                                 {"gpu_queries", p.gpu_queries},
                                 {"gpu_fallbacks", p.gpu_fallbacks},
                                 {"ineligible", p.ineligible},
                                 {"cpu_queries", p.cpu_queries},
                                 {"validated", p.validated},
                                 {"evictions", p.evictions},
                                 {"peak_cache_bytes", p.peak_bytes},
                                 {"cpu_ms", p.cpu_ms},
                                 {"lookup_ms", p.lookup_ms},
                                 {"prefetch_ms", p.prefetch_ms},
                                 {"alignment_ms", elapsed(p.start)},
                                 {"device", p.backend ? p.backend->device_name() : "none"}};
        BOOST_LOG_TRIVIAL(info) << "Seam alignment compute " << report.dump();
        if (const char* path = std::getenv("ORCA_SEAM_ALIGNMENT_REPORT"); path && *path) {
            std::ofstream output(std::filesystem::u8path(path), std::ios::app);
            output << report.dump() << '\n';
        }
    } catch (...) {
        BOOST_LOG_TRIVIAL(warning) << "Failed to report seam alignment counters";
    }
}
} // namespace Slic3r
