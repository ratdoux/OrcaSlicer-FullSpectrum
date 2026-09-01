#ifndef slic3r_ImageMap_FacetRasterizer_hpp_
#define slic3r_ImageMap_FacetRasterizer_hpp_

#include "VolumeData.hpp"

#include <functional>
#include <string>
#include <vector>

namespace Slic3r {
class TriangleMesh;
namespace ImageMap {

struct RasterizedFacet
{
    uint32_t    triangle_index{0};
    std::string encoded_states;
};

struct FacetRasterization
{
    std::vector<RasterizedFacet> facets;
    size_t                       sampled_leaf_count{0};
    size_t                       unresolved_palette_entries{0};
};

struct SourceColorVertex
{
    Vec3f position{Vec3f::Zero()};
    Vec3f normal{Vec3f::UnitZ()};
    RGBA  color{1.f, 1.f, 1.f, 1.f};
};

struct SourceColorRasterization
{
    std::vector<SourceColorVertex> vertices;
    std::vector<unsigned int>      indices;
    size_t                         sampled_leaf_count{0};
    size_t                         source_triangle_count{0};
    size_t                         lod_pass_count{0};
};

struct SourceColorRasterizationOptions
{
    // Adaptive subdivision is useful on small, coarse meshes, but a viewport
    // preview must never amplify an already dense source mesh without bound.
    size_t                     max_leaf_triangles{250'000};
    // ModelVolume validates immutable image-map data when it is attached.
    // Viewport workers may skip repeating that full topology/binding scan.
    bool                       source_data_validated{false};
    std::function<void(int)>   progress;
    std::function<bool(void)>  cancelled;
};

using PaletteFilamentResolver = std::function<unsigned int(const PaletteEntry &)>;

// Removes isolated sub-cell assignments from an adaptive quadtree without
// blurring established region boundaries. The first two leaf levels are
// cleaned so a one-sample colour fluctuation cannot become its own material
// island and perimeter seam.
void stabilize_adaptive_region_ids(std::vector<unsigned int> &ids, unsigned int subdivision_depth, unsigned int base_filament_id);

FacetRasterization rasterize_facets(const TriangleMesh             &mesh,
                                    const VolumeData               &data,
                                    unsigned int                    base_filament_id,
                                    const PaletteFilamentResolver  &resolve_filament);

// Builds a display mesh whose vertex colors come directly from the persistent
// texture / vertex-color source. It deliberately bypasses the finite filament
// palette used by Normal Mix and compatibility fallbacks.
SourceColorRasterization rasterize_source_colors(const TriangleMesh &mesh,
                                                 const VolumeData   &data,
                                                 RenderMode          render_mode,
                                                 const RGBA         &fallback_color = RGBA{1.f, 1.f, 1.f, 1.f});

SourceColorRasterization rasterize_source_colors(const TriangleMesh                     &mesh,
                                                 const VolumeData                       &data,
                                                 RenderMode                              render_mode,
                                                 const RGBA                             &fallback_color,
                                                 const SourceColorRasterizationOptions  &options);

} // namespace ImageMap
} // namespace Slic3r

#endif
