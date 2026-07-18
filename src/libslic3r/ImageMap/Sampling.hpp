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

RGBA sample_source(const VolumeData &data, const TriangleBinding &binding, const Vec3f &barycentric);
const PaletteEntry *nearest_palette_entry(const Zone &zone, const RGBA &color);
Vec3f barycentric_coordinates(const Vec3d &point, const Vec3d &a, const Vec3d &b, const Vec3d &c);

struct SurfaceSample
{
    RGBA                   color{1.f, 1.f, 1.f, 1.f};
    Vec3d                  closest_local_point{Vec3d::Zero()};
    double                 squared_distance{0.0};
    uint32_t               triangle_index{0};
    const Zone            *zone{nullptr};
    const PaletteEntry    *palette_entry{nullptr};
    const TriangleBinding *binding{nullptr};
};

// Immutable acceleration snapshot for one ModelVolume. The mesh and domain
// data are retained for the sampler lifetime, making concurrent layer reads
// safe without touching ModelVolume.
class SurfaceSampler
{
public:
    SurfaceSampler(std::shared_ptr<const TriangleMesh> mesh, std::shared_ptr<const VolumeData> data);
    ~SurfaceSampler();
    SurfaceSampler(SurfaceSampler &&) noexcept;
    SurfaceSampler &operator=(SurfaceSampler &&) noexcept;
    SurfaceSampler(const SurfaceSampler &) = delete;
    SurfaceSampler &operator=(const SurfaceSampler &) = delete;

    std::optional<SurfaceSample> sample(const Vec3d &local_point,
                                        double       max_distance_mm,
                                        std::optional<RenderMode> render_mode = std::nullopt) const;

private:
    std::shared_ptr<const TriangleMesh> m_mesh;
    std::shared_ptr<const VolumeData>   m_data;
    std::unique_ptr<AABBMesh>           m_aabb;
    std::vector<std::vector<size_t>>    m_bindings_by_triangle;
};

} // namespace ImageMap
} // namespace Slic3r

#endif
