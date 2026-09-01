#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "libslic3r/Format/OBJImageMap.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/Format/Assimp.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/FullSpectrumKSPairResidual.hpp"
#include "libslic3r/ImageMap/BoundaryModulation.hpp"
#include "libslic3r/ImageMap/ContinuousColorSolver.hpp"
#include "libslic3r/ImageMap/FacetRasterizer.hpp"
#include "libslic3r/ImageMap/PerimeterEnvelopeRenderer.hpp"
#include "libslic3r/ImageMap/Projection.hpp"
#include "libslic3r/ImageMap/Sampling.hpp"
#include "libslic3r/ImageMap/SimplePmCalibration.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ObjColorUtils.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <sstream>
#include <set>

using Catch::Approx;

using namespace Slic3r;

namespace {

void append_glb_u16(std::vector<uint8_t>& bytes, uint16_t value)
{
    bytes.push_back(uint8_t(value));
    bytes.push_back(uint8_t(value >> 8));
}

void append_glb_u32(std::vector<uint8_t>& bytes, uint32_t value)
{
    bytes.push_back(uint8_t(value));
    bytes.push_back(uint8_t(value >> 8));
    bytes.push_back(uint8_t(value >> 16));
    bytes.push_back(uint8_t(value >> 24));
}

void append_glb_float(std::vector<uint8_t>& bytes, float value)
{
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    append_glb_u32(bytes, bits);
}

bool write_textured_triangle_glb(const boost::filesystem::path& path, const std::vector<uint8_t>& png_bytes)
{
    std::vector<uint8_t> bin;
    for (float value : std::array<float, 9>{0.f, 0.f, 0.f, 0.01f, 0.f, 0.f, 0.f, 0.01f, 0.f})
        append_glb_float(bin, value);
    for (float value : std::array<float, 6>{0.f, 0.f, 1.f, 0.f, 0.f, 1.f})
        append_glb_float(bin, value);
    append_glb_u16(bin, 0);
    append_glb_u16(bin, 1);
    append_glb_u16(bin, 2);
    while (bin.size() % 4 != 0)
        bin.push_back(0);
    const size_t image_offset = bin.size();
    bin.insert(bin.end(), png_bytes.begin(), png_bytes.end());
    const size_t image_size = png_bytes.size();
    while (bin.size() % 4 != 0)
        bin.push_back(0);

    std::ostringstream json_stream;
    json_stream << "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":" << bin.size()
                << "}],\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36,\"target\":34962},"
                   "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24,\"target\":34962},"
                   "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":6,\"target\":34963},"
                   "{\"buffer\":0,\"byteOffset\":"
                << image_offset << ",\"byteLength\":" << image_size
                << "}],\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
                   "\"min\":[0,0,0],\"max\":[0.01,0.01,0]},"
                   "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
                   "{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
                   "\"images\":[{\"bufferView\":3,\"mimeType\":\"image/png\",\"name\":\"embedded.png\"}],"
                   "\"textures\":[{\"source\":0}],\"materials\":[{\"pbrMetallicRoughness\":{"
                   "\"baseColorFactor\":[1,1,1,1],\"baseColorTexture\":{\"index\":0}}}],"
                   "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":1},"
                   "\"indices\":2,\"material\":0}]}],\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
    std::string json = json_stream.str();
    while (json.size() % 4 != 0)
        json.push_back(' ');

    const uint32_t       total_length = uint32_t(12 + 8 + json.size() + 8 + bin.size());
    std::vector<uint8_t> glb;
    glb.reserve(total_length);
    append_glb_u32(glb, 0x46546c67);
    append_glb_u32(glb, 2);
    append_glb_u32(glb, total_length);
    append_glb_u32(glb, uint32_t(json.size()));
    append_glb_u32(glb, 0x4e4f534a);
    glb.insert(glb.end(), json.begin(), json.end());
    append_glb_u32(glb, uint32_t(bin.size()));
    append_glb_u32(glb, 0x004e4942);
    glb.insert(glb.end(), bin.begin(), bin.end());

    boost::nowide::ofstream output(path.string(), std::ios::binary);
    output.write(reinterpret_cast<const char*>(glb.data()), std::streamsize(glb.size()));
    return output.good();
}

} // namespace

TEST_CASE("Simple perimeter modulation can synchronize the whole object cadence", "[ImageMap][PerimeterModulation]")
{
    const std::vector<std::string> physical_colors = {"#FF00FF", "#00AEEF"};
    MixedFilamentDefinition       definition;
    definition.identity.stable_id                         = 91001;
    definition.source.kind                                = MixedFilamentSourceKind::Custom;
    definition.recipe.kind                                = MixedFilamentRecipeKind::WeightedBlend;
    definition.recipe.blend.components                    = {{{1}, 50}, {{2}, 50}};
    definition.behavior.distribution                      = MixedFilamentDistributionMode::LayerCycle;
    definition.behavior.surface_bias.perimeter_modulation = true;
    MixedFilamentManager manager;
    REQUIRE(manager.add_custom_filament_definition(definition, physical_colors));

    Model        model;
    ModelObject* object = model.add_object();
    ModelVolume* volume = object->add_volume(make_cube(1., 1., 1.));
    ImageMap::VolumeData data;
    data.topology_fingerprint = ImageMap::topology_fingerprint(volume->mesh());
    ImageMap::Zone zone;
    zone.stable_id                        = "synchronized-simple-pm";
    zone.render_mode                      = ImageMap::RenderMode::PerimeterModulationV2;
    zone.synchronize_whole_object_cadence = true;
    zone.palette.push_back({RGBA{0.5f, 0.f, 0.5f, 1.f}, definition.identity.stable_id, 3});
    data.zones.push_back(zone);
    ImageMap::TriangleBinding binding;
    binding.triangle_index = 0;
    binding.zone_index     = 0;
    binding.source.kind    = ImageMap::SourceKind::VertexColors;
    data.triangle_bindings.push_back(binding);
    REQUIRE(volume->set_image_map_data(std::move(data)));

    CHECK(ImageMap::model_whole_object_cadence_filament(*object, manager, physical_colors.size(), 0, 0.2f, 0.2f) == 1);
    CHECK(ImageMap::model_whole_object_cadence_filament(*object, manager, physical_colors.size(), 1, 0.4f, 0.2f) == 2);
    CHECK(ImageMap::model_whole_object_cadence_filament(*object, manager, physical_colors.size(), 2, 0.6f, 0.2f) == 1);

    ImageMap::VolumeData disabled = *volume->image_map_data();
    disabled.zones.front().synchronize_whole_object_cadence = false;
    REQUIRE(volume->set_image_map_data(std::move(disabled)));
    CHECK_FALSE(ImageMap::model_whole_object_cadence_filament(*object, manager, physical_colors.size(), 0, 0.2f, 0.2f));
}

TEST_CASE("Orthographic image projection generates local UV bindings without a mesh unwrap", "[ImageMap][Projection]")
{
    indexed_triangle_set its;
    its.vertices = {Vec3f(-5.f, -5.f, 0.f), Vec3f(5.f, -5.f, 0.f), Vec3f(5.f, 5.f, 0.f), Vec3f(-5.f, 5.f, 0.f)};
    its.indices.emplace_back(0, 1, 2);
    its.indices.emplace_back(0, 2, 3);
    TriangleMesh mesh(std::move(its));

    ImageMap::TextureAsset texture{"projection", "projection.png", 2, 2,
                                   {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255}};
    ImageMap::Zone zone;
    zone.stable_id = "projected-zone";
    zone.palette.push_back({RGBA{1.f, 0.f, 0.f, 1.f}, 0, 1});
    zone.palette.push_back({RGBA{0.f, 1.f, 0.f, 1.f}, 0, 2});

    ImageMap::OrthographicProjection projection;
    projection.center        = Vec3d::Zero();
    projection.normal        = Vec3d::UnitZ();
    projection.up            = Vec3d::UnitY();
    projection.width_mm      = 4.0;
    projection.height_mm     = 4.0;
    projection.max_depth_mm  = 1.0;
    projection.seed_triangle = 0;

    ImageMap::VolumeData data;
    const ImageMap::ProjectionResult result =
        ImageMap::append_orthographic_projection(mesh, std::move(texture), std::move(zone), projection, data);
    REQUIRE(result);
    CHECK(result.projected_triangle_count == 2);
    REQUIRE(data.validate(mesh).valid);
    REQUIRE(data.triangle_bindings.size() == 2);
    CHECK(data.triangle_bindings.front().source.wrap_u == ImageMap::WrapMode::Transparent);
    CHECK(data.triangle_bindings.front().source.wrap_v == ImageMap::WrapMode::Transparent);
    CHECK(data.triangle_bindings.front().source.uvs[0].x() == Approx(-0.75f));
    CHECK(data.triangle_bindings.front().source.uvs[0].y() == Approx(-0.75f));
    CHECK(data.triangle_bindings.front().source.uvs[2].x() == Approx(1.75f));
    CHECK(data.triangle_bindings.front().source.uvs[2].y() == Approx(1.75f));

    ImageMap::TextureAsset second_texture{"projection-two", "second.png", 1, 1, {0, 0, 255, 255}};
    ImageMap::Zone         second_zone;
    second_zone.stable_id                = "projected-zone-two";
    second_zone.display_name             = "second.png";
    second_zone.render_mode              = ImageMap::RenderMode::AdaptiveLocalizedCycles;
    second_zone.adaptive_modulation_mode = ImageMap::AdaptiveModulationMode::LocalZHeight;
    second_zone.palette.push_back({RGBA{0.f, 0.f, 1.f, 1.f}, 42, 5});
    projection.flip_horizontal = true;
    projection.flip_vertical   = true;
    const ImageMap::ProjectionResult second_result =
        ImageMap::append_orthographic_projection(mesh, std::move(second_texture), std::move(second_zone), projection, data);
    REQUIRE(second_result);
    REQUIRE(data.zones.size() == 2);
    REQUIRE(data.texture_assets.size() == 2);
    REQUIRE(data.triangle_bindings.size() == 4);
    CHECK(data.triangle_bindings[2].source.uvs[0].x() == Approx(1.75f));
    CHECK(data.triangle_bindings[2].source.uvs[0].y() == Approx(1.75f));

    REQUIRE(ImageMap::remove_zone(data, "projected-zone"));
    REQUIRE(data.validate(mesh).valid);
    REQUIRE(data.zones.size() == 1);
    CHECK(data.zones.front().stable_id == "projected-zone-two");
    CHECK(data.zones.front().display_name == "second.png");
    CHECK(data.zones.front().render_mode == ImageMap::RenderMode::AdaptiveLocalizedCycles);
    CHECK(data.zones.front().adaptive_modulation_mode == ImageMap::AdaptiveModulationMode::LocalZHeight);
    REQUIRE(data.texture_assets.size() == 1);
    CHECK(data.texture_assets.front().stable_id == "projection-two");
    REQUIRE(data.triangle_bindings.size() == 2);
    CHECK(std::all_of(data.triangle_bindings.begin(), data.triangle_bindings.end(), [](const ImageMap::TriangleBinding& binding) {
        return binding.zone_index == 0 && binding.source.texture_asset_index == 0;
    }));
    CHECK_FALSE(ImageMap::remove_zone(data, "missing-zone"));
}

TEST_CASE("Projected texture borders preserve their background color", "[ImageMap][Projection][Sampling]")
{
    ImageMap::VolumeData data;
    data.texture_assets.push_back({"projection", "projection.png", 1, 1, {255, 0, 0, 255}});
    ImageMap::TriangleBinding binding;
    binding.source.kind                = ImageMap::SourceKind::Texture;
    binding.source.texture_asset_index = 0;
    binding.source.wrap_u              = ImageMap::WrapMode::Transparent;
    binding.source.wrap_v              = ImageMap::WrapMode::Transparent;
    binding.source.uvs                 = {Vec2f(-1.f, 0.5f), Vec2f(-1.f, 0.5f), Vec2f(-1.f, 0.5f)};
    binding.source.corner_colors       = {RGBA{0.f, 0.f, 1.f, 1.f}, RGBA{0.f, 0.f, 1.f, 1.f}, RGBA{0.f, 0.f, 1.f, 1.f}};

    const RGBA outside = ImageMap::sample_source(data, binding, Vec3f(1.f, 0.f, 0.f));
    CHECK(outside[0] == Approx(0.f));
    CHECK(outside[1] == Approx(0.f));
    CHECK(outside[2] == Approx(1.f));
    CHECK(ImageMap::sample_source_opacity(data, binding, Vec3f(1.f, 0.f, 0.f)) == Approx(0.f));

    binding.source.uvs = {Vec2f(1.f, 0.5f), Vec2f(1.f, 0.5f), Vec2f(1.f, 0.5f)};
    const RGBA edge = ImageMap::sample_source(data, binding, Vec3f(1.f, 0.f, 0.f));
    CHECK(edge[0] == Approx(1.f));
    CHECK(edge[2] == Approx(0.f));
    CHECK(ImageMap::sample_source_opacity(data, binding, Vec3f(1.f, 0.f, 0.f)) == Approx(1.f));
}

TEST_CASE("Per-image processing preserves neutral samples and boosts tones and edges", "[ImageMap][Sampling][ImageProcessing]")
{
    ImageMap::VolumeData data;
    data.texture_assets.push_back({"processing", "processing.png", 3, 1,
                                   {0, 0, 0, 255, 128, 128, 128, 255, 192, 192, 192, 255}});
    data.zones.emplace_back();
    ImageMap::TriangleBinding binding;
    binding.zone_index                 = 0;
    binding.source.kind                = ImageMap::SourceKind::Texture;
    binding.source.texture_asset_index = 0;
    binding.source.wrap_u              = ImageMap::WrapMode::Clamp;
    binding.source.wrap_v              = ImageMap::WrapMode::Clamp;
    binding.source.uvs                 = {Vec2f(0.5f, 0.5f), Vec2f(0.5f, 0.5f), Vec2f(0.5f, 0.5f)};

    const RGBA neutral = ImageMap::sample_source(data, binding, Vec3f(1.f, 0.f, 0.f));
    CHECK(neutral[0] == Approx(128.f / 255.f));
    CHECK(neutral[1] == Approx(neutral[0]));
    CHECK(neutral[2] == Approx(neutral[0]));

    data.zones[0].image_edge_boost_percent = 100.f;
    const RGBA sharpened = ImageMap::sample_source(data, binding, Vec3f(1.f, 0.f, 0.f));
    CHECK(sharpened[0] > neutral[0] + 0.05f);

    data.zones[0].image_edge_boost_percent = 0.f;
    data.zones[0].image_exposure_ev        = 1.f;
    const RGBA exposed = ImageMap::sample_source(data, binding, Vec3f(1.f, 0.f, 0.f));
    CHECK(exposed[0] == Approx(1.f).margin(0.01f));

    data.zones[0].image_exposure_ev       = 0.f;
    data.zones[0].image_contrast_percent = 200.f;
    binding.source.uvs = {Vec2f(1.f, 0.5f), Vec2f(1.f, 0.5f), Vec2f(1.f, 0.5f)};
    const RGBA contrasted = ImageMap::sample_source(data, binding, Vec3f(1.f, 0.f, 0.f));
    CHECK(contrasted[0] == Approx(1.f));

    data.texture_assets[0].rgba = {0, 0, 0, 255, 128, 64, 32, 255, 192, 192, 192, 255};
    data.zones[0].image_contrast_percent   = 100.f;
    data.zones[0].image_saturation_percent = 0.f;
    binding.source.uvs = {Vec2f(0.5f, 0.5f), Vec2f(0.5f, 0.5f), Vec2f(0.5f, 0.5f)};
    const RGBA grayscale = ImageMap::sample_source(data, binding, Vec3f(1.f, 0.f, 0.f));
    CHECK(grayscale[0] == Approx(grayscale[1]));
    CHECK(grayscale[1] == Approx(grayscale[2]));

    data.zones[0].tone_gamma                = 2.f;
    data.zones[0].overhang_contrast_percent = 100.f;
    const RGBA gamma_adjusted = ImageMap::adjusted_modulation_target_color(RGBA{0.25f, 0.25f, 0.25f, 1.f}, data.zones[0]);
    CHECK(gamma_adjusted[0] == Approx(0.5f));

    data.zones[0].overhang_contrast_percent = 200.f;
    std::vector<double> component_weights{0.25, 0.75};
    ImageMap::apply_modulation_component_contrast(component_weights, data.zones[0]);
    CHECK(component_weights[0] == Approx(0.0));
    CHECK(component_weights[1] == Approx(1.0));
}

TEST_CASE("Orthographic image projection stays on the clicked connected surface", "[ImageMap][Projection]")
{
    indexed_triangle_set its;
    its.vertices = {Vec3f(-1.f, -1.f, 0.f), Vec3f(1.f, -1.f, 0.f), Vec3f(0.f, 1.f, 0.f),
                    Vec3f(-1.f, -1.f, -0.2f), Vec3f(1.f, -1.f, -0.2f), Vec3f(0.f, 1.f, -0.2f)};
    its.indices.emplace_back(0, 1, 2);
    its.indices.emplace_back(3, 4, 5);
    TriangleMesh mesh(std::move(its));

    ImageMap::TextureAsset texture{"projection", "projection.png", 1, 1, {255, 0, 0, 255}};
    ImageMap::Zone         zone;
    zone.stable_id = "projected-zone";
    zone.palette.push_back({RGBA{1.f, 0.f, 0.f, 1.f}, 0, 1});

    ImageMap::OrthographicProjection projection;
    projection.normal        = Vec3d::UnitZ();
    projection.up            = Vec3d::UnitY();
    projection.width_mm      = 4.0;
    projection.height_mm     = 4.0;
    projection.max_depth_mm  = 1.0;
    projection.seed_triangle = 0;

    ImageMap::VolumeData data;
    const ImageMap::ProjectionResult result =
        ImageMap::append_orthographic_projection(mesh, std::move(texture), std::move(zone), projection, data);
    REQUIRE(result);
    CHECK(result.projected_triangle_count == 1);
    REQUIRE(data.triangle_bindings.size() == 1);
    CHECK(data.triangle_bindings.front().triangle_index == 0);
}

TEST_CASE("GLB image-map import preserves embedded textures and printable units", "[ImageMap][Assimp][GLB]")
{
    const boost::filesystem::path directory = boost::filesystem::temp_directory_path() /
                                              boost::filesystem::unique_path("fs-glb-image-map-%%%%-%%%%");
    REQUIRE(boost::filesystem::create_directories(directory));
    struct RemoveTemporaryDirectory
    {
        boost::filesystem::path path;
        ~RemoveTemporaryDirectory() { boost::filesystem::remove_all(path); }
    } remove_temporary_directory{directory};

    const boost::filesystem::path texture_path = directory / "embedded.png";
    REQUIRE(png::write_rgb_to_file(texture_path.string(), 1, 1, std::vector<uint8_t>{25, 125, 225}));
    boost::nowide::ifstream    texture_input(texture_path.string(), std::ios::binary);
    const std::vector<uint8_t> png_bytes((std::istreambuf_iterator<char>(texture_input)), std::istreambuf_iterator<char>());
    REQUIRE_FALSE(png_bytes.empty());

    const boost::filesystem::path glb_path = directory / "triangle.glb";
    REQUIRE(write_textured_triangle_glb(glb_path, png_bytes));

    Model       model;
    ObjInfo     info;
    std::string message;
    REQUIRE(load_assimp_color_mesh(glb_path.string().c_str(), &model, info, message));
    CHECK(message.empty());
    CHECK(info.source_format == "GLB");
    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->volumes.size() == 1);
    const TriangleMesh& mesh = model.objects.front()->volumes.front()->mesh();
    REQUIRE(mesh.its.vertices.size() == 3);
    REQUIRE(mesh.its.indices.size() == 1);
    const BoundingBoxf3 bounds = mesh.bounding_box();
    CHECK(bounds.size().x() == Approx(10.f));
    CHECK(bounds.size().z() == Approx(10.f));
    CHECK(bounds.size().y() == Approx(0.f));
    REQUIRE(info.triangle_uvs_valid == std::vector<uint8_t>{1});
    REQUIRE(info.triangle_texture_files.size() == 1);
    REQUIRE(info.embedded_textures.size() == 1);
    const auto& texture = info.embedded_textures.begin()->second;
    CHECK(texture.display_name == "embedded.png");
    CHECK(texture.width == 1);
    CHECK(texture.height == 1);
    CHECK(texture.rgba == std::vector<uint8_t>{25, 125, 225, 255});

    ObjImageMapSamplePlan plan;
    REQUIRE(build_obj_image_map_sample_plan(mesh, info, 1.f, 32, plan));
    CHECK(plan.loaded_texture_count == 1);
    CHECK(plan.textured_triangle_count == 1);
    REQUIRE_FALSE(plan.colors.empty());
    CHECK(plan.colors.front()[0] == Approx(25.f / 255.f));
    CHECK(plan.colors.front()[1] == Approx(125.f / 255.f));
    CHECK(plan.colors.front()[2] == Approx(225.f / 255.f));

    ImageMap::Zone zone;
    zone.stable_id = "glb-test-zone";
    zone.palette.push_back({plan.colors.front(), 0, 1});
    ImageMap::VolumeData data;
    REQUIRE(build_obj_image_map_volume_data(mesh, info, ObjColorImportSource::ImageTexture, {}, std::move(zone), data));
    REQUIRE(data.texture_assets.size() == 1);
    CHECK(data.texture_assets.front().rgba == std::vector<uint8_t>{25, 125, 225, 255});

    bool             callback_called = false;
    ObjImportColorFn color_callback  = [&callback_called](std::vector<RGBA>& colors, bool, std::vector<unsigned char>& filament_ids,
                                                         unsigned char&, ObjColorImportContext& context) {
        callback_called = true;
        CHECK(context.source == ObjColorImportSource::ImageTexture);
        CHECK(context.mode == ObjColorImportMode::ImageMap);
        REQUIRE_FALSE(colors.empty());
        filament_ids.assign(colors.size(), 1);
        context.image_map_render_mode              = ObjImageMapRenderMode::PerimeterModulationV2;
        context.image_map_palette_colors           = {colors.front()};
        context.image_map_palette_filament_ids     = {1};
        context.image_map_palette_mixed_stable_ids = {0};
    };
    Model routed_model = Model::read_from_file(glb_path.string(), nullptr, nullptr, LoadStrategy::AddDefaultInstances, nullptr, nullptr,
                                               nullptr, nullptr, nullptr, nullptr, nullptr, 0, color_callback);
    CHECK(callback_called);
    REQUIRE(routed_model.objects.size() == 1);
    REQUIRE(routed_model.objects.front()->volumes.size() == 1);
    REQUIRE(routed_model.objects.front()->volumes.front()->has_image_map_data());
    REQUIRE(routed_model.objects.front()->volumes.front()->image_map_data()->zones.size() == 1);
    CHECK(routed_model.objects.front()->volumes.front()->image_map_data()->zones.front().render_mode ==
          ImageMap::RenderMode::PerimeterModulationV2);
}

TEST_CASE("PLY vertex colors enter the shared image-map workflow without axis conversion", "[ImageMap][Assimp][PLY]")
{
    const boost::filesystem::path directory = boost::filesystem::temp_directory_path() /
                                              boost::filesystem::unique_path("fs-ply-image-map-%%%%-%%%%");
    REQUIRE(boost::filesystem::create_directories(directory));
    struct RemoveTemporaryDirectory
    {
        boost::filesystem::path path;
        ~RemoveTemporaryDirectory() { boost::filesystem::remove_all(path); }
    } remove_temporary_directory{directory};

    const boost::filesystem::path ply_path = directory / "triangle.ply";
    boost::nowide::ofstream       output(ply_path.string(), std::ios::binary);
    output << "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
              "property uchar red\nproperty uchar green\nproperty uchar blue\nproperty uchar alpha\n"
              "element face 1\nproperty list uchar int vertex_indices\nend_header\n"
              "0 0 0 255 0 0 255\n10 0 0 0 255 0 255\n0 10 0 0 0 255 255\n3 0 1 2\n";
    output.close();
    REQUIRE(output.good());

    Model       model;
    ObjInfo     info;
    std::string message;
    REQUIRE(load_assimp_color_mesh(ply_path.string().c_str(), &model, info, message));
    CHECK(message.empty());
    CHECK(info.source_format == "PLY");
    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->volumes.size() == 1);
    const TriangleMesh& mesh   = model.objects.front()->volumes.front()->mesh();
    const BoundingBoxf3 bounds = mesh.bounding_box();
    CHECK(bounds.size().x() == Approx(10.f));
    CHECK(bounds.size().y() == Approx(10.f));
    CHECK(bounds.size().z() == Approx(0.f));
    REQUIRE(info.vertex_colors.size() == 3);
    CHECK(info.vertex_colors[0] == RGBA{1.f, 0.f, 0.f, 1.f});
    CHECK(info.vertex_colors[1] == RGBA{0.f, 1.f, 0.f, 1.f});
    CHECK(info.vertex_colors[2] == RGBA{0.f, 0.f, 1.f, 1.f});
    CHECK(info.face_colors.empty());

    ImageMap::Zone zone;
    zone.stable_id = "ply-test-zone";
    zone.palette.push_back({info.vertex_colors.front(), 0, 1});
    ImageMap::VolumeData data;
    REQUIRE(build_obj_image_map_volume_data(mesh, info, ObjColorImportSource::VertexColors, {}, std::move(zone), data));
    REQUIRE(data.triangle_bindings.size() == 1);
    CHECK(data.triangle_bindings.front().source.kind == ImageMap::SourceKind::VertexColors);
    CHECK(data.triangle_bindings.front().source.corner_colors[0] == RGBA{1.f, 0.f, 0.f, 1.f});
    CHECK(data.triangle_bindings.front().source.corner_colors[1] == RGBA{0.f, 1.f, 0.f, 1.f});
    CHECK(data.triangle_bindings.front().source.corner_colors[2] == RGBA{0.f, 0.f, 1.f, 1.f});
}

TEST_CASE("BBS 3MF round trip restores image maps after creating model volumes", "[3mf][ImageMap]")
{
    const boost::filesystem::path working_dir = boost::filesystem::temp_directory_path() /
                                                boost::filesystem::unique_path("fs-image-map-roundtrip-%%%%-%%%%");
    REQUIRE(boost::filesystem::create_directory(working_dir));
    const boost::filesystem::path path = working_dir / "roundtrip.3mf";
    struct RemoveTemporaryDirectory {
        boost::filesystem::path path;
        ~RemoveTemporaryDirectory() { boost::filesystem::remove_all(path); }
    } remove_temporary_directory{working_dir};

    Model source_model;
    source_model.set_backup_path(working_dir.string());
    ModelObject *source_object = source_model.add_object();
    source_object->add_instance();
    ModelVolume *source_volume = source_object->add_volume(make_cube(1., 1., 1.));

    ImageMap::VolumeData data;
    data.topology_fingerprint = ImageMap::topology_fingerprint(source_volume->mesh());
    data.texture_assets.push_back({"roundtrip-texture", "Round-trip texture", 2, 1,
                                   {12, 34, 56, 255, 210, 180, 90, 255}});
    ImageMap::Zone zone;
    zone.stable_id   = "roundtrip-zone";
    zone.display_name = "Round-trip image map";
    zone.render_mode = ImageMap::RenderMode::PerimeterModulationV2;
    zone.color_mix_model = ImageMap::ColorMixModel::FilamentMixer;
    zone.synchronize_whole_object_cadence = true;
    zone.modulation_sample_spacing_mm      = 0.02f;
    zone.disable_broad_path_smoothing      = true;
    zone.gaussian_smoothing_strength       = 0.75f;
    zone.first_path_smoothing_strength     = 0.5f;
    zone.second_path_smoothing_strength    = 0.25f;
    zone.tone_gamma                        = 1.2f;
    zone.overhang_contrast_percent         = 135.f;
    zone.image_exposure_ev                 = 0.7f;
    zone.image_contrast_percent            = 145.f;
    zone.image_saturation_percent          = 80.f;
    zone.image_edge_boost_percent          = 175.f;
    zone.palette.push_back({RGBA{1.f, 0.f, 0.f, 1.f}, 0, 1});
    data.zones.push_back(std::move(zone));
    ImageMap::TriangleBinding binding;
    binding.triangle_index             = 0;
    binding.zone_index                 = 0;
    binding.source.kind                = ImageMap::SourceKind::Texture;
    binding.source.texture_asset_index = 0;
    binding.source.wrap_u              = ImageMap::WrapMode::Transparent;
    binding.source.wrap_v              = ImageMap::WrapMode::Transparent;
    binding.source.uvs                 = {Vec2f(0.f, 0.f), Vec2f(1.f, 0.f), Vec2f(0.f, 1.f)};
    data.triangle_bindings.push_back(binding);
    REQUIRE(source_volume->set_image_map_data(std::move(data)));

    PresetBundle preset_bundle;
    DynamicPrintConfig config = preset_bundle.project_config;
    StoreParams store;
    store.model  = &source_model;
    store.config = &config;
    store.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipStatic;
    const std::string path_string = path.string();
    store.path = path_string.c_str();
    REQUIRE(store_bbs_3mf(store));

    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    Model imported_model;
    PlateDataPtrs plates;
    std::vector<Preset *> presets;
    bool is_bbs = false;
    Semver version;
    REQUIRE(load_bbs_3mf(path_string.c_str(), &config, &substitutions, &imported_model, &plates, &presets, &is_bbs, &version, nullptr,
                         LoadStrategy::LoadModel));
    REQUIRE(imported_model.objects.size() == 1);
    REQUIRE(imported_model.objects.front()->volumes.size() == 1);
    ModelVolume *imported_volume = imported_model.objects.front()->volumes.front();
    REQUIRE(imported_volume->has_image_map_data());
    REQUIRE(imported_volume->image_map_data()->texture_assets.size() == 1);
    CHECK(imported_volume->image_map_data()->texture_assets.front().rgba ==
          std::vector<uint8_t>{12, 34, 56, 255, 210, 180, 90, 255});
    REQUIRE(imported_volume->image_map_data()->zones.size() == 1);
    CHECK(imported_volume->image_map_data()->zones.front().display_name == "Round-trip image map");
    CHECK(imported_volume->image_map_data()->zones.front().render_mode == ImageMap::RenderMode::PerimeterModulationV2);
    CHECK(imported_volume->image_map_data()->zones.front().color_mix_model == ImageMap::ColorMixModel::FilamentMixer);
    CHECK(imported_volume->image_map_data()->zones.front().synchronize_whole_object_cadence);
    CHECK(imported_volume->image_map_data()->zones.front().modulation_sample_spacing_mm == Approx(0.02f));
    CHECK(imported_volume->image_map_data()->zones.front().disable_broad_path_smoothing);
    CHECK(imported_volume->image_map_data()->zones.front().gaussian_smoothing_strength == Approx(0.75f));
    CHECK(imported_volume->image_map_data()->zones.front().first_path_smoothing_strength == Approx(0.5f));
    CHECK(imported_volume->image_map_data()->zones.front().second_path_smoothing_strength == Approx(0.25f));
    CHECK(imported_volume->image_map_data()->zones.front().tone_gamma == Approx(1.2f));
    CHECK(imported_volume->image_map_data()->zones.front().overhang_contrast_percent == Approx(135.f));
    CHECK(imported_volume->image_map_data()->zones.front().image_exposure_ev == Approx(0.7f));
    CHECK(imported_volume->image_map_data()->zones.front().image_contrast_percent == Approx(145.f));
    CHECK(imported_volume->image_map_data()->zones.front().image_saturation_percent == Approx(80.f));
    CHECK(imported_volume->image_map_data()->zones.front().image_edge_boost_percent == Approx(175.f));
    REQUIRE(imported_volume->image_map_data()->zones.front().palette.size() == 1);
    CHECK(imported_volume->image_map_data()->zones.front().palette.front().target_color == RGBA{1.f, 0.f, 0.f, 1.f});
    REQUIRE(imported_volume->image_map_data()->triangle_bindings.size() == 1);
    CHECK(imported_volume->image_map_data()->triangle_bindings.front().source.wrap_u == ImageMap::WrapMode::Transparent);
    CHECK(imported_volume->image_map_data()->triangle_bindings.front().source.wrap_v == ImageMap::WrapMode::Transparent);
    CHECK(imported_volume->image_map_data()->triangle_bindings.front().source.uvs ==
          std::array<Vec2f, 3>{Vec2f(0.f, 0.f), Vec2f(1.f, 0.f), Vec2f(0.f, 1.f)});
}

TEST_CASE("OBJ color quantization bounds training while classifying every source region", "[ObjImageMap][Quantization]")
{
    QuantKMeans quantizer;

    cv::Mat rare_color_input(QuantKMeans::MAX_TRAINING_SAMPLES + 4096, 1, CV_8UC3,
                             cv::Scalar(255, 0, 0));
    rare_color_input.at<cv::Vec3b>(rare_color_input.rows - 1, 0) = cv::Vec3b(0, 255, 0);
    const cv::Mat training = quantizer.bounded_training_sample(rare_color_input,
                                                               QuantKMeans::MAX_TRAINING_SAMPLES);
    CHECK(training.rows <= QuantKMeans::MAX_TRAINING_SAMPLES);
    CHECK(std::any_of(training.begin<cv::Vec3b>(), training.end<cv::Vec3b>(),
                      [](const cv::Vec3b& color) { return color == cv::Vec3b(0, 255, 0); }));

    const std::array<std::array<float, 4>, 4> palette{{
        {1.f, 0.f, 0.f, 1.f},
        {0.f, 1.f, 0.f, 1.f},
        {0.f, 0.f, 1.f, 1.f},
        {1.f, 1.f, 1.f, 1.f},
    }};
    std::vector<std::array<float, 4>> source_colors(size_t(QuantKMeans::MAX_TRAINING_SAMPLES) + 4096);
    for (size_t idx = 0; idx < source_colors.size(); ++idx)
        source_colors[idx] = palette[idx % palette.size()];

    std::vector<std::array<float, 4>> cluster_results;
    std::vector<int>                  labels;
    std::vector<int>                  progress;
    REQUIRE(quantizer.apply(source_colors, cluster_results, labels, 4, 4, 2,
                            [&progress](int current, int) {
                                progress.push_back(current);
                                return true;
                            }));
    CHECK(cluster_results.size() == palette.size());
    CHECK(labels.size() == source_colors.size());
    CHECK(std::all_of(labels.begin(), labels.end(),
                      [&cluster_results](int label) {
                          return label >= 0 && size_t(label) < cluster_results.size();
                      }));
    REQUIRE_FALSE(progress.empty());
    CHECK(progress.back() == 100);

    std::vector<std::array<float, 4>> extended_source_colors;
    extended_source_colors.reserve(256);
    for (int color_index = 0; color_index < 256; ++color_index) {
        const float channel = float(color_index) / 255.f;
        extended_source_colors.push_back({channel, channel, channel, 1.f});
    }
    // Use the native RGB space here so the 256 deliberately distinct input
    // values are not merged by the 8-bit Lab conversion before clustering.
    REQUIRE(quantizer.apply(extended_source_colors, cluster_results, labels, 256, 256, 0));
    CHECK(cluster_results.size() == 256);
    CHECK(labels.size() == extended_source_colors.size());
}

TEST_CASE("Continuous image-map colors solve physical filament weights without a display palette", "[ImageMap][ColorSolver]")
{
    struct ColorEngineRestore
    {
        MixedFilamentColorEngine previous;
        ~ColorEngineRestore() { MixedFilamentManager::set_color_engine(previous); }
    } restore{MixedFilamentManager::color_engine()};
    MixedFilamentManager::set_color_engine(MixedFilamentColorEngine::FilamentMixer);

    ImageMap::ContinuousColorSolver solver({{"#FF0000", std::nullopt, std::nullopt}, {"#0000FF", std::nullopt, std::nullopt}});
    REQUIRE(solver.valid());
    CHECK(solver.candidate_count() == ImageMap::continuous_color_solver_candidate_count(2));
    CHECK(solver.candidate_count() == 41);

    const std::vector<double> red  = solver.solve(RGBA{1.f, 0.f, 0.f, 1.f});
    const std::vector<double> blue = solver.solve(RGBA{0.f, 0.f, 1.f, 1.f});
    REQUIRE(red.size() == 2);
    REQUIRE(blue.size() == 2);
    CHECK(red[0] == Approx(1.0));
    CHECK(red[1] == Approx(0.0));
    CHECK(blue[0] == Approx(0.0));
    CHECK(blue[1] == Approx(1.0));

    const std::vector<double> red_exposure = solver.solve_modulation(RGBA{1.f, 0.f, 0.f, 1.f});
    REQUIRE(red_exposure.size() == 2);
    CHECK(red_exposure[0] == Approx(1.0));
    CHECK(red_exposure[1] == Approx(0.0));

    const std::vector<double> purple = solver.solve(RGBA{0.5f, 0.f, 0.5f, 1.f});
    REQUIRE(purple.size() == 2);
    CHECK(purple[0] > 0.0);
    CHECK(purple[1] > 0.0);
    CHECK(purple[0] + purple[1] == Approx(1.0));
    const std::optional<RGBA> reconstructed_purple = solver.predict_weights(purple);
    REQUIRE(reconstructed_purple);

    SECTION("five-bit texture lookups are stable and reusable")
    {
        const RGBA target{0.517f, 0.021f, 0.483f, 1.f};
        const std::vector<double> first  = solver.solve_quantized_5bit(target);
        const std::vector<double> second = solver.solve_quantized_5bit(target);
        REQUIRE(first.size() == 2);
        CHECK(second == first);
        CHECK(first[0] + first[1] == Approx(1.0));
    }

    SECTION("candidate mixing model is selectable per image map")
    {
        const std::vector<ImageMap::ContinuousColorComponent> components{{"#E6007E", std::nullopt, std::nullopt},
                                                                          {"#00AEEF", std::nullopt, std::nullopt},
                                                                          {"#F4E842", std::nullopt, std::nullopt}};
        ImageMap::ContinuousColorSolver ks_solver(components, ImageMap::ColorMixModel::FullSpectrumKmKs);
        ImageMap::ContinuousColorSolver filament_mixer_solver(components, ImageMap::ColorMixModel::FilamentMixer);
        REQUIRE(ks_solver.valid());
        REQUIRE(filament_mixer_solver.valid());
        CHECK(ks_solver.candidate_count() == filament_mixer_solver.candidate_count());

        const RGBA target{0.38f, 0.22f, 0.62f, 1.f};
        const std::optional<RGBA> ks_prediction             = ks_solver.predict_color(target);
        const std::optional<RGBA> filament_mixer_prediction = filament_mixer_solver.predict_color(target);
        REQUIRE(ks_prediction);
        REQUIRE(filament_mixer_prediction);
        auto distance = [](const RGBA& lhs, const RGBA& rhs) {
            double squared = 0.0;
            for (size_t channel = 0; channel < 3; ++channel)
                squared += std::pow(double(lhs[channel]) - double(rhs[channel]), 2.0);
            return std::sqrt(squared);
        };
        CHECK(distance(*ks_prediction, *filament_mixer_prediction) > 0.005);
    }

    SECTION("closest-mixture exposure preserves an exact neutral component")
    {
        ImageMap::ContinuousColorSolver cmy_gray_solver({{"#FF0080", std::nullopt, std::nullopt},
                                                          {"#F9ED3D", std::nullopt, std::nullopt},
                                                          {"#A3A3A3", std::nullopt, std::nullopt},
                                                          {"#08ABFB", std::nullopt, std::nullopt}});
        REQUIRE(cmy_gray_solver.valid());
        const std::vector<double> gray = cmy_gray_solver.solve(RGBA{163.f / 255.f, 163.f / 255.f, 163.f / 255.f, 1.f});
        REQUIRE(gray.size() == 4);
        CHECK(gray[0] == Approx(0.0));
        CHECK(gray[1] == Approx(0.0));
        CHECK(gray[2] == Approx(1.0));
        CHECK(gray[3] == Approx(0.0));
    }

    SECTION("adaptive recipes choose the smallest useful physical subset")
    {
        const std::vector<ImageMap::ContinuousColorComponent> components{{"#FF0080", std::nullopt, std::nullopt},
                                                                          {"#F9ED3D", std::nullopt, std::nullopt},
                                                                          {"#A3A3A3", std::nullopt, std::nullopt},
                                                                          {"#08ABFB", std::nullopt, std::nullopt}};
        ImageMap::ContinuousColorRecipeSolver recipe_solver(components, 4);
        REQUIRE(recipe_solver.valid());

        ColorRGB purple_rgb;
        const std::optional<std::string> purple_hex = full_spectrum_ks_blend_color_multi(
            std::vector<std::pair<std::string, int>>{{components[0].color_hex, 75}, {components[3].color_hex, 25}});
        REQUIRE(purple_hex);
        REQUIRE(decode_color(*purple_hex, purple_rgb));
        const ImageMap::ContinuousColorRecipe purple =
            recipe_solver.solve(RGBA{purple_rgb.r(), purple_rgb.g(), purple_rgb.b(), 1.f}, 15);
        REQUIRE(purple.valid());
        CHECK(purple.component_indices == std::vector<size_t>{0, 3});
        CHECK(purple.component_percents.size() == 2);
        CHECK(std::accumulate(purple.component_percents.begin(), purple.component_percents.end(), 0) == 100);
        CHECK(purple.layer_sequence == purple.component_indices);
        CHECK(purple.layer_sequence == std::vector<size_t>{0, 3});
        CHECK(purple.component_percents[0] > purple.component_percents[1]);
        for (size_t layer = 1; layer < 12; ++layer) {
            const std::optional<size_t> previous = purple.component_index_for_layer(layer - 1);
            const std::optional<size_t> current  = purple.component_index_for_layer(layer);
            REQUIRE(previous);
            REQUIRE(current);
            CHECK(*previous != *current);
        }

        const ImageMap::ContinuousColorRecipe gray = recipe_solver.solve(RGBA{163.f / 255.f, 163.f / 255.f, 163.f / 255.f, 1.f}, 15);
        REQUIRE(gray.valid());
        CHECK(gray.component_indices == std::vector<size_t>{2});
        CHECK(gray.layer_sequence == std::vector<size_t>{2});
    }

    const std::optional<RGBA> predicted_purple = solver.predict_color(RGBA{0.5f, 0.f, 0.5f, 0.75f});
    REQUIRE(predicted_purple);
    CHECK((*predicted_purple)[3] == Approx(0.75f));

    SECTION("perimeter projection uses the closest compact physical recipe")
    {
        const std::vector<double> reconstructed = ImageMap::compact_modulation_weights({0.55, 0.45, 0.0, 0.0});
        REQUIRE(reconstructed.size() == 4);
        CHECK(reconstructed[0] == Approx(1.0));
        CHECK(reconstructed[1] == Approx(0.45 / 0.55));
        CHECK(reconstructed[2] == Approx(0.0));
        CHECK(reconstructed[3] == Approx(0.0));

        ImageMap::ContinuousColorSolver cmy_solver({{"#FF0080", std::nullopt, std::nullopt},
                                                     {"#F9ED3D", std::nullopt, std::nullopt},
                                                     {"#A3A3A3", std::nullopt, std::nullopt},
                                                     {"#08ABFB", std::nullopt, std::nullopt}});
        REQUIRE(cmy_solver.valid());
        const std::optional<std::string> target_hex = full_spectrum_ks_blend_color_multi(
            std::vector<std::pair<std::string, int>>{{"#FF0080", 65}, {"#F9ED3D", 20}, {"#A3A3A3", 10}, {"#08ABFB", 5}});
        REQUIRE(target_hex);
        ColorRGB target_rgb;
        REQUIRE(decode_color(*target_hex, target_rgb));
        const RGBA target{target_rgb.r(), target_rgb.g(), target_rgb.b(), 0.75f};

        const std::vector<double> recipe = cmy_solver.solve(target);
        const std::vector<double> exposure = cmy_solver.solve_modulation(target);
        REQUIRE(recipe.size() == 4);
        REQUIRE(exposure.size() == recipe.size());
        const double strongest_recipe = *std::max_element(recipe.begin(), recipe.end());
        REQUIRE(strongest_recipe > 0.0);
        CHECK(*std::max_element(exposure.begin(), exposure.end()) == Approx(1.0));
        for (size_t component_idx = 0; component_idx < recipe.size(); ++component_idx)
            CHECK(exposure[component_idx] == Approx(recipe[component_idx] / strongest_recipe));

        const std::optional<RGBA> nearest_preview    = cmy_solver.predict_color(target);
        const std::optional<RGBA> modulation_preview = cmy_solver.predict_modulation_color(target);
        REQUIRE(nearest_preview);
        REQUIRE(modulation_preview);
        CHECK((*modulation_preview)[0] == Approx((*nearest_preview)[0]));
        CHECK((*modulation_preview)[1] == Approx((*nearest_preview)[1]));
        CHECK((*modulation_preview)[2] == Approx((*nearest_preview)[2]));
        CHECK((*modulation_preview)[3] == Approx(0.75f));
    }

    std::vector<FullSpectrumKSPairResidualColorInput> expected_inputs;
    if (purple[0] > 0.0)
        expected_inputs.push_back({"#FF0000", int(std::lround(purple[0] * 40.0)), std::nullopt, std::nullopt});
    if (purple[1] > 0.0)
        expected_inputs.push_back({"#0000FF", int(std::lround(purple[1] * 40.0)), std::nullopt, std::nullopt});
    REQUIRE(expected_inputs.size() == 2);
    const std::optional<std::string> expected_hex = full_spectrum_ks_blend_color_multi(expected_inputs);
    REQUIRE(expected_hex);
    ColorRGB expected_rgb;
    REQUIRE(decode_color(*expected_hex, expected_rgb));
    CHECK((*predicted_purple)[0] == Approx(expected_rgb.r()));
    CHECK((*predicted_purple)[1] == Approx(expected_rgb.g()));
    CHECK((*predicted_purple)[2] == Approx(expected_rgb.b()));
}

TEST_CASE("Simple perimeter modulation preserves its automatic physical palette", "[ImageMap][ColorSolver][PerimeterModulation]")
{
    struct ColorEngineRestore
    {
        MixedFilamentColorEngine previous;
        ~ColorEngineRestore() { MixedFilamentManager::set_color_engine(previous); }
    } restore{MixedFilamentManager::color_engine()};
    MixedFilamentManager::set_color_engine(MixedFilamentColorEngine::FilamentMixer);

    const std::vector<ImageMap::ContinuousColorComponent> components{
        {"#FF0000", std::nullopt, std::nullopt},
        {"#00FF00", std::nullopt, std::nullopt},
        {"#0000FF", std::nullopt, std::nullopt},
        {"#FFFFFF", std::nullopt, std::nullopt},
    };
    const std::vector<RGBA> red_blue_source{
        RGBA{1.f, 0.f, 0.f, 1.f},
        RGBA{0.5f, 0.f, 0.5f, 1.f},
        RGBA{0.f, 0.f, 1.f, 1.f},
    };

    // Automatic mode deliberately retains the white component even though the
    // representative colors use only red and blue. A texture may contain a
    // large neutral/background region outside that bounded representative set,
    // and dropping white would turn it into alternating RGB layer stripes.
    CHECK(ImageMap::select_continuous_color_components(components, red_blue_source) == std::vector<size_t>{0, 1, 2, 3});
    CHECK(ImageMap::select_continuous_color_components(components, red_blue_source, 2) == std::vector<size_t>{0, 2});
    CHECK(ImageMap::select_continuous_color_components(components, red_blue_source, 3).size() == 3);
    CHECK(ImageMap::select_continuous_color_components(components, red_blue_source, 4) == std::vector<size_t>{0, 1, 2, 3});
    CHECK(ImageMap::continuous_color_solver_max_component_count() >= 4);
}

TEST_CASE("Perimeter modulation stitches only small coplanar UV cracks", "[ImageMap][PerimeterModulation][UV]")
{
    auto make_data = [] {
        ImageMap::VolumeData data;
        ImageMap::TextureAsset texture;
        texture.stable_id = "texture";
        texture.width     = 2048;
        texture.height    = 2048;
        data.texture_assets.emplace_back(std::move(texture));
        ImageMap::Zone zone;
        zone.stable_id   = "zone";
        zone.render_mode = ImageMap::RenderMode::PerimeterModulationV2;
        data.zones.emplace_back(std::move(zone));

        ImageMap::TriangleBinding first;
        first.triangle_index              = 0;
        first.source.kind                 = ImageMap::SourceKind::Texture;
        first.source.texture_asset_index = 0;
        first.source.uvs                  = {Vec2f(0.804362f, 0.001758f), Vec2f(0.999993f, 0.001275f),
                                             Vec2f(0.999993f, 0.205478f)};
        ImageMap::TriangleBinding second;
        second.triangle_index              = 1;
        second.source.kind                 = ImageMap::SourceKind::Texture;
        second.source.texture_asset_index = 0;
        second.source.uvs                  = {Vec2f(0.804865f, 0.005842f), Vec2f(0.999993f, 0.210044f),
                                              Vec2f(0.804362f, 0.210044f)};
        data.triangle_bindings = {first, second};
        return data;
    };

    indexed_triangle_set its;
    its.vertices = {Vec3f(0.f, 0.f, 0.f), Vec3f(1.f, 0.f, 0.f), Vec3f(1.f, 1.f, 0.f), Vec3f(0.f, 1.f, 0.f)};
    its.indices.emplace_back(0, 1, 2);
    its.indices.emplace_back(0, 2, 3);
    const TriangleMesh mesh(std::move(its));

    SECTION("a sub-pixel-to-twelve-pixel internal crack is stitched")
    {
        ImageMap::VolumeData data = make_data();
        CHECK(ImageMap::stitch_perimeter_modulation_uv_cracks(mesh, data) == 1);
        CHECK((data.triangle_bindings[0].source.uvs[0] - data.triangle_bindings[1].source.uvs[0]).norm() == Approx(0.f));
        CHECK((data.triangle_bindings[0].source.uvs[2] - data.triangle_bindings[1].source.uvs[1]).norm() == Approx(0.f));
    }

    SECTION("a genuine UV island seam is preserved")
    {
        ImageMap::VolumeData data = make_data();
        data.triangle_bindings[1].source.uvs[0] += Vec2f(0.1f, 0.1f);
        data.triangle_bindings[1].source.uvs[1] += Vec2f(0.1f, 0.1f);
        CHECK(ImageMap::stitch_perimeter_modulation_uv_cracks(mesh, data) == 0);
        CHECK((data.triangle_bindings[0].source.uvs[0] - data.triangle_bindings[1].source.uvs[0]).norm() > 0.1f);
    }
}

TEST_CASE("Image-map spectrum sampling is bounded and representative", "[ImageMap][Spectrum]")
{
    std::vector<RGBA> source_colors(900, RGBA{1.f, 0.f, 0.f, 1.f});
    source_colors.insert(source_colors.end(), 100, RGBA{0.f, 0.f, 1.f, 1.f});
    source_colors.insert(source_colors.end(), 100, RGBA{0.f, 1.f, 0.f, 0.f});

    const std::vector<RGBA> representative = ImageMap::representative_source_colors(source_colors, 8, 1024);
    REQUIRE(representative.size() == 2);
    CHECK(std::any_of(representative.begin(), representative.end(),
                      [](const RGBA& color) { return color[0] > 0.9f && color[1] < 0.1f && color[2] < 0.1f; }));
    CHECK(std::any_of(representative.begin(), representative.end(),
                      [](const RGBA& color) { return color[2] > 0.9f && color[0] < 0.1f && color[1] < 0.1f; }));
    CHECK(ImageMap::representative_source_colors(source_colors, 1, 64).size() == 1);

    ImageMap::VolumeData volume_data;
    ImageMap::Zone       zone;
    zone.render_mode = ImageMap::RenderMode::PerimeterModulationV2;
    volume_data.zones.push_back(zone);
    ImageMap::TextureAsset texture;
    texture.stable_id = "spectrum-texture";
    texture.width     = 2;
    texture.height    = 1;
    texture.rgba      = {255, 0, 0, 255, 0, 0, 255, 255};
    volume_data.texture_assets.push_back(texture);
    const std::vector<RGBA> texture_colors = ImageMap::representative_source_colors(volume_data,
                                                                                    ImageMap::RenderMode::PerimeterModulationV2, 8, 16);
    CHECK(texture_colors.size() == 2);
    CHECK(ImageMap::representative_source_colors(volume_data, ImageMap::RenderMode::NormalMix, 8, 16).empty());

    SECTION("labeled source spectra retain each adaptive cluster's colors")
    {
        const std::vector<RGBA> labeled_colors{
            RGBA{1.f, 0.f, 0.f, 1.f}, RGBA{1.f, 0.25f, 0.f, 1.f}, RGBA{0.f, 0.f, 1.f, 1.f},
            RGBA{0.25f, 0.f, 1.f, 1.f}, RGBA{0.f, 1.f, 0.f, 1.f}};
        const std::vector<int> labels{0, 0, 1, 1, -1};

        const std::vector<std::vector<RGBA>> spectra =
            ImageMap::representative_labeled_source_colors(labeled_colors, labels, 2, 8, 32);
        REQUIRE(spectra.size() == 2);
        REQUIRE(spectra[0].size() == 2);
        REQUIRE(spectra[1].size() == 2);
        CHECK(std::all_of(spectra[0].begin(), spectra[0].end(), [](const RGBA& color) { return color[0] > color[2]; }));
        CHECK(std::all_of(spectra[1].begin(), spectra[1].end(), [](const RGBA& color) { return color[2] > color[0]; }));
    }

    SECTION("adaptive palette spectra are split by localized cycle")
    {
        ImageMap::VolumeData adaptive_data;
        ImageMap::Zone       adaptive_zone;
        adaptive_zone.render_mode = ImageMap::RenderMode::AdaptiveLocalizedCycles;
        adaptive_zone.palette.push_back({RGBA{1.f, 0.f, 0.f, 1.f}, 101, 5});
        adaptive_zone.palette.push_back({RGBA{0.f, 0.f, 1.f, 1.f}, 202, 6});
        adaptive_data.zones.push_back(adaptive_zone);

        ImageMap::TextureAsset used_texture;
        used_texture.stable_id = "adaptive-used";
        used_texture.width     = 4;
        used_texture.height    = 1;
        used_texture.rgba      = {255, 0, 0, 255, 255, 80, 0, 255, 0, 0, 255, 255, 80, 0, 255, 255};
        adaptive_data.texture_assets.push_back(used_texture);
        ImageMap::TextureAsset unused_texture;
        unused_texture.stable_id = "adaptive-unused";
        unused_texture.width     = 1;
        unused_texture.height    = 1;
        unused_texture.rgba      = {0, 255, 0, 255};
        adaptive_data.texture_assets.push_back(unused_texture);

        ImageMap::TriangleBinding binding;
        binding.zone_index                 = 0;
        binding.source.kind                = ImageMap::SourceKind::Texture;
        binding.source.texture_asset_index = 0;
        adaptive_data.triangle_bindings.push_back(binding);

        const std::vector<std::vector<RGBA>> spectra = ImageMap::representative_palette_source_colors(adaptive_data, 0, 8, 32);
        REQUIRE(spectra.size() == 2);
        CHECK(spectra[0].size() >= 2);
        CHECK(spectra[1].size() >= 2);
        CHECK(std::all_of(spectra[0].begin(), spectra[0].end(), [](const RGBA& color) { return color[0] > color[2] && color[1] < 0.5f; }));
        CHECK(std::all_of(spectra[1].begin(), spectra[1].end(), [](const RGBA& color) { return color[2] > color[0] && color[1] < 0.5f; }));
        CHECK(ImageMap::representative_palette_source_colors(adaptive_data, 1, 8, 32).empty());
    }
}

TEST_CASE("OBJ texture metadata stays aligned when material ranges contain quads", "[ObjImageMap]")
{
    const boost::filesystem::path directory = boost::filesystem::temp_directory_path() /
                                              boost::filesystem::unique_path("fs-obj-material-map-%%%%-%%%%");
    REQUIRE(boost::filesystem::create_directories(directory));
    const boost::filesystem::path mtl_path = directory / "material.mtl";
    const boost::filesystem::path obj_path = directory / "model.obj";
    {
        boost::nowide::ofstream output(mtl_path.string());
        REQUIRE(output.is_open());
        output << "newmtl textured\nKd 1 1 1\nmap_Kd texture.png\n"
                  "newmtl plain\nKd 0.2 0.3 0.4\n";
    }
    {
        boost::nowide::ofstream output(obj_path.string());
        REQUIRE(output.is_open());
        output << "mtllib material.mtl\n"
                  "v 0 0 0\nv 2 0 0\nv 2 2 0\nv 0 2 0\nv 1 3 0\n"
                  "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvt 0.5 1\n"
                  "usemtl textured\nf 1/1 2/2 3/3 4/4\n"
                  "usemtl plain\nf 1/1 3/3 5/5\n";
    }

    TriangleMesh                           mesh;
    ObjInfo                                info;
    std::string                            message;
    std::vector<std::pair<size_t, size_t>> parse_progress;
    REQUIRE(load_obj(obj_path.string().c_str(), &mesh, info, message,
                     [&parse_progress](ObjImageMapProgressStage stage, size_t current, size_t total) {
                         if (stage == ObjImageMapProgressStage::ParseGeometry)
                             parse_progress.emplace_back(current, total);
                         return true;
                     }));
    REQUIRE_FALSE(parse_progress.empty());
    CHECK(parse_progress.front().first == 0);
    CHECK(parse_progress.back().first == parse_progress.back().second);
    REQUIRE(mesh.its.indices.size() == 3);
    REQUIRE(info.triangle_texture_files.size() == 3);
    const std::string expected_texture_path = (directory / "texture.png").string();
    CHECK(info.triangle_texture_files[0] == expected_texture_path);
    CHECK(info.triangle_texture_files[1] == expected_texture_path);
    CHECK(info.triangle_texture_files[2].empty());
    CHECK(info.triangle_uvs_valid == std::vector<uint8_t>{1, 1, 1});
    CHECK(info.face_colors.size() == 3);
    CHECK(info.obj_directory == directory.string());

    boost::filesystem::remove_all(directory);
}

TEST_CASE("Textured OBJ import triangulates n-gons and resolves MTL texture options", "[ObjImageMap]")
{
    const boost::filesystem::path directory = boost::filesystem::temp_directory_path() /
                                              boost::filesystem::unique_path("fs-obj-textured-ngon-%%%%-%%%%");
    REQUIRE(boost::filesystem::create_directories(directory));
    const boost::filesystem::path mtl_path = directory / "material.mtl";
    const boost::filesystem::path obj_path = directory / "model.obj";
    {
        boost::nowide::ofstream output(mtl_path.string());
        REQUIRE(output.is_open());
        output << "newmtl textured\nKd 1 1 1\n"
                  "map_Kd -clamp on -s 1 1 1 \"texture with spaces.png\"\n";
    }
    {
        boost::nowide::ofstream output(obj_path.string());
        REQUIRE(output.is_open());
        output << "mtllib material.mtl\n"
                  "v 0 0 0\nv 2 0 0\nv 2 2 0\nv 1 1 0\nv 0 2 0\n"
                  "vt 0 0\nvt 1 0\nvt 1 1\nvt 0.5 0.5\nvt 0 1\n"
                  "usemtl textured\n"
                  "f 1/-5 2/-4 3/-3 4/-2 5/-1\n";
    }

    TriangleMesh mesh;
    ObjInfo      info;
    std::string  message;
    REQUIRE(load_obj(obj_path.string().c_str(), &mesh, info, message));
    REQUIRE(mesh.its.indices.size() == 3);
    REQUIRE(info.triangle_texture_files.size() == 3);
    const std::string expected_texture_path = (directory / "texture with spaces.png").string();
    CHECK(std::all_of(info.triangle_texture_files.begin(), info.triangle_texture_files.end(),
                      [&expected_texture_path](const std::string& path) { return path == expected_texture_path; }));
    CHECK(info.triangle_uvs_valid == std::vector<uint8_t>{1, 1, 1});
    CHECK(info.face_colors.size() == 3);

    boost::filesystem::remove_all(directory);
}

TEST_CASE("OBJ image-map facet trees preserve quantized filament regions", "[ObjImageMap]")
{
    CHECK(obj_image_map_leaf_count(0) == 1);
    CHECK(obj_image_map_leaf_count(3) == 64);

    const std::vector<unsigned char> ids{1, 2, 3, 4};
    CHECK(encode_obj_image_map_triangle_filaments(ids, 0, 1, 1) == "080C1C3");
    CHECK(encode_obj_image_map_triangle_filaments(std::vector<unsigned char>{2, 2, 2, 2}, 0, 1, 2).empty());
    const std::vector<unsigned char> high_ids{17, 120, 255, 1};
    const std::string high_encoded = encode_obj_image_map_triangle_filaments(high_ids, 0, 1, 1);
    REQUIRE_FALSE(high_encoded.empty());

    Model        model;
    ModelObject* object = model.add_object();
    object->add_instance();
    ModelVolume* volume = object->add_volume(make_cube(1., 1., 1.));

    ObjImageMapSamplePlan plan;
    plan.colors.resize(ids.size());
    plan.triangle_subdivision_depths.assign(volume->mesh().its.indices.size(), int8_t(-1));
    plan.triangle_subdivision_depths.front() = 1;
    plan.textured_triangle_count             = 1;

    REQUIRE(Model::obj_import_image_map_deal(ids, plan, 1, &model));
    CHECK(object->config.opt_int("extruder") == 1);
    CHECK(volume->config.opt_int("extruder") == 1);
    CHECK(volume->mmu_segmentation_facets.get_triangle_as_string(0) == "080C1C3");
    volume->mmu_segmentation_facets.set_triangle_from_string(1, high_encoded);
    CHECK(volume->mmu_segmentation_facets.get_triangle_as_string(1) == high_encoded);
    const auto& used_states = volume->mmu_segmentation_facets.get_data().used_states;
    CHECK(used_states[17]);
    CHECK(used_states[120]);
    CHECK(used_states[255]);
}

TEST_CASE("OBJ image-map sampling decodes UV textures with a bounded detail plan", "[ObjImageMap]")
{
    const boost::filesystem::path directory = boost::filesystem::temp_directory_path() /
                                              boost::filesystem::unique_path("fs-obj-image-map-%%%%-%%%%");
    REQUIRE(boost::filesystem::create_directories(directory));
    const boost::filesystem::path texture_path = directory / "texture.png";
    const std::vector<uint8_t>    pixels       = {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255};
    REQUIRE(png::write_rgb_to_file(texture_path.string(), 2, 2, pixels));

    indexed_triangle_set its;
    its.vertices = {Vec3f(0.f, 0.f, 0.f), Vec3f(4.f, 0.f, 0.f), Vec3f(0.f, 4.f, 0.f)};
    its.indices.emplace_back(0, 1, 2);
    TriangleMesh mesh(std::move(its));

    ObjInfo info;
    info.obj_directory          = directory.string();
    info.face_colors            = {RGBA{1.f, 1.f, 1.f, 1.f}};
    info.triangle_uvs           = {{Vec2f(0.f, 0.f), Vec2f(1.f, 0.f), Vec2f(0.f, 1.f)}};
    info.triangle_uvs_valid     = {1};
    info.triangle_texture_files = {"texture.png"};

    ObjImageMapSamplePlan              plan;
    std::string                        warning;
    std::set<ObjImageMapProgressStage> progress_stages;
    REQUIRE(build_obj_image_map_sample_plan(mesh, info, 1.f, 128, plan, &warning,
                                            [&progress_stages](ObjImageMapProgressStage stage, size_t, size_t) {
                                                progress_stages.emplace(stage);
                                                return true;
                                            }));
    CHECK(warning.empty());
    CHECK(progress_stages.count(ObjImageMapProgressStage::DecodeTextures) == 1);
    CHECK(progress_stages.count(ObjImageMapProgressStage::AnalyzeSurface) == 1);
    CHECK(progress_stages.count(ObjImageMapProgressStage::AllocateSamples) == 1);
    CHECK(progress_stages.count(ObjImageMapProgressStage::SampleColors) == 1);
    CHECK(plan.loaded_texture_count == 1);
    CHECK(plan.textured_triangle_count == 1);
    REQUIRE(plan.triangle_subdivision_depths.size() == 1);
    CHECK(plan.triangle_subdivision_depths.front() == 1);
    CHECK(plan.colors.size() == 4);
    std::vector<RGBA> distinct_colors = plan.colors;
    std::sort(distinct_colors.begin(), distinct_colors.end());
    distinct_colors.erase(std::unique(distinct_colors.begin(), distinct_colors.end()), distinct_colors.end());
    CHECK(distinct_colors.size() >= 3);

    SECTION("an explicitly selected texture works without an MTL texture reference")
    {
        info.triangle_texture_files.clear();
        info.has_uv_png = false;

        ObjImageMapSamplePlan selected_plan;
        std::string           selected_warning;
        REQUIRE(build_obj_image_map_sample_plan_with_texture(mesh, info, texture_path.string(), 1.f, 128, selected_plan, &selected_warning));
        CHECK(selected_warning.empty());
        CHECK(selected_plan.loaded_texture_count == 1);
        CHECK(selected_plan.textured_triangle_count == 1);
        CHECK(selected_plan.triangle_subdivision_depths == std::vector<int8_t>{1});
        CHECK(selected_plan.colors.size() == 4);
    }

    boost::filesystem::remove_all(directory);
}

TEST_CASE("OBJ image-map import retains texture pixels and UVs as persistent model data", "[ObjImageMap][ImageMap]")
{
    const boost::filesystem::path directory = boost::filesystem::temp_directory_path() /
                                              boost::filesystem::unique_path("fs-obj-persistent-map-%%%%-%%%%");
    REQUIRE(boost::filesystem::create_directories(directory));
    const boost::filesystem::path texture_path = directory / "texture.png";
    REQUIRE(png::write_rgb_to_file(texture_path.string(), 1, 1, std::vector<uint8_t>{90, 120, 150}));

    indexed_triangle_set its;
    its.vertices = {Vec3f(0.f, 0.f, 0.f), Vec3f(4.f, 0.f, 0.f), Vec3f(0.f, 4.f, 0.f)};
    its.indices.emplace_back(0, 1, 2);
    TriangleMesh mesh(std::move(its));

    ObjInfo info;
    info.obj_directory          = directory.string();
    info.face_colors            = {RGBA{1.f, 1.f, 1.f, 1.f}};
    info.triangle_uvs           = {{Vec2f(0.f, 0.f), Vec2f(1.f, 0.f), Vec2f(0.f, 1.f)}};
    info.triangle_uvs_valid     = {1};
    info.triangle_texture_files = {texture_path.string()};

    ImageMap::Zone zone;
    zone.stable_id = "zone-test";
    zone.palette.push_back({RGBA{0.35f, 0.47f, 0.59f, 1.f}, 0, 1});
    zone.palette.push_back({RGBA{0.8f, 0.2f, 0.1f, 1.f}, 0, 5});
    ImageMap::VolumeData data;
    std::string          warning;
    REQUIRE(build_obj_image_map_volume_data(mesh, info, ObjColorImportSource::ImageTexture, {}, std::move(zone), data, &warning));
    CHECK(warning.empty());
    REQUIRE(data.texture_assets.size() == 1);
    CHECK(data.texture_assets.front().rgba == std::vector<uint8_t>{90, 120, 150, 255});
    REQUIRE(data.triangle_bindings.size() == 1);
    CHECK(data.triangle_bindings.front().source.uvs[1] == Vec2f(1.f, 0.f));

    Model        model;
    ModelObject* object = model.add_object();
    object->add_instance();
    ModelVolume* volume = object->add_volume(mesh);
    REQUIRE(Model::obj_import_persistent_image_map_deal(std::move(data), 1, &model));
    CHECK(volume->has_image_map_data());
    CHECK(volume->mmu_segmentation_facets.empty());
    CHECK(volume->get_extruders() == std::vector<int>{1, 5});
    CHECK(volume->get_extruders_from_multi_material_painting() == std::vector<size_t>{0, 4});

    std::vector<unsigned int> remap(6, 0);
    remap[1] = 1;
    remap[5] = 3;
    remap_model_filament_ids(model, remap, 5);
    const std::shared_ptr<const ImageMap::VolumeData> remapped_data = volume->image_map_data();
    REQUIRE(remapped_data);
    REQUIRE(remapped_data->zones.front().palette.size() == 2);
    CHECK(remapped_data->zones.front().palette[1].fallback_filament_id == 3);
    CHECK(volume->get_extruders() == std::vector<int>{1, 3});

    boost::filesystem::remove_all(directory);
}

TEST_CASE("Image-map sources are sampled and rasterized only for the current slice", "[ImageMap][FacetRasterizer]")
{
    indexed_triangle_set its;
    its.vertices = {Vec3f(0.f, 0.f, 0.f), Vec3f(4.f, 0.f, 0.f), Vec3f(0.f, 4.f, 0.f)};
    its.indices.emplace_back(0, 1, 2);
    auto mesh = std::make_shared<TriangleMesh>(std::move(its));

    auto data                  = std::make_shared<ImageMap::VolumeData>();
    data->topology_fingerprint = ImageMap::topology_fingerprint(*mesh);
    data->texture_assets.push_back({"green", "green", 1, 1, {0, 255, 0, 255}});
    ImageMap::Zone zone;
    zone.stable_id             = "zone";
    zone.target_sample_size_mm = 1.5f;
    zone.max_facet_samples     = 128;
    zone.palette.push_back({RGBA{1.f, 0.f, 0.f, 1.f}, 0, 1});
    zone.palette.push_back({RGBA{0.f, 1.f, 0.f, 1.f}, 0, 2});
    data->zones.push_back(zone);
    ImageMap::TriangleBinding binding;
    binding.triangle_index             = 0;
    binding.source.kind                = ImageMap::SourceKind::Texture;
    binding.source.texture_asset_index = 0;
    binding.source.uvs                 = {Vec2f(0.f, 0.f), Vec2f(1.f, 0.f), Vec2f(0.f, 1.f)};
    data->triangle_bindings.push_back(binding);
    REQUIRE(data->validate(*mesh).valid);

    ImageMap::SurfaceSampler sampler(mesh, data);
    const auto               sample = sampler.sample(Vec3d(1.0, 1.0, 0.05), 0.1);
    REQUIRE(sample);
    REQUIRE(sample->palette_entry != nullptr);
    CHECK(sample->palette_entry->fallback_filament_id == 2);
    CHECK(sample->color[1] == Approx(1.f));

    const ImageMap::FacetRasterization rasterized = ImageMap::rasterize_facets(*mesh, *data, 255, [](const ImageMap::PaletteEntry& entry) {
        return entry.fallback_filament_id == 2 ? 120u : entry.fallback_filament_id;
    });
    CHECK(rasterized.unresolved_palette_entries == 0);
    CHECK(rasterized.sampled_leaf_count == 16);
    REQUIRE(rasterized.facets.size() == 1);
    CHECK_FALSE(rasterized.facets.front().encoded_states.empty());
}

TEST_CASE("Large low-poly image-map faces reach the configured spatial resolution", "[ImageMap][FacetRasterizer][Projection]")
{
    indexed_triangle_set its;
    its.vertices = {Vec3f(0.f, 0.f, 0.f), Vec3f(100.f, 0.f, 0.f), Vec3f(0.f, 100.f, 0.f)};
    its.indices.emplace_back(0, 1, 2);
    TriangleMesh mesh(std::move(its));

    ImageMap::VolumeData data;
    data.topology_fingerprint = ImageMap::topology_fingerprint(mesh);
    data.texture_assets.push_back({"large-face", "large-face", 1, 1, {0, 255, 0, 255}});
    ImageMap::Zone zone;
    zone.stable_id             = "large-face-zone";
    zone.target_sample_size_mm = 0.4f;
    zone.max_facet_samples     = 200'000;
    zone.palette.push_back({RGBA{0.f, 1.f, 0.f, 1.f}, 0, 2});
    data.zones.push_back(zone);
    ImageMap::TriangleBinding binding;
    binding.triangle_index             = 0;
    binding.source.kind                = ImageMap::SourceKind::Texture;
    binding.source.texture_asset_index = 0;
    binding.source.uvs                 = {Vec2f(0.f, 0.f), Vec2f(1.f, 0.f), Vec2f(0.f, 1.f)};
    data.triangle_bindings.push_back(binding);
    REQUIRE(data.validate(mesh).valid);

    const ImageMap::FacetRasterization rasterized =
        ImageMap::rasterize_facets(mesh, data, 1, [](const ImageMap::PaletteEntry& entry) { return entry.fallback_filament_id; });
    CHECK(rasterized.sampled_leaf_count == 65'536);
    CHECK(rasterized.sampled_leaf_count <= zone.max_facet_samples);
    REQUIRE(rasterized.facets.size() == 1);
    CHECK_FALSE(rasterized.facets.front().encoded_states.empty());
}

TEST_CASE("Adaptive image-map regions suppress isolated material cells", "[ImageMap][FacetRasterizer][Adaptive]")
{
    SECTION("a single sibling outlier is merged into its surrounding region")
    {
        std::vector<unsigned int> ids{3, 3, 2, 3};
        ImageMap::stabilize_adaptive_region_ids(ids, 1, 3);
        CHECK(ids == std::vector<unsigned int>{3, 3, 3, 3});
    }

    SECTION("an established two-by-two boundary is preserved")
    {
        std::vector<unsigned int> ids{2, 2, 3, 3};
        ImageMap::stabilize_adaptive_region_ids(ids, 1, 3);
        CHECK(ids == std::vector<unsigned int>{2, 2, 3, 3});
    }

    SECTION("a one-cell-wide streak is removed at the next parent level")
    {
        std::vector<unsigned int> ids{2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};
        ImageMap::stabilize_adaptive_region_ids(ids, 2, 3);
        CHECK(std::count(ids.begin(), ids.end(), 2u) == 0);
        CHECK(std::count(ids.begin(), ids.end(), 3u) == 16);
    }

    SECTION("a balanced printable boundary is preserved")
    {
        std::vector<unsigned int> ids{2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3};
        ImageMap::stabilize_adaptive_region_ids(ids, 2, 3);
        CHECK(std::count(ids.begin(), ids.end(), 2u) == 8);
        CHECK(std::count(ids.begin(), ids.end(), 3u) == 8);
    }

    SECTION("uniform adaptive facets do not retain a fully subdivided tree")
    {
        indexed_triangle_set its;
        its.vertices = {Vec3f(0.f, 0.f, 0.f), Vec3f(4.f, 0.f, 0.f), Vec3f(0.f, 4.f, 0.f)};
        its.indices.emplace_back(0, 1, 2);
        TriangleMesh mesh(std::move(its));

        ImageMap::VolumeData data;
        data.topology_fingerprint = ImageMap::topology_fingerprint(mesh);
        data.texture_assets.push_back({"green", "green", 1, 1, {0, 255, 0, 255}});
        ImageMap::Zone zone;
        zone.stable_id             = "adaptive-uniform-zone";
        zone.render_mode           = ImageMap::RenderMode::AdaptiveLocalizedCycles;
        zone.target_sample_size_mm = 1.5f;
        zone.max_facet_samples     = 128;
        zone.palette.push_back({RGBA{0.f, 1.f, 0.f, 1.f}, 0, 2});
        data.zones.push_back(zone);
        ImageMap::TriangleBinding binding;
        binding.triangle_index             = 0;
        binding.source.kind                = ImageMap::SourceKind::Texture;
        binding.source.texture_asset_index = 0;
        binding.source.uvs                 = {Vec2f(0.f, 0.f), Vec2f(1.f, 0.f), Vec2f(0.f, 1.f)};
        data.triangle_bindings.push_back(binding);
        REQUIRE(data.validate(mesh).valid);

        const ImageMap::FacetRasterization rasterized =
            ImageMap::rasterize_facets(mesh, data, 1, [](const ImageMap::PaletteEntry& entry) { return entry.fallback_filament_id; });
        REQUIRE(rasterized.facets.size() == 1);
        CHECK(rasterized.sampled_leaf_count == 16);
        CHECK(rasterized.facets.front().encoded_states == "8");
    }
}

TEST_CASE("V2 source sampling retains raw colors independently of its display palette", "[ImageMap][Sampling]")
{
    indexed_triangle_set its;
    its.vertices = {Vec3f(0.f, 0.f, 0.f), Vec3f(4.f, 0.f, 0.f), Vec3f(0.f, 4.f, 0.f)};
    its.indices.emplace_back(0, 1, 2);
    auto mesh = std::make_shared<TriangleMesh>(std::move(its));

    auto data                  = std::make_shared<ImageMap::VolumeData>();
    data->topology_fingerprint = ImageMap::topology_fingerprint(*mesh);
    ImageMap::Zone zone;
    zone.stable_id             = "continuous-zone";
    zone.render_mode           = ImageMap::RenderMode::PerimeterModulationV2;
    zone.target_sample_size_mm = 2.f;
    zone.palette.push_back({RGBA{1.f, 0.f, 0.f, 1.f}, 0, 1});
    data->zones.push_back(zone);
    ImageMap::TriangleBinding binding;
    binding.triangle_index       = 0;
    binding.source.kind          = ImageMap::SourceKind::VertexColors;
    binding.source.corner_colors = {RGBA{1.f, 0.f, 0.f, 1.f}, RGBA{0.f, 1.f, 0.f, 1.f}, RGBA{0.f, 0.f, 1.f, 1.f}};
    data->triangle_bindings.push_back(binding);

    ImageMap::SurfaceSampler sampler(mesh, data);
    const auto               continuous = sampler.sample(Vec3d(1.0, 1.0, 0.02), 0.1, ImageMap::RenderMode::PerimeterModulationV2);
    REQUIRE(continuous);
    REQUIRE(continuous->palette_entry != nullptr);
    const RGBA palette_red{1.f, 0.f, 0.f, 1.f};
    CHECK(continuous->palette_entry->target_color == palette_red);
    CHECK(continuous->color[0] + continuous->color[1] + continuous->color[2] == Approx(1.f));
    CHECK(continuous->color[1] == Approx(0.25f));
    CHECK(continuous->color[2] == Approx(0.25f));

    const ImageMap::SourceColorRasterization preview = ImageMap::rasterize_source_colors(*mesh, *data,
                                                                                         ImageMap::RenderMode::PerimeterModulationV2);
    CHECK(preview.source_triangle_count == 1);
    CHECK(preview.sampled_leaf_count == 16);
    CHECK(preview.vertices.size() == 48);
    CHECK(preview.indices.size() == preview.vertices.size());
    CHECK(std::any_of(preview.vertices.begin(), preview.vertices.end(), [](const ImageMap::SourceColorVertex& vertex) {
        return vertex.color[1] > 0.9f && vertex.color[0] < 0.1f && vertex.color[2] < 0.1f;
    }));
    CHECK(std::any_of(preview.vertices.begin(), preview.vertices.end(), [](const ImageMap::SourceColorVertex& vertex) {
        return vertex.color[2] > 0.9f && vertex.color[0] < 0.1f && vertex.color[1] < 0.1f;
    }));

    const ImageMap::SourceColorRasterization wrong_mode = ImageMap::rasterize_source_colors(*mesh, *data, ImageMap::RenderMode::NormalMix);
    CHECK(wrong_mode.vertices.empty());

    SECTION("viewport detail is capped and reports progress")
    {
        std::vector<int>                          progress_updates;
        ImageMap::SourceColorRasterizationOptions options;
        options.max_leaf_triangles                       = 4;
        options.progress                                 = [&progress_updates](int progress) { progress_updates.push_back(progress); };
        const ImageMap::SourceColorRasterization bounded = ImageMap::rasterize_source_colors(*mesh, *data,
                                                                                             ImageMap::RenderMode::PerimeterModulationV2,
                                                                                             RGBA{1.f, 1.f, 1.f, 1.f}, options);
        CHECK(bounded.sampled_leaf_count == 4);
        CHECK(bounded.vertices.size() == 12);
        CHECK_FALSE(progress_updates.empty());
        CHECK(progress_updates.back() == 85);
    }

    SECTION("viewport work can be cancelled")
    {
        ImageMap::SourceColorRasterizationOptions options;
        options.cancelled                                  = []() { return true; };
        const ImageMap::SourceColorRasterization cancelled = ImageMap::rasterize_source_colors(*mesh, *data,
                                                                                               ImageMap::RenderMode::PerimeterModulationV2,
                                                                                               RGBA{1.f, 1.f, 1.f, 1.f}, options);
        CHECK(cancelled.vertices.empty());
    }
}

TEST_CASE("Adaptive modulation routes only its perimeter mode through XY ownership", "[ImageMap][PerimeterModulation][LocalZ]")
{
    ImageMap::AdaptiveModulationMode adaptive_mode;
    SECTION("perimeter modulation uses XY ownership") { adaptive_mode = ImageMap::AdaptiveModulationMode::Perimeter; }
    SECTION("Local-Z height modulation bypasses XY ownership") { adaptive_mode = ImageMap::AdaptiveModulationMode::LocalZHeight; }

    Model        model;
    ModelObject* object = model.add_object();
    object->add_instance();
    ModelVolume* volume = object->add_volume(make_cube(1., 1., 1.));

    ImageMap::VolumeData data;
    data.topology_fingerprint = ImageMap::topology_fingerprint(volume->mesh());
    ImageMap::Zone zone;
    zone.stable_id   = "adaptive-perimeter-zone";
    zone.render_mode = ImageMap::RenderMode::AdaptiveLocalizedCycles;
    zone.adaptive_modulation_mode = adaptive_mode;
    zone.palette.push_back({RGBA{0.4f, 0.2f, 0.7f, 1.f}, 4242, 5});
    data.zones.push_back(zone);
    data.triangle_bindings.push_back(ImageMap::TriangleBinding{});
    REQUIRE(volume->set_image_map_data(std::move(data)));

    CHECK(ImageMap::model_has_perimeter_modulation(*object) ==
          (adaptive_mode == ImageMap::AdaptiveModulationMode::Perimeter));
    CHECK(ImageMap::model_uses_perimeter_modulation_filament(*object, 4242, 0) ==
          (adaptive_mode == ImageMap::AdaptiveModulationMode::Perimeter));
    CHECK_FALSE(ImageMap::model_uses_perimeter_modulation_filament(*object, 9999, 5));
}

TEST_CASE("V2 dense source preview produces a genuinely bounded LOD", "[ImageMap][Sampling][LOD]")
{
    constexpr int        grid_size = 8;
    indexed_triangle_set its;
    for (int y = 0; y < grid_size; ++y)
        for (int x = 0; x < grid_size; ++x)
            its.vertices.emplace_back(float(x), float(y), 0.f);
    for (int y = 0; y + 1 < grid_size; ++y) {
        for (int x = 0; x + 1 < grid_size; ++x) {
            const int lower_left  = y * grid_size + x;
            const int lower_right = lower_left + 1;
            const int upper_left  = lower_left + grid_size;
            const int upper_right = upper_left + 1;
            its.indices.emplace_back(lower_left, lower_right, upper_right);
            its.indices.emplace_back(lower_left, upper_right, upper_left);
        }
    }
    auto mesh = std::make_shared<TriangleMesh>(std::move(its));

    auto data                  = std::make_shared<ImageMap::VolumeData>();
    data->topology_fingerprint = ImageMap::topology_fingerprint(*mesh);
    ImageMap::Zone zone;
    zone.stable_id   = "dense-zone";
    zone.render_mode = ImageMap::RenderMode::PerimeterModulationV2;
    zone.palette.push_back({RGBA{1.f, 0.f, 0.f, 1.f}, 0, 1});
    data->zones.push_back(zone);
    for (size_t triangle_idx = 0; triangle_idx < mesh->its.indices.size(); ++triangle_idx) {
        ImageMap::TriangleBinding binding;
        binding.triangle_index       = uint32_t(triangle_idx);
        binding.source.kind          = ImageMap::SourceKind::VertexColors;
        binding.source.corner_colors = {RGBA{1.f, 0.f, 0.f, 1.f}, RGBA{0.f, 1.f, 0.f, 1.f}, RGBA{0.f, 0.f, 1.f, 1.f}};
        data->triangle_bindings.push_back(binding);
    }

    ImageMap::SourceColorRasterizationOptions options;
    options.max_leaf_triangles                       = 12;
    const ImageMap::SourceColorRasterization preview = ImageMap::rasterize_source_colors(*mesh, *data,
                                                                                         ImageMap::RenderMode::PerimeterModulationV2,
                                                                                         RGBA{1.f, 1.f, 1.f, 1.f}, options);
    REQUIRE_FALSE(preview.indices.empty());
    CHECK(preview.source_triangle_count == mesh->its.indices.size());
    CHECK(preview.lod_pass_count >= 1);
    CHECK(preview.lod_pass_count <= 4);
    CHECK(preview.sampled_leaf_count <= options.max_leaf_triangles);
    CHECK(preview.indices.size() == preview.sampled_leaf_count * 3);
    CHECK(preview.vertices.size() <= preview.sampled_leaf_count * 3);
}

TEST_CASE("V2 image-map boundary modulation is bounded and corner safe", "[ImageMap][BoundaryModulation]")
{
    ExPolygon square;
    square.contour.points = {Point(scale_(0.0), scale_(0.0)), Point(scale_(10.0), scale_(0.0)), Point(scale_(10.0), scale_(10.0)),
                             Point(scale_(0.0), scale_(10.0))};
    ImageMap::BoundaryModulationOptions options;
    options.sample_spacing_mm       = 0.25f;
    options.max_abs_displacement_mm = 0.35f;

    SECTION("constant inset preserves the offset through square corners")
    {
        const ImageMap::BoundaryModulationResult result = ImageMap::modulate_boundary({square}, options, [](const Vec2d&, const Vec2d&) {
            return ImageMap::BoundaryDisplacement{0.2f, 0.6f};
        });
        REQUIRE(result.changed);
        REQUIRE(result.geometry.size() == 1);
        CHECK_FALSE(result.geometry.front().contains(Point(scale_(0.0), scale_(5.0)), true));
        CHECK_FALSE(result.geometry.front().contains(Point(scale_(10.0), scale_(5.0)), true));
        CHECK(result.geometry.front().contains(Point(scale_(0.2), scale_(5.0)), true));
        CHECK(result.geometry.front().contains(Point(scale_(9.8), scale_(5.0)), true));
        CHECK(result.fallback_polygons == 0);
    }

    SECTION("negative offsets are clamped and modulation remains inside the authored envelope")
    {
        const ImageMap::BoundaryModulationResult result =
            ImageMap::modulate_boundary({square}, options, [](const Vec2d& point, const Vec2d&) {
                return ImageMap::BoundaryDisplacement{point.x() < 5.0 ? -0.35f : 0.35f, 0.6f};
            });
        REQUIRE(result.changed);
        REQUIRE_FALSE(result.geometry.empty());
        const BoundingBox bounds = get_extents(result.geometry);
        CHECK(unscale<double>(bounds.min.x()) >= -0.001);
        CHECK(unscale<double>(bounds.min.y()) >= -0.001);
        CHECK(unscale<double>(bounds.max.x()) <= 10.001);
        CHECK(unscale<double>(bounds.max.y()) <= 10.001);
        CHECK(result.sampled_points >= 100);
        CHECK(result.fallback_polygons == 0);
        for (const ExPolygon& polygon : result.geometry)
            CHECK(polygon.is_valid());
    }

    SECTION("ultra detail samples the complete boundary at 0.02 mm")
    {
        options.sample_spacing_mm = 0.02f;
        const ImageMap::BoundaryModulationResult result =
            ImageMap::modulate_boundary({square}, options, [](const Vec2d& point, const Vec2d&) {
                return ImageMap::BoundaryDisplacement{point.x() < 5.0 ? 0.f : 0.35f, 0.6f, 0.f, 0.f};
            });
        REQUIRE(result.changed);
        CHECK(result.sampled_points >= 1996);
        CHECK(result.fallback_polygons == 0);
    }

    SECTION("centered modulation preserves a thin hollow wall across a hard color transition")
    {
        ExPolygon ring = square;
        ring.holes.emplace_back(Points{Point(scale_(1.0), scale_(1.0)), Point(scale_(1.0), scale_(9.0)), Point(scale_(9.0), scale_(9.0)),
                                       Point(scale_(9.0), scale_(1.0))});
        options.max_abs_displacement_mm         = 0.63f;
        options.center_displacement_on_boundary = true;

        const ImageMap::BoundaryModulationResult result = ImageMap::modulate_boundary({ring}, options, [](const Vec2d& point, const Vec2d&) {
            return ImageMap::BoundaryDisplacement{point.x() < 5.0 ? 0.f : 0.63f, 0.6f};
        });

        REQUIRE(result.changed);
        REQUIRE(result.geometry.size() == 1);
        REQUIRE(result.geometry.front().holes.size() == 1);
        CHECK(result.geometry.front().is_valid());
        const BoundingBox bounds = get_extents(result.geometry);
        CHECK(unscale<double>(bounds.min.x()) < -0.25);
        CHECK_FALSE(result.geometry.front().contains(Point(scale_(10.0), scale_(5.0)), true));
        CHECK(result.geometry.front().contains(Point(scale_(9.6), scale_(5.0)), true));
        CHECK(result.fallback_polygons == 0);
    }
}

TEST_CASE("Layer-plane image-map sampling selects the outward-facing wall", "[ImageMap][Sampling][PerimeterModulation]")
{
    indexed_triangle_set its;
    // X=0 wall, deliberately wound towards +X even though the queried model
    // boundary faces -X. Real textured imports frequently mix winding this
    // way and must not lose modulation on the reversed triangles.
    its.vertices.emplace_back(0.f, 0.f, 0.f);
    its.vertices.emplace_back(0.f, 0.f, 1.f);
    its.vertices.emplace_back(0.f, 1.f, 0.f);
    // Y=0 wall, wound towards -Y.
    its.vertices.emplace_back(0.f, 0.f, 0.f);
    its.vertices.emplace_back(1.f, 0.f, 0.f);
    its.vertices.emplace_back(0.f, 0.f, 1.f);
    its.indices.emplace_back(0, 2, 1);
    its.indices.emplace_back(3, 4, 5);
    auto mesh = std::make_shared<TriangleMesh>(std::move(its));

    auto data                  = std::make_shared<ImageMap::VolumeData>();
    data->topology_fingerprint = ImageMap::topology_fingerprint(*mesh);
    ImageMap::Zone zone;
    zone.render_mode = ImageMap::RenderMode::PerimeterModulationV2;
    zone.palette.push_back({RGBA{1.f, 0.f, 0.f, 1.f}, 0, 1});
    zone.palette.push_back({RGBA{0.f, 0.f, 1.f, 1.f}, 0, 2});
    data->zones.emplace_back(zone);

    ImageMap::TriangleBinding x_wall;
    x_wall.triangle_index       = 0;
    x_wall.source.kind          = ImageMap::SourceKind::FaceColor;
    x_wall.source.corner_colors = {RGBA{1.f, 0.f, 0.f, 1.f}, RGBA{1.f, 0.f, 0.f, 1.f}, RGBA{1.f, 0.f, 0.f, 1.f}};
    data->triangle_bindings.emplace_back(x_wall);
    ImageMap::TriangleBinding y_wall;
    y_wall.triangle_index       = 1;
    y_wall.source.kind          = ImageMap::SourceKind::FaceColor;
    y_wall.source.corner_colors = {RGBA{0.f, 0.f, 1.f, 1.f}, RGBA{0.f, 0.f, 1.f, 1.f}, RGBA{0.f, 0.f, 1.f, 1.f}};
    data->triangle_bindings.emplace_back(y_wall);

    ImageMap::LayerPlaneSampler sampler(mesh, data, Transform3d::Identity(), 0.5);
    REQUIRE(sampler.segment_count() == 2);
    const std::optional<ImageMap::LayerPlaneSample> x_sample = sampler.sample(Vec2d(0., 0.), Vec2d(-1., 0.), 0.1,
                                                                              ImageMap::RenderMode::PerimeterModulationV2);
    const std::optional<ImageMap::LayerPlaneSample> y_sample = sampler.sample(Vec2d(0., 0.), Vec2d(0., -1.), 0.1,
                                                                              ImageMap::RenderMode::PerimeterModulationV2);
    REQUIRE(x_sample);
    REQUIRE(y_sample);
    CHECK(x_sample->triangle_index == 0);
    CHECK(y_sample->triangle_index == 1);
    CHECK(x_sample->color[0] == Approx(1.f));
    CHECK(y_sample->color[2] == Approx(1.f));

    const std::vector<ImageMap::LayerPlaneFieldSample> field_samples =
        sampler.field_samples(0.1, ImageMap::RenderMode::PerimeterModulationV2);
    REQUIRE(field_samples.size() == 10);
    double x_wall_weight = 0.0;
    double y_wall_weight = 0.0;
    for (const ImageMap::LayerPlaneFieldSample& field_sample : field_samples) {
        CHECK(field_sample.integration_weight_mm == Approx(0.1));
        if (field_sample.sample.triangle_index == 0) {
            CHECK(field_sample.print_point.x() == Approx(0.0));
            CHECK(field_sample.sample.color[0] == Approx(1.f));
            x_wall_weight += field_sample.integration_weight_mm;
        } else if (field_sample.sample.triangle_index == 1) {
            CHECK(field_sample.print_point.y() == Approx(0.0));
            CHECK(field_sample.sample.color[2] == Approx(1.f));
            y_wall_weight += field_sample.integration_weight_mm;
        } else {
            FAIL("Unexpected triangle in the layer component-weight field");
        }
    }
    CHECK(x_wall_weight == Approx(0.5));
    CHECK(y_wall_weight == Approx(0.5));
}

TEST_CASE("Simple PM calibration chart is one guarded recipe field", "[ImageMap][Calibration]")
{
    const std::vector<ImageMap::ContinuousColorComponent> components = {
        {"#101010", 1.0, std::string("black")},
        {"#f0f0f0", 4.0, std::string("white")},
        {"#00a0d0", 2.0, std::string("cyan")},
        {"#d02070", 1.5, std::string("magenta")}};
    ImageMap::SimplePmCalibrationChartSettings settings;
    settings.texture_width    = 640;
    settings.texture_height   = 480;

    const ImageMap::SimplePmCalibrationChart chart = ImageMap::make_simple_pm_calibration_chart(
        components, ImageMap::ColorMixModel::FilamentMixer, settings);
    REQUIRE(chart.valid());
    CHECK(chart.texture.valid());
    CHECK(chart.patches.size() <= chart.capacity);
    CHECK(chart.columns == 19);
    CHECK(chart.rows == 14);
    CHECK(chart.capacity == 266);
    CHECK(chart.patches.size() == 266);
    CHECK(chart.patches.size() < chart.total_recipe_count);
    CHECK(chart.columns > 1);
    CHECK(chart.rows > 1);

    std::set<std::vector<uint8_t>> recipes;
    size_t solid_anchors = 0;
    for (const ImageMap::SimplePmCalibrationPatch& patch : chart.patches) {
        CHECK(recipes.insert(patch.component_units).second);
        CHECK(std::accumulate(patch.component_units.begin(), patch.component_units.end(), 0) == chart.total_units);
        solid_anchors += patch.solid_anchor ? 1u : 0u;
    }
    CHECK(solid_anchors == components.size());

    // The physical separation is a guard-colored strip in the same texture,
    // not empty space between independently printable swatch objects.
    REQUIRE(chart.patches.size() >= 2);
    const auto& first = chart.patches[0];
    const auto& second = chart.patches[1];
    REQUIRE(first.uv_rect[1] == Approx(second.uv_rect[1]));
    const float gutter_u = 0.5f * (first.uv_rect[2] + second.uv_rect[0]);
    const float center_v = 0.5f * (first.uv_rect[1] + first.uv_rect[3]);
    const size_t x = std::min<size_t>(size_t(gutter_u * float(chart.texture.width)), chart.texture.width - 1);
    const size_t y = std::min<size_t>(size_t(center_v * float(chart.texture.height)), chart.texture.height - 1);
    const size_t offset = (y * chart.texture.width + x) * 4;
    CHECK(float(chart.texture.rgba[offset]) / 255.f == Approx(chart.guard_color[0]).margin(1.f / 255.f));
    CHECK(float(chart.texture.rgba[offset + 1]) / 255.f == Approx(chart.guard_color[1]).margin(1.f / 255.f));
    CHECK(float(chart.texture.rgba[offset + 2]) / 255.f == Approx(chart.guard_color[2]).margin(1.f / 255.f));
}

TEST_CASE("Simple PM calibration photo reader registers its generated plaque", "[ImageMap][Calibration]")
{
    const std::vector<ImageMap::ContinuousColorComponent> components = {
        {"#101010", std::nullopt, std::string("black")},
        {"#f0f0f0", std::nullopt, std::string("white")},
        {"#00a0d0", std::nullopt, std::string("cyan")}};
    ImageMap::SimplePmCalibrationChartSettings settings;
    settings.plaque_width_mm  = 64.f;
    settings.plaque_height_mm = 48.f;
    settings.texture_width    = 640;
    settings.texture_height   = 480;
    const ImageMap::SimplePmCalibrationChart chart = ImageMap::make_simple_pm_calibration_chart(
        components, ImageMap::ColorMixModel::FilamentMixer, settings);
    REQUIRE(chart.valid());

    const ImageMap::SimplePmPhotoAnalysis analysis = ImageMap::analyze_simple_pm_calibration_photo(
        chart.texture.rgba, chart.texture.width, chart.texture.height, components,
        ImageMap::ColorMixModel::FilamentMixer, settings);
    INFO(analysis.error);
    REQUIRE(analysis.success);
    CHECK(analysis.profile.signature == chart.signature);
    CHECK(analysis.profile.components.size() == components.size());
    CHECK(analysis.accepted_patch_count > chart.patches.size() / 2);
    CHECK(analysis.profile.observations.size() == analysis.accepted_patch_count);
}
