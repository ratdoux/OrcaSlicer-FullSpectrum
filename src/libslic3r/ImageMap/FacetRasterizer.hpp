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

using PaletteFilamentResolver = std::function<unsigned int(const PaletteEntry &)>;

FacetRasterization rasterize_facets(const TriangleMesh             &mesh,
                                    const VolumeData               &data,
                                    unsigned int                    base_filament_id,
                                    const PaletteFilamentResolver  &resolve_filament);

} // namespace ImageMap
} // namespace Slic3r

#endif
