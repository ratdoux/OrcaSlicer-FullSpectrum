#include "LocalZMaterialGraph.hpp"
#include "ClipperUtils.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <stdexcept>

namespace Slic3r {
const ExPolygon& LocalZMaterialGraph::mask(size_t node) const
{
    const auto& n = nodes.at(node);
    return slices[n.slice].regions[n.region].masks[n.polygon];
}

const LocalZMaterialGraph::Region& LocalZMaterialGraph::region(size_t node) const
{
    const auto& n = nodes.at(node);
    return slices[n.slice].regions[n.region];
}

ExPolygons LocalZMaterialGraph::footprint(size_t domain, size_t slice) const
{
    ExPolygons masks;
    for (size_t node : nodes_by_slice.at(slice))
        if (nodes[node].domain == domain)
            masks.push_back(mask(node));
    return union_ex(masks);
}

LocalZMaterialGraph LocalZMaterialGraph::build(std::vector<Slice> input, const std::function<void()>& cancel)
{
    LocalZMaterialGraph graph;
    graph.slices = std::move(input);
    graph.nodes_by_slice.resize(graph.slices.size());
    std::vector<size_t> parent;
    auto                root = [&](size_t node) {
        while (parent[node] != node) {
            parent[node] = parent[parent[node]];
            node         = parent[node];
        }
        return node;
    };
    for (size_t slice = 0; slice < graph.slices.size(); ++slice) {
        if (cancel)
            cancel();
        const auto& sample = graph.slices[slice];
        if (!std::isfinite(sample.bottom) || !std::isfinite(sample.top) || !std::isfinite(sample.sample_z) || sample.top <= sample.bottom ||
            sample.sample_z < sample.bottom - 1e-6 || sample.sample_z > sample.top + 1e-6 ||
            (slice && sample.bottom < graph.slices[slice - 1].top - 1e-6))
            throw std::invalid_argument("Invalid Local-Z material geometry samples");
        auto& current = graph.nodes_by_slice[slice];
        for (size_t region = 0; region < sample.regions.size(); ++region)
            for (size_t polygon = 0; polygon < sample.regions[region].masks.size(); ++polygon) {
                const auto& mask = sample.regions[region].masks[polygon];
                if (mask.empty() || mask.area() <= 0.)
                    continue;
                const size_t node = graph.nodes.size();
                parent.push_back(node);
                current.push_back(node);
                Node entry;
                entry.slice   = slice;
                entry.region  = region;
                entry.polygon = polygon;
                entry.bounds  = get_extents(mask);
                graph.nodes.push_back(std::move(entry));
            }
        std::sort(current.begin(), current.end(),
                  [&](size_t a, size_t b) { return graph.nodes[a].bounds.min.x() < graph.nodes[b].bounds.min.x(); });
        if (slice == 0 || std::abs(sample.bottom - graph.slices[slice - 1].top) > 1e-6)
            continue;
        // Only actual cross-section overlap establishes a continuation. A tiny
        // or fast-moving feature that needs additional samples stays a topology
        // event; no arbitrary nozzle-width filter deletes it or joins an air gap.
        for (size_t a : graph.nodes_by_slice[slice - 1]) {
            const auto& before = graph.region(a);
            for (size_t b : current) {
                if (graph.nodes[b].bounds.min.x() > graph.nodes[a].bounds.max.x())
                    break;
                const auto& after = graph.region(b);
                if (before.source != after.source || before.configuration != after.configuration ||
                    !graph.nodes[a].bounds.overlap(graph.nodes[b].bounds) ||
                    intersection_ex(ExPolygons{graph.mask(a)}, ExPolygons{graph.mask(b)}).empty())
                    continue;
                graph.nodes[a].above.push_back(b);
                graph.nodes[b].below.push_back(a);
                const size_t ra = root(a), rb = root(b);
                parent[std::max(ra, rb)] = std::min(ra, rb);
            }
        }
    }
    std::map<size_t, size_t> domains;
    for (size_t node = 0; node < graph.nodes.size(); ++node) {
        auto&        n         = graph.nodes[node];
        const size_t component = root(node);
        auto         found     = domains.find(component);
        if (found == domains.end()) {
            Domain domain;
            domain.id          = graph.domains.size();
            domain.first_slice = domain.last_slice = n.slice;
            domain.source                          = graph.region(node).source;
            domain.configuration                   = graph.region(node).configuration;
            found                                  = domains.emplace(component, domain.id).first;
            graph.domains.push_back(std::move(domain));
        }
        n.domain     = found->second;
        auto& domain = graph.domains[n.domain];
        domain.nodes.push_back(node);
        domain.last_slice = std::max(domain.last_slice, n.slice);
        domain.births += n.below.empty();
        domain.deaths += n.above.empty();
        domain.splits += n.above.size() > 1;
        domain.merges += n.below.size() > 1;
    }
    std::map<std::pair<size_t, size_t>, size_t> contacts;
    auto                                        contact = [&](size_t a, size_t b) -> Contact& {
        const auto key   = std::minmax(graph.nodes[a].domain, graph.nodes[b].domain);
        auto       found = contacts.find(key);
        if (found == contacts.end()) {
            found = contacts.emplace(key, graph.contacts.size()).first;
            graph.contacts.push_back({key.first, key.second, {}, {}});
        }
        return graph.contacts[found->second];
    };
    for (size_t slice = 0; slice < graph.slices.size(); ++slice) {
        if (cancel)
            cancel();
        const auto& current = graph.nodes_by_slice[slice];
        for (size_t ai = 0; ai < current.size(); ++ai) {
            const size_t a        = current[ai];
            const auto   expanded = offset_ex(ExPolygons{graph.mask(a)}, scale_(.00001));
            const auto   bounds   = graph.nodes[a].bounds.inflated(scale_(.00001));
            for (size_t bi = ai + 1; bi < current.size(); ++bi) {
                const size_t b = current[bi];
                if (graph.nodes[b].bounds.min.x() > bounds.max.x())
                    break;
                if (graph.nodes[a].domain == graph.nodes[b].domain || !bounds.overlap(graph.nodes[b].bounds) ||
                    intersection_ex(expanded, ExPolygons{graph.mask(b)}).empty())
                    continue;
                auto& layers = contact(a, b).slices;
                if (layers.empty() || layers.back() != slice)
                    layers.push_back(slice);
            }
        }
        if (slice == 0 || std::abs(graph.slices[slice].bottom - graph.slices[slice - 1].top) > 1e-6)
            continue;
        for (size_t a : graph.nodes_by_slice[slice - 1])
            for (size_t b : current) {
                if (graph.nodes[b].bounds.min.x() > graph.nodes[a].bounds.max.x())
                    break;
                if (graph.nodes[a].domain == graph.nodes[b].domain || !graph.nodes[a].bounds.overlap(graph.nodes[b].bounds) ||
                    intersection_ex(ExPolygons{graph.mask(a)}, ExPolygons{graph.mask(b)}).empty())
                    continue;
                auto& layers = contact(a, b).vertical_slices;
                contact(a, b).vertical_nodes.emplace_back(a, b);
                if (layers.empty() || layers.back() != slice - 1)
                    layers.push_back(slice - 1);
            }
    }
    return graph;
}
} // namespace Slic3r
