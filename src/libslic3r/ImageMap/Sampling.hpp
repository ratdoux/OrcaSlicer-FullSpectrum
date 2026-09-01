#ifndef slic3r_ImageMap_Sampling_hpp_
#define slic3r_ImageMap_Sampling_hpp_

#include "VolumeData.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace Slic3r {

class AABBMesh;
class TriangleMesh;

namespace ImageMap {

RGBA                sample_source(const VolumeData& data, const TriangleBinding& binding, const Vec3f& barycentric);
float               sample_source_opacity(const VolumeData& data, const TriangleBinding& binding, const Vec3f& barycentric);
const PaletteEntry* nearest_palette_entry(const Zone& zone, const RGBA& color);
Vec3f               barycentric_coordinates(const Vec3d& point, const Vec3d& a, const Vec3d& b, const Vec3d& c);

// Shared source/target processing used by slicing and the prepare-tab
// printable-result preview. Keeping these operations here prevents the live
// preview from drifting away from the values consumed by the renderer.
RGBA sample_processed_texture(const TextureAsset& asset, const Vec2f& uv, WrapMode wrap_u, WrapMode wrap_v, const Zone& zone);
RGBA adjusted_modulation_target_color(RGBA color, const Zone& zone);
void apply_modulation_component_contrast(std::vector<double>& weights, const Zone& zone);

// Produces a small, deterministic palette that represents both dominant and
// visually distinct source colors. Sampling and histogram sizes are bounded so
// this is safe to use in import and sidebar UI paths.
std::vector<RGBA> representative_source_colors(const std::vector<RGBA>& source_colors, size_t max_colors = 8, size_t max_samples = 8192);
std::vector<RGBA> representative_source_colors(const VolumeData& data,
                                               RenderMode        render_mode,
                                               size_t            max_colors  = 8,
                                               size_t            max_samples = 8192);

// Partitions a sampled source by an existing label assignment, then builds one
// bounded representative spectrum per label in a pair of linear passes.
std::vector<std::vector<RGBA>> representative_labeled_source_colors(const std::vector<RGBA>& source_colors,
                                                                    const std::vector<int>&  labels,
                                                                    size_t                   label_count,
                                                                    size_t                   max_colors  = 8,
                                                                    size_t                   max_samples = 8192);

// Builds one bounded source-color spectrum for each palette entry in a zone.
// Texture and vertex colors are assigned to the nearest palette target, so the
// result describes the colors actually represented by each localized cycle.
// Binding inspection is bounded as well; the outer vector follows
// Zone::palette order.
std::vector<std::vector<RGBA>> representative_palette_source_colors(const VolumeData& data,
                                                                    size_t            zone_index,
                                                                    size_t            max_colors  = 8,
                                                                    size_t            max_samples = 8192);

struct SurfaceSample
{
    RGBA                   color{1.f, 1.f, 1.f, 1.f};
    Vec3d                  closest_local_point{Vec3d::Zero()};
    double                 squared_distance{0.0};
    uint32_t               triangle_index{0};
    const Zone*            zone{nullptr};
    const PaletteEntry*    palette_entry{nullptr};
    const TriangleBinding* binding{nullptr};
};

// Immutable acceleration snapshot for one ModelVolume. The mesh and domain
// data are retained for the sampler lifetime, making concurrent layer reads
// safe without touching ModelVolume.
class SurfaceSampler
{
public:
    SurfaceSampler(std::shared_ptr<const TriangleMesh> mesh, std::shared_ptr<const VolumeData> data);
    ~SurfaceSampler();
    SurfaceSampler(SurfaceSampler&&) noexcept;
    SurfaceSampler& operator=(SurfaceSampler&&) noexcept;
    SurfaceSampler(const SurfaceSampler&)            = delete;
    SurfaceSampler& operator=(const SurfaceSampler&) = delete;

    std::optional<SurfaceSample> sample(const Vec3d&              local_point,
                                        double                    max_distance_mm,
                                        std::optional<RenderMode> render_mode = std::nullopt) const;

private:
    std::shared_ptr<const TriangleMesh> m_mesh;
    std::shared_ptr<const VolumeData>   m_data;
    std::unique_ptr<AABBMesh>           m_aabb;
    std::vector<std::vector<size_t>>    m_bindings_by_triangle;
};

struct LayerPlaneSample
{
    RGBA                   color{1.f, 1.f, 1.f, 1.f};
    Vec3f                  barycentric{Vec3f(1.f / 3.f, 1.f / 3.f, 1.f / 3.f)};
    double                 squared_distance{0.0};
    uint32_t               triangle_index{0};
    const VolumeData*      data{nullptr};
    const Zone*            zone{nullptr};
    const PaletteEntry*    palette_entry{nullptr};
    const TriangleBinding* binding{nullptr};
};

// A physical sample taken from a mesh/slicing-plane intersection.  The
// integration weight represents the length of surface intersection owned by
// this sample, allowing callers to reconstruct a two-dimensional component
// field without biasing densely tessellated parts of the mesh.
struct LayerPlaneFieldSample
{
    Vec2d            print_point{Vec2d::Zero()};
    Vec2d            outward{Vec2d::Zero()};
    LayerPlaneSample sample;
    double           integration_weight_mm{0.0};
};

// Samples only the mesh segments which intersect a single slicing plane.
// Queries are also matched to the wall-normal axis of the boundary. Triangle
// winding is intentionally ignored because many imported textured meshes mix
// inward- and outward-wound faces. This prevents a nearby perpendicular face
// around a corner from leaking its texture across the current wall without
// rejecting the correct face solely because its winding is reversed.
class LayerPlaneSampler
{
public:
    LayerPlaneSampler(std::shared_ptr<const TriangleMesh> mesh,
                      std::shared_ptr<const VolumeData>   data,
                      const Transform3d&                  local_to_print,
                      double                              print_z);
    ~LayerPlaneSampler();
    LayerPlaneSampler(LayerPlaneSampler&&) noexcept;
    LayerPlaneSampler& operator=(LayerPlaneSampler&&) noexcept;
    LayerPlaneSampler(const LayerPlaneSampler&)            = delete;
    LayerPlaneSampler& operator=(const LayerPlaneSampler&) = delete;

    std::optional<LayerPlaneSample> sample(const Vec2d&              print_point,
                                           const Vec2d&              outward,
                                           double                    max_distance_mm,
                                           std::optional<RenderMode> render_mode = std::nullopt) const;

    // Uniform midpoint samples of every mapped surface segment intersecting
    // this layer. These are the source points for ImageMap-style 2D weight
    // reconstruction; midpoint sampling avoids double-counting shared
    // triangle endpoints.
    std::vector<LayerPlaneFieldSample> field_samples(double                    sample_pitch_mm,
                                                      std::optional<RenderMode> render_mode = std::nullopt) const;

    size_t segment_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ImageMap
} // namespace Slic3r

#endif
