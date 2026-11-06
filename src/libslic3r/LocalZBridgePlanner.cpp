#include "LocalZBridgePlanner.hpp"
#include "ClipperUtils.hpp"
#include "Layer.hpp"
#include "Print.hpp"
#include <algorithm>
#include <numeric>
#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>
namespace Slic3r {
namespace {
struct LocalZBridgeAlignment
{
    std::vector<unsigned int> slot_filaments;
    // Phase-locked windows only: the key of every slot.
    std::vector<LocalZBridgeKey> slot_keys;
    // Per group member, the absolute Z of every slot interface.
    std::vector<std::vector<double>> member_bounds;
};

struct LocalZBridgeSlotRef
{
    std::shared_ptr<const LocalZBridgeField> field;
    size_t                                   slot{0};
};
} // namespace

LocalZBridgeWindowLayout compute_local_z_bridge_window_layout(size_t layer_count, size_t anchor, double nominal_height, double longest_cycle)
{
    LocalZBridgeWindowLayout result;
    result.anchor = anchor;
    if (anchor >= layer_count || longest_cycle <= EPSILON || nominal_height <= EPSILON)
        return result;
    result.layers = std::max<size_t>(1, size_t(std::ceil(longest_cycle / nominal_height - 1e-6)));
    result.count  = std::max<size_t>(1, (layer_count - anchor) / result.layers);
    return result;
}
double local_z_max_layer_height_for_extruder(const PrintConfig& print_config, unsigned int extruder_id)
{
    const size_t extruder_idx    = extruder_id > 0 ? size_t(extruder_id - 1) : 0;
    const double configured_max  = print_config.max_layer_height.values.empty() ? 0.0 : print_config.max_layer_height.get_at(extruder_idx);
    const double nozzle_diameter = print_config.nozzle_diameter.values.empty() ? 0.4 : print_config.nozzle_diameter.get_at(extruder_idx);
    return std::max(0.01, configured_max > EPSILON ? configured_max : 0.75 * nozzle_diameter);
}
ExPolygons collect_layer_region_slices(const Layer& layer)
{
    ExPolygons out;
    for (const LayerRegion* layerm : layer.regions())
        append(out, to_expolygons(layerm->slices.surfaces));
    if (!out.empty())
        out = union_ex(out);
    return out;
}
// Zones closer than this count as neighbors. It spans the 0.10 mm guard that
// full-domain Local-Z keeps between mixed rows and single-filament regions.
constexpr double LOCAL_Z_BRIDGE_TOUCH_TOLERANCE_MM = 0.15;

// Identifies a sublayer across the zones of a phase-locked bridge window. A
// template sublayer sits at position 2 * its index in the window's stack; a
// single-filament layer that matches none sits at an odd position between two.

// One pass of a bridge zone: a filament over an absolute Z range.

// A zone that can join a continuous mix bridge: one Local-Z mixed row, or one
// island of a single-filament region, painted or in the object's default color.

LocalZBridgeZone make_local_z_bridge_zone(ExPolygons                    masks,
                                          std::vector<LocalZBridgePass> passes,
                                          double                        base_z,
                                          std::vector<unsigned int>     filaments,
                                          size_t                        mixed_row,
                                          unsigned int                  channel)
{
    LocalZBridgeZone zone;
    zone.bbox   = get_extents(masks);
    zone.area   = std::abs(area(masks));
    zone.masks  = std::move(masks);
    zone.base_z = passes.empty() ? base_z : passes.front().lo;
    for (const LocalZBridgePass& pass : passes)
        filaments.push_back(pass.filament);
    filaments.erase(std::remove(filaments.begin(), filaments.end(), 0u), filaments.end());
    sort_remove_duplicates(filaments);
    zone.filaments = std::move(filaments);
    zone.passes    = std::move(passes);
    zone.mixed_row = mixed_row;
    zone.channel   = channel;
    return zone;
}

static bool local_z_bridge_zones_share_filament(const LocalZBridgeZone& lhs, const LocalZBridgeZone& rhs)
{
    return std::find_first_of(lhs.filaments.begin(), lhs.filaments.end(), rhs.filaments.begin(), rhs.filaments.end()) !=
           lhs.filaments.end();
}

// Passes of a Local-Z row, bottom to top, or nothing when a pass does not
// print exactly one physical filament or the passes leave a gap. A nonzero
// dependency group keeps only that row's passes.
std::vector<LocalZBridgePass> local_z_bridge_row_passes(const std::vector<SubLayerPlan>& plans, size_t num_physical, size_t dependency_group)
{
    std::vector<LocalZBridgePass> passes;
    for (const SubLayerPlan& plan : plans) {
        if (dependency_group != 0 && plan.dependency_group != dependency_group)
            continue;
        size_t       painted_slots = 0;
        unsigned int filament      = 0;
        for (size_t slot = 0; slot < plan.painted_masks_by_extruder.size(); ++slot)
            if (!plan.painted_masks_by_extruder[slot].empty()) {
                ++painted_slots;
                filament = unsigned(slot + 1);
            }
        if (painted_slots != 1 || filament > num_physical || plan.adaptive_nonplanar || plan.bridge_field || plan.flow_height <= EPSILON)
            return {};
        passes.push_back({filament, plan.print_z - plan.flow_height, plan.print_z});
    }
    std::sort(passes.begin(), passes.end(), [](const LocalZBridgePass& lhs, const LocalZBridgePass& rhs) { return lhs.lo < rhs.lo; });
    for (size_t pass_idx = 1; pass_idx < passes.size(); ++pass_idx)
        if (std::abs(passes[pass_idx].lo - passes[pass_idx - 1].hi) > 1e-6)
            return {};
    return passes;
}

// Restacks one nominal layer of a row with one pass per filament in ascending
// filament order. Every zone of a layer then stacks shared filaments in the
// same order, and neighboring layers alternate filaments like an unbridged mix.
std::vector<LocalZBridgePass> local_z_bridge_ordered_layer_passes(const std::vector<LocalZBridgePass>& passes, size_t num_physical)
{
    if (passes.empty())
        return {};
    std::vector<double> height_by_filament(num_physical + 1, 0.);
    for (const LocalZBridgePass& pass : passes)
        height_by_filament[pass.filament] += pass.hi - pass.lo;
    std::vector<LocalZBridgePass> ordered;
    double                        z = passes.front().lo;
    for (unsigned int filament = 1; filament <= num_physical; ++filament)
        if (height_by_filament[filament] > EPSILON) {
            ordered.push_back({filament, z, z + height_by_filament[filament]});
            z = ordered.back().hi;
        }
    ordered.back().hi = passes.back().hi;
    return ordered;
}

// Bottom of a nominal layer: the previous layer's top, so consecutive layers
// partition Z exactly.
double local_z_bridge_layer_bottom(const PrintObject& print_object, size_t layer_id)
{
    const Layer* layer = print_object.get_layer(int(layer_id));
    return layer_id > 0 ? print_object.get_layer(int(layer_id - 1))->print_z : layer->print_z - layer->height;
}

// With independent Local-Z heights, bridged mixes phase-lock their cycles to
// shared windows of whole nominal layers, long enough for the longest cycle.
// In a window every mix repeats its own cycle as many whole times as fit,
// stretched to fill the window, with its filaments in ascending order. The
// n-th cycle of every mix then sits at nearly the same height, so sublayers
// match across mixes by cycle and filament, and a single-filament zone matches
// each nominal layer to the nearest sublayer of its filament. A matched layer
// spans different nominal layers in different zones; each part prints on the
// nominal layer that holds its top.

// Where an object's phase-locked windows lie: `count` windows of `layers`
// nominal layers from `anchor`, the last one also taking the layers left at
// the top. No windows when `count` is zero.

LocalZBridgeWindow build_local_z_bridge_window(const PrintObject&                                           print_object,
                                               size_t                                                       first_layer,
                                               size_t                                                       last_layer,
                                               const std::vector<std::vector<LocalZIndependentDirectPass>>& row_cadences,
                                               const std::vector<uint8_t>&                                  rows_in_window,
                                               size_t                                                       num_physical,
                                               const PrintConfig&                                           print_cfg)
{
    LocalZBridgeWindow window;
    window.first_layer = first_layer;
    window.last_layer  = last_layer;
    window.z_lo        = local_z_bridge_layer_bottom(print_object, first_layer);
    window.z_hi        = print_object.get_layer(int(last_layer))->print_z;
    window.row_passes.assign(row_cadences.size(), {});
    window.single_passes.assign(num_physical, {});
    const double height = window.z_hi - window.z_lo;

    // Every row's cycle as total height per filament, in ascending filament order.
    struct RowCycle
    {
        size_t                                       row{0};
        std::vector<std::pair<unsigned int, double>> components;
        double                                       height{0.};
        size_t                                       repeats{1};
    };
    std::vector<RowCycle> cycles;
    size_t                max_repeats = 1;
    for (size_t row_idx = 0; row_idx < row_cadences.size() && height > EPSILON; ++row_idx) {
        if (row_idx >= rows_in_window.size() || rows_in_window[row_idx] == 0)
            continue;
        RowCycle cycle;
        cycle.row = row_idx;
        for (const LocalZIndependentDirectPass& pass : row_cadences[row_idx]) {
            if (pass.extruder_id == 0 || pass.extruder_id > num_physical || pass.height <= EPSILON)
                continue;
            auto component = std::find_if(cycle.components.begin(), cycle.components.end(),
                                          [&pass](const auto& entry) { return entry.first == pass.extruder_id; });
            if (component == cycle.components.end())
                cycle.components.emplace_back(pass.extruder_id, pass.height);
            else
                component->second += pass.height;
            cycle.height += pass.height;
        }
        if (cycle.height <= EPSILON)
            continue;
        std::sort(cycle.components.begin(), cycle.components.end());
        cycle.repeats = std::max<size_t>(1, size_t(std::floor(height / cycle.height + 1e-6)));
        max_repeats   = std::max(max_repeats, cycle.repeats);
        cycles.emplace_back(std::move(cycle));
    }

    // Template sublayers are (cycle slot, filament, split). A row that repeats
    // fewer times maps each of its cycles to the nearest cycle slot of the row
    // that repeats most; a sublayer above the printer's maximum layer height
    // splits into equal passes.
    using TemplateKey = std::tuple<size_t, unsigned int, size_t>;
    std::vector<std::vector<std::pair<TemplateKey, double>>> row_entries(cycles.size());
    std::vector<TemplateKey>                                 keys;
    for (size_t cycle_idx = 0; cycle_idx < cycles.size(); ++cycle_idx) {
        const RowCycle& cycle = cycles[cycle_idx];
        const double    scale = height / (double(cycle.repeats) * cycle.height);
        for (size_t repeat = 0; repeat < cycle.repeats; ++repeat) {
            const size_t slot = std::min(max_repeats - 1,
                                         size_t(std::floor((double(repeat) + 0.5) * double(max_repeats) / double(cycle.repeats) - 1e-9)));
            for (const auto& [filament, component_height] : cycle.components) {
                const double scaled = component_height * scale;
                const size_t splits = std::max<size_t>(1,
                                                       size_t(std::ceil(
                                                           scaled / local_z_max_layer_height_for_extruder(print_cfg, filament) - EPSILON)));
                for (size_t split = 0; split < splits; ++split) {
                    row_entries[cycle_idx].emplace_back(TemplateKey{slot, filament, split}, scaled / double(splits));
                    keys.emplace_back(slot, filament, split);
                }
            }
        }
    }
    sort_remove_duplicates(keys);

    // Rows' keyed passes, and the consensus stack: every row's interfaces
    // averaged, with zero height where a row lacks a sublayer.
    std::vector<double> consensus(keys.size() + 1, 0.);
    for (size_t cycle_idx = 0; cycle_idx < cycles.size(); ++cycle_idx) {
        std::vector<LocalZBridgePass>& passes = window.row_passes[cycles[cycle_idx].row];
        double                         z      = window.z_lo;
        for (const auto& [key, entry_height] : row_entries[cycle_idx]) {
            const int index = int(std::lower_bound(keys.begin(), keys.end(), key) - keys.begin());
            passes.push_back({std::get<1>(key), z, z + entry_height, LocalZBridgeKey{2 * index, 0, 0}});
            z += entry_height;
        }
        passes.back().hi = window.z_hi;
        double top       = window.z_lo;
        size_t pass_idx  = 0;
        consensus.front() += top / double(cycles.size());
        for (size_t index = 0; index < keys.size(); ++index) {
            if (pass_idx < passes.size() && passes[pass_idx].key.position == int(2 * index))
                top = passes[pass_idx++].hi;
            consensus[index + 1] += top / double(cycles.size());
        }
    }
    if (cycles.empty())
        std::fill(consensus.begin(), consensus.end(), window.z_lo);

    // Each single-filament layer matches at most one template sublayer of its
    // filament, keeping both in order and maximizing their overlap with the
    // consensus stack. An unmatched layer sits between the template sublayers
    // around its middle.
    std::vector<std::pair<double, double>> layers;
    for (size_t layer_id = first_layer; layer_id <= last_layer; ++layer_id)
        layers.emplace_back(local_z_bridge_layer_bottom(print_object, layer_id), print_object.get_layer(int(layer_id))->print_z);
    const size_t layer_count = layers.size();
    for (unsigned int filament = 1; filament <= num_physical; ++filament) {
        std::vector<int> filament_keys;
        for (size_t index = 0; index < keys.size(); ++index)
            if (std::get<1>(keys[index]) == filament)
                filament_keys.push_back(int(index));
        const size_t key_count = filament_keys.size();
        auto         overlap   = [&](size_t layer_idx, size_t key_idx) {
            const int index = filament_keys[key_idx];
            return std::min(layers[layer_idx].second, consensus[index + 1]) - std::max(layers[layer_idx].first, consensus[index]);
        };
        // best[i][j]: largest overlap of the first i layers with the first j keys.
        std::vector<std::vector<double>>  best(layer_count + 1, std::vector<double>(key_count + 1, 0.));
        std::vector<std::vector<uint8_t>> choice(layer_count + 1, std::vector<uint8_t>(key_count + 1, 0));
        for (size_t i = 1; i <= layer_count; ++i)
            for (size_t j = 1; j <= key_count; ++j) {
                best[i][j]   = best[i - 1][j];
                choice[i][j] = 0;
                if (best[i][j - 1] > best[i][j]) {
                    best[i][j]   = best[i][j - 1];
                    choice[i][j] = 1;
                }
                if (const double shared = overlap(i - 1, j - 1); shared > EPSILON && best[i - 1][j - 1] + shared >= best[i][j]) {
                    best[i][j]   = best[i - 1][j - 1] + shared;
                    choice[i][j] = 2;
                }
            }
        std::vector<int> matched(layer_count, -1);
        for (size_t i = layer_count, j = key_count; i > 0 && j > 0;) {
            if (choice[i][j] == 2) {
                matched[i - 1] = filament_keys[j - 1];
                --i;
                --j;
            } else if (choice[i][j] == 1) {
                --j;
            } else {
                --i;
            }
        }

        std::vector<LocalZBridgePass>& passes = window.single_passes[filament - 1];
        for (size_t layer_idx = 0; layer_idx < layer_count; ++layer_idx) {
            LocalZBridgeKey key;
            if (matched[layer_idx] >= 0) {
                key.position = 2 * matched[layer_idx];
            } else {
                int previous = -1;
                for (size_t other = 0; other < layer_idx; ++other)
                    if (matched[other] >= 0)
                        previous = matched[other];
                int next = int(keys.size());
                for (size_t other = layer_count; other-- > layer_idx + 1;)
                    if (matched[other] >= 0)
                        next = matched[other];
                const double middle = 0.5 * (layers[layer_idx].first + layers[layer_idx].second);
                int          before = 0;
                while (before < int(keys.size()) && 0.5 * (consensus[before] + consensus[before + 1]) < middle)
                    ++before;
                before = std::clamp(before, previous + 1, next);
                key    = LocalZBridgeKey{2 * before - 1, filament, unsigned(layer_idx)};
            }
            passes.push_back({filament, layers[layer_idx].first, layers[layer_idx].second, key});
        }
    }
    return window;
}

// Painted single-filament islands and the islands of the object's default
// color, taken from the full paint segmentation of one layer. Each prints its
// filament over the whole nominal layer. The default color is the physical
// filament each parent region assigns to its walls; mixed defaults are Local-Z
// rows instead.
std::vector<LocalZBridgeZone> collect_local_z_bridge_single_filament_zones(const Layer&                   layer,
                                                                           const std::vector<ExPolygons>& layer_segmentation,
                                                                           size_t                         num_physical)
{
    std::vector<LocalZBridgeZone> zones;
    const double                  z_lo        = layer.print_z - layer.height;
    auto                          add_islands = [&](const ExPolygons& masks, unsigned int filament, unsigned int channel) {
        for (const ExPolygon& island : masks)
            zones.emplace_back(
                make_local_z_bridge_zone(ExPolygons{island}, {{filament, z_lo, layer.print_z}}, z_lo, {}, size_t(-1), channel));
    };
    for (unsigned int filament = 1; filament <= num_physical && filament < layer_segmentation.size(); ++filament)
        if (!layer_segmentation[filament].empty())
            add_islands(union_ex(layer_segmentation[filament]), filament, filament);
    if (!layer_segmentation.empty() && !layer_segmentation.front().empty()) {
        for (const LayerRegion* layerm : layer.regions()) {
            if (layerm == nullptr || layerm->slices.empty())
                continue;
            const int filament = layerm->region().config().wall_filament.value;
            if (filament < 1 || size_t(filament) > num_physical)
                continue;
            add_islands(intersection_ex(layer_segmentation.front(), to_expolygons(layerm->slices.surfaces)), unsigned(filament), 0);
        }
    }
    return zones;
}

// Zones that touch and share a filament bridge. A group needs at least two
// zones, one of them mixed; its largest zone comes first.
static std::vector<std::vector<size_t>> group_local_z_bridge_zones(const std::vector<LocalZBridgeZone>& zones)
{
    std::vector<size_t> parent(zones.size());
    std::iota(parent.begin(), parent.end(), size_t(0));
    auto find_root = [&parent](size_t idx) {
        while (parent[idx] != idx)
            idx = parent[idx] = parent[parent[idx]];
        return idx;
    };
    const coord_t touch_tolerance = coord_t(scale_(LOCAL_Z_BRIDGE_TOUCH_TOLERANCE_MM));
    for (size_t lhs = 0; lhs < zones.size(); ++lhs) {
        BoundingBox inflated = zones[lhs].bbox;
        inflated.offset(touch_tolerance);
        ExPolygons inflated_masks;
        for (size_t rhs = lhs + 1; rhs < zones.size(); ++rhs) {
            if (find_root(lhs) == find_root(rhs) || !inflated.overlap(zones[rhs].bbox) ||
                !local_z_bridge_zones_share_filament(zones[lhs], zones[rhs]))
                continue;
            if (inflated_masks.empty())
                inflated_masks = offset_ex(zones[lhs].masks, float(touch_tolerance));
            if (!intersection_ex(inflated_masks, zones[rhs].masks).empty())
                parent[find_root(lhs)] = find_root(rhs);
        }
    }

    std::vector<std::vector<size_t>> by_root(zones.size());
    for (size_t idx = 0; idx < zones.size(); ++idx)
        by_root[find_root(idx)].push_back(idx);
    std::vector<std::vector<size_t>> groups;
    for (std::vector<size_t>& members : by_root) {
        if (members.size() < 2 || std::none_of(members.begin(), members.end(), [&zones](size_t member) { return zones[member].is_mixed(); }))
            continue;
        std::sort(members.begin(), members.end(), [&zones](size_t lhs, size_t rhs) {
            if (std::abs(zones[lhs].area - zones[rhs].area) > EPSILON)
                return zones[lhs].area > zones[rhs].area;
            return lhs < rhs;
        });
        groups.emplace_back(std::move(members));
    }
    return groups;
}

// Every zone of a layer stacks its passes in ascending filament order, so the
// slots are the group's filaments in that order and each filament connects
// across every zone that prints it. A filament gets as many slots as the most
// passes any zone prints with it, so a pass split at the maximum layer height
// keeps its own slot. A zone gives a slot it does not print zero height at
// that point of its stack.
static LocalZBridgeAlignment align_local_z_bridge_zones(const std::vector<LocalZBridgeZone>& zones, const std::vector<size_t>& members)
{
    LocalZBridgeAlignment alignment;
    std::vector<size_t>   slots_per_filament;
    for (size_t member : members) {
        std::vector<size_t> passes_per_filament;
        for (const LocalZBridgePass& pass : zones[member].passes) {
            if (pass.filament >= passes_per_filament.size())
                passes_per_filament.resize(pass.filament + 1, 0);
            ++passes_per_filament[pass.filament];
        }
        if (passes_per_filament.size() > slots_per_filament.size())
            slots_per_filament.resize(passes_per_filament.size(), 0);
        for (size_t filament = 0; filament < passes_per_filament.size(); ++filament)
            slots_per_filament[filament] = std::max(slots_per_filament[filament], passes_per_filament[filament]);
    }
    for (size_t filament = 0; filament < slots_per_filament.size(); ++filament)
        alignment.slot_filaments.insert(alignment.slot_filaments.end(), slots_per_filament[filament], unsigned(filament));
    for (size_t member : members) {
        const LocalZBridgeZone& zone = zones[member];
        std::vector<double>     bounds{zone.base_z};
        size_t                  pass_idx = 0;
        for (unsigned int filament : alignment.slot_filaments) {
            double top = bounds.back();
            if (pass_idx < zone.passes.size() && zone.passes[pass_idx].filament == filament)
                top = zone.passes[pass_idx++].hi;
            bounds.push_back(top);
        }
        alignment.member_bounds.emplace_back(std::move(bounds));
    }
    return alignment;
}

// In a phase-locked window every zone lists its passes in key order, so the
// slots are the group's keys in that order and a matched sublayer connects
// across every zone that prints it. A zone gives a slot it does not print
// zero height at that point of its stack.
static LocalZBridgeAlignment align_local_z_bridge_window_zones(const std::vector<LocalZBridgeZone>& zones,
                                                               const std::vector<size_t>&           members)
{
    std::vector<std::pair<LocalZBridgeKey, unsigned int>> slots;
    for (size_t member : members)
        for (const LocalZBridgePass& pass : zones[member].passes)
            slots.emplace_back(pass.key, pass.filament);
    std::sort(slots.begin(), slots.end(), [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
    slots.erase(std::unique(slots.begin(), slots.end(), [](const auto& lhs, const auto& rhs) { return lhs.first == rhs.first; }),
                slots.end());

    LocalZBridgeAlignment alignment;
    for (const auto& slot : slots) {
        alignment.slot_filaments.push_back(slot.second);
        alignment.slot_keys.push_back(slot.first);
    }
    for (size_t member : members) {
        const LocalZBridgeZone& zone = zones[member];
        std::vector<double>     bounds{zone.base_z};
        size_t                  pass_idx = 0;
        for (const auto& slot : slots) {
            double top = bounds.back();
            if (pass_idx < zone.passes.size() && zone.passes[pass_idx].key == slot.first)
                top = zone.passes[pass_idx++].hi;
            bounds.push_back(top);
        }
        alignment.member_bounds.emplace_back(std::move(bounds));
    }
    return alignment;
}

// Settings shared by every bridge field of an object.

// A group's field. In a phase-locked window the field holds the window's
// whole stack, and the slot range lists the slots that can have a top on the
// current nominal layer.

static LocalZBridgeGroupField make_local_z_bridge_field(const std::vector<LocalZBridgeZone>& zones,
                                                        const std::vector<size_t>&           members,
                                                        const LocalZBridgeSettings&          settings,
                                                        const LocalZBridgeField::Span*       window_tops = nullptr)
{
    LocalZBridgeAlignment  alignment = window_tops != nullptr ? align_local_z_bridge_window_zones(zones, members) :
                                                                align_local_z_bridge_zones(zones, members);
    LocalZBridgeGroupField result;
    result.end_slot = alignment.slot_filaments.size();
    if (window_tops != nullptr) {
        // A blended top lies between the members' tops.
        result.first_slot = result.end_slot;
        size_t last_slot  = 0;
        for (size_t slot = 0; slot < alignment.slot_filaments.size(); ++slot) {
            double lowest_top  = std::numeric_limits<double>::infinity();
            double highest_top = -std::numeric_limits<double>::infinity();
            bool   printed     = false;
            for (const std::vector<double>& bounds : alignment.member_bounds) {
                lowest_top  = std::min(lowest_top, bounds[slot + 1]);
                highest_top = std::max(highest_top, bounds[slot + 1]);
                printed     = printed || bounds[slot + 1] - bounds[slot] > 1e-6;
            }
            if (printed && highest_top > window_tops->lower + LocalZBridgeField::TOP_TOLERANCE_MM &&
                lowest_top <= window_tops->upper + LocalZBridgeField::TOP_TOLERANCE_MM) {
                result.first_slot = std::min(result.first_slot, slot);
                last_slot         = slot;
            }
        }
        if (result.first_slot == alignment.slot_filaments.size())
            return {};
        result.end_slot = last_slot + 1;
        result.keys     = std::move(alignment.slot_keys);
    }

    std::vector<LocalZBridgeField::Row> rows;
    rows.reserve(members.size());
    for (size_t member_idx = 0; member_idx < members.size(); ++member_idx) {
        const LocalZBridgeZone& zone = zones[members[member_idx]];
        rows.push_back({zone.masks, std::move(alignment.member_bounds[member_idx]), !zone.is_mixed(), zone.filaments});
    }
    result.field = std::make_shared<const LocalZBridgeField>(settings.transition_width_mm, std::move(alignment.slot_filaments),
                                                             std::move(rows), settings.options);
    if (result.field->rows().size() != members.size())
        return {};
    return result;
}

// Emits one bridge pass per printed slot. A pass mask is the slot's island:
// where its layer is printed, inside the group outline. Export prints the
// island as its own region, with walls along its outline. With a window's top
// range, a pass prints only the parts of its slot whose top lies on this
// nominal layer, so its island keeps the zones whose own top does and the
// transition bands, where blended tops cross nominal layers.
static void append_local_z_bridge_passes(const std::vector<LocalZBridgeSlotRef>& slots,
                                         const ExPolygons&                       group_masks,
                                         size_t                                  layer_id,
                                         size_t                                  dependency_group,
                                         size_t                                  num_physical,
                                         size_t&                                 pass_counter,
                                         std::vector<SubLayerPlan>&              plans,
                                         const LocalZBridgeField::Span*          window_tops = nullptr)
{
    // The strip reaches 2 mm past the band, so walls along its cut edge lie
    // where every zone keeps its own top and nothing prints on this layer.
    std::vector<std::pair<const LocalZBridgeField*, ExPolygons>> band_strips;
    auto band_strip_of = [&band_strips](const LocalZBridgeField& field) -> const ExPolygons& {
        for (const auto& [strip_field, strip] : band_strips)
            if (strip_field == &field)
                return strip;
        const float reach = float(scale_(field.transition_width_mm() + 2.0));
        ExPolygons  strip;
        for (const LocalZBridgeField::Row& row : field.rows())
            append(strip, diff_ex(offset_ex(row.masks, reach), row.masks));
        band_strips.emplace_back(&field, union_ex(strip));
        return band_strips.back().second;
    };
    const double min_piece_area   = sqr(scale_(0.01));
    size_t       dependency_order = 0;
    for (const LocalZBridgeSlotRef& slot_ref : slots) {
        const LocalZBridgeField& field    = *slot_ref.field;
        const size_t             slot_idx = slot_ref.slot;
        const unsigned int       filament = field.slot_filaments()[slot_idx];
        if (!field.slot_used(slot_idx) || filament == 0 || filament > num_physical)
            continue;
        ExPolygons slot_masks = field.slot_island(slot_idx, group_masks);
        if (window_tops != nullptr && !slot_masks.empty()) {
            ExPolygons printed = band_strip_of(field);
            for (const LocalZBridgeField::Row& row : field.rows()) {
                const double top = row.bounds[slot_idx + 1];
                if (top - row.bounds[slot_idx] > 1e-6 && LocalZBridgeField::top_in(*window_tops, top))
                    append(printed, row.masks);
            }
            slot_masks = intersection_ex(slot_masks, union_ex(printed));
        }
        // Neighboring zones meet along edges that clipping leaves slightly
        // apart, so an island can hold zero-width holes and outlines that
        // double back on themselves. Insetting either for walls opens a hole
        // ringed by wall loops or pushes a wall out of the object; a closing
        // far below a bead width removes both and keeps every real edge.
        slot_masks = closing_ex(slot_masks, float(scale_(0.01)));
        // Clipping can leave zero-area slivers along shared edges. Insetting
        // one for walls grows it out of the object instead of removing it.
        slot_masks.erase(std::remove_if(slot_masks.begin(), slot_masks.end(),
                                        [&min_piece_area](const ExPolygon& piece) { return piece.area() < min_piece_area; }),
                         slot_masks.end());
        if (slot_masks.empty())
            continue;
        const LocalZBridgeField::Span bounds = field.slot_bounds(slot_idx);
        SubLayerPlan                  plan;
        plan.layer_id          = layer_id;
        plan.pass_index        = pass_counter++;
        plan.split_interval    = true;
        plan.z_lo              = bounds.lower;
        plan.z_hi              = window_tops != nullptr ? std::min(bounds.upper, window_tops->upper) : bounds.upper;
        plan.print_z           = plan.z_hi;
        plan.flow_height       = field.slot_max_height(slot_idx);
        plan.dependency_group  = dependency_group;
        plan.dependency_order  = dependency_order++;
        plan.bridge_field      = slot_ref.field;
        plan.bridge_slot_index = slot_idx;
        if (window_tops != nullptr) {
            plan.bridge_top_lower = window_tops->lower;
            plan.bridge_top_upper = window_tops->upper;
        }
        plan.painted_masks_by_extruder.assign(num_physical, ExPolygons());
        plan.fixed_painted_masks_by_extruder.assign(num_physical, ExPolygons());
        plan.painted_masks_by_extruder[filament - 1] = std::move(slot_masks);
        plans.emplace_back(std::move(plan));
    }
}

// Groups one layer's zones, appends one bridge pass per slot of every group
// and records each group's region merge. Zones of a phase-locked window match
// slots by key and print only the parts whose top lies in `window_tops`.
LocalZBridgeLayerResult build_local_z_bridge_layer(const std::vector<LocalZBridgeZone>& zones,
                                                   size_t                               layer_id,
                                                   size_t                               layer_count,
                                                   size_t                               num_physical,
                                                   const LocalZBridgeSettings&          settings,
                                                   size_t&                              pass_counter,
                                                   std::vector<SubLayerPlan>&           plans,
                                                   LocalZBridgeRegionMerges*            merges,
                                                   const LocalZBridgeField::Span*       window_tops,
                                                   LocalZBridgeOutlineCache*            outline_cache)
{
    LocalZBridgeLayerResult                      result;
    std::vector<LocalZBridgeOutlineCache::Entry> new_entries;
    for (const std::vector<size_t>& members : group_local_z_bridge_zones(zones)) {
        LocalZBridgeGroupField group_field = make_local_z_bridge_field(zones, members, settings, window_tops);
        if (group_field.field == nullptr)
            continue;
        ExPolygons              group_masks;
        size_t                  dependency_group = std::numeric_limits<size_t>::max();
        LocalZBridgeRegionMerge merge;
        // The largest zone keeps the island, so its region configuration applies.
        merge.target_channel = zones[members.front()].channel;
        for (size_t member : members) {
            const LocalZBridgeZone& zone = zones[member];
            append(group_masks, zone.masks);
            if (zone.is_mixed()) {
                dependency_group = std::min(dependency_group, zone.mixed_row + 1);
                result.bridged_rows.push_back(zone.mixed_row);
                merge.whole_channels.push_back(zone.channel);
            } else {
                append(result.single_filament_masks, zone.masks);
                merge.islands.emplace_back(zone.channel, zone.masks);
            }
        }
        group_masks                  = union_ex(group_masks);
        const BoundingBox group_bbox = get_extents(group_masks);

        std::vector<LocalZBridgeSlotRef> slots;
        for (size_t slot = group_field.first_slot; slot < group_field.end_slot; ++slot) {
            LocalZBridgeSlotRef slot_ref{group_field.field, slot};
            if (outline_cache != nullptr && slot < group_field.keys.size()) {
                const LocalZBridgeKey& key = group_field.keys[slot];
                for (const LocalZBridgeOutlineCache::Entry& entry : outline_cache->entries) {
                    if (entry.layer_id >= layer_id || !entry.bbox.overlap(group_bbox))
                        continue;
                    const auto begin = entry.group.keys.begin() + entry.group.first_slot;
                    const auto end   = entry.group.keys.begin() + entry.group.end_slot;
                    const auto found = std::find(begin, end, key);
                    if (found == end || intersection_ex(entry.outline, group_masks).empty())
                        continue;
                    slot_ref = {entry.group.field, size_t(found - entry.group.keys.begin())};
                    break;
                }
            }
            slots.push_back(std::move(slot_ref));
        }
        append_local_z_bridge_passes(slots, group_masks, layer_id, dependency_group, num_physical, pass_counter, plans, window_tops);
        if (outline_cache != nullptr)
            new_entries.push_back({layer_id, std::move(group_field), group_masks, group_bbox});
        append(result.outline, group_masks);
        if (merges != nullptr && layer_id < layer_count) {
            if (merges->size() < layer_count)
                merges->resize(layer_count);
            (*merges)[layer_id].emplace_back(std::move(merge));
        }
    }
    if (outline_cache != nullptr)
        outline_cache->entries.insert(outline_cache->entries.end(), std::make_move_iterator(new_entries.begin()),
                                      std::make_move_iterator(new_entries.end()));
    if (result.single_filament_masks.size() > 1)
        result.single_filament_masks = union_ex(result.single_filament_masks);
    if (result.outline.size() > 1)
        result.outline = union_ex(result.outline);
    return result;
}

// Within participating bridge domains, each layer takes the zones of the
// window's middle layer. A point then belongs to one zone for
// the whole window, so the parts of a sublayer printed on different layers
// come from one stack. Zones that move from layer to layer, as on
// texture-painted surfaces, would otherwise leave sublayer parts printed twice
// or never. Color boundaries move in whole windows, one mix cycle. Where a
// layer reaches past the middle layer's slice, it keeps its own zones.
void align_local_z_segmentation_to_bridge_windows(const PrintObject&                    print_object,
                                                  std::vector<std::vector<ExPolygons>>& segmentation,
                                                  const LocalZBridgePlan&               plan,
                                                  const std::function<void()>&          throw_on_cancel)
{
    const auto&  layout      = plan.layout;
    const size_t layer_count = print_object.layer_count();
    if (layout.count == 0 || layout.layers <= 1 || segmentation.size() != layer_count)
        return;
    std::vector<size_t> window_middle(layer_count, size_t(-1));
    for (size_t window = 0; window < layout.count; ++window) {
        const size_t first = layout.first_layer(window);
        const size_t last  = std::min(layout.last_layer(window, layer_count), layer_count - 1);
        for (size_t layer_id = first; layer_id <= last; ++layer_id)
            window_middle[layer_id] = first + (last - first) / 2;
    }
    // Layer islands are not merged yet; the regions hold the slices.
    std::vector<ExPolygons> slices(layer_count), domains(layer_count);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, layer_count), [&](const tbb::blocked_range<size_t>& range) {
        for (size_t layer_id = range.begin(); layer_id < range.end(); ++layer_id)
            if (window_middle[layer_id] != size_t(-1)) {
                throw_on_cancel();
                const Layer& layer    = *print_object.get_layer(int(layer_id));
                slices[layer_id]      = collect_layer_region_slices(layer);
                const size_t physical = print_object.print()->config().filament_colour.size();
                auto         zones    = collect_local_z_bridge_single_filament_zones(layer, segmentation[layer_id], physical);
                for (size_t channel = physical + 1; channel < segmentation[layer_id].size() && channel < plan.channel_filaments.size();
                     ++channel) {
                    const auto& filaments = plan.channel_filaments[channel];
                    if (filaments.empty() || segmentation[layer_id][channel].empty())
                        continue;
                    zones.push_back(make_local_z_bridge_zone(segmentation[layer_id][channel], {}, layer.print_z - layer.height, filaments,
                                                             channel - physical - 1, unsigned(channel)));
                }
                for (const auto& group : group_local_z_bridge_zones(zones))
                    for (size_t member : group)
                        append(domains[layer_id], zones[member].masks);
                domains[layer_id] = union_ex(domains[layer_id]);
            }
    });
    // Middle layers keep their zones, so other layers may read them meanwhile.
    tbb::parallel_for(tbb::blocked_range<size_t>(0, layer_count), [&](const tbb::blocked_range<size_t>& range) {
        for (size_t layer_id = range.begin(); layer_id < range.end(); ++layer_id) {
            const size_t middle = window_middle[layer_id];
            if (middle == size_t(-1) || middle == layer_id)
                continue;
            throw_on_cancel();
            const ExPolygons&              slice           = slices[layer_id];
            const ExPolygons               shared          = intersection_ex(domains[layer_id], domains[middle]);
            const ExPolygons               own             = diff_ex(slice, shared);
            const std::vector<ExPolygons>& middle_channels = segmentation[middle];
            std::vector<ExPolygons>&       channels        = segmentation[layer_id];
            for (size_t channel_idx = 0; channel_idx < channels.size(); ++channel_idx) {
                ExPolygons aligned;
                if (channel_idx < middle_channels.size() && !middle_channels[channel_idx].empty())
                    aligned = intersection_ex(middle_channels[channel_idx], shared);
                if (!channels[channel_idx].empty() && !own.empty())
                    append(aligned, intersection_ex(channels[channel_idx], own));
                channels[channel_idx] = aligned.size() > 1 ? union_ex(aligned) : std::move(aligned);
            }
        }
    });
}

void merge_local_z_bridge_regions(std::vector<std::vector<ExPolygons>>& segmentation, const LocalZBridgeRegionMerges& merges)
{
    const float island_margin = scaled<float>(10. * EPSILON);
    for (size_t layer_id = 0; layer_id < merges.size() && layer_id < segmentation.size(); ++layer_id) {
        std::vector<ExPolygons>& channels = segmentation[layer_id];
        for (const LocalZBridgeRegionMerge& merge : merges[layer_id]) {
            if (merge.target_channel >= channels.size())
                continue;
            ExPolygons merged;
            for (unsigned int channel : merge.whole_channels)
                if (channel < channels.size()) {
                    append(merged, std::move(channels[channel]));
                    channels[channel].clear();
                }
            for (const auto& [channel, masks] : merge.islands)
                if (channel < channels.size()) {
                    channels[channel] = diff_ex(channels[channel], offset_ex(masks, island_margin));
                    append(merged, masks);
                }
            ExPolygons& target = channels[merge.target_channel];
            append(target, std::move(merged));
            // Neighboring segmentation cells can leave hairline gaps; close them
            // so the merged island gets one outline.
            target = closing_ex(union_ex(target), scaled<float>(10. * EPSILON));
        }
    }
}

// Removes single-filament islands that joined a bridge from full-domain fixed
// passes; the bridge passes print them instead.
void remove_local_z_bridged_fixed_masks(std::vector<SubLayerPlan>& fixed_plans, const ExPolygons& bridged_masks)
{
    if (bridged_masks.empty())
        return;
    for (SubLayerPlan& plan : fixed_plans)
        for (ExPolygons& masks : plan.fixed_painted_masks_by_extruder)
            if (!masks.empty())
                masks = diff_ex(masks, bridged_masks);
    fixed_plans.erase(std::remove_if(fixed_plans.begin(), fixed_plans.end(),
                                     [](const SubLayerPlan& plan) {
                                         return std::all_of(plan.fixed_painted_masks_by_extruder.begin(),
                                                            plan.fixed_painted_masks_by_extruder.end(),
                                                            [](const ExPolygons& masks) { return masks.empty(); });
                                     }),
                      fixed_plans.end());
}

} // namespace Slic3r
