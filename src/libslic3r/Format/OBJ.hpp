#ifndef slic3r_Format_OBJ_hpp_
#define slic3r_Format_OBJ_hpp_
#include "libslic3r/Color.hpp"
#include "libslic3r/Point.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
namespace Slic3r {

class TriangleMesh;
class Model;
class ModelObject;
struct ObjImageMapSamplePlan;

enum class ObjColorImportSource : uint8_t { VertexColors, FaceColors, ImageTexture };

enum class ObjColorImportMode : uint8_t { Colors, ImageMap };

enum class ObjImageMapRenderMode : uint8_t { NormalMix, PerimeterModulationV2, AdaptiveLocalizedCycles };
enum class ObjAdaptiveModulationMode : uint8_t { Perimeter, LocalZHeight };
enum class ObjImageMapColorMixModel : uint8_t { FullSpectrumKmKs, FilamentMixer };

enum class ObjImageMapProgressStage : uint8_t
{
    ParseGeometry,
    DecodeTextures,
    AnalyzeSurface,
    AllocateSamples,
    SampleColors,
    QuantizeColors,
    CreateMixedFilaments,
    StoreSource
};

using ObjImageMapProgressFn = std::function<bool(ObjImageMapProgressStage stage, size_t current, size_t total)>;

struct ObjColorImportContext
{
    ObjColorImportSource source{ObjColorImportSource::VertexColors};
    ObjColorImportMode   mode{ObjColorImportMode::Colors};
    bool                 vertex_colors_available{false};
    bool                 face_colors_available{false};
    bool                 texture_coordinates_available{false};
    bool                 detected_texture_available{false};
    bool                 source_change_requested{false};
    ObjColorImportSource requested_source{ObjColorImportSource::VertexColors};
    ObjColorImportMode   requested_mode{ObjColorImportMode::Colors};
    std::string          requested_texture_file;
    std::string          warning_message;
    ObjImageMapRenderMode image_map_render_mode{ObjImageMapRenderMode::NormalMix};
    ObjAdaptiveModulationMode image_map_adaptive_modulation_mode{ObjAdaptiveModulationMode::Perimeter};
    ObjImageMapColorMixModel image_map_color_mix_model{ObjImageMapColorMixModel::FullSpectrumKmKs};
    int                   image_map_minimum_component_percent{15};
    bool                  image_map_synchronize_whole_object_cadence{false};
    float                 image_map_modulation_sample_spacing_mm{0.16f};
    bool                  image_map_disable_broad_path_smoothing{false};
    float                 image_map_gaussian_smoothing_strength{1.f};
    float                 image_map_first_path_smoothing_strength{1.f};
    float                 image_map_second_path_smoothing_strength{1.f};
    float                 image_map_tone_gamma{1.f};
    float                 image_map_overhang_contrast_percent{100.f};
    float                 image_map_exposure_ev{0.f};
    float                 image_map_contrast_percent{100.f};
    float                 image_map_saturation_percent{100.f};
    float                 image_map_edge_boost_percent{0.f};
    std::vector<RGBA>      image_map_palette_colors;
    std::vector<unsigned char> image_map_palette_filament_ids;
    std::vector<uint64_t>  image_map_palette_mixed_stable_ids;
    ObjImageMapProgressFn   image_map_progress_fn;
    // GUI-only, transient notification used to refresh the prepare-tab
    // prediction while an image-map settings dialog is open.
    std::function<void()>   image_map_preview_changed_fn;
    // The modal settings window owns the active event loop, so the canvas's
    // normal idle repaint is suspended. This callback explicitly presents a
    // worker result and returns true while more repaint polling is required.
    std::function<bool()>   image_map_preview_refresh_fn;
};

typedef std::function<void(std::vector<RGBA>&          input_colors,
                           bool                        is_single_color,
                           std::vector<unsigned char>& filament_ids,
                           unsigned char&              first_extruder_id,
                           ObjColorImportContext&      context)>
    ObjImportColorFn;

// Decoded texture pixels supplied by container formats such as GLB and FBX.
// File-based formats leave this map empty and keep using triangle_texture_files.
struct ObjEmbeddedTexture
{
    std::string          display_name;
    uint32_t             width{0};
    uint32_t             height{0};
    std::vector<uint8_t> rgba;
};

// Load an OBJ file into a provided model.
struct ObjInfo
{
    std::vector<RGBA>                    vertex_colors;
    std::vector<RGBA>                    face_colors;
    bool                                 is_single_mtl{false};
    std::vector<std::array<Vec2f, 3>>    uvs;
    std::vector<std::array<Vec2f, 3>>    triangle_uvs;
    std::vector<uint8_t>                 triangle_uvs_valid;
    std::vector<std::string>             triangle_texture_files;
    std::string                          obj_directory;
    std::map<std::string, bool>          pngs;
    std::unordered_map<int, std::string> uv_map_pngs;
    std::unordered_map<std::string, ObjEmbeddedTexture> embedded_textures;
    std::string                          source_format{"OBJ"};
    bool                                 has_uv_png{false};
};
extern bool load_obj(const char* path,
                     TriangleMesh* mesh,
                     ObjInfo& vertex_colors,
                     std::string& message,
                     const ObjImageMapProgressFn& progress_fn = {});
extern bool load_obj(const char* path,
                     Model* model,
                     ObjInfo& vertex_colors,
                     std::string& message,
                     const char* object_name = nullptr,
                     const ObjImageMapProgressFn& progress_fn = {});

extern bool store_obj(const char* path, TriangleMesh* mesh);
extern bool store_obj(const char* path, ModelObject* model);
extern bool store_obj(const char* path, Model* model);

}; // namespace Slic3r

#endif /* slic3r_Format_OBJ_hpp_ */
