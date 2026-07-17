#include <catch2/catch.hpp>

#include "libslic3r/Format/OBJImageMap.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

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

    boost::filesystem::remove_all(directory);
}
