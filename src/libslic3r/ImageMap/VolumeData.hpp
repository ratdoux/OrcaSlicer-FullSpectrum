#ifndef slic3r_ImageMap_VolumeData_hpp_
#define slic3r_ImageMap_VolumeData_hpp_

#include "../Color.hpp"
#include "../Point.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {

class TriangleMesh;

namespace ImageMap {

inline constexpr uint32_t VOLUME_DATA_SCHEMA_VERSION = 1;

enum class SourceKind : uint8_t
{
    Texture,
    VertexColors,
    FaceColor
};

enum class RenderMode : uint8_t
{
    NormalMix,
    PerimeterModulationV2
};

enum class WrapMode : uint8_t
{
    Repeat,
    Clamp
};

struct TextureAsset
{
    std::string          stable_id;
    std::string          display_name;
    uint32_t             width{0};
    uint32_t             height{0};
    std::vector<uint8_t> rgba;

    bool valid() const;

    template<class Archive> void serialize(Archive &ar)
    {
        ar(stable_id, display_name, width, height, rgba);
    }
};

struct SurfaceSource
{
    SourceKind          kind{SourceKind::FaceColor};
    int32_t             texture_asset_index{-1};
    WrapMode            wrap_u{WrapMode::Repeat};
    WrapMode            wrap_v{WrapMode::Repeat};
    std::array<Vec2f, 3> uvs{Vec2f::Zero(), Vec2f::Zero(), Vec2f::Zero()};
    std::array<RGBA, 3>  corner_colors{RGBA{1.f, 1.f, 1.f, 1.f}, RGBA{1.f, 1.f, 1.f, 1.f}, RGBA{1.f, 1.f, 1.f, 1.f}};

    template<class Archive> void serialize(Archive &ar)
    {
        ar(kind, texture_asset_index, wrap_u, wrap_v, uvs, corner_colors);
    }
};

struct TriangleBinding
{
    uint32_t      triangle_index{0};
    uint32_t      zone_index{0};
    SurfaceSource source;

    template<class Archive> void serialize(Archive &ar)
    {
        ar(triangle_index, zone_index, source);
    }
};

struct PaletteEntry
{
    RGBA         target_color{1.f, 1.f, 1.f, 1.f};
    uint64_t     mixed_filament_stable_id{0};
    unsigned int fallback_filament_id{1};

    template<class Archive> void serialize(Archive &ar)
    {
        ar(target_color, mixed_filament_stable_id, fallback_filament_id);
    }
};

struct Zone
{
    std::string               stable_id;
    std::string               display_name;
    bool                      enabled{true};
    int                       priority{0};
    RenderMode                render_mode{RenderMode::NormalMix};
    int                       minimum_component_percent{15};
    float                     target_sample_size_mm{0.4f};
    size_t                    max_facet_samples{200000};
    float                     modulation_sample_spacing_mm{0.25f};
    float                     corner_smoothing_radius_mm{0.6f};
    std::vector<PaletteEntry> palette;

    template<class Archive> void serialize(Archive &ar)
    {
        ar(stable_id,
           display_name,
           enabled,
           priority,
           render_mode,
           minimum_component_percent,
           target_sample_size_mm,
           max_facet_samples,
           modulation_sample_spacing_mm,
           corner_smoothing_radius_mm,
           palette);
    }
};

struct ValidationResult
{
    bool                     valid{true};
    std::vector<std::string> errors;

    explicit operator bool() const { return valid; }
};

struct VolumeData
{
    uint32_t                     schema_version{VOLUME_DATA_SCHEMA_VERSION};
    uint64_t                     topology_fingerprint{0};
    std::vector<TextureAsset>    texture_assets;
    std::vector<Zone>            zones;
    std::vector<TriangleBinding> triangle_bindings;

    ValidationResult validate(const TriangleMesh &mesh) const;
    bool             empty() const { return zones.empty() || triangle_bindings.empty(); }

    template<class Archive> void serialize(Archive &ar)
    {
        ar(schema_version, topology_fingerprint, texture_assets, zones, triangle_bindings);
    }
};

uint64_t topology_fingerprint(const TriangleMesh &mesh);

} // namespace ImageMap
} // namespace Slic3r

#endif
