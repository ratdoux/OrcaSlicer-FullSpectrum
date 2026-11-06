#ifndef slic3r_LocalZBridgePlanner_hpp_
#define slic3r_LocalZBridgePlanner_hpp_
#include "LocalZBridge.hpp"
#include "BoundingBox.hpp"
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>
namespace Slic3r {
class PrintObject;
class PrintConfig;
class Layer;
struct SubLayerPlan;
struct LocalZIndependentDirectPass
{
    unsigned int extruder_id{0};
    double       height{0.0};
};

struct LocalZBridgeKey
{
    int          position{0};
    unsigned int filament{0};
    unsigned int layer{0};

    bool operator<(const LocalZBridgeKey& rhs) const
    {
        return std::tie(position, filament, layer) < std::tie(rhs.position, rhs.filament, rhs.layer);
    }
    bool operator==(const LocalZBridgeKey& rhs) const { return position == rhs.position && filament == rhs.filament && layer == rhs.layer; }
};

struct LocalZBridgePass
{
    unsigned int filament{0};
    double       lo{0.};
    double       hi{0.};
    // Phase-locked windows match slots by key instead of by filament.
    LocalZBridgeKey key;
};

struct LocalZBridgeZone
{
    ExPolygons masks;
    // The zone's own passes on this layer, bottom to top, contiguous and in
    // ascending filament order.
    std::vector<LocalZBridgePass> passes;
    // Bottom of the zone's stack on this layer. A mix following its own
    // cadence may print nothing on a layer; its stack then has zero height here.
    double base_z{0.};
    // Every filament the zone can print, sorted, so neighbors bridge even on
    // layers where the zone prints none of the filaments they share.
    std::vector<unsigned int> filaments;
    // Local-Z row of a mixed zone, or size_t(-1) for a single-filament zone.
    size_t mixed_row{size_t(-1)};
    // Segmentation channel holding the masks. A mixed zone owns its whole
    // channel; a single-filament zone owns only its island of the channel.
    unsigned int channel{0};
    BoundingBox  bbox;
    double       area{0.};

    bool is_mixed() const { return mixed_row != size_t(-1); }
};

struct LocalZBridgeWindow
{
    size_t first_layer{0};
    size_t last_layer{0};
    double z_lo{0.};
    double z_hi{0.};
    // Per mixed row, its keyed sublayers over [z_lo, z_hi], bottom to top;
    // empty for rows the window does not phase-lock.
    std::vector<std::vector<LocalZBridgePass>> row_passes;
    // Per physical filament, one keyed pass per nominal layer.
    std::vector<std::vector<LocalZBridgePass>> single_passes;
};

struct LocalZBridgeWindowLayout
{
    size_t anchor{0};
    size_t layers{1};
    size_t count{0};

    size_t first_layer(size_t window) const { return anchor + window * layers; }
    size_t last_layer(size_t window, size_t layer_count) const
    {
        return window + 1 == count ? layer_count - 1 : first_layer(window) + layers - 1;
    }
};

struct LocalZBridgeSettings
{
    double                     transition_width_mm{0.};
    LocalZBridgeField::Options options;
};

struct LocalZBridgeGroupField
{
    std::shared_ptr<const LocalZBridgeField> field;
    std::vector<LocalZBridgeKey>             keys;
    size_t                                   first_slot{0};
    size_t                                   end_slot{0};
};

struct LocalZBridgeRegionMerge
{
    unsigned int                                     target_channel{0};
    std::vector<unsigned int>                        whole_channels;
    std::vector<std::pair<unsigned int, ExPolygons>> islands;
};

struct LocalZBridgeLayerResult
{
    // Mixed rows whose own passes the bridges replaced.
    std::vector<size_t> bridged_rows;
    // Single-filament islands the bridges print.
    ExPolygons single_filament_masks;
    // Outline of every bridge group.
    ExPolygons outline;
};

struct LocalZBridgeOutlineCache
{
    struct Entry
    {
        size_t                 layer_id{0};
        LocalZBridgeGroupField group;
        ExPolygons             outline;
        BoundingBox            bbox;
    };
    size_t             window_first_layer{std::numeric_limits<size_t>::max()};
    std::vector<Entry> entries;

    void start_window(size_t first_layer)
    {
        if (window_first_layer != first_layer) {
            window_first_layer = first_layer;
            entries.clear();
        }
    }
};
using LocalZBridgeRegionMerges = std::vector<std::vector<LocalZBridgeRegionMerge>>;

// Prepared from authored paint before alignment. Its window boundaries, slot
// keys and cadence tables never depend on the order in which layers are built.
struct LocalZBridgePlan
{
    LocalZBridgeWindowLayout               layout;
    std::vector<uint8_t>                   phase_locked_rows;
    std::vector<std::vector<unsigned int>> channel_filaments;
    std::vector<LocalZBridgeWindow>        windows;
};

LocalZBridgeWindowLayout compute_local_z_bridge_window_layout(size_t layer_count,
                                                              size_t anchor,
                                                              double nominal_height,
                                                              double longest_cycle);

LocalZBridgeZone make_local_z_bridge_zone(ExPolygons                    masks,
                                          std::vector<LocalZBridgePass> passes,
                                          double                        base_z,
                                          std::vector<unsigned int>     filaments,
                                          size_t                        mixed_row,
                                          unsigned int                  channel);

std::vector<LocalZBridgePass> local_z_bridge_row_passes(const std::vector<SubLayerPlan>& plans,
                                                        size_t                           num_physical,
                                                        size_t                           dependency_group = 0);

std::vector<LocalZBridgePass> local_z_bridge_ordered_layer_passes(const std::vector<LocalZBridgePass>& passes, size_t num_physical);

double local_z_bridge_layer_bottom(const PrintObject& print_object, size_t layer_id);

LocalZBridgeWindow build_local_z_bridge_window(const PrintObject&                                           print_object,
                                               size_t                                                       first_layer,
                                               size_t                                                       last_layer,
                                               const std::vector<std::vector<LocalZIndependentDirectPass>>& row_cadences,
                                               const std::vector<uint8_t>&                                  rows_in_window,
                                               size_t                                                       num_physical,
                                               const PrintConfig&                                           print_cfg);

std::vector<LocalZBridgeZone> collect_local_z_bridge_single_filament_zones(const Layer&                   layer,
                                                                           const std::vector<ExPolygons>& layer_segmentation,
                                                                           size_t                         num_physical);

LocalZBridgeLayerResult build_local_z_bridge_layer(const std::vector<LocalZBridgeZone>& zones,
                                                   size_t                               layer_id,
                                                   size_t                               layer_count,
                                                   size_t                               num_physical,
                                                   const LocalZBridgeSettings&          settings,
                                                   size_t&                              pass_counter,
                                                   std::vector<SubLayerPlan>&           plans,
                                                   LocalZBridgeRegionMerges*            merges,
                                                   const LocalZBridgeField::Span*       window_tops   = nullptr,
                                                   LocalZBridgeOutlineCache*            outline_cache = nullptr);

void align_local_z_segmentation_to_bridge_windows(const PrintObject&                    print_object,
                                                  std::vector<std::vector<ExPolygons>>& segmentation,
                                                  const LocalZBridgePlan&               plan,
                                                  const std::function<void()>&          throw_on_cancel);

void merge_local_z_bridge_regions(std::vector<std::vector<ExPolygons>>& segmentation, const LocalZBridgeRegionMerges& merges);

void remove_local_z_bridged_fixed_masks(std::vector<SubLayerPlan>& fixed_plans, const ExPolygons& bridged_masks);

double local_z_max_layer_height_for_extruder(const PrintConfig& print_config, unsigned int extruder_id);

ExPolygons collect_layer_region_slices(const Layer& layer);
} // namespace Slic3r
#endif
