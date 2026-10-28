#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "material_band_fixtures.hpp"
#include "libslic3r/MaterialBand/Production.hpp"
#include "libslic3r/MaterialBand/InputAdapter.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"

#include <boost/filesystem.hpp>
#include <tbb/task_arena.h>
#include <fstream>
#include <sstream>

using namespace Slic3r;
using namespace Slic3r::MaterialBand;

namespace {
struct TemporaryDirectory
{
    boost::filesystem::path path = boost::filesystem::temp_directory_path() /
                                   boost::filesystem::unique_path("material-band-production-%%%%-%%%%");
    TemporaryDirectory() { boost::filesystem::create_directories(path); }
    ~TemporaryDirectory()
    {
        boost::system::error_code ignored;
        boost::filesystem::remove_all(path, ignored);
    }
};
const FinalPrintPlan& accepted(const Print& print)
{
    const auto result = print.material_band_result();
    REQUIRE(result);
    if (result->fallback) {
        const Vec3d witness = result->fallback->witness_mm.value_or(Vec3d::Zero());
        INFO("Witness: " << witness.transpose());
        FAIL(failure_name(result->fallback->code) << ": " << result->fallback->detail);
    }
    REQUIRE(result->used_material_bands());
    return *result->plan;
}
void slice(Print& print, MaterialBandFixtures::Fixture& fixture, int threads = 2)
{
    print.apply(fixture.model, fixture.config);
    tbb::task_arena(threads).execute([&] { print.process(); });
}
void save_demo(MaterialBandFixtures::Fixture& fixture, Print& print, const std::string& name)
{
    const char* directory = std::getenv("MATERIAL_BAND_PRODUCTION_DIR");
    if (!directory)
        return;
    boost::filesystem::create_directories(directory);
    fixture.model.set_backup_path(directory);
    const std::string path = (boost::filesystem::path(directory) / (name + ".3mf")).string();
    StoreParams       store;
    store.path     = path.c_str();
    store.model    = &fixture.model;
    store.config   = &fixture.config;
    store.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipStatic | SaveStrategy::SkipAuxiliary;
    REQUIRE(store_bbs_3mf(store));
    GCodeProcessorResult parsed;
    print.export_gcode((boost::filesystem::path(directory) / (name + ".gcode")).string(), &parsed);
    CHECK(parsed.material_band_cycles.size() == print.material_band_result()->plan->cycles.size());
}
} // namespace

TEST_CASE("Material band production exports one validated plan and logical preview", "[material_band][production]")
{
    const bool relative = GENERATE(false, true);
    auto       fixture  = MaterialBandFixtures::make_fixture("MB01");
    MaterialBandFixtures::enable_material_bands(fixture);
    fixture.config.set("use_relative_e_distances", relative);
    Print print;
    print.apply(fixture.model, fixture.config);
    StringObjectException warning;
    CHECK(print.validate(&warning).string.empty());
    slice(print, fixture);
    const auto& plan = accepted(print);
    CHECK(print.validate(&warning).string.empty());
    REQUIRE(plan.cycles.size() == 10);
    CHECK(plan.object_volume_mm3[0] == Approx(172.8).epsilon(1e-6));
    CHECK(plan.object_volume_mm3[1] == Approx(172.8).epsilon(1e-6));
    if (!relative)
        save_demo(fixture, print, "MB01");
    TemporaryDirectory   dir;
    GCodeProcessorResult parsed;
    print.export_gcode((dir.path / "accepted.gcode").string(), &parsed);
    if (const char* directory = std::getenv("MATERIAL_BAND_PRODUCTION_DIR")) {
        boost::filesystem::create_directories(directory);
        boost::filesystem::copy_file(dir.path / "accepted.gcode",
                                     boost::filesystem::path(directory) / (relative ? "relative.gcode" : "absolute.gcode"),
                                     boost::filesystem::copy_options::overwrite_existing);
    }
    REQUIRE(parsed.material_band_cycles.size() == plan.cycles.size());
    std::map<uint64_t, const PrintOperation*> operations;
    for (const auto& op : plan.operations)
        operations.emplace(op.id, &op);
    std::array<double, 2> object{}, purge{};
    std::array<size_t, 2> object_segments{}, purge_segments{};
    size_t                deposits = 0;
    for (const auto& record : parsed.material_band_moves) {
        const auto& move = parsed.moves.at(record.move_id);
        if (move.type != EMoveType::Extrude)
            continue;
        REQUIRE(operations.count(record.operation) == 1);
        const auto& op = *operations.at(record.operation);
        REQUIRE(op.kind == OperationKind::Deposit);
        CHECK((move.position.cast<double>() - op.position).norm() <= .0018);
        CHECK(record.material == move.extruder_id + 1);
        CHECK(record.cycle == op.logical_cycle);
        CHECK(record.width == Approx(op.width_mm).margin(1e-6));
        CHECK(record.height_start == Approx(op.height_start_mm).margin(1e-6));
        CHECK(record.height_end == Approx(op.height_end_mm).margin(1e-6));
        const double diameter = plan.machine.filament_diameter_mm[move.extruder_id];
        const double volume   = move.delta_extruder * PI * diameter * diameter / 4.;
        CAPTURE(relative, record.operation, move.gcode_id);
        CHECK(volume == Approx(op.volume_mm3).margin(.00006));
        (record.deposition == 1 ? object : purge)[move.extruder_id] += volume;
        ++(record.deposition == 1 ? object_segments : purge_segments)[move.extruder_id];
        ++deposits;
    }
    REQUIRE(deposits == std::count_if(plan.operations.begin(), plan.operations.end(),
                                      [](const PrintOperation& op) { return op.kind == OperationKind::Deposit; }));
    for (size_t i = 0; i < 2; ++i) {
        // Relative E rounds every segment independently. Absolute E additionally
        // includes reader float conversion; bound both by one 1e-5 mm E quantum
        // per segment, separately for object and purge deposition.
        const double quantum_volume = 1e-5 * PI * std::pow(plan.machine.filament_diameter_mm[i], 2) / 4.;
        CHECK(object[i] == Approx(plan.object_volume_mm3[i]).margin(object_segments[i] * quantum_volume));
        CHECK(purge[i] == Approx(plan.purge_volume_mm3[i]).margin(purge_segments[i] * quantum_volume));
    }
    const double expected_total = plan.object_volume_mm3[0] + plan.object_volume_mm3[1] + plan.purge_volume_mm3[0] +
                                  plan.purge_volume_mm3[1];
    const double total_quantum = deposits * 1e-5 * PI * 1.75 * 1.75 / 4.;
    CHECK(print.print_statistics().total_extruded_volume == Approx(expected_total).margin(total_quantum));
    const double tower_length = (plan.purge_volume_mm3[0] + plan.purge_volume_mm3[1]) / (PI * 1.75 * 1.75 / 4.);
    CHECK(print.print_statistics().total_wipe_tower_filament == Approx(tower_length).margin(.0001));
    GCodeProcessor from_disk;
    from_disk.process_file((dir.path / "accepted.gcode").string());
    REQUIRE(from_disk.result().material_band_cycles.size() == 10);
    CHECK(from_disk.result().material_band_moves.size() == parsed.material_band_moves.size());
    for (size_t i = 0; i < parsed.material_band_cycles.size(); ++i) {
        CHECK(parsed.material_band_cycles[i].id == plan.cycles[i].id);
        CHECK(parsed.material_band_cycles[i].top_z == Approx(plan.cycles[i].top_z_mm).margin(1e-6));
        CHECK_FALSE(parsed.material_band_cycles[i].ranges.empty());
    }
}

TEST_CASE("Material band production rejects opaque motion and rebuilds after engine toggles", "[material_band][production]")
{
    auto fixture = MaterialBandFixtures::make_fixture("MB01");
    MaterialBandFixtures::enable_material_bands(fixture);
    Print print;
    slice(print, fixture);
    accepted(print);
    const auto first = print.material_band_result();
    slice(print, fixture);
    CHECK(print.material_band_result() == first);
    fixture.config.set("machine_start_gcode", std::string("G28\nG1 X17 E2\n"));
    slice(print, fixture);
    const auto fallback = print.material_band_result();
    REQUIRE(fallback);
    REQUIRE(fallback->fallback);
    CHECK_FALSE(fallback->used_material_bands());
    CHECK(fallback->fallback->detail == "opaque_start_or_end_gcode");
    REQUIRE_FALSE(print.objects().front()->layers().empty());
    TemporaryDirectory   dir;
    GCodeProcessorResult parsed;
    print.export_gcode((dir.path / "fallback.gcode").string(), &parsed);
    CHECK(parsed.material_band_moves.empty());
    fixture.config.set("machine_start_gcode", std::string("G90\nM83\n"));
    slice(print, fixture);
    CHECK(accepted(print).object_volume_mm3 == first->plan->object_volume_mm3);
    CHECK(print.material_band_result()->revision.generation > first->revision.generation);
    fixture.config.set_deserialize_strict("local_z_engine", "legacy");
    slice(print, fixture);
    CHECK_FALSE(print.material_band_result());
    REQUIRE_FALSE(print.objects().front()->layers().empty());
}

TEST_CASE("Material band fallback retains the legacy absolute E tower restriction", "[material_band][production]")
{
    auto fixture = MaterialBandFixtures::make_fixture("MB01");
    MaterialBandFixtures::enable_material_bands(fixture);
    fixture.config.set("use_relative_e_distances", false);
    fixture.config.set("machine_start_gcode", std::string("G28\nG1 X17 E2\n"));
    Print print;
    print.apply(fixture.model, fixture.config);
    CHECK_THROWS_WITH(tbb::task_arena(2).execute([&] { print.process(); }),
                      "Material bands fell back (opaque_start_or_end_gcode). The legacy wipe tower requires relative extruder addressing.");
    REQUIRE(print.material_band_result());
    CHECK_FALSE(print.material_band_result()->plan);
    CHECK(print.objects().front()->layers().empty());
    StringObjectException warning;
    CHECK_FALSE(print.validate(&warning).string.empty());
}

TEST_CASE("Material band engine and meaningful controls survive production 3MF persistence", "[material_band][production]")
{
    auto fixture = MaterialBandFixtures::make_fixture("MB02-transition");
    MaterialBandFixtures::enable_material_bands(fixture);
    fixture.config.set_deserialize_strict("material_band_boundary_policy", "allow_transition");
    fixture.config.set_deserialize_strict("material_band_transition_width", "2");
    fixture.config.set_deserialize_strict("material_band_transition_error", "25%");
    TemporaryDirectory dir;
    fixture.model.set_backup_path(dir.path.string());
    const auto  file = (dir.path / "settings.3mf").string();
    StoreParams store;
    store.path     = file.c_str();
    store.model    = &fixture.model;
    store.config   = &fixture.config;
    store.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipStatic | SaveStrategy::SkipAuxiliary;
    REQUIRE(store_bbs_3mf(store));
    Model loaded;
    loaded.set_backup_path((dir.path / "loaded").string());
    boost::filesystem::create_directories(dir.path / "loaded/Metadata/fullspectrum");
    boost::filesystem::create_directories(dir.path / "loaded/3D/Objects");
    DynamicPrintConfig config;
    config.apply(PresetBundle{}.project_config);
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    PlateDataPtrs             plates;
    std::vector<Preset*>      presets;
    bool                      is_bbs = false;
    Semver                    version;
    const bool loaded_ok = load_bbs_3mf(file.c_str(), &config, &substitutions, &loaded, &plates, &presets, &is_bbs, &version, nullptr,
                                        LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence);
    release_PlateData_list(plates);
    for (auto* preset : presets)
        delete preset;
    REQUIRE(loaded_ok);
    for (const auto& key : fixture.config.keys())
        if (is_engine_option(key)) {
            REQUIRE(config.has(key));
            CHECK(config.opt_serialize(key) == fixture.config.opt_serialize(key));
        }
}

TEST_CASE("Material band production preserves pure interiors beneath painted wall recipes", "[material_band][production]")
{
    const auto id = GENERATE("MB02-transition", "MB03");
    CAPTURE(id);
    auto fixture = MaterialBandFixtures::make_fixture(id);
    MaterialBandFixtures::enable_material_bands(fixture);
    fixture.config.set_deserialize_strict("material_band_boundary_policy", "allow_transition");
    fixture.config.set_deserialize_strict("material_band_transition_width", "2");
    fixture.config.set_deserialize_strict("material_band_transition_error", "25%");
    Print print;
    slice(print, fixture);
    const auto&  plan  = accepted(print);
    const double core  = (12. - 1.8) * (8. - 1.8) * 2.4;
    const double shell = 12. * 8. * 2.4 - core;
    CHECK(plan.object_volume_mm3[0] == Approx(core + shell * .5).margin(.001));
    CHECK(plan.object_volume_mm3[1] == Approx(shell * .5).margin(.001));
    size_t continuous_crossings = 0;
    for (const auto& object : plan.schedule->objects) {
        if (object.object == std::numeric_limits<uint64_t>::max())
            continue;
        for (const auto& path : object.paths->paths) {
            if (path.band.domain >> 63) {
                CHECK(path.band.material.value == 1);
                CHECK(path.shared_interface == 0);
            } else if (path.role == PathRole::Perimeter) {
                CHECK(path.vertices.front() == path.vertices.back());
                double min_z = path.vertices.front().z(), max_z = min_z;
                for (const auto& point : path.vertices) {
                    min_z = std::min(min_z, point.z());
                    max_z = std::max(max_z, point.z());
                }
                if (max_z - min_z > .1)
                    ++continuous_crossings;
            }
        }
    }
    CHECK(continuous_crossings >= 10);
    save_demo(fixture, print, id);
    fixture.config.set_deserialize_strict("material_band_boundary_policy", "preserve_painted_edge");
    print.apply(fixture.model, fixture.config);
    const auto rejected = plan_production_print(print, {1, 2, 3, 4, 5, 6, 7});
    REQUIRE(rejected->fallback);
    CHECK(rejected->fallback->detail == "discontinuous_interface");
}

TEST_CASE("Material band dense infill honors pattern and direction without adding core walls", "[material_band][production][infill]")
{
    const auto   pattern = GENERATE("rectilinear", "monotonic", "concentric");
    const auto   id      = GENERATE("MB01", "MB02-transition");
    const double angle   = GENERATE(0., 45., 17.);
    CAPTURE(pattern, id, angle);
    auto fixture = MaterialBandFixtures::make_fixture(id);
    MaterialBandFixtures::enable_material_bands(fixture);
    fixture.config.set_deserialize_strict("material_band_boundary_policy", "allow_transition");
    fixture.config.set_deserialize_strict("material_band_transition_width", "2");
    fixture.config.set_deserialize_strict("material_band_transition_error", "25%");
    for (const char* key : {"internal_solid_infill_pattern", "top_surface_pattern", "bottom_surface_pattern"})
        fixture.config.set_deserialize_strict(key, pattern);
    fixture.config.set("solid_infill_direction", angle);
    Print print;
    slice(print, fixture);
    const auto&           plan        = accepted(print);
    size_t                closed_fill = 0, core_walls = 0, fills = 0;
    std::array<double, 2> dominant_length{};
    const bool            concentric = std::string(pattern) == "concentric";
    for (const auto& object : plan.schedule->objects) {
        if (object.object == std::numeric_limits<uint64_t>::max())
            continue;
        for (const auto& path : object.paths->paths) {
            if (path.band.domain >> 63 && path.role == PathRole::Perimeter)
                ++core_walls;
            if (path.role != PathRole::DenseFill)
                continue;
            ++fills;
            closed_fill += (path.vertices.front() - path.vertices.back()).norm() < 1e-8;
            if (concentric)
                continue;
            const double radians = (angle + (path.band.macro_cycle % 2 ? 90. : 0.)) * PI / 180.;
            const Vec2d  direction(std::cos(radians), std::sin(radians));
            for (size_t i = 1; i < path.vertices.size(); ++i) {
                const Vec2d delta = (path.vertices[i] - path.vertices[i - 1]).head<2>();
                CHECK(std::abs(direction.x() * delta.y() - direction.y() * delta.x()) < .00001);
                dominant_length[path.band.macro_cycle % 2] += delta.norm();
            }
        }
    }
    CHECK(core_walls == 0);
    CHECK(fills > 0);
    if (concentric)
        CHECK(closed_fill >= 10);
    else {
        CHECK(dominant_length[0] > 0.);
        CHECK(dominant_length[1] > 0.);
    }
    const double expected = std::string(id) == "MB01" ? 12. * 12. * 2.4 : 12. * 8. * 2.4;
    CHECK(plan.object_volume_mm3[0] + plan.object_volume_mm3[1] == Approx(expected).margin(.01));
    if (angle == 45.)
        save_demo(fixture, print, std::string(id) + "-" + pattern);
}

TEST_CASE("Material band does not silently replace an unsupported dense infill pattern", "[material_band][production][infill]")
{
    auto fixture = MaterialBandFixtures::make_fixture("MB01");
    MaterialBandFixtures::enable_material_bands(fixture);
    fixture.config.set_deserialize_strict("internal_solid_infill_pattern", "hilbertcurve");
    Print print;
    print.apply(fixture.model, fixture.config);
    const auto result = plan_production_print(print, {1, 2, 3, 4, 5, 6, 7});
    REQUIRE(result->fallback);
    CHECK(result->fallback->detail == "dense_infill_pattern_requires_adapter");
}

TEST_CASE("Material band infill respects minimum top and bottom shell thickness", "[material_band][production][infill]")
{
    auto fixture = MaterialBandFixtures::make_fixture("MB01");
    MaterialBandFixtures::enable_material_bands(fixture);
    fixture.config.set_deserialize_strict("internal_solid_infill_pattern", "concentric");
    fixture.config.set_deserialize_strict("bottom_surface_pattern", "rectilinear");
    fixture.config.set_deserialize_strict("top_surface_pattern", "monotonic");
    fixture.config.set("bottom_shell_layers", 1);
    fixture.config.set("bottom_shell_thickness", .48);
    fixture.config.set("top_shell_layers", 1);
    fixture.config.set("top_shell_thickness", .24);
    Print print;
    slice(print, fixture);
    const auto&            plan = accepted(print);
    std::array<size_t, 10> closed{};
    for (const auto& object : plan.schedule->objects)
        if (object.object != std::numeric_limits<uint64_t>::max())
            for (const auto& path : object.paths->paths)
                if (path.role == PathRole::DenseFill && (path.vertices.front() - path.vertices.back()).norm() < 1e-8)
                    ++closed.at(path.band.macro_cycle);
    for (size_t cycle = 0; cycle < closed.size(); ++cycle)
        CHECK((closed[cycle] > 0) == (cycle >= 2 && cycle < 9));
}

TEST_CASE("Material band alternating infill cannot conceal unsupported gaps around a hole", "[material_band][production][infill]")
{
    auto fixture = MaterialBandFixtures::make_axial_fixture("hole_plate", 64, .48);
    MaterialBandFixtures::enable_material_bands(fixture);
    Print print;
    print.apply(fixture.model, fixture.config);
    const auto result = plan_production_print(print, {1, 2, 3, 4, 5, 6, 7});
    REQUIRE(result->fallback);
    CHECK_FALSE(result->plan);
    CHECK(result->fallback->detail == "unsupported_bead_footprint");
}

TEST_CASE("Material band copies share geometry and edits revert to a fresh deterministic plan", "[material_band][production][cache]")
{
    const int threads = GENERATE(1, 2, 4);
    auto      fixture = MaterialBandFixtures::make_fixture("MB01");
    MaterialBandFixtures::enable_material_bands(fixture);
    auto* object = fixture.model.objects.front();
    object->volumes.front()->set_scaling_factor(Vec3d(1., 1., .2));
    object->instances.front()->set_rotation(Z, PI / 2.);
    object->instances.front()->set_mirror(Vec3d(-1., 1., 1.));
    auto* copy = object->add_instance(*object->instances.front());
    copy->set_offset(Vec3d(100., 20., 0.));
    Print print;
    slice(print, fixture, threads);
    accepted(print);
    const auto original = print.material_band_result();
    REQUIRE(original->instance_count == 2);
    CHECK(original->geometry_reuses == 1);
    CHECK(original->plan->object_volume_mm3[0] == Approx(69.12).margin(.0001));
    CHECK(original->plan->object_volume_mm3[1] == Approx(69.12).margin(.0001));
    slice(print, fixture, threads);
    CHECK(print.material_band_result() == original);
    copy->set_offset(Vec3d(105., 20., 0.));
    slice(print, fixture, threads);
    CHECK(print.material_band_result() != original);
    CHECK(accepted(print).object_volume_mm3 == original->plan->object_volume_mm3);
    copy->set_offset(Vec3d(100., 20., 0.));
    slice(print, fixture, threads);
    const auto reverted = print.material_band_result();
    Print      fresh;
    slice(fresh, fixture, threads);
    const auto& fresh_plan = accepted(fresh);
    REQUIRE(reverted->plan->operations.size() == fresh_plan.operations.size());
    REQUIRE(reverted->plan->operations.size() == original->plan->operations.size());
    for (size_t i = 0; i < fresh_plan.operations.size(); ++i) {
        const auto& a = reverted->plan->operations[i];
        const auto& b = fresh_plan.operations[i];
        const auto& c = original->plan->operations[i];
        CHECK(a.kind == b.kind);
        CHECK(a.material.value == b.material.value);
        CHECK(a.position == b.position);
        CHECK(a.position == c.position);
        CHECK(a.volume_mm3 == b.volume_mm3);
    }
    const auto stale = plan_production_print(fresh, original->revision, {}, [] { return true; });
    REQUIRE(stale->fallback);
    CHECK(stale->fallback->code == FailureCode::StaleInput);
    size_t     polls     = 0;
    const auto cancelled = plan_production_print(fresh, original->revision, [&] { return ++polls > 10; });
    REQUIRE(cancelled->fallback);
    CHECK(cancelled->fallback->code == FailureCode::Cancelled);
    CHECK_FALSE(cancelled->plan);
    for (auto* instance : object->instances)
        instance->set_offset(instance->get_offset() + Vec3d(300., 0., 0.));
    Print another_plate;
    another_plate.set_plate_origin(Vec3d(300., 0., 0.));
    slice(another_plate, fixture, threads);
    const auto& local_plan = accepted(another_plate);
    REQUIRE(local_plan.operations.size() == fresh_plan.operations.size());
    for (size_t i = 0; i < local_plan.operations.size(); ++i)
        CHECK((local_plan.operations[i].position - fresh_plan.operations[i].position).norm() < 1e-9);
}

TEST_CASE("Material band process support declarations reject unsupported combinations", "[material_band][production][compatibility]")
{
    const auto scenario = GENERATE("Arachne", "Sparse", "Support", "Sequential", "Envelope", "Opaque filament", "Manual tool");
    CAPTURE(scenario);
    auto fixture = MaterialBandFixtures::make_fixture("MB01");
    MaterialBandFixtures::enable_material_bands(fixture);
    std::string expected;
    if (std::string(scenario) == "Arachne") {
        fixture.config.set_deserialize_strict("wall_generator", "arachne");
        expected = "dense_classic_profile_required";
    }
    if (std::string(scenario) == "Sparse") {
        fixture.config.set_deserialize_strict("sparse_infill_density", "15%");
        expected = "dense_classic_profile_required";
    }
    if (std::string(scenario) == "Support") {
        fixture.config.set("enable_support", true);
        expected = "support_or_variable_process_requires_adapter";
    }
    if (std::string(scenario) == "Sequential") {
        fixture.config.set_deserialize_strict("print_sequence", "by object");
        expected = "unsupported_auxiliary_or_sequential_profile";
    }
    if (std::string(scenario) == "Envelope") {
        fixture.config.option<ConfigOptionString>("material_band_tool_sections")->value.clear();
        expected = "complete_tool_envelope_required";
    }
    if (std::string(scenario) == "Opaque filament") {
        fixture.config.option<ConfigOptionStrings>("filament_start_gcode")->values = {"G1 E10", ""};
        expected                                                                   = "opaque_custom_gcode:filament_start_gcode";
    }
    if (std::string(scenario) == "Manual tool") {
        fixture.config.set("manual_filament_change", true);
        expected = "manual_tool_selection_requires_adapter";
    }
    Print print;
    print.apply(fixture.model, fixture.config);
    const auto result = plan_production_print(print, {1, 2, 3, 4, 5, 6, 7});
    REQUIRE(result->fallback);
    CHECK_FALSE(result->plan);
    CHECK(result->fallback->detail == expected);
}

TEST_CASE("Material band tool envelope text rejects incomplete or malformed geometry", "[material_band][production][compatibility]")
{
    const auto text = GENERATE("", " ", "0,1", "0,1,2,", "0,1,2, ", "0,,2", "0,1,2x", "0,1,nan", "0,1,inf", "0,1,-2", "0,1,1e999");
    CAPTURE(text);
    auto fixture = MaterialBandFixtures::make_fixture("MB01");
    MaterialBandFixtures::enable_material_bands(fixture);
    const auto* definition = print_config_def.get("material_band_tool_sections");
    REQUIRE(definition);
    CHECK(definition->type == coString);
    fixture.config.set_deserialize_strict("material_band_tool_sections", text);
    Print print;
    print.apply(fixture.model, fixture.config);
    const auto result = plan_production_print(print, {1, 2, 3, 4, 5, 6, 7});
    REQUIRE(result->fallback);
    CHECK_FALSE(result->plan);
    CHECK(result->fallback->detail == "complete_tool_envelope_required");
}

TEST_CASE("Material band preview preserves disjoint logical cycles and rejects partial tags", "[material_band][unit][preview]")
{
    const bool  partial = GENERATE(false, true);
    PrintConfig config;
    config.gcode_flavor.value = gcfRepRapFirmware;
    GCodeProcessor processor;
    processor.reset();
    processor.apply_config(config);
    processor.initialize("material-band-preview.gcode");
    processor.process_buffer("G21\nG90\nM83\nT0\nG10 P0 S200\nG1 X10 Y10 Z0.12 F600\n;TYPE:Internal solid infill\n");
    for (size_t i = 0; i < 3; ++i) {
        const size_t       cycle = i == 1 ? 1 : 0;
        std::ostringstream code;
        code << ";MB_CYCLE " << cycle << " TOP " << (cycle ? .48 : .24) << " OBJECT 7 DOMAIN 9 MATERIAL 1 SLOT 0 INTERFACE 2 OPERATION "
             << (i + 1) << '\n';
        if (!partial || i != 2)
            code << ";MB_BEAD WIDTH 0.45 HEIGHT_START 0.12 HEIGHT_END 0.13\n";
        code << ";MB_OPERATION DEPOSITION\nG1 X" << 11 + i << " Z" << .12 + .01 * i << " E0.02 F600\n";
        processor.process_buffer(code.str());
    }
    processor.finalize(false);
    const auto& parsed = processor.result();
    if (partial) {
        CHECK(parsed.material_band_cycles.empty());
        CHECK(parsed.material_band_moves.empty());
    } else {
        REQUIRE(parsed.material_band_cycles.size() == 2);
        CHECK(parsed.material_band_cycles[0].ranges.size() == 2);
        CHECK(parsed.material_band_cycles[1].ranges.size() == 1);
        size_t count = 0;
        for (const auto& move : parsed.moves)
            if (move.type == EMoveType::Extrude) {
                CHECK(move.delta_extruder == Approx(.02).margin(1e-6));
                CHECK(move.height == Approx(.125));
                CHECK(move.width == Approx(.45));
                ++count;
            }
        CHECK(count == 3);
    }
}

TEST_CASE("Material band production clips actual curved solids before scheduling", "[material_band][production][curved]")
{
    const auto shape = GENERATE("cylinder", "annulus", "frustum", "hole_plate");
    CAPTURE(shape);
    auto fixture = MaterialBandFixtures::make_axial_fixture(shape, 64, .48);
    MaterialBandFixtures::enable_material_bands(fixture);
    if (std::string(shape) == "frustum") {
        fixture.config.set("layer_height", .12);
        fixture.config.set("initial_layer_print_height", .12);
    }
    if (std::string(shape) == "hole_plate") {
        for (const char* key : {"internal_solid_infill_pattern", "top_surface_pattern", "bottom_surface_pattern"})
            fixture.config.set_deserialize_strict(key, "concentric");
        fixture.config.set("solid_infill_direction", 0.);
    }
    Print print;
    slice(print, fixture);
    const auto&  plan   = accepted(print);
    const double factor = 64. * std::sin(2. * PI / 64.) / 2.;
    const double radius = std::string(shape) == "cylinder" ? 5. : 6.;
    const double slope  = std::string(shape) == "frustum" ? -2. / 4.8 : 0.;
    const double inner  = std::string(shape) == "annulus" ? 5. : std::string(shape) == "hole_plate" ? 2. : 0.;
    const double height = .48;
    const double volume = std::string(shape) == "hole_plate" ?
                              (144. - factor * inner * inner) * height :
                              factor * ((radius * radius - inner * inner) * height + radius * slope * height * height +
                                        slope * slope * height * height * height / 3.);
    for (size_t m = 0; m < MAX_MATERIALS; ++m)
        CHECK(plan.object_volume_mm3[m] == Approx(m < 2 ? volume / 2. : 0.).epsilon(.01));
    for (const auto& op : plan.operations)
        if (op.kind == OperationKind::Deposit && op.deposition == DepositionKind::Object) {
            const Vec2d local = op.position.head<2>() - Vec2d(20., 20.);
            if (inner > 0.)
                CHECK(local.norm() >= inner * std::cos(PI / 32.) - .001);
        }
}

TEST_CASE("Material band pure taper is manufactured without an absent-material residue", "[material_band][production][curved]")
{
    auto fixture = MaterialBandFixtures::make_axial_fixture("frustum", 64, .12);
    MaterialBandFixtures::enable_material_bands(fixture);
    fixture.model.objects.front()->config.set("extruder", 1);
    fixture.config.set("layer_height", .12);
    fixture.config.set("initial_layer_print_height", .12);
    Print print;
    slice(print, fixture);
    const auto&  plan   = accepted(print);
    const double factor = 64. * std::sin(2. * PI / 64.) / 2.;
    const double h = .12, slope = -2. / 4.8;
    const double expected = factor * (36. * h + 6. * slope * h * h + slope * slope * h * h * h / 3.);
    CHECK(plan.object_volume_mm3[0] == Approx(expected).epsilon(.01));
    CHECK(plan.object_volume_mm3[1] == 0.);
    for (const auto& op : plan.operations)
        if (op.kind == OperationKind::Deposit && op.deposition == DepositionKind::Object) {
            CHECK(op.material.value == 1);
            CHECK(op.interface_id == 0);
        }
}

TEST_CASE("Material band independently painted projects preserve split leaves and virtual identities",
          "[material_band][integration][painted_3mf]")
{
    const auto                    name = GENERATE("triangle-boundary", "multiple-virtual-mixes");
    TemporaryDirectory            dir;
    MaterialBandFixtures::Fixture fixture;
    fixture.model.set_backup_path(dir.path.string());
    boost::filesystem::create_directories(dir.path / "Metadata/fullspectrum");
    boost::filesystem::create_directories(dir.path / "3D/Objects");
    fixture.config.apply(PresetBundle{}.project_config);
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    PlateDataPtrs             plates;
    std::vector<Preset*>      presets;
    bool                      is_bbs = false;
    Semver                    version;
    const std::string         filename = std::string(TEST_DATA_DIR) + "/material_band/painted/" + name + ".3mf";
    const bool ok = load_bbs_3mf(filename.c_str(), &fixture.config, &substitutions, &fixture.model, &plates, &presets, &is_bbs, &version,
                                 nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence);
    release_PlateData_list(plates);
    for (auto* preset : presets)
        delete preset;
    REQUIRE(ok);
    REQUIRE(fixture.model.objects.size() == 1);
    InputContext context;
    context.revision    = {1, 2, 3, 4, 5, 6, 7};
    context.phase.order = {{{1}, {2}}};
    const auto result   = snapshot_input(*fixture.model.objects.front(), fixture.config, context);
    REQUIRE(std::holds_alternative<std::shared_ptr<const KernelInput>>(result));
    const auto&      snapshot   = *std::get<std::shared_ptr<const KernelInput>>(result);
    size_t           leaf_count = 0;
    std::set<double> fractions;
    for (const auto& region : snapshot.regions) {
        leaf_count += region.painted_surface.triangles.size();
        fractions.insert(region.fraction_a);
    }
    CHECK(leaf_count > snapshot.solid.triangles.size());
    CHECK(fractions.count(.25) == 1);
    CHECK(fractions.count(.75) == 1);
    CHECK(fractions.count(1.) == 1);
    MaterialBandFixtures::enable_material_bands(fixture);
    Print print;
    print.apply(fixture.model, fixture.config);
    const auto planned = plan_production_print(print, context.revision);
    REQUIRE(planned->fallback);
    CHECK_FALSE(planned->plan);
    CHECK(planned->fallback->detail == "top_or_bottom_paint_requires_domain_adapter");
}

TEST_CASE("Material band reports concurrent brim and sparse infill restrictions", "[material_band][production][compatibility]")
{
    auto fixture = MaterialBandFixtures::make_fixture("MB01");
    MaterialBandFixtures::enable_material_bands(fixture);
    fixture.config.set("brim_width", 5.);
    fixture.config.set_deserialize_strict("sparse_infill_density", "15%");
    fixture.config.set_deserialize_strict("sparse_infill_pattern", "gyroid");
    Print print;
    print.apply(fixture.model, fixture.config);
    const auto result = plan_production_print(print, {1, 2, 3, 4, 5, 6, 7});
    REQUIRE(result->fallback);
    CHECK(result->unsupported_settings.size() == 2);
    CHECK(result->fallback_message().find("brim_width=5 mm") != std::string::npos);
    CHECK(result->fallback_message().find("15%; gyroid") != std::string::npos);
}
