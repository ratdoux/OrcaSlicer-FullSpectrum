#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "material_band_fixtures.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <boost/filesystem.hpp>
#include <tbb/task_arena.h>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>

using namespace Slic3r;
using namespace MaterialBandFixtures;

namespace {
struct TemporaryDirectory
{
    boost::filesystem::path path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("material-band-%%%%-%%%%");
    TemporaryDirectory() { boost::filesystem::create_directories(path); }
    ~TemporaryDirectory()
    {
        boost::system::error_code error;
        boost::filesystem::remove_all(path, error);
    }
};

bool save_fixture(const boost::filesystem::path& path, Fixture& fixture)
{
    fixture.model.set_backup_path(path.parent_path().string());
    StoreParams store;
    const auto  filename = path.string();
    store.path           = filename.c_str();
    store.model          = &fixture.model;
    store.config         = &fixture.config;
    store.strategy       = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipStatic | SaveStrategy::SkipAuxiliary;
    return store_bbs_3mf(store);
}

void check_closed_mesh(const TriangleMesh& mesh, double expected_volume)
{
    std::map<std::pair<int, int>, std::pair<int, int>> edges;
    double                                             volume = 0.;
    for (const auto& face : mesh.its.indices) {
        for (int vertex : {face[0], face[1], face[2]})
            REQUIRE(size_t(vertex) < mesh.its.vertices.size());
        const Vec3d a = mesh.its.vertices[face[0]].cast<double>();
        const Vec3d b = mesh.its.vertices[face[1]].cast<double>();
        const Vec3d c = mesh.its.vertices[face[2]].cast<double>();
        REQUIRE((b - a).cross(c - a).norm() > 1e-9);
        volume += a.dot(b.cross(c)) / 6.;
        for (int i = 0; i < 3; ++i) {
            int   x = face[i], y = face[(i + 1) % 3];
            auto& edge = edges[{std::min(x, y), std::max(x, y)}];
            ++edge.first;
            edge.second += x < y ? 1 : -1;
        }
    }
    for (const auto& edge : edges) {
        CHECK(edge.second.first == 2);
        CHECK(edge.second.second == 0);
    }
    CHECK(volume == Approx(expected_volume).epsilon(1e-6));
}
} // namespace

TEST_CASE("Material band fixtures have closed solids and independent slab oracles", "[material_band][unit]")
{
    const auto   id      = GENERATE("MB01", "MB02-edge", "MB02-transition", "MB03");
    auto         fixture = make_fixture(id);
    const auto&  spec    = fixture.manifest;
    const double x = spec.at("dimensions_mm").at(0), y = spec.at("dimensions_mm").at(1), z = spec.at("dimensions_mm").at(2);
    const auto&  mesh = fixture.model.objects.front()->volumes.front()->mesh();
    REQUIRE(mesh.its.indices.size() == spec.at("mesh_triangles").get<size_t>());
    check_closed_mesh(mesh, x * y * z);
    // Independent midpoint quadrature over the analytic box. Symmetric targets
    // and permitted transitions must each integrate to half the macro volume.
    double           target = 0., allowed = 0.;
    constexpr size_t samples = 12000;
    for (size_t i = 0; i < samples; ++i) {
        const double sample_x = x * (i + .5) / samples;
        target += target_fraction(spec, sample_x);
        allowed += permitted_fraction(spec, sample_x);
        CHECK(std::abs(permitted_fraction(spec, sample_x) - target_fraction(spec, sample_x)) <=
              spec.at("transition_fraction_error").get<double>() + 1e-12);
    }
    const double slab_volume = x * y * .24;
    CHECK(target / samples * slab_volume == Approx(.5 * slab_volume).margin(1e-8));
    CHECK(allowed / samples * slab_volume == Approx(.5 * slab_volume).margin(1e-8));
    if (spec.at("field") == "linear") {
        CHECK(.24 * target_fraction(spec, 0.) == Approx(.06));
        CHECK(.24 * target_fraction(spec, x) == Approx(.18));
    }
    if (spec.at("boundary_policy") == "allow_transition") {
        CHECK(permitted_fraction(spec, 5.) == Approx(.25));
        CHECK(permitted_fraction(spec, 7.) == Approx(.75));
        CHECK(.24 * (permitted_fraction(spec, 7.) - permitted_fraction(spec, 5.)) / 2. == Approx(.06));
    }
}

TEST_CASE("Material band production fixtures retain physical recipes and painted shell scope", "[material_band][integration]")
{
    const auto         id      = GENERATE("MB01", "MB02-edge", "MB03");
    auto               fixture = make_fixture(id);
    TemporaryDirectory directory;
    const auto         file = directory.path / "painted.3mf";
    REQUIRE(save_fixture(file, fixture));
    Model loaded;
    loaded.set_backup_path((directory.path / "loaded").string());
    boost::filesystem::create_directories(directory.path / "loaded/Metadata/fullspectrum");
    boost::filesystem::create_directories(directory.path / "loaded/3D/Objects");
    DynamicPrintConfig config;
    config.apply(PresetBundle{}.project_config);
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    PlateDataPtrs             plates;
    std::vector<Preset*>      presets;
    bool                      is_bbs = false;
    Semver                    version;
    const bool loaded_ok = load_bbs_3mf(file.string().c_str(), &config, &substitutions, &loaded, &plates, &presets, &is_bbs, &version,
                                        nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence);
    release_PlateData_list(plates);
    for (auto* preset : presets)
        delete preset;
    REQUIRE(loaded_ok);
    REQUIRE(loaded.objects.size() == 1);
    REQUIRE(loaded.objects.front()->volumes.size() == 1);
    const auto&       original           = *fixture.model.objects.front()->volumes.front();
    const auto&       volume             = *loaded.objects.front()->volumes.front();
    const Transform3d original_transform = fixture.model.objects.front()->instances.front()->get_matrix() * original.get_matrix();
    const Transform3d loaded_transform   = loaded.objects.front()->instances.front()->get_matrix() * volume.get_matrix();
    CHECK(original.mmu_segmentation_facets.equals(volume.mmu_segmentation_facets));
    REQUIRE(original.mesh().its.indices.size() == volume.mesh().its.indices.size());
    for (size_t face = 0; face < original.mesh().its.indices.size(); ++face) {
        for (int corner = 0; corner < 3; ++corner) {
            const Vec3d source_point = original_transform *
                                       original.mesh().its.vertices[original.mesh().its.indices[face][corner]].cast<double>();
            const Vec3d loaded_point = loaded_transform *
                                       volume.mesh().its.vertices[volume.mesh().its.indices[face][corner]].cast<double>();
            CAPTURE(face, corner, source_point.x(), loaded_point.x());
            CHECK(source_point.isApprox(loaded_point, 1e-6));
        }
    }
    MixedFilamentManager manager;
    manager.auto_generate({"#FF0000", "#0000FF"});
    manager.load_custom_entries(config.opt_string("mixed_filament_definitions"), {"#FF0000", "#0000FF"});
    const auto recipes = manager.mixed_filament_definitions(2);
    const auto strips  = fixture.manifest.at("mesh_strips").get<size_t>();
    REQUIRE(std::count_if(recipes.begin(), recipes.end(),
                          [](const auto& recipe) { return recipe.source.kind == MixedFilamentSourceKind::Custom; }) == strips);
    for (size_t i = 0; i < strips; ++i) {
        const int index = manager.mixed_index_from_filament_id(unsigned(3 + i), 2);
        REQUIRE(index >= 0);
        REQUIRE(size_t(index) < recipes.size());
        const auto& recipe = recipes[size_t(index)];
        CHECK(recipe.identity.stable_id == 10001 + i);
        REQUIRE(recipe.recipe.blend.components.size() == 2);
        CHECK(recipe.recipe.blend.components[0].filament.id == 1);
        CHECK(recipe.recipe.blend.components[1].filament.id == 2);
        const auto painted = volume.mmu_segmentation_facets.get_facets(volume, static_cast<EnforcerBlockerType>(3 + i));
        REQUIRE_FALSE(painted.indices.empty());
        for (const auto& triangle : painted.indices) {
            const Vec3d  a            = loaded_transform * painted.vertices[triangle[0]].cast<double>() - Vec3d(20., 20., 0.);
            const Vec3d  b            = loaded_transform * painted.vertices[triangle[1]].cast<double>() - Vec3d(20., 20., 0.);
            const Vec3d  c            = loaded_transform * painted.vertices[triangle[2]].cast<double>() - Vec3d(20., 20., 0.);
            const double expected     = target_fraction(fixture.manifest, (a.x() + b.x() + c.x()) / 3.);
            const double quantization = fixture.manifest.at("production_recipe_quantization_fraction");
            CAPTURE(id, i, a.x(), b.x(), c.x(), recipe.recipe.blend.components[0].percent, volume.get_offset().x());
            CHECK(std::abs(recipe.recipe.blend.components[0].percent / 100. - expected) <= quantization + 1e-7);
            if (std::string(id) != "MB01")
                CHECK(std::abs((b - a).cross(c - a).z()) < 1e-6);
        }
    }
    // Keep virtual region IDs here to check segmentation before physical
    // cadence collapse. This verifies the real Print loading/slicing route.
    config.set("mixed_filament_region_collapse", false);
    Print print;
    print.set_status_silent();
    print.apply(loaded, config);
    auto* object = print.get_object(0);
    object->slice();
    REQUIRE(object->layer_count() == 10);
    std::map<unsigned int, size_t> region_surfaces;
    for (const auto* layer : object->layers())
        for (const auto* region : layer->regions())
            region_surfaces[region->region().config().wall_filament.value] += region->slices.surfaces.size();
    for (size_t i = 0; i < strips; ++i)
        CHECK(region_surfaces[unsigned(3 + i)] > 0);
    if (std::string(id) != "MB01")
        CHECK(region_surfaces[1] > 0);
}

TEST_CASE("Material band feature-off baseline capture", "[.][material_band][baseline]")
{
    const char* output_dir = std::getenv("MATERIAL_BAND_BASELINE_DIR");
    const char* case_id    = std::getenv("MATERIAL_BAND_BASELINE_CASE");
    REQUIRE(output_dir != nullptr);
    REQUIRE(case_id != nullptr);
    tbb::task_arena               threads(2);
    auto                          fixture = make_feature_off_fixture(case_id);
    const boost::filesystem::path output(output_dir);
    boost::filesystem::create_directories(output);
    nlohmann::json effective_config;
    for (const auto& key : fixture.config.keys())
        effective_config[key] = fixture.config.opt_serialize(key);
    std::ofstream(output.string() + "/profile.json") << effective_config.dump(2) << '\n';
    REQUIRE(save_fixture(output / "input.3mf", fixture));
    Print print;
    // The pinned Print leaves these runtime fields uninitialized. Explicitly
    // retain the printer-family/origin used by the recorded baseline captures.
    const char* family     = std::getenv("MATERIAL_BAND_BASELINE_FAMILY");
    print.is_BBL_printer() = !family || std::string(family) != "generic";
    print.set_plate_origin(Vec3d::Zero());
    print.set_status_silent();
    using Clock      = std::chrono::steady_clock;
    const auto start = Clock::now();
    threads.execute([&] { print.apply(fixture.model, fixture.config); });
    threads.execute([&] { print.get_object(0)->slice(); });
    const auto sliced = Clock::now();
    threads.execute([&] { print.process(); });
    const auto           processed = Clock::now();
    GCodeProcessorResult result;
    const auto           gcode = threads.execute([&] { return print.export_gcode((output / "output.gcode").string(), &result, nullptr); });
    const auto           exported = Clock::now();
    REQUIRE(boost::filesystem::exists(gcode));
    const auto* object   = print.get_object(0);
    size_t      surfaces = 0, paths = 0, segments = 0;
    for (const auto* layer : object->layers())
        for (const auto* region : layer->regions()) {
            surfaces += region->slices.surfaces.size();
            for (const auto* entities : {&region->perimeters, &region->fills}) {
                Polylines polylines;
                entities->collect_polylines(polylines);
                paths += polylines.size();
                for (const auto& polyline : polylines)
                    segments += polyline.points.empty() ? 0 : polyline.points.size() - 1;
            }
        }
    REQUIRE(paths > 0);
    nlohmann::json metrics = {{"case", case_id},
                              {"move_vertex_bytes", sizeof(GCodeProcessorResult::MoveVertex)},
                              {"runtime_printer_family", print.is_BBL_printer() ? "bbl" : "generic"},
                              {"plate_origin_mm", {0., 0., 0.}},
                              {"engine_used", "legacy"},
                              {"threads", 2},
                              {"layers", object->layer_count()},
                              {"slice_surfaces", surfaces},
                              {"path_polylines", paths},
                              {"path_segments", segments},
                              {"legacy_local_z_passes", object->local_z_sublayer_plan().size()},
                              {"preview_moves", result.moves.size()},
                              {"slice_ms", std::chrono::duration<double, std::milli>(sliced - start).count()},
                              {"paths_ms", std::chrono::duration<double, std::milli>(processed - sliced).count()},
                              {"export_ms", std::chrono::duration<double, std::milli>(exported - processed).count()}};
    std::ofstream(output.string() + "/metrics.json") << metrics.dump(2) << '\n';
}
