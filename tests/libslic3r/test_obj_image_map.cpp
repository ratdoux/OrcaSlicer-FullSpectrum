#include <catch2/catch.hpp>

#include "libslic3r/Format/OBJImageMap.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/ImageMap/BoundaryModulation.hpp"
#include "libslic3r/ImageMap/FacetRasterizer.hpp"
#include "libslic3r/ImageMap/Sampling.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>

using namespace Slic3r;

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

    TriangleMesh mesh;
    ObjInfo      info;
    std::string  message;
    REQUIRE(load_obj(obj_path.string().c_str(), &mesh, info, message));
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

    ObjImageMapSamplePlan plan;
    std::string           warning;
    REQUIRE(build_obj_image_map_sample_plan(mesh, info, 1.f, 128, plan, &warning));
    CHECK(warning.empty());
    CHECK(plan.loaded_texture_count == 1);
    CHECK(plan.textured_triangle_count == 1);
    REQUIRE(plan.triangle_subdivision_depths.size() == 1);
    CHECK(plan.triangle_subdivision_depths.front() == 1);
    CHECK(plan.colors.size() == 4);

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
    ImageMap::VolumeData data;
    std::string warning;
    REQUIRE(build_obj_image_map_volume_data(mesh, info, ObjColorImportSource::ImageTexture, {}, std::move(zone), data, &warning));
    CHECK(warning.empty());
    REQUIRE(data.texture_assets.size() == 1);
    CHECK(data.texture_assets.front().rgba == std::vector<uint8_t>{90, 120, 150, 255});
    REQUIRE(data.triangle_bindings.size() == 1);
    CHECK(data.triangle_bindings.front().source.uvs[1] == Vec2f(1.f, 0.f));

    Model model;
    ModelObject *object = model.add_object();
    object->add_instance();
    ModelVolume *volume = object->add_volume(mesh);
    REQUIRE(Model::obj_import_persistent_image_map_deal(std::move(data), 1, &model));
    CHECK(volume->has_image_map_data());
    CHECK(volume->mmu_segmentation_facets.empty());

    boost::filesystem::remove_all(directory);
}

TEST_CASE("Image-map sources are sampled and rasterized only for the current slice", "[ImageMap][FacetRasterizer]")
{
    indexed_triangle_set its;
    its.vertices = {Vec3f(0.f, 0.f, 0.f), Vec3f(4.f, 0.f, 0.f), Vec3f(0.f, 4.f, 0.f)};
    its.indices.emplace_back(0, 1, 2);
    auto mesh = std::make_shared<TriangleMesh>(std::move(its));

    auto data = std::make_shared<ImageMap::VolumeData>();
    data->topology_fingerprint = ImageMap::topology_fingerprint(*mesh);
    data->texture_assets.push_back({"green", "green", 1, 1, {0, 255, 0, 255}});
    ImageMap::Zone zone;
    zone.stable_id = "zone";
    zone.target_sample_size_mm = 1.5f;
    zone.max_facet_samples = 128;
    zone.palette.push_back({RGBA{1.f, 0.f, 0.f, 1.f}, 0, 1});
    zone.palette.push_back({RGBA{0.f, 1.f, 0.f, 1.f}, 0, 2});
    data->zones.push_back(zone);
    ImageMap::TriangleBinding binding;
    binding.triangle_index = 0;
    binding.source.kind = ImageMap::SourceKind::Texture;
    binding.source.texture_asset_index = 0;
    binding.source.uvs = {Vec2f(0.f, 0.f), Vec2f(1.f, 0.f), Vec2f(0.f, 1.f)};
    data->triangle_bindings.push_back(binding);
    REQUIRE(data->validate(*mesh).valid);

    ImageMap::SurfaceSampler sampler(mesh, data);
    const auto sample = sampler.sample(Vec3d(1.0, 1.0, 0.05), 0.1);
    REQUIRE(sample);
    REQUIRE(sample->palette_entry != nullptr);
    CHECK(sample->palette_entry->fallback_filament_id == 2);
    CHECK(sample->color[1] == Approx(1.f));

    const ImageMap::FacetRasterization rasterized = ImageMap::rasterize_facets(
        *mesh, *data, 1, [](const ImageMap::PaletteEntry &entry) { return entry.fallback_filament_id; });
    CHECK(rasterized.unresolved_palette_entries == 0);
    CHECK(rasterized.sampled_leaf_count == 16);
    REQUIRE(rasterized.facets.size() == 1);
    CHECK_FALSE(rasterized.facets.front().encoded_states.empty());
}

TEST_CASE("V2 image-map boundary modulation is bounded and corner safe", "[ImageMap][BoundaryModulation]")
{
    ExPolygon square;
    square.contour.points = {
        Point(scale_(0.0), scale_(0.0)),
        Point(scale_(10.0), scale_(0.0)),
        Point(scale_(10.0), scale_(10.0)),
        Point(scale_(0.0), scale_(10.0))
    };
    ImageMap::BoundaryModulationOptions options;
    options.sample_spacing_mm = 0.25f;
    options.max_abs_displacement_mm = 0.35f;

    SECTION("constant inset preserves the offset through square corners")
    {
        const ImageMap::BoundaryModulationResult result = ImageMap::modulate_boundary(
            {square}, options, [](const Vec2d &, const Vec2d &) {
                return ImageMap::BoundaryDisplacement{0.2f, 0.6f};
            });
        REQUIRE(result.changed);
        REQUIRE(result.geometry.size() == 1);
        const BoundingBox bounds = get_extents(result.geometry);
        CHECK(unscale<double>(bounds.min.x()) == Approx(0.2).margin(0.01));
        CHECK(unscale<double>(bounds.min.y()) == Approx(0.2).margin(0.01));
        CHECK(unscale<double>(bounds.max.x()) == Approx(9.8).margin(0.01));
        CHECK(unscale<double>(bounds.max.y()) == Approx(9.8).margin(0.01));
        CHECK(result.fallback_polygons == 0);
    }

    SECTION("outward modulation cannot create an acute transition spike")
    {
        const ImageMap::BoundaryModulationResult result = ImageMap::modulate_boundary(
            {square}, options, [](const Vec2d &point, const Vec2d &) {
                return ImageMap::BoundaryDisplacement{point.x() < 5.0 ? -0.35f : 0.35f, 0.6f};
            });
        REQUIRE(result.changed);
        REQUIRE_FALSE(result.geometry.empty());
        const BoundingBox bounds = get_extents(result.geometry);
        CHECK(unscale<double>(bounds.min.x()) >= -0.36);
        CHECK(unscale<double>(bounds.min.y()) >= -0.36);
        CHECK(unscale<double>(bounds.max.x()) <= 10.36);
        CHECK(unscale<double>(bounds.max.y()) <= 10.36);
        CHECK(result.sampled_points >= 100);
        CHECK(result.fallback_polygons == 0);
        for (const ExPolygon &polygon : result.geometry)
            CHECK(polygon.is_valid());
    }
}
