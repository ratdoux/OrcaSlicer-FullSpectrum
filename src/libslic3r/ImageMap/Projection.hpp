#ifndef slic3r_ImageMap_Projection_hpp_
#define slic3r_ImageMap_Projection_hpp_

#include "VolumeData.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace Slic3r {

class TriangleMesh;

namespace ImageMap {

// A UV-free decal projector. All values are expressed in ModelVolume mesh
// coordinates; the generated UVs are baked into TriangleBinding records.
struct OrthographicProjection
{
    Vec3d    center{Vec3d::Zero()};
    Vec3d    normal{Vec3d::UnitZ()};
    Vec3d    up{Vec3d::UnitY()};
    double   width_mm{20.0};
    double   height_mm{20.0};
    double   rotation_degrees{0.0};
    double   max_depth_mm{10.0};
    double   max_surface_angle_degrees{75.0};
    bool     flip_horizontal{false};
    bool     flip_vertical{false};
    uint32_t seed_triangle{std::numeric_limits<uint32_t>::max()};
    RGBA     background_color{1.f, 1.f, 1.f, 1.f};
};

struct ProjectionResult
{
    bool        success{false};
    size_t      projected_triangle_count{0};
    uint32_t    zone_index{0};
    int32_t     texture_asset_index{-1};
    std::string error;

    explicit operator bool() const { return success; }
};

// Appends a projected texture as a new, higher-priority image-map zone. The
// operation is transactional: data is unchanged if generation or validation
// fails. Existing image maps are sampled at projected triangle corners and
// used as the transparent background.
ProjectionResult append_orthographic_projection(const TriangleMesh            &mesh,
                                                 TextureAsset                   texture,
                                                 Zone                           zone,
                                                 const OrthographicProjection &projection,
                                                 VolumeData                    &data);

} // namespace ImageMap
} // namespace Slic3r

#endif
