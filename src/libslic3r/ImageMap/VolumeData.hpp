#ifndef slic3r_ImageMap_VolumeData_hpp_
#define slic3r_ImageMap_VolumeData_hpp_

#include "../Color.hpp"
#include "../Point.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <cereal/types/array.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

namespace Slic3r {

class TriangleMesh;

namespace ImageMap {

inline constexpr uint32_t VOLUME_DATA_SCHEMA_VERSION = 2;

enum class SourceKind : uint8_t
{
    Texture,
    VertexColors,
    FaceColor
};

enum class RenderMode : uint8_t
{
    NormalMix,
    PerimeterModulationV2,
    AdaptiveLocalizedCycles
};

enum class AdaptiveModulationMode : uint8_t
{
    Perimeter,
    LocalZHeight
};

// Selects how printable component candidates are converted back into an
// expected surface colour before the closest Oklab match is chosen.
enum class ColorMixModel : uint8_t
{
    FullSpectrumKmKs,
    FilamentMixer
};

enum class WrapMode : uint8_t
{
    Repeat,
    Clamp,
    // Preserve the source background whenever a generated UV falls outside
    // the texture rectangle. This is used by image projections, whose border
    // commonly cuts through a mesh triangle.
    Transparent
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
    AdaptiveModulationMode    adaptive_modulation_mode{AdaptiveModulationMode::Perimeter};
    ColorMixModel             color_mix_model{ColorMixModel::FullSpectrumKmKs};
    bool                      synchronize_whole_object_cadence{false};
    int                       minimum_component_percent{15};
    float                     target_sample_size_mm{0.4f};
    size_t                    max_facet_samples{200000};
    float                     modulation_sample_spacing_mm{0.16f};
    float                     corner_smoothing_radius_mm{0.6f};
    bool                      disable_broad_path_smoothing{false};
    float                     gaussian_smoothing_strength{1.f};
    float                     first_path_smoothing_strength{1.f};
    float                     second_path_smoothing_strength{1.f};
    float                     tone_gamma{1.f};
    float                     overhang_contrast_percent{100.f};
    float                     image_exposure_ev{0.f};
    float                     image_contrast_percent{100.f};
    float                     image_saturation_percent{100.f};
    float                     image_edge_boost_percent{0.f};
    std::vector<PaletteEntry> palette;

    template<class Archive> void serialize(Archive& ar)
    {
        ar(stable_id, display_name, enabled, priority, render_mode, adaptive_modulation_mode, color_mix_model,
           synchronize_whole_object_cadence,
           minimum_component_percent, target_sample_size_mm, max_facet_samples, modulation_sample_spacing_mm, corner_smoothing_radius_mm,
           disable_broad_path_smoothing, gaussian_smoothing_strength, first_path_smoothing_strength, second_path_smoothing_strength,
           tone_gamma, overhang_contrast_percent, image_exposure_ev, image_contrast_percent, image_saturation_percent,
           image_edge_boost_percent, palette);
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
    bool             content_equals(const VolumeData &other) const;

    // Immutable VolumeData instances are shared by the undo/redo stack. These
    // methods provide the same accounting interface as other large immutable
    // model payloads such as TriangleMesh.
    size_t memsize() const;
    size_t release_optional() { return 0; }
    void   restore_optional() {}

    template<class Archive> void serialize(Archive &ar)
    {
        ar(schema_version, topology_fingerprint, texture_assets, zones, triangle_bindings);
    }
};

uint64_t topology_fingerprint(const TriangleMesh &mesh);

// Removes one independently-authored image-map zone and compacts its binding
// and texture indices. Returns false without modifying data when the ID does
// not exist.
bool remove_zone(VolumeData &data, const std::string &stable_id);

// Repairs small UV cracks across the internal diagonal of a coplanar textured
// surface. Large gaps remain untouched because they represent authored UV
// islands. Returns the number of shared edges that were stitched.
size_t stitch_perimeter_modulation_uv_cracks(const TriangleMesh &mesh, VolumeData &data);

} // namespace ImageMap
} // namespace Slic3r

#endif
