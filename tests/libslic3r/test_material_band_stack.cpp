#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "material_band_fixtures.hpp"
#include "libslic3r/MaterialBand/StackPlanner.hpp"
#include "libslic3r/MaterialBand/InputAdapter.hpp"
#include "libslic3r/MaterialBand/Production.hpp"
#include "libslic3r/MaterialBand/SnapmakerU1.hpp"
#include "libslic3r/MaterialBand/PaintedDomains.hpp"
#include "libslic3r/MaterialBand/LayerProcess.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

#include <boost/filesystem.hpp>
#include <tbb/task_arena.h>
#include <fstream>
#include <chrono>
#include <limits>
#include <numeric>
#include <set>

using namespace Slic3r;
using namespace Slic3r::MaterialBand;

namespace {
MaterialBandFixtures::Fixture stack_fixture(
    size_t count, bool gradient, const std::string& shape = "box", bool explicit_widths = true, unsigned resolution = 64)
{
    auto fixture = shape == "box" ? MaterialBandFixtures::make_fixture("MB01") :
                                    MaterialBandFixtures::make_axial_fixture(shape, resolution, gradient ? 2.4 : .48);
    MaterialBandFixtures::enable_material_bands(fixture);
    auto& config = fixture.config;
    config.set_num_filaments(count);
    config.set_num_extruders(count);
    std::vector<std::string> colors{"#FF0000", "#0000FF", "#00FF00", "#FFFFFF"};
    colors.resize(count);
    config.option<ConfigOptionStrings>("filament_colour")->values = colors;
    config.option<ConfigOptionFloats>("filament_diameter")->values.assign(count, 1.75);
    config.option<ConfigOptionStrings>("filament_start_gcode")->values.assign(count, "");
    config.option<ConfigOptionStrings>("filament_end_gcode")->values.assign(count, "");
    config.option<ConfigOptionFloats>("filament_minimal_purge_on_wipe_tower")->values.assign(count, 0.);
    auto& matrix = config.option<ConfigOptionFloats>("flush_volumes_matrix")->values;
    matrix.resize(count * count);
    for (size_t a = 0; a < count; ++a)
        for (size_t b = 0; b < count; ++b)
            matrix[a * count + b] = a == b ? 0. : 1. + .1 * (a * count + b);
    config.set_key_value("material_band_gradient_error", new ConfigOptionPercent(10.));
    config.set("solid_infill_direction", 0.);
    config.set("solid_infill_rotate_template", std::string());
    if (shape == "hole_plate") {
        config.set_deserialize_strict("internal_solid_infill_pattern", "concentric");
        config.set_deserialize_strict("top_surface_pattern", "concentric");
        config.set_deserialize_strict("bottom_surface_pattern", "concentric");
    }
    MixedFilamentDefinition definition;
    definition.identity.stable_id = 31000 + count;
    definition.source.kind        = MixedFilamentSourceKind::Custom;
    for (unsigned int i = 0; i < count; ++i)
        definition.recipe.blend.components.push_back({{i + 1}, int(i + 1 == count ? 100 - (100 / count) * (count - 1) : 100 / count)});
    definition.behavior.distribution               = MixedFilamentDistributionMode::LayerCycle;
    definition.behavior.layer_cadence              = {1, 1};
    definition.behavior.gradient.enabled           = gradient;
    definition.behavior.gradient.component_a_start = 100;
    definition.behavior.gradient.component_a_end   = 0;
    // Explicit uniformly spaced stops are independent of the legacy sampler.
    for (size_t i = 0; i < count * 2 - 1; ++i)
        definition.behavior.gradient.stop_positions.push_back(float(i) / float(count * 2 - 2));
    if (explicit_widths)
        definition.behavior.gradient.solid_widths.assign(count, 0.f);
    MixedFilamentManager manager;
    manager.set_mixed_filament_definitions({definition}, colors);
    config.set("mixed_filament_definitions", manager.serialize_custom_entries());
    auto* object = fixture.model.objects.front();
    object->config.set("extruder", int(count + 1));
    object->volumes.front()->config.set("extruder", int(count + 1));
    object->volumes.front()->mmu_segmentation_facets.reset();
    return fixture;
}

InputContext input_context()
{
    InputContext context;
    context.phase.order = {{{1}, {2}}};
    return context;
}

const FinalPrintPlan& accepted(const Print& print)
{
    REQUIRE(print.material_band_result());
    if (print.material_band_result()->fallback) {
        const auto&        f = *print.material_band_result()->fallback;
        std::ostringstream detail;
        if (f.band)
            detail << " domain=" << f.band->domain << " cycle=" << f.band->macro_cycle << " material=" << f.band->material.value;
        if (f.witness_mm)
            detail << " witness=" << f.witness_mm->transpose();
        FAIL(failure_name(f.code) << ": " << f.detail << detail.str());
    }
    REQUIRE(print.material_band_result()->plan);
    return *print.material_band_result()->plan;
}

struct TemporaryDirectory
{
    boost::filesystem::path path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("material-stack-%%%%-%%%%");
    TemporaryDirectory() { boost::filesystem::create_directories(path); }
    ~TemporaryDirectory()
    {
        boost::system::error_code ignored;
        boost::filesystem::remove_all(path, ignored);
    }
};
} // namespace

TEST_CASE("Material stack solver closes three and four material volumes", "[material_band][stack]")
{
    const size_t                count = GENERATE(3u, 4u);
    std::vector<StackDoseCycle> cycles(7);
    for (size_t c = 0; c < cycles.size(); ++c) {
        cycles[c].height_mm  = .24;
        cycles[c].volume_mm3 = 10. + c;
        for (size_t m = 0; m < count; ++m)
            cycles[c].target[m] = 1. / count;
    }
    const auto result = solve_stack_doses(cycles, count, {}, 0., {}, {});
    REQUIRE(std::holds_alternative<std::shared_ptr<const StackDoseSolution>>(result));
    const auto& solution = *std::get<std::shared_ptr<const StackDoseSolution>>(result);
    for (size_t m = 0; m < MAX_MATERIALS; ++m)
        CHECK(solution.volume_mm3[m] == Approx(m < count ? 91. / count : 0.).margin(1e-10));
    CHECK(solution.maximum_fraction_error <= 1e-12);
    for (const auto& fractions : solution.fractions)
        CHECK(std::accumulate(fractions.begin(), fractions.end(), 0.) == Approx(1.).margin(1e-11));
}

TEST_CASE("Material stack solver conserves gradient dose without epsilon bands", "[material_band][stack]")
{
    // The tail must disappear, with its volume recovered in another cycle.
    std::vector<StackDoseCycle> cycles{{{{.95, .05, 0., 0.}}, .24, 10.}, {{{.5, .5, 0., 0.}}, .24, 20.}, {{{0., 1., 0., 0.}}, .24, 30.}};
    const auto                  exact = solve_stack_doses(cycles, 4, {}, 0., {}, {});
    REQUIRE(std::holds_alternative<Failure>(exact));
    CHECK(std::get<Failure>(exact).code == FailureCode::InfeasibleThickness);
    const auto result = solve_stack_doses(cycles, 4, {}, .1, {}, {});
    REQUIRE(std::holds_alternative<std::shared_ptr<const StackDoseSolution>>(result));
    const auto& solution = *std::get<std::shared_ptr<const StackDoseSolution>>(result);
    CHECK(solution.fractions[0][1] == 0.);
    CHECK(solution.volume_mm3[0] == Approx(19.5).margin(1e-8));
    CHECK(solution.volume_mm3[1] == Approx(40.5).margin(1e-8));
    CHECK(solution.maximum_fraction_error <= .1);
    for (const auto& f : solution.fractions) {
        CHECK(f[2] == 0.);
        CHECK(f[3] == 0.);
        for (double x : f)
            CHECK((x == 0. || x * .24 >= .04 - 1e-9));
    }
}

TEST_CASE("Material stack solver rejects malformed and interrupted work", "[material_band][stack]")
{
    auto              cycles = std::vector<StackDoseCycle>{{{{.25, .25, .25, .25}}, .24, 10.}};
    NumericTolerances tolerance;
    SolverLimits      limits;
    const std::string scenario = GENERATE("sum", "nan", "inactive", "bounds", "tolerance", "cancel", "stale", "memory", "iterations");
    FailureCode       code     = FailureCode::InvalidInput;
    size_t            count    = 4;
    if (scenario == "sum")
        cycles[0].target[0] = .2;
    if (scenario == "nan")
        cycles[0].target[0] = std::numeric_limits<double>::quiet_NaN();
    if (scenario == "inactive")
        count = 3;
    if (scenario == "bounds") {
        cycles[0].height_mm = .1;
        code                = FailureCode::InfeasibleThickness;
    }
    if (scenario == "tolerance")
        tolerance.relative_volume = -1.;
    if (scenario == "cancel")
        code = FailureCode::Cancelled;
    if (scenario == "stale")
        code = FailureCode::StaleInput;
    if (scenario == "memory") {
        limits.max_temporary_bytes = 1;
        code                       = FailureCode::ResourceLimit;
    }
    if (scenario == "iterations") {
        limits.max_projection_sweeps = 0;
        code                         = FailureCode::NonConverged;
    }
    const auto result = solve_stack_doses(
        cycles, count, {}, 0., tolerance, {}, limits, [&] { return scenario == "cancel"; }, [&] { return scenario == "stale"; });
    REQUIRE(std::holds_alternative<Failure>(result));
    CHECK(std::get<Failure>(result).code == code);
}

TEST_CASE("Material stack gradient input is independent of legacy Local Z settings", "[material_band][stack]")
{
    const size_t count           = GENERATE(2u, 3u, 4u);
    const bool   explicit_widths = GENERATE(false, true);
    auto         fixture         = stack_fixture(count, true, "box", explicit_widths);
    auto         first           = snapshot_input(*fixture.model.objects.front(), fixture.config, input_context());
    REQUIRE(std::holds_alternative<std::shared_ptr<const KernelInput>>(first));
    fixture.config.set("dithering_local_z_mode", true);
    fixture.config.set_key_value("dithering_local_z_gradient_middle_filament_window", new ConfigOptionPercent(70.));
    auto second = snapshot_input(*fixture.model.objects.front(), fixture.config, input_context());
    REQUIRE(std::holds_alternative<std::shared_ptr<const KernelInput>>(second));
    const auto& a = std::get<std::shared_ptr<const KernelInput>>(first)->regions.front();
    const auto& b = std::get<std::shared_ptr<const KernelInput>>(second)->regions.front();
    REQUIRE(a.height_gradient.size() == count * 2 - 1 + (explicit_widths ? 0 : count - 2));
    REQUIRE(a.height_gradient.size() == b.height_gradient.size());
    for (size_t i = 0; i < a.height_gradient.size(); ++i) {
        CHECK(a.height_gradient[i].position == b.height_gradient[i].position);
        CHECK(a.height_gradient[i].fractions == b.height_gradient[i].fractions);
    }
    CHECK(sample_height_gradient(a, 0.)[0] == 1.);
    CHECK(sample_height_gradient(a, 1.)[count - 1] == 1.);
    for (size_t m = 0; m < count; ++m)
        CHECK(sample_height_gradient(a, std::round(10000. * double(m) / (count - 1)) / 10000.)[m] == Approx(1.).margin(1e-6));
}

TEST_CASE("Material stacks export all physical tools and shared interfaces", "[material_band][stack][production]")
{
    const size_t count    = GENERATE(3u, 4u);
    const bool   gradient = GENERATE(false, true);
    const bool   relative = GENERATE(false, true);
    CAPTURE(count, gradient, relative);
    auto fixture = stack_fixture(count, gradient);
    fixture.config.set("use_relative_e_distances", relative);
    Print print;
    print.apply(fixture.model, fixture.config);
    tbb::task_arena(2).execute([&] { print.process(); });
    const auto& plan = accepted(print);
    REQUIRE(plan.machine.material_count == count);
    REQUIRE(plan.cycles.size() == 10);
    CHECK(std::accumulate(plan.object_volume_mm3.begin(), plan.object_volume_mm3.end(), 0.) == Approx(345.6).epsilon(1e-5));
    if (!gradient)
        for (size_t m = 0; m < count; ++m)
            CHECK(plan.object_volume_mm3[m] ==
                  Approx(345.6 * (m + 1 == count ? 100 - (100 / count) * (count - 1) : 100 / count) / 100.).epsilon(1e-5));
    if (gradient) {
        MaterialFractions                 targets{};
        std::array<MaterialFractions, 10> actual{};
        for (const auto& operation : plan.operations)
            if (operation.kind == OperationKind::Deposit && operation.deposition == DepositionKind::Object)
                actual.at(operation.logical_cycle)[operation.material.value - 1] += operation.volume_mm3;
        for (size_t c = 0; c < 10; ++c) {
            // Independent analytic linear color sequence over the ten equal
            // volume cycles. The last float mesh plane differs by < 0.1 um.
            const double      u        = (double(c) + .5) / 10. * (count - 1);
            const size_t      k        = size_t(u);
            const auto        stop     = [count](double i) { return std::round(10000. * i / (count - 1)) / 10000.; };
            const double      progress = (double(c) + .5) / 10.;
            const double      ratio    = progress <= stop(k + .5) ? .5 * (progress - stop(k)) / (stop(k + .5) - stop(k)) :
                                                                    .5 + .5 * (progress - stop(k + .5)) / (stop(k + 1) - stop(k + .5));
            MaterialFractions target{};
            target[k]     = 1. - ratio;
            target[k + 1] = ratio;
            for (size_t m = 0; m < count; ++m) {
                targets[m] += target[m] * 34.56;
                CHECK(std::abs(actual[c][m] / 34.56 - target[m]) <= .10001);
                if (target[m] == 0.)
                    CHECK(actual[c][m] == 0.);
            }
        }
        for (size_t m = 0; m < count; ++m)
            CHECK(plan.object_volume_mm3[m] == Approx(targets[m]).margin(.001));
    }
    std::map<std::pair<uint64_t, InterfaceId>, std::set<unsigned int>> owners;
    for (const auto& object : plan.schedule->objects) {
        if (object.paths->interfaces.empty())
            continue; // purge geometry has no material interfaces
        for (const auto& path : object.paths->paths) {
            for (InterfaceId id : {path.lower_interface, path.upper_interface})
                if (id)
                    owners[{object.object, id}].insert(path.band.material.value);
        }
    }
    REQUIRE_FALSE(owners.empty());
    for (const auto& owner : owners)
        CHECK(owner.second.size() == 2);
    if (!gradient)
        CHECK(owners.size() == (count - 1) * 10);
    std::set<unsigned int> primed;
    for (const auto& transition : plan.transitions) {
        const double required = (primed.insert(transition.to.value).second ? 1. : 0.) +
                                (transition.from.value ? 1. + .1 * ((transition.from.value - 1) * count + transition.to.value - 1) : 0.);
        CHECK(transition.required_purge_mm3 == Approx(required).margin(1e-6));
        CHECK(transition.deposited_purge_mm3 >= transition.required_purge_mm3 - 1e-6);
    }
    TemporaryDirectory temporary;
    const char*        output    = std::getenv("MATERIAL_BAND_STACK_DIR");
    const auto         directory = output && !relative ? boost::filesystem::path(output) : temporary.path;
    boost::filesystem::create_directories(directory);
    const std::string name = "MB-" + std::to_string(count) + (gradient ? "-gradient" : "-blend");
    if (!relative) {
        fixture.model.set_backup_path(directory.string());
        const auto  path = (directory / (name + ".3mf")).string();
        StoreParams store;
        store.path     = path.c_str();
        store.model    = &fixture.model;
        store.config   = &fixture.config;
        store.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipStatic | SaveStrategy::SkipAuxiliary;
        REQUIRE(store_bbs_3mf(store));
        Model loaded;
        loaded.set_backup_path((temporary.path / "loaded").string());
        boost::filesystem::create_directories(temporary.path / "loaded/Metadata/fullspectrum");
        boost::filesystem::create_directories(temporary.path / "loaded/3D/Objects");
        DynamicPrintConfig config;
        config.apply(PresetBundle{}.project_config);
        ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
        PlateDataPtrs             plates;
        std::vector<Preset*>      presets;
        bool                      is_bbs = false;
        Semver                    version;
        const bool loaded_ok = load_bbs_3mf(path.c_str(), &config, &substitutions, &loaded, &plates, &presets, &is_bbs, &version, nullptr,
                                            LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence);
        release_PlateData_list(plates);
        for (auto* preset : presets)
            delete preset;
        REQUIRE(loaded_ok);
        CHECK(config.opt_serialize("material_band_gradient_error") == fixture.config.opt_serialize("material_band_gradient_error"));
        auto restored = snapshot_input(*loaded.objects.front(), config, input_context());
        REQUIRE(std::holds_alternative<std::shared_ptr<const KernelInput>>(restored));
        CHECK(std::get<std::shared_ptr<const KernelInput>>(restored)->material_count == count);
        CHECK(std::get<std::shared_ptr<const KernelInput>>(restored)->regions.front().height_gradient.empty() == !gradient);
        auto original = snapshot_input(*fixture.model.objects.front(), fixture.config, input_context());
        REQUIRE(std::holds_alternative<std::shared_ptr<const KernelInput>>(original));
        const auto& a = std::get<std::shared_ptr<const KernelInput>>(original)->regions.front();
        const auto& b = std::get<std::shared_ptr<const KernelInput>>(restored)->regions.front();
        CHECK(a.fractions == b.fractions);
        REQUIRE(a.height_gradient.size() == b.height_gradient.size());
        for (size_t i = 0; i < a.height_gradient.size(); ++i) {
            CHECK(a.height_gradient[i].position == b.height_gradient[i].position);
            CHECK(a.height_gradient[i].fractions == b.height_gradient[i].fractions);
        }
    }
    GCodeProcessorResult parsed;
    print.export_gcode((directory / (name + ".gcode")).string(), &parsed);
    REQUIRE(parsed.material_band_cycles.size() == plan.cycles.size());
    std::array<double, MAX_MATERIALS> deposited{};
    std::array<size_t, MAX_MATERIALS> segments{};
    std::set<unsigned int>            tools;
    for (const auto& record : parsed.material_band_moves) {
        const auto& move = parsed.moves.at(record.move_id);
        if (move.type != EMoveType::Extrude || record.deposition != 1)
            continue;
        REQUIRE(record.material >= 1);
        REQUIRE(record.material <= count);
        CHECK(record.material == move.extruder_id + 1);
        CHECK(record.height_start >= .04 - 1e-6);
        CHECK(record.height_end >= .04 - 1e-6);
        const size_t m = record.material - 1;
        tools.insert(record.material);
        deposited[m] += move.delta_extruder * PI * std::pow(plan.machine.filament_diameter_mm[m], 2) / 4.;
        ++segments[m];
    }
    CHECK(tools.size() == count);
    for (size_t m = 0; m < count; ++m)
        CHECK(deposited[m] == Approx(plan.object_volume_mm3[m]).margin(segments[m] * 1e-5 * PI * 1.75 * 1.75 / 4.));
}

TEST_CASE("Material stacks clip curved and holed source solids", "[material_band][stack][production]")
{
    const std::string shape = GENERATE("cylinder", "frustum", "annulus", "hole_plate");
    const size_t      count = GENERATE(3u, 4u);
    CAPTURE(shape, count);
    auto  fixture = stack_fixture(count, false, shape);
    Print print;
    print.apply(fixture.model, fixture.config);
    tbb::task_arena(2).execute([&] { print.process(); });
    if (shape == "frustum") {
        // The current prismatic bead columns leave taper stair-step volume.
        // The multi-material dose contract must reject it, not relax its budget.
        REQUIRE(print.material_band_result());
        REQUIRE(print.material_band_result()->fallback);
        CHECK_FALSE(print.material_band_result()->plan);
        CHECK(print.material_band_result()->fallback->detail == "stack_planned_material_budget");
        return;
    }
    const auto&  plan   = accepted(print);
    const double factor = 32. * std::sin(2. * PI / 64.);
    const double volume = .48 * (shape == "cylinder"   ? 25. * factor :
                                 shape == "annulus"    ? 11. * factor :
                                 shape == "hole_plate" ? 144. - 4. * factor :
                                                         (36. + 6. * 5.8 + 5.8 * 5.8) * factor / 3.);
    for (size_t m = 0; m < count; ++m)
        CHECK(plan.object_volume_mm3[m] ==
              Approx(volume * (m + 1 == count ? 100 - (100 / count) * (count - 1) : 100 / count) / 100.).epsilon(1e-4));
}

TEST_CASE("Material stack gradients rebuild deterministically and retain explicit failures", "[material_band][stack][production]")
{
    const size_t count   = GENERATE(2u, 3u, 4u);
    auto         fixture = stack_fixture(count, true);
    Print        print;
    print.apply(fixture.model, fixture.config);
    tbb::task_arena(1).execute([&] { print.process(); });
    accepted(print);
    const auto original = print.material_band_result();
    print.apply(fixture.model, fixture.config);
    tbb::task_arena(4).execute([&] { print.process(); });
    CHECK(print.material_band_result() == original);
    fixture.config.set_key_value("material_band_gradient_error", new ConfigOptionPercent(0.));
    print.apply(fixture.model, fixture.config);
    tbb::task_arena(2).execute([&] { print.process(); });
    REQUIRE(print.material_band_result()->fallback);
    CHECK_FALSE(print.material_band_result()->plan);
    CHECK(print.material_band_result()->fallback->detail == "gradient_cycle_requires_more_dose_allowance_or_height");
    fixture.config.set_key_value("material_band_gradient_error", new ConfigOptionPercent(10.));
    fixture.config.set("dithering_local_z_mode", true);
    fixture.config.set_key_value("dithering_local_z_gradient_middle_filament_window", new ConfigOptionPercent(70.));
    print.apply(fixture.model, fixture.config);
    tbb::task_arena(4).execute([&] { print.process(); });
    const auto& rebuilt = accepted(print);
    REQUIRE(rebuilt.operations.size() == original->plan->operations.size());
    for (size_t i = 0; i < rebuilt.operations.size(); ++i) {
        const auto& a = rebuilt.operations[i];
        const auto& b = original->plan->operations[i];
        CHECK(a.kind == b.kind);
        CHECK(a.position == b.position);
        CHECK(a.volume_mm3 == b.volume_mm3);
        CHECK(a.material.value == b.material.value);
    }
}

TEST_CASE("Material stacks retain failures for unresolved thin fill and cap support", "[material_band][stack][production]")
{
    const std::string shape   = GENERATE("annulus", "hole_plate");
    const size_t      count   = GENERATE(3u, 4u);
    auto              fixture = stack_fixture(count, false, shape, true, 32);
    for (const char* key : {"internal_solid_infill_pattern", "top_surface_pattern", "bottom_surface_pattern"})
        fixture.config.set_deserialize_strict(key, "concentric");
    Print print;
    print.apply(fixture.model, fixture.config);
    const auto result = plan_production_print(print, {});
    REQUIRE(result->fallback);
    CHECK_FALSE(result->plan);
    CHECK(result->fallback->detail == (shape == "annulus" ? "clipped_path_coverage_budget" : "unsupported_bead_footprint"));
}

TEST_CASE("Material stack benchmark capture", "[material_band][stack_benchmark][.benchmark]")
{
    const char* input  = std::getenv("MATERIAL_BAND_BENCHMARK_INPUT");
    const char* output = std::getenv("MATERIAL_BAND_BENCHMARK_OUTPUT");
    REQUIRE(input);
    REQUIRE(output);
    std::ifstream stream(input);
    const auto    spec    = nlohmann::json::parse(stream);
    auto          fixture = stack_fixture(spec.at("material_count").get<size_t>(), spec.at("gradient").get<bool>());
    Print         print;
    print.apply(fixture.model, fixture.config);
    tbb::task_arena(2).execute([&] { print.process(); });
    const auto& plan       = accepted(print);
    const auto  result     = print.material_band_result();
    using Clock            = std::chrono::steady_clock;
    const auto cache_start = Clock::now();
    tbb::task_arena(2).execute([&] { print.process(); });
    const double cache_ms = std::chrono::duration<double, std::milli>(Clock::now() - cache_start).count();
    REQUIRE(print.material_band_result() == result);
    TemporaryDirectory   temporary;
    const auto           export_start = Clock::now();
    GCodeProcessorResult parsed;
    print.export_gcode((temporary.path / "stack.gcode").string(), &parsed);
    const double export_ms = std::chrono::duration<double, std::milli>(Clock::now() - export_start).count();
    REQUIRE(parsed.material_band_cycles.size() == plan.cycles.size());
    const nlohmann::json metrics{{"snapshot_ms", result->snapshot_ms},
                                 {"paths_ms", result->paths_ms},
                                 {"finalize_ms", result->finalize_ms},
                                 {"cache_hit_ms", cache_ms},
                                 {"export_ms", export_ms},
                                 {"operations", plan.operations.size()},
                                 {"cycles", plan.cycles.size()},
                                 {"object_volume_mm3", plan.object_volume_mm3},
                                 {"purge_volume_mm3", plan.purge_volume_mm3},
                                 {"engine_used", "material_bands"},
                                 {"fallback", ""}};
    std::ofstream(output) << metrics.dump(2) << '\n';
}

namespace {
MaterialBandFixtures::Fixture u1_fixture(size_t count, bool gradient, const std::string& shape = "box")
{
    auto                               fixture = stack_fixture(count, gradient, shape);
    DynamicPrintConfig                 stock;
    std::map<std::string, std::string> metadata;
    std::string                        reason;
    stock.load_from_json(std::string(TEST_DATA_DIR) + "/material_band/u1/stock-machine.json", ForwardCompatibilitySubstitutionRule::Disable,
                         metadata, reason);
    REQUIRE(reason.empty());
    fixture.config.apply(stock);
    fixture.config.set("material_band_stationary_toolchange", false);
    fixture.config.set("material_band_tool_sections", std::string());
    fixture.config.set("enable_arc_fitting", false);
    // The stock cylinder deliberately bounds the whole head at nozzle height.
    // Keep the tower outside it while rebuilding tower layers after long runs.
    fixture.config.option<ConfigOptionFloats>("wipe_tower_x")->values = {220.};
    fixture.config.option<ConfigOptionFloats>("wipe_tower_y")->values = {220.};
    fixture.config.option<ConfigOptionFloats>("retract_length_toolchange")->values.assign(4, 1.5);
    fixture.config.option<ConfigOptionFloats>("fan_min_speed")->values.assign(count, 70.);
    fixture.config.option<ConfigOptionInts>("nozzle_temperature_initial_layer")->values.assign(count, 215);
    fixture.config.option<ConfigOptionInts>("nozzle_temperature")->values.assign(count, 205);
    fixture.config.set("printer_settings_id", std::string("Snapmaker U1 (0.4 nozzle)"));
    fixture.config.set_key_value("print_settings_id", new ConfigOptionString("Material bands U1 verification"));
    fixture.config.set_key_value("filament_settings_id",
                                 new ConfigOptionStrings(std::vector<std::string>(count, "Material bands PLA verification")));
    fixture.config.set_key_value("inherits_group", new ConfigOptionStrings(std::vector<std::string>(count + 2, "")));
    fixture.config.set_key_value("different_settings_to_system", new ConfigOptionStrings(std::vector<std::string>(count + 2, "")));
    return fixture;
}
} // namespace

TEST_CASE("Material band U1 stock profile exports moving tool changes", "[material_band][u1][production]")
{
    const size_t count    = GENERATE(2u, 3u, 4u);
    const bool   gradient = GENERATE(false, true);
    const bool   relative = GENERATE(true, false);
    auto         fixture  = u1_fixture(count, gradient);
    fixture.config.set("use_relative_e_distances", relative);
    fixture.config.option<ConfigOptionStrings>("filament_end_gcode")->values.assign(count, "; filament end gcode\n");
    CHECK(validate_u1_scripts(fixture.config).empty());
    Print print;
    print.apply(fixture.model, fixture.config);
    tbb::task_arena(2).execute([&] { print.process(); });
    const auto& plan = accepted(print);
    CHECK(plan.profile.adapter == MachineAdapter::SnapmakerU1);
    CHECK_FALSE(plan.machine.shared_nozzle);
    CHECK(plan.machine.sections.front().radius_mm == 72.5);
    CHECK(plan.profile.bed_min == Vec2d(.5, 1.));
    double top = 0.;
    for (const auto& op : plan.operations)
        if (op.kind == OperationKind::Deposit)
            top = std::max(top, op.position.z());
    size_t changes = 0;
    for (const auto& op : plan.operations) {
        if (op.kind == OperationKind::U1ToolChange) {
            ++changes;
            CHECK(op.position.z() >= top + 2. - 1e-6);
        }
        CHECK(op.kind != OperationKind::Home);
        CHECK(op.kind != OperationKind::ToolChange);
    }
    CHECK(changes == plan.transitions.size());
    TemporaryDirectory temporary;
    const char*        output    = std::getenv("MATERIAL_BAND_U1_DIR");
    const auto         directory = output ? boost::filesystem::path(output) : temporary.path;
    boost::filesystem::create_directories(directory);
    const std::string    name = "U1-" + std::to_string(count) + (gradient ? "-gradient" : "-blend") + (relative ? "" : "-absolute");
    GCodeProcessorResult parsed;
    print.export_gcode((directory / (name + ".gcode")).string(), &parsed);
    REQUIRE(parsed.material_band_cycles.size() == plan.cycles.size());
    std::ifstream     stream((directory / (name + ".gcode")).string());
    const std::string gcode((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    CHECK(gcode.find(";MB_MACHINE Snapmaker U1") != std::string::npos);
    for (size_t physical = 0; physical < 4; ++physical)
        CHECK(gcode.find("SM_PRINT_AUTO_FEED EXTRUDER=" + std::to_string(physical) + "\n") != std::string::npos);
    CHECK(gcode.find("BED_MESH_CALIBRATE PROBE_COUNT=11,11") != std::string::npos);
    CHECK(gcode.find(";MB_U1_DOCK_BEGIN\nM400\nT1") != std::string::npos);
    CHECK(gcode.find("G10 P") == std::string::npos);
    // Preserved source settings in CONFIG_BLOCK are comments, not executed code.
    CHECK(gcode.substr(0, gcode.find("; CONFIG_BLOCK_START")).find("G1 X185 E15") == std::string::npos);
    CHECK(gcode.find("PRINT_END\nTIMELAPSE_STOP") != std::string::npos);
    if (output) {
        fixture.model.set_backup_path(directory.string());
        const auto  path = (directory / (name + ".3mf")).string();
        StoreParams store;
        store.path     = path.c_str();
        store.model    = &fixture.model;
        store.config   = &fixture.config;
        store.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipStatic | SaveStrategy::SkipAuxiliary;
        REQUIRE(store_bbs_3mf(store));
    }
}

TEST_CASE("Material band U1 rejects incompatible firmware and custom motion", "[material_band][u1]")
{
    const std::string scenario = GENERATE("firmware", "shared", "custom", "height", "nozzles");
    auto              fixture  = u1_fixture(4, true);
    std::string       expected;
    if (scenario == "firmware") {
        fixture.config.set_deserialize_strict("gcode_flavor", "reprapfirmware");
        expected = "u1_requires_klipper_independent_tools";
    } else if (scenario == "shared") {
        fixture.config.set("single_extruder_multi_material", true);
        expected = "u1_requires_klipper_independent_tools";
    } else if (scenario == "custom") {
        fixture.config.set("change_filament_gcode", std::string("G1 X10 E10"));
        expected = "u1_custom_script_requires_adapter:change_filament_gcode";
    } else if (scenario == "nozzles") {
        fixture.config.option<ConfigOptionFloats>("nozzle_diameter")->values[1] = .6;
        expected                                                                = "u1_matching_nozzle_diameters_required";
    } else {
        fixture.config.set("printable_height", 3.);
        expected = "u1_dock_clearance_outside_build_volume";
    }
    Print print;
    print.apply(fixture.model, fixture.config);
    const auto result = plan_production_print(print, {});
    REQUIRE(result->fallback);
    CHECK_FALSE(result->plan);
    CHECK(result->fallback->detail == expected);
}

TEST_CASE("Material band U1 accepts curved and holed multi material solids", "[material_band][u1][production]")
{
    const std::string shape   = GENERATE("cylinder", "annulus", "hole_plate");
    const size_t      count   = GENERATE(3u, 4u);
    auto              fixture = u1_fixture(count, false, shape);
    Print             print;
    print.apply(fixture.model, fixture.config);
    tbb::task_arena(2).execute([&] { print.process(); });
    const auto& plan = accepted(print);
    CHECK(plan.machine.material_count == count);
    CHECK(plan.schedule->index_visits < 100000000);
    const char* output = std::getenv("MATERIAL_BAND_U1_DIR");
    if (output) {
        GCodeProcessorResult parsed;
        print.export_gcode((boost::filesystem::path(output) / ("U1-" + std::to_string(count) + "-" + shape + ".gcode")).string(), &parsed);
        CHECK(parsed.material_band_cycles.size() == plan.cycles.size());
    }
}

TEST_CASE("Material band U1 recognizes the installed June stock template without accepting custom motion", "[material_band][u1]")
{
    auto          fixture = u1_fixture(4, true);
    std::ifstream stream(std::string(TEST_DATA_DIR) + "/material_band/u1/stock-change-filament-20260605.txt");
    REQUIRE(stream.good());
    const std::string code((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    fixture.config.set("change_filament_gcode", code);
    CHECK(validate_u1_scripts(fixture.config).empty());
    fixture.config.set("change_filament_gcode", code + "\nG1 E50\n");
    CHECK(validate_u1_scripts(fixture.config) == "u1_custom_script_requires_adapter:change_filament_gcode");
}

TEST_CASE("Material painted regions reject invalid curves and interrupted work", "[material_band][painted_regions]")
{
    auto       fixture  = stack_fixture(4, true);
    const auto captured = snapshot_input(*fixture.model.objects.front(), fixture.config, input_context());
    REQUIRE(std::holds_alternative<std::shared_ptr<const KernelInput>>(captured));
    auto            input = *std::get<std::shared_ptr<const KernelInput>>(captured);
    MachineEnvelope machine;
    machine.material_count       = 4;
    machine.sections             = {{0., 140., 72.5}};
    machine.coplanar_tip_contact = true;
    PathLimits        limits;
    const std::string scenario = GENERATE("knot", "fraction", "sum", "cancel", "stale", "cycles", "memory");
    CAPTURE(scenario);
    FailureCode expected = FailureCode::InvalidInput;
    if (scenario == "knot")
        input.regions[0].height_gradient[1].position = input.regions[0].height_gradient[0].position;
    if (scenario == "fraction")
        input.regions[0].height_gradient[0].fractions[0] = std::numeric_limits<double>::quiet_NaN();
    if (scenario == "sum")
        input.regions[0].height_gradient[0].fractions[0] = .3;
    if (scenario == "cancel")
        expected = FailureCode::Cancelled;
    if (scenario == "stale")
        expected = FailureCode::StaleInput;
    if (scenario == "cycles") {
        limits.max_cycles = 0;
        expected          = FailureCode::ResourceLimit;
    }
    if (scenario == "memory") {
        limits.max_temporary_bytes = 0;
        expected                   = FailureCode::ResourceLimit;
    }
    const auto result =
        plan_painted_regions(input, machine, 1, 2, {}, limits, [&] { return scenario == "cancel"; }, [&] { return scenario == "stale"; });
    REQUIRE(std::holds_alternative<Failure>(result));
    CHECK(std::get<Failure>(result).code == expected);
}

TEST_CASE("Material painted U1 reported gradient project slices both objects", "[material_band][painted_regions][u1][slow]")
{
    TemporaryDirectory temporary;
    Model              model;
    model.set_backup_path(temporary.path.string());
    boost::filesystem::create_directories(temporary.path / "Metadata/fullspectrum");
    boost::filesystem::create_directories(temporary.path / "3D/Objects");
    auto config = DynamicPrintConfig::full_print_config();
    for (const auto& key : config.keys()) {
        if (config.option(key)->type() != coEnums)
            continue;
        auto* initialized = config.def()->get(key)->create_default_option();
        initialized->set(config.option(key));
        config.set_key_value(key, initialized);
    }
    config.apply(PresetBundle{}.project_config);
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    PlateDataPtrs             plates;
    std::vector<Preset*>      presets;
    bool                      is_bbs = false;
    Semver                    version;
    const std::string         source = std::string(TEST_DATA_DIR) + "/material_band/production/painted/U1-4-gradient-reported.3mf";
    const bool loaded = load_bbs_3mf(source.c_str(), &config, &substitutions, &model, &plates, &presets, &is_bbs, &version, nullptr,
                                     LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence);
    release_PlateData_list(plates);
    for (auto* preset : presets)
        delete preset;
    REQUIRE(loaded);
    REQUIRE(model.objects.size() == 2);
    Print print;
    print.apply(model, config);
    tbb::task_arena(2).execute([&] { print.process(); });
    const auto& plan = accepted(print);
    CHECK(plan.schedule->objects.size() == 3);
    CHECK(std::any_of(plan.schedule->objects.begin(), plan.schedule->objects.end(),
                      [](const ScheduleObject& object) { return object.paths->ordered_region_paths; }));
    CHECK(std::accumulate(plan.object_volume_mm3.begin(), plan.object_volume_mm3.end(), 0.) ==
          Approx(27. * 27. * 27. + 12. * 12. * 2.4).epsilon(1e-4));
    for (double volume : plan.object_volume_mm3)
        CHECK(volume > 0.);
    if (const char* output = std::getenv("MATERIAL_BAND_PAINTED_DIR")) {
        const boost::filesystem::path directory(output);
        boost::filesystem::create_directories(directory);
        GCodeProcessorResult parsed;
        print.export_gcode((directory / "U1-4-gradient-reported.gcode").string(), &parsed);
        CHECK(parsed.material_band_cycles.size() == plan.cycles.size());
    }
}

TEST_CASE("Material painted regions preserve an independent interior with multiple U1 filaments", "[material_band][painted_regions][u1]")
{
    const size_t      count = GENERATE(2u, 3u, 4u);
    const std::string kind  = GENERATE("blend", "gradient", "pure", "adjacent", "caps", "recipes");
    const std::string shape = GENERATE("box", "cylinder", "annulus");
    CAPTURE(count, kind, shape);
    auto fixture = u1_fixture(count, kind == "gradient" || kind == "adjacent" || kind == "caps" || kind == "recipes", shape);
    if (count == 4 && (kind == "caps" || kind == "recipes")) {
        const std::string pattern = kind == "caps" ? "concentric" : "rectilinear";
        for (const char* key : {"internal_solid_infill_pattern", "top_surface_pattern", "bottom_surface_pattern"})
            fixture.config.set_deserialize_strict(key, pattern);
    }
    if (kind == "recipes") {
        MixedFilamentManager manager;
        const auto           colors = fixture.config.option<ConfigOptionStrings>("filament_colour")->values;
        manager.load_custom_entries(fixture.config.opt_string("mixed_filament_definitions"), colors);
        auto                    definitions = manager.mixed_filament_definitions(count);
        MixedFilamentDefinition second;
        second.identity.stable_id      = 32000 + count;
        second.source.kind             = MixedFilamentSourceKind::Custom;
        second.recipe.blend.components = {{{1}, 50}, {{unsigned(count)}, 50}};
        second.behavior.distribution   = MixedFilamentDistributionMode::LayerCycle;
        second.behavior.layer_cadence  = {1, 1};
        definitions.push_back(second);
        manager.set_mixed_filament_definitions(definitions, colors);
        fixture.config.set("mixed_filament_definitions", manager.serialize_custom_entries());
    }
    fixture.config.set("use_relative_e_distances", kind != "caps");
    fixture.config.set("flush_multiplier", 1.);
    auto& flush = fixture.config.option<ConfigOptionFloats>("flush_volumes_matrix")->values;
    for (size_t a = 0; a < count; ++a)
        for (size_t b = 0; b < count; ++b)
            flush[a * count + b] = a == b ? 0. : 1.;
    auto* object = fixture.model.objects.front();
    auto* volume = object->volumes.front();
    object->config.set("extruder", 1);
    volume->config.set("extruder", 1);
    const auto&      mesh = volume->mesh().its;
    TriangleSelector selector(volume->mesh());
    for (size_t i = 0; i < mesh.indices.size(); ++i) {
        const auto& t = mesh.indices[i];
        const Vec3f a = mesh.vertices[t[0]], b = mesh.vertices[t[1]], c = mesh.vertices[t[2]];
        const Vec3f normal   = (b - a).cross(c - a);
        unsigned    material = kind == "pure" ? (normal.x() > 0. ? unsigned(count) : 2u) : unsigned(count + 1);
        if (kind == "adjacent" && normal.x() > 0.)
            material = 2;
        if (kind == "recipes" && normal.x() > 0.)
            material = unsigned(count + 2);
        if (std::abs(normal.z()) > 1e-6) {
            if (kind != "caps")
                continue;
            material = normal.z() > 0. ? unsigned(count) : 2u;
        }
        selector.set_facet(int(i), static_cast<EnforcerBlockerType>(material));
    }
    volume->mmu_segmentation_facets.set(selector);
    Print print;
    print.apply(fixture.model, fixture.config);
    tbb::task_arena(2).execute([&] { print.process(); });
    const auto& plan = accepted(print);
    REQUIRE_FALSE(plan.schedule->objects.empty());
    const auto& paths = *plan.schedule->objects.front().paths;
    CHECK(paths.ordered_region_paths);
    size_t             walls = 0, fill = 0, tower_moves = 0;
    double             previous_z = 0.;
    std::set<unsigned> tools;
    for (const auto& path : paths.paths) {
        CHECK(path.vertices.front().z() >= previous_z - 1e-8);
        previous_z = path.vertices.front().z();
        tools.insert(path.band.material.value);
        (path.role == PathRole::Perimeter ? walls : fill)++;
        for (const auto& segment : path.segments) {
            REQUIRE_FALSE(segment.cells.empty());
            const double height = segment.cells.front().upper[0].z() - segment.cells.front().lower_z[0];
            CHECK(segment.height_start_mm == Approx(height).margin(1e-9));
            CHECK(segment.height_end_mm == Approx(height).margin(1e-9));
        }
    }
    CHECK(walls > 0);
    CHECK(fill > 0);
    CHECK(tools.size() >= 2);
    if (kind == "pure" || kind == "adjacent") {
        size_t       samples = 0, wrong = 0;
        const double center_y = (paths.solid_min.y() + paths.solid_max.y()) / 2.;
        for (const auto& path : paths.paths)
            for (const auto& s : path.segments)
                for (const auto& cell : s.cells) {
                    const Vec3d p = (cell.upper[0] + cell.upper[1] + cell.upper[2]) / 3.;
                    if (p.x() > paths.solid_max.x() - .25 && std::abs(p.y() - center_y) < 1.) {
                        ++samples;
                        wrong += path.band.material.value != (kind == "pure" ? count : 2);
                    }
                }
        CHECK(samples > 0);
        CHECK(wrong == 0);
    }
    if (kind == "caps") {
        size_t top_samples = 0, bottom_samples = 0, wrong = 0;
        for (const auto& path : paths.paths)
            for (const auto& s : path.segments)
                for (const auto& cell : s.cells) {
                    if (cell.upper[0].z() > paths.solid_max.z() - .24) {
                        ++top_samples;
                        wrong += path.band.material.value != count;
                    }
                    if (cell.upper[0].z() <= paths.solid_min.z() + .24 + 1e-8) {
                        ++bottom_samples;
                        wrong += path.band.material.value != 2;
                    }
                }
        CHECK(top_samples > 0);
        CHECK(bottom_samples > 0);
        CHECK(wrong == 0);
    }
    double model_volume = 0.;
    for (const auto& t : mesh.indices)
        model_volume += mesh.vertices[t[0]].cast<double>().dot(
                            mesh.vertices[t[1]].cast<double>().cross(mesh.vertices[t[2]].cast<double>())) /
                        6.;
    CHECK(std::accumulate(plan.object_volume_mm3.begin(), plan.object_volume_mm3.end(), 0.) == Approx(model_volume).epsilon(1e-4));
    if (shape == "box" && kind != "caps") {
        const double wall           = .9;
        size_t       interior_cells = 0, wrong_material = 0;
        for (const auto& path : paths.paths)
            for (const auto& s : path.segments)
                for (const auto& cell : s.cells) {
                    const Vec3d center = (cell.upper[0] + cell.upper[1] + cell.upper[2]) / 3.;
                    if (center.x() > paths.solid_min.x() + wall + 1e-5 && center.x() < paths.solid_max.x() - wall - 1e-5 &&
                        center.y() > paths.solid_min.y() + wall + 1e-5 && center.y() < paths.solid_max.y() - wall - 1e-5) {
                        ++interior_cells;
                        wrong_material += path.band.material.value != 1;
                    }
                }
        CHECK(interior_cells > 0);
        CHECK(wrong_material == 0);
        if (kind == "blend") {
            const Vec3d  size  = paths.solid_max - paths.solid_min;
            const double core  = (size.x() - 2. * wall) * (size.y() - 2. * wall) * size.z();
            const double shell = size.x() * size.y() * size.z() - core;
            for (size_t m = 0; m < count; ++m) {
                const double fraction = (m + 1 == count ? 100 - (100 / count) * (count - 1) : 100 / count) / 100.;
                CHECK(plan.object_volume_mm3[m] == Approx(shell * fraction + (m == 0 ? core : 0.)).epsilon(1e-4));
            }
        }
    }
    for (const auto& op : plan.operations)
        if (op.kind == OperationKind::Deposit && op.deposition != DepositionKind::Object) {
            ++tower_moves;
            CHECK(op.volume_mm3 > 0.);
            CHECK(op.position.x() >= plan.profile.purge.origin.x());
            CHECK(op.position.y() >= plan.profile.purge.origin.y());
        }
    CHECK(tower_moves > 0);
    CHECK(plan.schedule->objects.size() == 2);
    for (const auto& transition : plan.transitions)
        CHECK(transition.deposited_purge_mm3 >= transition.required_purge_mm3);
    if (count == 4 && kind == "pure" && shape == "box") {
        auto profile = plan.profile;
        for (size_t a = 0; a < count; ++a)
            for (size_t b = 0; b < count; ++b)
                profile.purge.volume_mm3[a][b] = a == b ? 0. : 1.;
        ScheduleInput input;
        input.revision         = plan.revision;
        input.machine          = plan.machine;
        input.objects          = {plan.schedule->objects.front()};
        input.initial_position = plan.schedule->initial_position;
        auto rebuilt           = finalize_print(input, profile);
        REQUIRE(std::holds_alternative<std::shared_ptr<const FinalPrintPlan>>(rebuilt));
        const auto& all_purges = *std::get<std::shared_ptr<const FinalPrintPlan>>(rebuilt);
        CHECK(all_purges.transitions.size() == plan.transitions.size());
        for (const auto& transition : all_purges.transitions)
            CHECK(transition.deposited_purge_mm3 >= transition.required_purge_mm3);
    }
    const char* output = std::getenv("MATERIAL_BAND_PAINTED_DIR");
    if (output) {
        const boost::filesystem::path directory(output);
        boost::filesystem::create_directories(directory);
        const std::string    name = "U1-painted-" + std::to_string(count) + "-" + kind + "-" + shape;
        GCodeProcessorResult parsed;
        print.export_gcode((directory / (name + ".gcode")).string(), &parsed);
        fixture.model.set_backup_path(directory.string());
        const std::string path = (directory / (name + ".3mf")).string();
        StoreParams       store;
        store.path     = path.c_str();
        store.model    = &fixture.model;
        store.config   = &fixture.config;
        store.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipStatic | SaveStrategy::SkipAuxiliary;
        REQUIRE(store_bbs_3mf(store));
    }
}

TEST_CASE("Material cycles retain first layer and fit every active paint recipe", "[material_band][layer_process]")
{
    KernelInput input;
    input.solid.vertices                  = {Vec3d(0., 0., 0.), Vec3d(10., 10., 48.)};
    input.process.min_active_thickness_mm = .06;
    input.process.max_active_thickness_mm = .24;
    for (double fraction : {1., .5, .6, .38}) {
        RegionIntent region;
        region.fractions = {{fraction, 1. - fraction, 0., 0.}};
        input.regions.push_back(region);
    }
    LayerProcessProfile profile;
    profile.nominal_height_mm = .08;
    profile.initial_height_mm = .2;
    const auto result         = plan_process_cycles(input, profile);
    REQUIRE(std::holds_alternative<std::vector<ProcessCycle>>(result));
    const auto& cycles = std::get<std::vector<ProcessCycle>>(result);
    CHECK(cycles.front().bottom_mm == 0.);
    CHECK(cycles.front().top_mm == .2);
    CHECK(cycles.back().top_mm == 48.);
    for (size_t i = 0; i < cycles.size(); ++i) {
        if (i)
            CHECK(cycles[i].bottom_mm == cycles[i - 1].top_mm);
        for (const auto& region : input.regions)
            for (double f : region.fractions)
                if (f > 0.) {
                    const double thickness = f * (cycles[i].top_mm - cycles[i].bottom_mm);
                    CHECK(thickness >= .06 - 1e-9);
                    CHECK(thickness <= .24 + 1e-9);
                }
    }
    profile.initial_height_mm = .08;
    CHECK(std::holds_alternative<Failure>(plan_process_cycles(input, profile)));
    profile.initial_height_mm = .2;
    CHECK(std::holds_alternative<Failure>(plan_process_cycles(input, profile, {}, [] { return true; })));
}

TEST_CASE("Material band U1 layer process keeps gyroid voids and a separate brim", "[material_band][layer_process][u1]")
{
    const size_t count    = GENERATE(2u, 3u, 4u);
    const bool   gradient = GENERATE(false, true);
    auto         fixture  = u1_fixture(count, gradient);
    fixture.config.set("layer_height", .08);
    fixture.config.set("initial_layer_print_height", .2);
    fixture.config.set("mixed_filament_height_lower_bound", count == 4 || gradient ? .04 : .06);
    fixture.config.option<ConfigOptionFloats>("initial_layer_speed")->values.assign(count, 9.);
    fixture.config.option<ConfigOptionFloats>("initial_layer_infill_speed")->values.assign(count, 11.);
    fixture.config.set("brim_width", 5.);
    fixture.config.set_deserialize_strict("brim_type", "outer_only");
    fixture.config.set_deserialize_strict("sparse_infill_density", "15%");
    fixture.config.set_deserialize_strict("sparse_infill_pattern", "gyroid");
    fixture.config.set_deserialize_strict("bottom_surface_pattern", "monotonic");
    fixture.config.set_deserialize_strict("top_surface_pattern", "monotonic");
    fixture.config.set("top_shell_layers", 2);
    fixture.config.set("bottom_shell_layers", 2);
    fixture.config.set("top_shell_thickness", .2);
    fixture.config.set("bottom_shell_thickness", .2);
    fixture.config.set("enable_prime_tower", true);
    fixture.config.set("prime_volume", 1.);
    Print print;
    print.apply(fixture.model, fixture.config);
    tbb::task_arena(2).execute([&] { print.process(); });
    const auto& plan = accepted(print);
    double      brim = 0., sparse = 0., first_top = 0., first_volume = 0.;
    for (const auto& op : plan.operations) {
        if (op.kind != OperationKind::Deposit)
            continue;
        const double diameter = plan.machine.filament_diameter_mm.at(op.material.value - 1);
        CHECK(op.volume_mm3 >= 1e-5 * PI * diameter * diameter / 4.);
        if (op.logical_cycle == 0)
            CHECK(op.speed_mm_s <= (op.role == PathRole::Perimeter || op.deposition == DepositionKind::Brim ? 9. : 11.));
        if (op.logical_cycle == 0 && op.deposition == DepositionKind::Object) {
            first_top = std::max(first_top, op.position.z());
            first_volume += op.volume_mm3;
        }
        if (op.deposition == DepositionKind::Brim) {
            brim += op.volume_mm3;
            CHECK(op.position.z() == Approx(.2));
        }
        if (op.role == PathRole::SparseFill)
            sparse += op.volume_mm3;
    }
    CHECK(brim > 0.);
    CHECK(sparse > 0.);
    CHECK(first_top == Approx(.2));
    const double first_target = fixture.manifest.at("dimensions_mm").at(0).get<double>() *
                                fixture.manifest.at("dimensions_mm").at(1).get<double>() * .2;
    CHECK(first_volume >= .99 * first_target);
    CHECK(first_volume <= 1.0001 * first_target);
    if (!gradient)
        CHECK(plan.object_volume_mm3[0] == Approx(plan.object_volume_mm3[1]).epsilon(1e-4));
    CHECK(std::accumulate(plan.object_volume_mm3.begin(), plan.object_volume_mm3.end(), 0.) < 300.);
    CHECK(std::accumulate(plan.brim_volume_mm3.begin(), plan.brim_volume_mm3.end(), 0.) == Approx(brim));
    for (size_t m = 0; m < count; ++m)
        CHECK(plan.object_volume_mm3[m] > 0.);
    CHECK(std::accumulate(plan.purge_volume_mm3.begin(), plan.purge_volume_mm3.end(), 0.) > 0.);
    TemporaryDirectory temporary;
    const char*        output    = std::getenv("MATERIAL_BAND_LAYER_DIR");
    const auto         directory = output ? boost::filesystem::path(output) : temporary.path;
    boost::filesystem::create_directories(directory);
    const std::string    name = "U1-layer-" + std::to_string(count) + (gradient ? "-gradient" : "-blend");
    GCodeProcessorResult parsed;
    print.export_gcode((directory / (name + ".gcode")).string(), &parsed);
    CHECK(parsed.material_band_cycles.size() == plan.cycles.size());
    if (output) {
        fixture.model.set_backup_path(directory.string());
        const auto  path = (directory / (name + ".3mf")).string();
        StoreParams store;
        store.path     = path.c_str();
        store.model    = &fixture.model;
        store.config   = &fixture.config;
        store.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipStatic | SaveStrategy::SkipAuxiliary;
        REQUIRE(store_bbs_3mf(store));
    }
}

TEST_CASE("Material layer infill and brims preserve curved cavities", "[material_band][layer_process][u1]")
{
    const std::string shape   = GENERATE("cylinder", "annulus", "hole_plate");
    const std::string pattern = GENERATE("gyroid", "rectilinear");
    CAPTURE(shape, pattern);
    auto fixture = u1_fixture(3, false, shape);
    fixture.model.objects.front()->volumes.front()->scale(1., 1., 5.);
    fixture.config.set("initial_layer_print_height", .2);
    fixture.config.set("layer_height", .08);
    fixture.config.set("mixed_filament_height_lower_bound", .04);
    fixture.config.set("brim_width", 1.);
    fixture.config.set_deserialize_strict("brim_type", "outer_only");
    fixture.config.set_deserialize_strict("sparse_infill_density", "15%");
    fixture.config.set_deserialize_strict("sparse_infill_pattern", pattern);
    fixture.config.set("top_shell_layers", 2);
    fixture.config.set("bottom_shell_layers", 2);
    fixture.config.set("top_shell_thickness", .2);
    fixture.config.set("bottom_shell_thickness", .2);
    fixture.config.set("enable_prime_tower", false);
    Print print;
    print.apply(fixture.model, fixture.config);
    const auto production = plan_production_print(print, {1, 2, 3, 4, 5, 6, 7});
    if (production->fallback)
        FAIL(production->fallback->detail);
    REQUIRE(production->plan);
    const auto& plan        = *production->plan;
    const auto  center      = fixture.model.objects.front()->instances.front()->get_offset().head<2>().eval();
    double      brim_volume = 0.;
    for (const auto& op : plan.operations) {
        if (op.kind != OperationKind::Deposit)
            continue;
        if (shape != "cylinder") {
            const double hole_radius = shape == "annulus" ? 5. : 2.;
            CHECK((op.position.head<2>() - center).norm() >= hole_radius * std::cos(PI / 64.) - .01);
        }
        if (op.deposition == DepositionKind::Brim)
            brim_volume += op.volume_mm3;
    }
    CHECK(brim_volume > 0.);
}

TEST_CASE("Material layer process bounds retained geometry and rejects missing infill preprocessing", "[material_band][layer_process]")
{
    const std::string scenario = GENERATE("memory", "segments", "paths", "cancel", "stale", "adaptive", "lightning");
    CAPTURE(scenario);
    auto       fixture  = stack_fixture(2, false);
    const auto snapshot = snapshot_input(*fixture.model.objects.front(), fixture.config, input_context());
    REQUIRE(std::holds_alternative<std::shared_ptr<const KernelInput>>(snapshot));
    const auto&     input = *std::get<std::shared_ptr<const KernelInput>>(snapshot);
    MachineEnvelope machine;
    machine.sections = {{0., 10., .2}};
    LayerProcessProfile profile;
    profile.initial_height_mm = profile.nominal_height_mm = .24;
    profile.top_shell_mm = profile.bottom_shell_mm = 0.;
    if (scenario == "adaptive")
        profile.sparse_pattern = ipAdaptiveCubic;
    if (scenario == "lightning")
        profile.sparse_pattern = ipLightning;
    PathLimits limits;
    if (scenario == "memory")
        limits.max_temporary_bytes = 1;
    if (scenario == "segments")
        limits.max_segments = 8;
    if (scenario == "paths")
        limits.max_paths = 1;
    const auto result =
        plan_layer_process(input, machine, 1, profile, limits, [&] { return scenario == "cancel"; }, [&] { return scenario == "stale"; });
    REQUIRE(std::holds_alternative<Failure>(result));
    const auto& failure = std::get<Failure>(result);
    if (scenario == "cancel")
        CHECK(failure.code == FailureCode::Cancelled);
    else if (scenario == "stale")
        CHECK(failure.code == FailureCode::StaleInput);
    else if (scenario == "adaptive" || scenario == "lightning")
        CHECK(failure.detail == "layer_infill_requires_volume_preprocessing");
    else
        CHECK(failure.code == FailureCode::ResourceLimit);
}

TEST_CASE("Material layer skins bridge a side recess onto actual wall anchors", "[material_band][layer_process][u1]")
{
    const std::string scenario = GENERATE("unpainted", "painted_skin", "painted_perimeter", "wide_bridge", "too_wide_bridge");
    CAPTURE(scenario);
    const bool painted = scenario == "painted_skin" || scenario == "painted_perimeter", perimeter_only = scenario == "painted_perimeter";
    // A closed, manifold block with a 2 x 4 x .8 mm recess open at one side.
    // Its first roof skin must bridge the recess before sparse fill resumes.
    indexed_triangle_set                    mesh;
    const float                             recess_end  = scenario == "too_wide_bridge" ? 26.f : 20.f;
    const std::array<std::vector<float>, 3> coordinates = scenario == "wide_bridge" || scenario == "too_wide_bridge" ?
                                                              std::array<std::vector<float>, 3>{{{0.f, recess_end, recess_end + 4.f},
                                                                                                 {0.f, 4.f, recess_end, recess_end + 4.f},
                                                                                                 {0.f, .8f, 1.6f}}} :
                                                              std::array<std::vector<float>, 3>{
                                                                  {{0.f, 2.f, 12.f}, {0.f, 4.f, 8.f, 12.f}, {0.f, .8f, 1.6f}}};
    const auto                              vertex      = [](int x, int y, int z) { return (x * 4 + y) * 3 + z; };
    for (float x : coordinates[0])
        for (float y : coordinates[1])
            for (float z : coordinates[2])
                mesh.vertices.emplace_back(x, y, z);
    const auto filled = [](const std::array<int, 3>& p) {
        return p[0] >= 0 && p[0] < 2 && p[1] >= 0 && p[1] < 3 && p[2] >= 0 && p[2] < 2 && !(p[0] == 0 && p[1] == 1 && p[2] == 0);
    };
    for (int x = 0; x < 2; ++x)
        for (int y = 0; y < 3; ++y)
            for (int z = 0; z < 2; ++z) {
                const std::array<int, 3> cell{x, y, z};
                if (!filled(cell))
                    continue;
                for (int axis = 0; axis < 3; ++axis)
                    for (int side = 0; side < 2; ++side) {
                        auto neighbor = cell;
                        neighbor[axis] += side ? 1 : -1;
                        if (filled(neighbor))
                            continue;
                        std::array<int, 4>                      face;
                        const std::array<std::array<int, 2>, 4> corners{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
                        for (size_t i = 0; i < 4; ++i) {
                            auto p = cell;
                            p[axis] += side;
                            p[(axis + 1) % 3] += corners[i][0];
                            p[(axis + 2) % 3] += corners[i][1];
                            face[i] = vertex(p[0], p[1], p[2]);
                        }
                        if (!side)
                            std::reverse(face.begin(), face.end());
                        mesh.indices.emplace_back(face[0], face[1], face[2]);
                        mesh.indices.emplace_back(face[0], face[2], face[3]);
                    }
            }
    auto fixture = u1_fixture(2, false);
    fixture.model.objects.front()->volumes.front()->set_mesh(std::move(mesh));
    fixture.model.objects.front()->invalidate_bounding_box();
    if (painted) {
        MixedFilamentManager manager;
        const auto           colors = fixture.config.option<ConfigOptionStrings>("filament_colour")->values;
        manager.load_custom_entries(fixture.config.opt_string("mixed_filament_definitions"), colors);
        auto                    definitions = manager.mixed_filament_definitions(2);
        MixedFilamentDefinition second;
        second.identity.stable_id      = 32901;
        second.source.kind             = MixedFilamentSourceKind::Custom;
        second.recipe.blend.components = {{{1}, 40}, {{2}, 60}};
        second.behavior.distribution   = MixedFilamentDistributionMode::LayerCycle;
        second.behavior.layer_cadence  = {1, 1};
        definitions.push_back(second);
        manager.set_mixed_filament_definitions(definitions, colors);
        fixture.config.set("mixed_filament_definitions", manager.serialize_custom_entries());
        auto* object = fixture.model.objects.front();
        auto* volume = object->volumes.front();
        object->config.set("extruder", 1);
        volume->config.set("extruder", 1);
        TriangleSelector selector(volume->mesh());
        const auto&      source         = volume->mesh().its;
        size_t           roof_triangles = 0;
        for (size_t i = 0; i < source.indices.size(); ++i) {
            const auto& triangle = source.indices[i];
            const Vec3f a = source.vertices[triangle[0]], b = source.vertices[triangle[1]], c = source.vertices[triangle[2]];
            const bool  paint_face = perimeter_only ?
                                         (b - a).cross(c - a).x() < 0. && a.x() == 0.f && std::min({a.z(), b.z(), c.z()}) >= .8f :
                                         (b - a).cross(c - a).z() < 0. && std::abs(a.z() - .8f) < 1e-6f;
            if (paint_face)
                selector.set_facet(int(i), static_cast<EnforcerBlockerType>(3 + roof_triangles++ % 2));
        }
        volume->mmu_segmentation_facets.set(selector);
    }
    fixture.config.set("initial_layer_print_height", .2);
    fixture.config.set("layer_height", .08);
    fixture.config.set("mixed_filament_height_lower_bound", painted ? .06 : .04);
    fixture.config.set_deserialize_strict("sparse_infill_density", perimeter_only ? "0%" : "15%");
    fixture.config.set_deserialize_strict("sparse_infill_pattern", "gyroid");
    fixture.config.set("top_shell_layers", perimeter_only ? 0 : 2);
    fixture.config.set("bottom_shell_layers", perimeter_only ? 0 : 2);
    fixture.config.set("top_shell_thickness", perimeter_only ? 0. : .2);
    fixture.config.set("bottom_shell_thickness", perimeter_only ? 0. : .2);
    fixture.config.set("enable_prime_tower", false);
    Print print;
    print.apply(fixture.model, fixture.config);
    const auto production = plan_production_print(print, {1, 2, 3, 4, 5, 6, 7});
    if (scenario == "too_wide_bridge") {
        REQUIRE(production->fallback);
        CHECK_FALSE(production->plan);
        CHECK(production->fallback->detail == "unsupported_bead_footprint");
        return;
    }
    if (production->fallback)
        FAIL(production->fallback->detail);
    REQUIRE(production->plan);
    size_t bridges = 0, sparse = 0;
    for (const auto& op : production->plan->operations)
        if (op.kind == OperationKind::Deposit && op.deposition == DepositionKind::Object) {
            bridges += op.role == PathRole::Bridge;
            sparse += op.role == PathRole::SparseFill;
        }
    if (perimeter_only) {
        CHECK(sparse == 0);
        size_t source_bridges = 0;
        for (const auto& object : production->plan->schedule->objects)
            for (const auto& path : object.paths->paths)
                source_bridges += path.role == PathRole::Bridge;
        CHECK(source_bridges == 0);
    } else {
        CHECK(bridges > 0);
        CHECK(sparse > 0);
    }
    if (!painted)
        CHECK(production->plan->object_volume_mm3[0] == Approx(production->plan->object_volume_mm3[1]).epsilon(.01));
    else {
        CHECK(production->plan->object_volume_mm3[1] > 0.);
        size_t foundations = 0;
        for (const auto& object : production->plan->schedule->objects)
            for (const auto& path : object.paths->paths) {
                foundations += path.band.domain % 512 == 510;
                for (const auto& segment : path.segments) {
                    CHECK(segment.height_start_mm >= .06 - 1e-5);
                    CHECK(segment.height_start_mm <= .24 + 1e-5);
                }
            }
        CHECK(foundations > 0);
    }
}
