#include "LocalZMaterialPlanner.hpp"
#include "ClipperUtils.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <tuple>

namespace Slic3r {
namespace {
constexpr double Z_EPSILON = 1e-6;
using Graph                = LocalZMaterialGraph;
using Planner              = LocalZMaterialPlanner;

bool same_recipe(const std::vector<LocalZCadence::Component>& a, const std::vector<LocalZCadence::Component>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].tool != b[i].tool || std::abs(a[i].weight - b[i].weight) > 1e-9 || a[i].limits.minimum != b[i].limits.minimum ||
            a[i].limits.maximum != b[i].limits.maximum)
            return false;
    return true;
}

struct Request
{
    size_t                       domain = 0, first = 0, last = 0;
    double                       bottom = 0., top = 0.;
    LocalZCadence::Result        cadence;
    std::vector<LocalZMixBridge> independent;
};

std::shared_ptr<LocalZMixBridge> nominal_cell(
    const Graph::Slice& slice, ExPolygons masks, unsigned source, unsigned tool, size_t domain, size_t configuration, bool fallback)
{
    auto cell                    = std::make_shared<LocalZMixBridge>();
    cell->z_lo                   = slice.bottom;
    cell->z_hi                   = slice.top;
    cell->planar_fallback        = fallback;
    cell->nominal_band           = true;
    cell->material_planner       = true;
    cell->material_domains       = {domain};
    cell->material_configuration = configuration;
    cell->source_filament_ids    = {source};
    cell->zones.push_back({std::move(masks), {slice.top - slice.bottom}, {tool}, source});
    LocalZMixBridge::Pass pass;
    pass.zones       = {0};
    pass.filament_id = tool;
    pass.ceiling     = slice.top;
    pass.flat_height = slice.top - slice.bottom;
    cell->passes.push_back(std::move(pass));
    return cell;
}
} // namespace

LocalZMaterialPlanner::Plan LocalZMaterialPlanner::build(const Graph& graph, const Options& options, const std::function<void()>& cancel)
{
    Plan         plan;
    const double quantum = options.cadence.z_quantum;
    if (quantum <= 0. || !std::isfinite(options.nozzle_diameter) || options.nozzle_diameter <= 0.)
        throw std::invalid_argument("Material Local-Z requires a positive coordinate quantum and nozzle diameter");
    // Preserve graph identity, but end a planning run at a real contact change.
    // A moving strip near the top must not coarsen the same domain below it.
    std::vector<std::set<size_t>> moving_layers(graph.domains.size());
    for (const auto& contact : graph.contacts) {
        if (cancel)
            cancel();
        for (const auto& pair : contact.vertical_nodes) {
            const auto& before = graph.nodes[pair.first];
            const auto& after  = graph.nodes[pair.second];
            if (before.above.empty() || after.below.empty())
                continue;
            if (intersection_ex(offset_ex(ExPolygons{graph.mask(pair.first)}, -scale_(quantum)), ExPolygons{graph.mask(pair.second)}).empty())
                continue;
            for (size_t parent : {before.domain, after.domain})
                if (graph.region(graph.domains[parent].nodes.front()).recipe.size() > 1) {
                    moving_layers[parent].insert(before.slice);
                    moving_layers[parent].insert(after.slice);
                }
        }
    }
    std::vector<Graph::Domain>            domains;
    std::vector<size_t>                   parents, node_run(graph.nodes.size());
    std::vector<bool>                     moving_runs;
    std::vector<std::map<size_t, size_t>> run_at_slice(graph.domains.size());
    for (const auto& parent : graph.domains) {
        size_t run = size_t(-1);
        for (size_t node : parent.nodes) {
            const auto& n      = graph.nodes[node];
            const bool  moving = moving_layers[parent.id].count(n.slice) != 0;
            if (run == size_t(-1) || moving_runs[run] != moving) {
                Graph::Domain domain;
                domain.id            = domains.size();
                domain.source        = parent.source;
                domain.configuration = parent.configuration;
                domain.first_slice = domain.last_slice = n.slice;
                run                                    = domain.id;
                domains.push_back(std::move(domain));
                parents.push_back(parent.id);
                moving_runs.push_back(moving);
            }
            domains[run].nodes.push_back(node);
            domains[run].last_slice          = n.slice;
            node_run[node]                   = run;
            run_at_slice[parent.id][n.slice] = run;
        }
    }
    std::vector<std::map<size_t, ExPolygons>>           footprints(domains.size());
    std::vector<std::vector<LocalZCadence::Component>>  recipes(domains.size());
    std::vector<std::string>                            fallback(domains.size());
    std::vector<double>                                 ideal(domains.size(), 0.), periods(domains.size(), 0.);
    std::vector<std::vector<Request>>                   requests(domains.size());
    std::vector<std::vector<std::pair<double, double>>> repairs(domains.size());
    for (const auto& domain : domains) {
        if (cancel)
            cancel();
        auto& recipe = recipes[domain.id];
        recipe       = graph.region(domain.nodes.front()).recipe;
        if (recipe.empty())
            throw std::invalid_argument("Material Local-Z domain has no physical recipe");
        if (moving_runs[domain.id])
            fallback[domain.id] = "moving_material_boundary";
        for (size_t node : domain.nodes) {
            const auto& n = graph.nodes[node];
            append(footprints[domain.id][n.slice], ExPolygons{graph.mask(node)});
            if (!same_recipe(recipe, graph.region(node).recipe))
                fallback[domain.id] = "changing_recipe_target";
        }
        for (auto& sample : footprints[domain.id])
            sample.second = union_ex(sample.second);
    }
    std::map<std::pair<size_t, size_t>, size_t> contracts;
    auto                                        contact = [&](size_t a, size_t b, ContactKind kind) {
        if (a == b)
            return;
        const auto key      = std::minmax(a, b);
        const auto inserted = contracts.emplace(key, plan.contacts.size());
        if (inserted.second)
            plan.contacts.push_back({key.first, key.second, kind});
        else if (kind == ContactKind::moving_boundary)
            plan.contacts[inserted.first->second].kind = kind;
    };
    for (const auto& edge : graph.contacts) {
        for (size_t slice : edge.slices) {
            const size_t a = run_at_slice[edge.a].at(slice), b = run_at_slice[edge.b].at(slice);
            contact(a, b, moving_runs[a] || moving_runs[b] ? ContactKind::moving_boundary : ContactKind::separate_islands);
        }
        for (const auto& pair : edge.vertical_nodes)
            contact(node_run[pair.first], node_run[pair.second], ContactKind::vertical_handover);
    }
    for (size_t run = 1; run < domains.size(); ++run)
        if (parents[run - 1] == parents[run])
            contact(run - 1, run, ContactKind::vertical_handover);

    auto generate = [&](const std::vector<const Request*>& group, std::vector<LocalZMixBridge>& generated) {
        const auto&                        request       = *group.front();
        const size_t                       configuration = domains[request.domain].configuration;
        std::vector<LocalZMixBridge::Zone> zones;
        std::vector<double>                maximum;
        double                             minimum = std::numeric_limits<double>::infinity();
        for (const auto* item : group) {
            const auto& domain = domains[item->domain];
            if (domain.configuration != configuration)
                return false;
            LocalZMixBridge::Zone zone;
            for (auto sample = footprints[domain.id].lower_bound(item->first);
                 sample != footprints[domain.id].end() && sample->first <= item->last; ++sample)
                append(zone.masks, sample->second);
            zone.masks = union_ex(zone.masks);
            // The temporary source is a domain identity, not a palette index.
            // Recover authored source IDs before publication.
            zone.source_filament_id = unsigned(domain.id + 1);
            for (const auto& bead : item->cadence.beads) {
                zone.component_ids.push_back(bead.tool);
                zone.heights.push_back(bead.height);
            }
            for (const auto& component : recipes[domain.id]) {
                minimum = std::min(minimum, component.limits.minimum);
                maximum.resize(std::max(maximum.size(), size_t(component.tool)), 0.);
                auto& cap = maximum[component.tool - 1];
                cap       = cap == 0. ? component.limits.maximum : std::min(cap, component.limits.maximum);
            }
            zones.push_back(std::move(zone));
        }
        generated = LocalZMixBridge::build(request.bottom, request.top, minimum, maximum, std::move(zones), {}, options.transition_factor,
                                           .5 * options.nozzle_diameter + .1, {}, false);
        if (generated.empty())
            return false;
        std::set<size_t> represented;
        for (auto& bridge : generated) {
            // Equal ordered slots have translation-invariant monotone interface
            // envelopes: each bead remains between the authored slot bounds.
            // Anchor/extra-color topology does not have that certificate yet.
            if (!bridge.shared_stack || !bridge.extra_colors.empty() || !bridge.tongue_planes.empty() ||
                (group.size() > 1 && !bridge.split_runs.empty()))
                return false;
            bridge.material_planner       = true;
            bridge.material_configuration = configuration;
            for (unsigned source : bridge.source_filament_ids) {
                bridge.material_domains.push_back(size_t(source - 1));
                represented.insert(size_t(source - 1));
            }
            const auto first = std::min_element(group.begin(), group.end(),
                                                [](const auto* a, const auto* b) { return a->first < b->first; });
            const auto last  = std::max_element(group.begin(), group.end(), [](const auto* a, const auto* b) { return a->last < b->last; });
            ExPolygons neighbors;
            for (size_t slice = (*first)->first; slice <= (*last)->last; ++slice) {
                LocalZMixBridge::LayerFootprint footprint;
                footprint.layer_id = graph.slices[slice].layer_id;
                footprint.slice_z  = graph.slices[slice].sample_z;
                footprint.height   = graph.slices[slice].top - graph.slices[slice].bottom;
                for (const auto& zone : bridge.zones) {
                    const auto domain = size_t(zone.source_filament_id - 1);
                    const auto found  = footprints[domain].find(slice);
                    footprint.zones.push_back(found == footprints[domain].end() ? ExPolygons{} : found->second);
                    append(footprint.masks, footprint.zones.back());
                }
                footprint.masks = union_ex(footprint.masks);
                if (footprint.masks.empty())
                    return false;
                append(neighbors, diff_ex(graph.slices[slice].outline, offset_ex(footprint.masks, scale_(.00001))));
                bridge.layer_footprints.push_back(std::move(footprint));
            }
            neighbors = union_ex(neighbors);
            ExPolygons swept;
            for (const auto& zone : bridge.zones)
                append(swept, zone.masks);
            if (!LocalZMixBridge::clean_residual(intersection_ex(union_ex(swept), neighbors)).empty())
                return false;
            bridge.follows_surface    = bridge.layer_footprints.size() > 1;
            bridge.boundary_tolerance = .25 * options.nozzle_diameter;
            if (!neighbors.empty() && !bridge.pin_foreign_boundary(neighbors, bridge.nozzle_clearance) &&
                bridge.advances_near(neighbors, bridge.nozzle_clearance))
                return false;
            const size_t first_slice   = (*first)->first;
            bridge.lower_support_masks = first_slice ? graph.slices[first_slice - 1].outline : graph.slices[first_slice].outline;
            if (bridge.follows_surface && !bridge.fit_contours(bridge.boundary_tolerance, nullptr, .5 * options.nozzle_diameter))
                return false;
            // Check each tool's actual interface dimensions, including pinned
            // boundaries, rather than treating the smallest tool minimum as all.
            for (size_t pass = 0; pass < bridge.passes.size(); ++pass) {
                const auto&           bead = bridge.passes[pass];
                LocalZCadence::Limits limits;
                limits.maximum = std::numeric_limits<double>::infinity();
                for (size_t domain : bridge.material_domains)
                    for (const auto& component : recipes[domain])
                        if (component.tool == bead.filament_id) {
                            limits.minimum = std::max(limits.minimum, component.limits.minimum);
                            limits.maximum = std::min(limits.maximum, component.limits.maximum);
                        }
                if (bridge.split_runs.empty())
                    for (const auto& zone : bridge.zones)
                        if (zone.heights[bead.component] < limits.minimum - 1e-6 || zone.heights[bead.component] > limits.maximum + 1e-6)
                            return false;
                for (const auto& line : to_lines(bridge.masks(bead)))
                    for (const auto& point : {line.a, line.midpoint()}) {
                        const auto   heights = bridge.surfaces(point, pass);
                        const double h       = heights.second - heights.first;
                        if (h < limits.minimum - 1e-5 || h > limits.maximum + 1e-5)
                            return false;
                    }
            }
            bridge.source_filament_ids.clear();
            for (auto& zone : bridge.zones) {
                zone.source_filament_id = domains.at(zone.source_filament_id - 1).source;
                bridge.source_filament_ids.push_back(zone.source_filament_id);
            }
            bridge.authored_zones = bridge.zones;
        }
        return represented.size() == group.size();
    };

    for (const auto& domain : domains) {
        if (cancel)
            cancel();
        if (recipes[domain.id].size() == 1)
            continue;
        const double bottom = graph.slices[domain.first_slice].bottom, top = graph.slices[domain.last_slice].top;
        // The unconstrained candidate explains what this recipe requires even
        // when the real feature is too short to contain a complete cycle.
        auto       settings  = options.cadence;
        const auto candidate = LocalZCadence::minimum(recipes[domain.id], settings);
        ideal[domain.id]     = candidate.minimum_period;
        periods[domain.id]   = candidate.period;
        if (!candidate && fallback[domain.id].empty())
            fallback[domain.id] = LocalZCadence::status_name(candidate.status);
        if (!fallback[domain.id].empty())
            continue;
        if (candidate.period > top - bottom + Z_EPSILON) {
            fallback[domain.id] = "domain_too_short";
            continue;
        }
        settings.maximum_period = std::min(settings.maximum_period, top - bottom);
        const auto tiled        = LocalZCadence::tile(recipes[domain.id], std::round((top - bottom) / quantum) * quantum, settings);
        if (!tiled) {
            fallback[domain.id] = LocalZCadence::status_name(tiled.status);
            continue;
        }
        double lo = bottom;
        for (const auto& cycle : tiled.cycles) {
            Request request;
            request.domain  = domain.id;
            request.bottom  = lo;
            request.top     = lo + cycle.period;
            request.cadence = cycle;
            request.first   = domain.first_slice;
            while (graph.slices[request.first].top <= lo + Z_EPSILON && request.first < domain.last_slice)
                ++request.first;
            request.last = request.first;
            while (request.last < domain.last_slice && graph.slices[request.last + 1].bottom < request.top - Z_EPSILON)
                ++request.last;
            if (!generate({&request}, request.independent))
                request.independent.clear();
            requests[domain.id].push_back(std::move(request));
            lo += cycle.period;
        }
        auto&             cycles = requests[domain.id];
        std::vector<bool> repair(cycles.size(), false), boundary(cycles.size() + 1, false);
        boundary.front() = boundary.back() = true;
        for (size_t i = 1; i < cycles.size(); ++i) {
            const auto& cycle = cycles[i];
            boundary[i]       = std::abs(cycle.bottom - graph.slices[cycle.first].bottom) < Z_EPSILON;
        }
        for (size_t i = 0; i < cycles.size(); ++i)
            if (cycles[i].independent.empty()) {
                size_t begin = i, end = i + 1;
                while (!boundary[begin])
                    --begin;
                while (!boundary[end])
                    ++end;
                // Nominal repair must cover whole original slabs. Cutting one
                // at an arbitrary cadence plane can create sub-minimum beads.
                std::fill(repair.begin() + begin, repair.begin() + end, true);
            }
        for (size_t i = 0; i < cycles.size();) {
            if (!repair[i]) {
                ++i;
                continue;
            }
            const size_t begin = i;
            while (i < cycles.size() && repair[i])
                ++i;
            const auto range = std::make_pair(cycles[begin].bottom, cycles[i - 1].top);
            repairs[domain.id].push_back(range);
            plan.diagnostics.push_back(
                {domain.id, "contour_or_interface_contract", range.first, range.second, candidate.minimum_period, candidate.period});
        }
        size_t kept = 0;
        for (size_t i = 0; i < cycles.size(); ++i)
            if (!repair[i]) {
                if (kept != i)
                    cycles[kept] = std::move(cycles[i]);
                ++kept;
            }
        cycles.resize(kept);
        if (!cycles.empty()) {
            // A repaired cycle was not emitted. Its rounding correction must
            // not remain in the reported height debt. Nominal repairs account
            // for their own volume debt separately below.
            std::vector<double> emitted_debt(recipes[domain.id].size(), 0.);
            for (const auto& cycle : cycles)
                for (size_t i = 0; i < cycle.cadence.error.size(); ++i)
                    emitted_debt[i] -= cycle.cadence.error[i];
            plan.diagnostics.push_back({domain.id, tiled.closure_adjusted ? "finite_domain_closure" : "minimum_cadence", bottom, top,
                                        candidate.minimum_period, candidate.period, std::move(emitted_debt)});
        }
    }

    // Repair ranges end on complete nominal slabs and complete recipe cycles.
    // An unsuccessful interface cannot strand a remainder or discard valid
    // cycles outside that range. Nominal cells still own their island paths.
    std::vector<std::vector<double>> nominal_debt(graph.domains.size());
    for (const auto& domain : domains) {
        const bool pure = recipes[domain.id].size() == 1;
        if (!pure && fallback[domain.id].empty() && repairs[domain.id].empty())
            continue;
        auto& debt = nominal_debt[parents[domain.id]];
        if (debt.empty())
            debt.assign(recipes[domain.id].size(), 0.);
        for (const auto& sample : footprints[domain.id]) {
            if (cancel)
                cancel();
            const auto& slice = graph.slices[sample.first];
            if (!pure && fallback[domain.id].empty() &&
                std::none_of(repairs[domain.id].begin(), repairs[domain.id].end(), [&](const auto& range) {
                    return slice.bottom >= range.first - Z_EPSILON && slice.top <= range.second + Z_EPSILON;
                }))
                continue;
            const auto& recipe = graph
                                     .region(*std::find_if(domain.nodes.begin(), domain.nodes.end(),
                                                           [&](size_t n) { return graph.nodes[n].slice == sample.first; }))
                                     .recipe;
            const double volume     = area(sample.second) * SCALING_FACTOR * SCALING_FACTOR * (slice.top - slice.bottom);
            const double weight_sum = std::accumulate(recipe.begin(), recipe.end(), 0.,
                                                      [](double sum, const auto& c) { return sum + c.weight; });
            if (recipe.size() != debt.size() || weight_sum <= 0.)
                throw std::runtime_error("Material Local-Z cannot terminate a changing component topology");
            size_t selected = recipe.size();
            for (size_t i = 0; i < recipe.size(); ++i) {
                debt[i] += volume * recipe[i].weight / weight_sum;
                const double h = slice.top - slice.bottom;
                if (h >= recipe[i].limits.minimum - Z_EPSILON && h <= recipe[i].limits.maximum + Z_EPSILON &&
                    (selected == recipe.size() || debt[i] > debt[selected]))
                    selected = i;
            }
            if (selected == recipe.size())
                throw std::runtime_error("Material Local-Z has no valid nominal termination at Z=" + std::to_string(slice.top) +
                                         ". The nominal layer must satisfy the active printer layer-height limits.");
            debt[selected] -= volume;
            auto cell = nominal_cell(slice, sample.second, domain.source, recipe[selected].tool, domain.id, domain.configuration, !pure);
            plan.cells.push_back({std::move(cell), domain.configuration, {domain.id}});
            ++plan.nominal_cells;
        }
        if (!pure && !fallback[domain.id].empty())
            plan.diagnostics.push_back({domain.id,
                                        fallback[domain.id],
                                        graph.slices[domain.first_slice].bottom,
                                        graph.slices[domain.last_slice].top,
                                        ideal[domain.id],
                                        periods[domain.id],
                                        {},
                                        debt});
        else if (!pure)
            for (auto& diagnostic : plan.diagnostics)
                if (diagnostic.domain == domain.id && diagnostic.reason == "contour_or_interface_contract")
                    diagnostic.nominal_volume_debt = debt;
    }

    // Equal period/phase is a contact contract, not an object-wide scheduling
    // grid. Quantization keys only identify already solved cycle endpoints.
    using Key = std::tuple<long long, long long, size_t>;
    std::map<Key, std::vector<const Request*>> groups;
    for (const auto& domain : requests)
        for (const auto& request : domain)
            groups[{std::llround(request.bottom / quantum), std::llround(request.top / quantum), domains[request.domain].configuration}]
                .push_back(&request);
    for (const auto& group : groups) {
        if (cancel)
            cancel();
        std::vector<LocalZMixBridge> joined;
        const bool                   combined = options.join_equal_periods && group.second.size() > 1 && generate(group.second, joined);
        auto                         publish  = [&](const LocalZMixBridge& bridge) {
            auto geometry = std::make_shared<LocalZMixBridge>(bridge);
            if (geometry->material_domains.size() > 1)
                ++plan.joined_cycles;
            for (size_t domain : geometry->material_domains)
                if (std::abs(geometry->z_hi - geometry->z_lo - periods[domain]) < Z_EPSILON)
                    ++plan.minimum_cycles;
                else
                    ++plan.closure_cycles;
            plan.cells.push_back({std::move(geometry), bridge.material_configuration, bridge.material_domains});
        };
        if (combined)
            for (const auto& bridge : joined)
                publish(bridge);
        else
            for (const auto* request : group.second)
                for (const auto& bridge : request->independent)
                    publish(bridge);
    }
    std::set<std::pair<size_t, size_t>> joined_contacts;
    for (const auto& cell : plan.cells) {
        for (size_t domain : cell.domains) {
            if (recipes[domain].size() == 1)
                continue;
            double volume = 0.;
            for (const auto& sample : footprints[domain]) {
                const auto&  slice = graph.slices[sample.first];
                const double h     = std::max(0., std::min(slice.top, cell.geometry->z_hi) - std::max(slice.bottom, cell.geometry->z_lo));
                volume += area(sample.second) * SCALING_FACTOR * SCALING_FACTOR * h;
            }
            plan.mixed_volume += volume;
            if (cell.geometry->planar_fallback)
                plan.nominal_volume += volume;
            else if (std::abs(cell.geometry->z_hi - cell.geometry->z_lo - periods[domain]) < Z_EPSILON)
                plan.minimum_volume += volume;
            else
                plan.closure_volume += volume;
            if (cell.domains.size() > 1 && !cell.geometry->planar_fallback)
                plan.joined_volume += volume;
        }
        for (size_t a = 0; a < cell.domains.size(); ++a)
            for (size_t b = a + 1; b < cell.domains.size(); ++b)
                joined_contacts.insert(std::minmax(cell.domains[a], cell.domains[b]));
    }
    for (auto& contract : plan.contacts) {
        if (contract.kind != ContactKind::separate_islands)
            continue;
        if (std::abs(periods[contract.a] - periods[contract.b]) > Z_EPSILON)
            contract.kind = ContactKind::unequal_period;
        if (joined_contacts.count(std::minmax(contract.a, contract.b)))
            contract.kind = ContactKind::equal_period_join;
    }
    // Adjacent cells of the same physical material on the same planes form one
    // island. Union them before path generation, avoiding duplicate internal
    // perimeters; different physical materials still own separate boundaries.
    std::map<std::tuple<long long, long long, size_t, unsigned>, size_t> nominal_groups;
    std::vector<Cell>                                                    compact;
    for (const auto& cell : plan.cells) {
        if (!cell.geometry->nominal_band) {
            compact.push_back(cell);
            continue;
        }
        const auto key      = std::make_tuple(std::llround(cell.geometry->z_lo / quantum), std::llround(cell.geometry->z_hi / quantum),
                                              cell.configuration, cell.geometry->passes.front().filament_id);
        const auto inserted = nominal_groups.emplace(key, compact.size());
        if (inserted.second) {
            compact.push_back(cell);
            continue;
        }
        auto& previous = compact[inserted.first->second];
        auto  merged   = std::make_shared<LocalZMixBridge>(*previous.geometry);
        for (const auto& zone : cell.geometry->zones) {
            merged->passes.front().zones.push_back(merged->zones.size());
            merged->zones.push_back(zone);
        }
        merged->planar_fallback |= cell.geometry->planar_fallback;
        append(merged->material_domains, cell.domains);
        append(merged->source_filament_ids, cell.geometry->source_filament_ids);
        previous.domains  = merged->material_domains;
        previous.geometry = std::move(merged);
    }
    plan.cells         = std::move(compact);
    plan.nominal_cells = nominal_groups.size();
    for (auto& diagnostic : plan.diagnostics)
        diagnostic.source_domain = parents[diagnostic.domain];
    return plan;
}
} // namespace Slic3r
