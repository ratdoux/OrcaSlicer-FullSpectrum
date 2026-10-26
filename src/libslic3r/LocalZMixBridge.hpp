#ifndef slic3r_LocalZMixBridge_hpp_
#define slic3r_LocalZMixBridge_hpp_

#include "ExPolygon.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include <memory>

#include <vector>

namespace Slic3r {

class Layer;
struct LocalZMixBridge;
struct LocalZMixBridgeGeometry;
// Immutable, cycle-local skin coverage, prepared before parallel path generation.
struct LocalZMixBridgeFillSurfaces
{
    ExPolygons                                 solid;
    std::vector<std::pair<double, ExPolygons>> tops, bottoms;
    static LocalZMixBridgeFillSurfaces         build(const std::vector<Layer*>& layers, const LocalZMixBridge& bridge);
};
namespace FillAdaptive {
struct Octree;
}
namespace FillLightning {
class Generator;
}

// Generated from one physical-material region, not clipped from another
// material's perimeter network. Shared by export and prime-tower prediction.
struct LocalZMixBridgePaths
{
    size_t                                             region_id = 0;
    ExtrusionEntityCollection                          perimeters;
    ExPolygons                                         fill_masks;
    ExtrusionEntityCollection                          fills;
    // Layer-region owner of each top-level fill collection (modifier settings).
    std::vector<size_t>                                fill_region_ids;
    ExPolygons                                         fill_no_overlap;
    static std::shared_ptr<const LocalZMixBridgePaths> with_fills(const LocalZMixBridgePaths&        paths,
                                                                  Layer&                             layer,
                                                                  const LocalZMixBridge&             bridge,
                                                                  size_t                             pass_index,
                                                                  const LocalZMixBridgeFillSurfaces& surfaces,
                                                                  FillAdaptive::Octree*              adaptive,
                                                                  FillAdaptive::Octree*              support,
                                                                  FillLightning::Generator*          lightning);
    static std::shared_ptr<const LocalZMixBridgePaths> build(Layer&            layer,
                                                             const ExPolygons& masks,
                                                             unsigned          filament_id,
                                                             size_t            owner_region_id = size_t(-1));
    // Compatible nominal beads share one perimeter outline before color clipping.
    static std::vector<std::shared_ptr<const LocalZMixBridgePaths>> build_partitioned(
        Layer& layer, const std::vector<ExPolygons>& masks, const std::vector<unsigned>& filament_ids);
};

// Shared spatial interfaces, independent of path seams and travel direction.
// Equal sets share the whole stack. Unlike sets insert extra-color islands
// before a shared carrier color, with a single-anchor fallback when needed.
struct LocalZMixBridge
{
    struct Zone
    {
        ExPolygons          masks;
        std::vector<double> heights;
        // Consecutive repeated IDs are separately printable slots of one material.
        std::vector<unsigned int> component_ids;
        unsigned int              source_filament_id = 0;
        // Pure zones retain nominal passes, optionally sharing the final one.
        bool preserve_nominal_layers = false;
        bool join_terminal_pass      = false;
    };
    struct Pass
    {
        std::vector<size_t> zones;
        size_t              component   = 0;
        unsigned int        filament_id = 0;
        double              ceiling     = 0.;
        double              flat_height = 0.;
        int                 tongue      = -1;
        ExPolygons          masks;
        int                 extra_color = -1;
        // Cross-section at this bead's spatially varying mid-height.
        ExPolygons contour_masks;
        bool       contour_fitted = false;
    };
    struct SplitRun
    {
        size_t first = 0, last = 0;
        double cap = 0.;
        // Optional lower beads. The last bead always covers the full run.
        std::vector<ExPolygons> masks;
    };
    struct ExtraColor
    {
        unsigned filament_id = 0;
        // Authored thickness per mixed zone; zero means the color is absent.
        std::vector<double> heights;
        ExPolygons          masks;
        double              maximum_height = 0.;
    };
    struct LayerFootprint
    {
        size_t                  layer_id = 0;
        ExPolygons              masks;
        std::vector<ExPolygons> zones;
        double                  slice_z = 0.;
        double                  height  = 0.;
    };

    double z_lo          = 0.;
    double z_hi          = 0.;
    // Internal repeats do not create new roofs or floors.
    std::pair<double, double> skin_envelope{0., 0.};
    std::pair<double, double> skin_range() const
    {
        return skin_envelope.second > skin_envelope.first ? skin_envelope : std::make_pair(z_lo, z_hi);
    }
    double maximum_slope = 0.12;
    double nozzle_clearance = 0.;
    bool   shared_stack  = true;
    // A bounded ordinary core or nominal edge strip, with flat interfaces.
    bool   planar_fallback = false;
    // A moving region confined to one nominal layer. Changing exterior edges
    // reserve a printable shell and generate fitted paths inside that shell.
    bool   nominal_band = false;
    // Versioned material planner owns cells before generating any paths.
    bool material_planner = false;
    size_t material_configuration = size_t(-1);
    std::vector<size_t> material_domains;
    // Shared interfaces extend through the changing exterior; ownership stays
    // on nominal slices while individual beads use their own cross-sections.
    bool       follows_surface = false;
    ExPolygons lower_support_masks;
    // Same-color merging tips may taper below the normal bead minimum.
    double                                 tongue_tip_height = .01;
    std::vector<double>                    tongue_planes;
    std::vector<ExPolygons>                tongue_masks;
    std::vector<Zone>                      zones;
    // Preserve recipes before sparse carriers and shared anchors normalize them.
    // Rejected joins must rebuild from authored colors, never from derived zones.
    std::vector<Zone>                      authored_zones;
    std::vector<Pass>                      passes;
    std::vector<unsigned int>              source_filament_ids;
    std::vector<std::pair<double, double>> anchor_limits;
    std::vector<SplitRun>                  split_runs;
    // Extra colors borrow volume from the terminal carrier run. Other common
    // colors keep their authored heights when an extra tapers out.
    std::vector<ExtraColor> extra_colors;
    double                  extra_maximum_height = 0.;
    unsigned                extra_carrier        = 0;
    double                  minimum_height       = 0.;
    bool                    extra_masks_ready    = false;
    bool                    allow_sparse_stack   = true;
    // Place the common anchor last without changing residual color order.
    bool terminal_anchor = false;
    // Immutable spatial index shared by all passes of this bridge.
    std::shared_ptr<const LocalZMixBridgeGeometry> geometry;
    // Explicit per-interval ownership suppresses fallback throughout the bead
    // stack. Surface-following passes have separate midpoint cross-sections.
    std::vector<LayerFootprint> layer_footprints;
    double                      boundary_tolerance = 0.;
    double                      boundary_anchor    = 0.;
    std::vector<double>         boundary_interfaces;

    // Discard sub-resolution residual ribbons before they influence height fields.
    static ExPolygons clean_residual(const ExPolygons& masks);

    // Only reserve fixed fallback islands that remain wide enough after
    // support clipping and the moving-shell reservation.
    static ExPolygons fallback_core(const ExPolygons& candidate, const ExPolygons& support, const ExPolygons& shell, double minimum_width);
    // Common physical material across adjacent slabs, leaving printable nominal
    // remainders wherever the ownership changes.
    static ExPolygons coalescing_core(const std::vector<ExPolygons>& footprints,
                                      const std::vector<ExPolygons>& actual,
                                      double                         minimum_width);

    static bool enabled_from_environment();
    static std::vector<std::pair<unsigned, double>> allocate_flat_layers(const std::vector<double>& weights,
                                                                         double                     height,
                                                                         double                     minimum,
                                                                         const std::vector<double>& maximum,
                                                                         std::vector<double>&       error,
                                                                         double                     footprint_area = 1.);
    // Align nominal fallback colors with an adjoining exact stack, keeping
    // slab heights and accounting for the redistributed per-color volume.
    static bool align_flat_layers(std::vector<std::pair<unsigned, double>>& beads,
                                    const std::vector<double>&                footprint_areas,
                                    const std::vector<unsigned>&              order,
                                    const std::vector<double>&                maximum,
                                    std::vector<double>&                      error);
    // Repeat a complete recipe as finely as its smallest printable component permits.
    static std::vector<std::pair<unsigned, double>> repeat_flat_layers(const std::vector<std::pair<unsigned, double>>& beads,
                                                                       double                                          minimum,
                                                                       const std::vector<double>&                      maximum,
                                                                       size_t                                          maximum_repeats = 0);
    // Match fixed nominal slots to a possibly repeated exact cadence. Starts are
    // relative to the exact stack bottom, including any gaps between slots.
    static bool align_flat_layers_to_cadence(std::vector<std::pair<unsigned, double>>&       beads,
                                             const std::vector<double>&                      areas,
                                             const std::vector<double>&                      starts,
                                             const std::vector<std::pair<unsigned, double>>& cadence,
                                             const std::vector<double>&                      maximum,
                                             std::vector<double>&                            error);
    // Refine one connected group, rechecking its foreign boundary. Empty
    // means retain the original; no partial refinement is ever published.
    std::vector<LocalZMixBridge> shorter_cycles(const std::vector<double>&   nominal_heights,
                                                const std::vector<double>&   maximum,
                                                const std::vector<unsigned>& terminal_preference,
                                                double                       transition_length_factor = 1.,
                                                const ExPolygons&            neighbors                = {},
                                                double                       minimum_contour_width    = 0.,
                                                ExPolygons*                  rejected_contours        = nullptr) const;
    // Neighboring fallback may repeat only with the same period, phase and order.
    bool        shares_cadence(double bottom, double top, size_t repeats, const std::vector<unsigned>& order) const;
    static bool sparse_stack_fits(const std::vector<Zone>& zones, double minimum_height, const std::vector<double>& maximum_heights);
    static std::vector<LocalZMixBridge> build(double                       z_lo,
                                              double                       z_hi,
                                              double                       minimum_height,
                                              const std::vector<double>&   maximum_heights,
                                              std::vector<Zone>            zones,
                                              const std::vector<double>&   nominal_heights          = {},
                                              double                       transition_length_factor = 1.,
                                              double                       nozzle_clearance         = 0.,
                                              const std::vector<unsigned>& terminal_preference      = {},
                                              bool                         allow_sparse             = true);
    ExPolygons                          masks(const Pass& pass) const;
    ExPolygons                          masks(const Pass& pass, size_t layer_id) const;
    ExPolygons                          ownership(size_t layer_id) const;
    bool                                advances_near(const ExPolygons& neighbors, double clearance) const;
    bool                                pin_foreign_boundary(const ExPolygons& neighbors, double clearance);
    bool                                fit_contours(double maximum_error, ExPolygons* rejected = nullptr, double minimum_width = 0.);
    double                              pure_distance(const Point& point) const;
    double                              terminal_interface(const Point& point, size_t tongue_count) const;
    double                              schedule_z(const Pass& pass) const;
    double                              interface_z(const Point& point, size_t component = 0) const;
    std::pair<double, double>           surfaces(const Point& point, size_t pass) const;
    std::vector<ExtrusionPathSloped>    modulate(const ExtrusionPath& path, size_t component, double z_offset) const;

private:
    static std::vector<LocalZMixBridge> build_impl(double                       bottom,
                                                   double                       top,
                                                   double                       minimum,
                                                   const std::vector<double>&   maximum,
                                                   std::vector<Zone>            input,
                                                   const std::vector<double>&   nominal_heights,
                                                   double                       transition_length_factor,
                                                   double                       nozzle_clearance,
                                                   const std::vector<unsigned>& terminal_preference,
                                                   bool                         split_disconnected,
                                                   bool                         allow_sparse);
};

} // namespace Slic3r
#endif
