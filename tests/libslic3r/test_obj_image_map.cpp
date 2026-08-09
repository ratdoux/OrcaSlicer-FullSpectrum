#include <catch2/catch.hpp>

#include "libslic3r/Format/OBJImageMap.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/FullSpectrumKSPairResidual.hpp"
#include "libslic3r/ImageMap/BoundaryModulation.hpp"
#include "libslic3r/ImageMap/ContinuousColorSolver.hpp"
#include "libslic3r/ImageMap/FacetRasterizer.hpp"
#include "libslic3r/ImageMap/PerimeterEnvelopeRenderer.hpp"
#include "libslic3r/ImageMap/Sampling.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ObjColorUtils.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cmath>
#include <set>

using namespace Slic3r;

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

    const std::vector<double> purple = solver.solve(RGBA{0.5f, 0.f, 0.5f, 1.f});
    REQUIRE(purple.size() == 2);
    CHECK(purple[0] > 0.0);
    CHECK(purple[1] > 0.0);
    CHECK(purple[0] + purple[1] == Approx(1.0));

    const std::optional<RGBA> predicted_purple = solver.predict_color(RGBA{0.5f, 0.f, 0.5f, 0.75f});
    REQUIRE(predicted_purple);
    CHECK((*predicted_purple)[3] == Approx(0.75f));

    SECTION("perimeter projection does not amplify imperceptible texture changes")
    {
        ImageMap::ContinuousColorSolver cmy_solver({{"#FF0080", std::nullopt, std::nullopt},
                                                     {"#F9ED3D", std::nullopt, std::nullopt},
                                                     {"#08ABFB", std::nullopt, std::nullopt}}, true);
        REQUIRE(cmy_solver.valid());
        const RGBA             first{0.961259f, 0.0602889f, 0.230826f, 1.f};
        const RGBA             second{0.96146f, 0.0617287f, 0.233602f, 1.f};
        const std::vector<int> base_percents{34, 33, 33};
        const std::vector<float> nearest_first =
            mixed_filament_surface_offsets_for_apparent_weights(base_percents, cmy_solver.solve(first), 0.4f);
        const std::vector<float> nearest_second =
            mixed_filament_surface_offsets_for_apparent_weights(base_percents, cmy_solver.solve(second), 0.4f);
        const std::vector<float> modulation_first =
            mixed_filament_surface_offsets_for_apparent_weights(base_percents, cmy_solver.solve_modulation(first), 0.4f);
        const std::vector<float> modulation_second =
            mixed_filament_surface_offsets_for_apparent_weights(base_percents, cmy_solver.solve_modulation(second), 0.4f);
        REQUIRE(nearest_first.size() == 3);
        REQUIRE(nearest_second.size() == 3);
        REQUIRE(modulation_first.size() == 3);
        REQUIRE(modulation_second.size() == 3);
        CHECK(std::abs(nearest_first[0] - nearest_second[0]) > 0.03f);
        CHECK(std::abs(modulation_first[0] - modulation_second[0]) < 0.005f);

        const std::optional<RGBA> modulation_preview = cmy_solver.predict_modulation_color(RGBA{first[0], first[1], first[2], 0.75f});
        REQUIRE(modulation_preview);
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

TEST_CASE("Simple perimeter modulation selects only source-relevant physical filaments", "[ImageMap][ColorSolver][PerimeterModulation]")
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

    CHECK(ImageMap::select_continuous_color_components(components, red_blue_source, 0, 0.15) == std::vector<size_t>{0, 2});
    CHECK(ImageMap::select_continuous_color_components(components, red_blue_source, 3, 0.15).size() == 3);
    CHECK(ImageMap::select_continuous_color_components(components, red_blue_source, 4, 0.15) == std::vector<size_t>{0, 1, 2, 3});
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

TEST_CASE("Adaptive image maps participate in perimeter modulation ownership", "[ImageMap][PerimeterModulation]")
{
    Model        model;
    ModelObject* object = model.add_object();
    object->add_instance();
    ModelVolume* volume = object->add_volume(make_cube(1., 1., 1.));

    ImageMap::VolumeData data;
    data.topology_fingerprint = ImageMap::topology_fingerprint(volume->mesh());
    ImageMap::Zone zone;
    zone.stable_id   = "adaptive-perimeter-zone";
    zone.render_mode = ImageMap::RenderMode::AdaptiveLocalizedCycles;
    zone.palette.push_back({RGBA{0.4f, 0.2f, 0.7f, 1.f}, 4242, 5});
    data.zones.push_back(zone);
    data.triangle_bindings.push_back(ImageMap::TriangleBinding{});
    REQUIRE(volume->set_image_map_data(std::move(data)));

    CHECK(ImageMap::model_has_perimeter_modulation(*object));
    CHECK(ImageMap::model_uses_perimeter_modulation_filament(*object, 4242, 0));
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
        const BoundingBox bounds = get_extents(result.geometry);
        CHECK(unscale<double>(bounds.min.x()) == Approx(0.2).margin(0.01));
        CHECK(unscale<double>(bounds.min.y()) == Approx(0.2).margin(0.01));
        CHECK(unscale<double>(bounds.max.x()) == Approx(9.8).margin(0.01));
        CHECK(unscale<double>(bounds.max.y()) == Approx(9.8).margin(0.01));
        CHECK(result.fallback_polygons == 0);
    }

    SECTION("outward modulation cannot create an acute transition spike")
    {
        const ImageMap::BoundaryModulationResult result =
            ImageMap::modulate_boundary({square}, options, [](const Vec2d& point, const Vec2d&) {
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
        for (const ExPolygon& polygon : result.geometry)
            CHECK(polygon.is_valid());
    }
}
