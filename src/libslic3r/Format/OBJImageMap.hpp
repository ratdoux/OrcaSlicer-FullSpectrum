#ifndef slic3r_Format_OBJImageMap_hpp_
#define slic3r_Format_OBJImageMap_hpp_

#include "OBJ.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {

class TriangleMesh;

struct ObjImageMapSamplePlan
{
    std::vector<RGBA> colors;
    // -1 means that the source triangle has no usable image texture. Non-negative
    // values describe a balanced four-way triangle subdivision depth.
    std::vector<int8_t> triangle_subdivision_depths;
    size_t              textured_triangle_count{0};
    size_t              loaded_texture_count{0};

    bool empty() const { return colors.empty() || textured_triangle_count == 0; }
};

// Builds a finite-resolution surface-color plan from OBJ UV coordinates. The
// plan is later quantized by ObjColorDialog and baked into MMU facet states, so
// the resulting regions can use FullSpectrum mixed-filament surface bias.
bool build_obj_image_map_sample_plan(const TriangleMesh&    mesh,
                                     const ObjInfo&         obj_info,
                                     float                  target_sample_size_mm,
                                     size_t                 max_samples,
                                     ObjImageMapSamplePlan& out_plan,
                                     std::string*           warning = nullptr);

size_t obj_image_map_leaf_count(unsigned int subdivision_depth);

// Returns the 3MF/MMU facet string for one balanced subdivision tree. An empty
// string means every leaf uses the base filament and no annotation is needed.
std::string encode_obj_image_map_triangle_filaments(const std::vector<unsigned char>& filament_ids,
                                                    size_t                            offset,
                                                    unsigned int                      subdivision_depth,
                                                    unsigned char                     base_filament_id);

} // namespace Slic3r

#endif
