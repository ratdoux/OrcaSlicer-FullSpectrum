#include <catch2/catch_test_macros.hpp>

#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/MixedFilamentLayerTracker.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Support/TreeSupport.hpp"
#include <tbb/task_arena.h>
#include <cstdlib>
#include <atomic>
#include <optional>
#include <string>

using namespace Slic3r;

namespace {
ExPolygon rectangle(coord_t x0, coord_t y0, coord_t x1, coord_t y1)
{
    return ExPolygon(Polygon(Points{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}));
}
} // namespace

TEST_CASE("Mixed painted components retain cadence through splits and merges", "[CPUSlicing][MixedFilament]")
{
    // All layers remain alive, as in the segmentation pass that owns the masks.
    const std::vector<ExPolygons>       layers{{rectangle(0, 0, 100, 100)},
                                               {rectangle(0, 0, 40, 100), rectangle(60, 0, 100, 100), rectangle(200, 0, 300, 100)},
                                               {rectangle(0, 0, 300, 100)},
                                               {},
                                               {rectangle(0, 0, 100, 100)}};
    const std::vector<std::vector<int>> expected{{0}, {1, 1, 0}, {2}, {}, {0}};
    MixedFilamentLayerTracker           tracker;
    std::vector<int>                    indices;
    for (size_t i = 0; i < layers.size(); ++i) {
        tracker.track_layer(layers[i], int(i), indices);
        REQUIRE(indices == expected[i]);
    }
}

TEST_CASE("Mixed painted cadence uses polygon overlap including holes", "[CPUSlicing][MixedFilament]")
{
    ExPolygon ring = rectangle(0, 0, 100, 100);
    Polygon   hole = rectangle(20, 20, 80, 80).contour;
    hole.reverse();
    ring.holes.push_back(hole);
    const ExPolygons          first{ring};
    const ExPolygons          second{rectangle(30, 30, 70, 70), rectangle(5, 5, 15, 15), ExPolygon{}};
    MixedFilamentLayerTracker tracker;
    std::vector<int>          indices;
    tracker.track_layer(first, 5, indices);
    tracker.track_layer(second, 6, indices);
    REQUIRE(indices == std::vector<int>{0, 1, -1});
}

TEST_CASE("Mixed painted channels have independent activation runs", "[CPUSlicing][MixedFilament]")
{
    const ExPolygons          masks{rectangle(0, 0, 100, 100)};
    MixedFilamentLayerTracker first, second;
    std::vector<int>          first_indices, second_indices;
    first.track_layer(masks, 0, first_indices);
    second.track_layer(masks, 2, second_indices);
    first.track_layer(masks, 3, first_indices);
    second.track_layer(masks, 3, second_indices);
    REQUIRE(first_indices == std::vector<int>{3});
    REQUIRE(second_indices == std::vector<int>{1});
}

TEST_CASE("Read-only extrusion flattening borrows leaves without changing order", "[CPUSlicing][GCode]")
{
    ExtrusionPath path(erPerimeter, 0.08, 0.4f, 0.2f);
    path.polyline = Polyline(Points{{0, 0}, {100, 0}, {100, 100}});
    ExtrusionEntityCollection inner;
    inner.no_sort = true;
    inner.append(ExtrusionLoop(path));
    inner.append(ExtrusionMultiPath(path));
    ExtrusionEntityCollection source;
    source.append(path);
    source.append(inner);
    source.append(ExtrusionEntityCollection{});
    source.append(path);

    const auto  leaves = source.flatten_refs();
    const auto* nested = static_cast<const ExtrusionEntityCollection*>(source.entities[1]);
    REQUIRE(leaves == std::vector<const ExtrusionEntity*>{source.entities[0], nested->entities[0], nested->entities[1], source.entities[3]});
    const auto copied = source.flatten(false);
    REQUIRE(leaves.size() == copied.entities.size());
    for (size_t i = 0; i < leaves.size(); ++i) {
        CHECK(leaves[i] != copied.entities[i]);
        CHECK(leaves[i]->role() == copied.entities[i]->role());
        CHECK(leaves[i]->is_loop() == copied.entities[i]->is_loop());
        Points original_points, copied_points;
        leaves[i]->collect_points(original_points);
        copied.entities[i]->collect_points(copied_points);
        CHECK(original_points == copied_points);
    }
    REQUIRE(ExtrusionEntityCollection{}.flatten_refs().empty());
}

namespace {
struct CpuReuseEnvironment
{
    const std::array<const char*, 3>          names{"ORCA_WALL_SUPPORT", "ORCA_SHELL_VOLUME", "ORCA_SHELL_PROJECTION"};
    std::array<std::optional<std::string>, 3> previous;
    explicit CpuReuseEnvironment(const char* mode)
    {
        for (size_t i = 0; i < names.size(); ++i) {
            if (const char* value = std::getenv(names[i]))
                previous[i] = value;
            set(names[i], mode);
        }
    }
    ~CpuReuseEnvironment()
    {
        for (size_t i = 0; i < names.size(); ++i)
            set(names[i], previous[i] ? previous[i]->c_str() : nullptr);
    }
    static void set(const char* name, const char* value)
    {
#ifdef _WIN32
        _putenv_s(name, value ? value : "");
#else
        if (value)
            setenv(name, value, 1);
        else
            unsetenv(name);
#endif
    }
};
} // namespace
TEST_CASE("Shell reuse preserves fill geometry for adjoining material regions", "[CPUSlicing][ShellReuse]")
{
    auto slice = [](const char* mode, bool interfaces) {
        CpuReuseEnvironment environment(mode);
        auto                config = DynamicPrintConfig::full_print_config();
        config.set_num_extruders(2);
        config.set_num_filaments(2);
        config.set("layer_height", 0.2);
        config.set("initial_layer_print_height", 0.2);
        config.set("interface_shells", interfaces);
        config.set_key_value("ensure_vertical_shell_thickness", new ConfigOptionEnum<EnsureVerticalShellThickness>(evstAll));
        config.set("top_shell_layers", 5);
        config.set("bottom_shell_layers", 4);
        config.set("enable_prime_tower", false);
        config.set("single_extruder_multi_material", false);
        config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#0000FF"};
        config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
        Model model;
        auto* object = model.add_object();
        auto* left   = object->add_volume(make_cube(8., 10., 4.));
        left->config.set("extruder", 1);
        auto* right = object->add_volume(make_cube(8., 10., 3.));
        right->translate(Vec3d(8., 0., 0.));
        right->config.set("extruder", 2);
        for (const char* option : {"wall_filament", "sparse_infill_filament", "solid_infill_filament"}) {
            left->config.set(option, 1);
            right->config.set(option, 2);
        }
        object->add_instance()->set_offset(Vec3d(100., 100., 0.));
        object->ensure_on_bed();
        Print print;
        print.is_BBL_printer() = false;
        print.set_status_silent();
        print.apply(model, config);
        print.process();
        std::vector<Polygons> result;
        REQUIRE(print.get_object(0)->num_printing_regions() == 2);
        for (const Layer* layer : print.get_object(0)->layers())
            for (const LayerRegion* region : layer->regions())
                for (SurfaceType type : {stInternal, stInternalVoid, stInternalSolid})
                    result.push_back(to_polygons(region->fill_surfaces.filter_by_type(type)));
        return result;
    };
    for (bool interfaces : {false, true}) {
        const auto reference = slice("cpu", interfaces);
        const auto reused    = slice("validate", interfaces);
        REQUIRE(reused == reference);
    }
}

TEST_CASE("Tree overhang geometry and labels agree across worker counts", "[CPUSlicing][TreeOverhang]")
{
    struct Snapshot
    {
        std::vector<ExPolygons>       overhangs, cantilevers;
        std::vector<std::vector<int>> types;
        bool                          sharp_tail, cantilever;
        double                        max_cantilever;
    };
    struct FilterEnvironment
    {
        std::optional<std::string> previous;
        explicit FilterEnvironment(const char* mode)
        {
            if (const char* value = std::getenv("ORCA_TREE_OVERHANG_FILTER"))
                previous = value;
            CpuReuseEnvironment::set("ORCA_TREE_OVERHANG_FILTER", mode);
        }
        ~FilterEnvironment() { CpuReuseEnvironment::set("ORCA_TREE_OVERHANG_FILTER", previous ? previous->c_str() : nullptr); }
    };
    auto detect = [](int workers, bool critical_only, const char* mode, bool cancel = false) {
        FilterEnvironment environment(mode);
        auto              config = DynamicPrintConfig::full_print_config();
        config.set_num_extruders(2);
        config.set_num_filaments(2);
        config.set("layer_height", 0.2);
        config.set("initial_layer_print_height", 0.2);
        config.set("enable_support", true);
        config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stTreeAuto));
        config.set("support_remove_small_overhang", false);
        config.set("support_critical_regions_only", critical_only);
        config.set("max_bridge_length", 10.0);
        config.set("enable_prime_tower", false);
        config.set("single_extruder_multi_material", false);
        config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#0000FF"};
        config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
        Model model;
        auto* object  = model.add_object();
        auto  add_box = [&](double x, double z, double width, double height, int extruder) {
            auto* volume = object->add_volume(make_cube(width, 10., height));
            volume->translate(Vec3d(x, 0., z));
            volume->config.set("extruder", extruder);
            for (const char* key : {"wall_filament", "sparse_infill_filament", "solid_infill_filament"})
                volume->config.set(key, extruder);
            return volume;
        };
        add_box(0., 0., 4., 3., 1);
        add_box(20., 0., 4., 3., 1);
        add_box(0., 3., 30., 1., 1);
        add_box(30., 3., 4., 1., 2);
        add_box(42., 1., 3., 1., 1); // Floating component exercises sharp-tail propagation.
        add_box(12., 0., 3., 5., 1)->set_type(ModelVolumeType::SUPPORT_ENFORCER);
        add_box(27., 0., 2., 5., 1)->set_type(ModelVolumeType::SUPPORT_BLOCKER);
        object->add_instance()->set_offset(Vec3d(100., 100., 0.));
        Print print;
        print.is_BBL_printer() = false;
        print.set_status_silent();
        print.apply(model, config);
        print.process();
        auto* printed = print.get_object(0);
        // Repeat detection on finished perimeters/surfaces with fresh derived state.
        for (Layer* layer : printed->layers()) {
            layer->cantilevers.clear();
            layer->sharp_tails.clear();
            layer->sharp_tails_height.clear();
        }
        REQUIRE(printed->num_printing_regions() >= 2);
        TreeSupport     support(*printed, printed->slicing_parameters());
        tbb::task_arena arena(workers);
        if (cancel) {
            std::atomic<int> callbacks{0};
            support.throw_on_cancel = [&] {
                if (callbacks.fetch_add(1) == 2)
                    print.cancel();
                if (print.canceled())
                    throw CanceledException();
            };
            REQUIRE_THROWS_AS(arena.execute([&] { support.detect_overhangs(); }), CanceledException);
            CHECK(support.overhang_types.empty()); // No partial pointer map is published.
            return Snapshot{};
        }
        arena.execute([&] { support.detect_overhangs(); });
        Snapshot result;
        for (const Layer* layer : printed->layers()) {
            result.overhangs.push_back(layer->loverhangs);
            result.cantilevers.push_back(layer->cantilevers);
            std::vector<int> types;
            for (const auto& polygon : layer->loverhangs) {
                const auto found = support.overhang_types.find(&polygon);
                REQUIRE(found != support.overhang_types.end());
                types.push_back(int(found->second));
            }
            result.types.push_back(std::move(types));
        }
        result.sharp_tail     = support.has_sharp_tails;
        result.cantilever     = support.has_cantilever;
        result.max_cantilever = support.max_cantilever_dist;
        REQUIRE(std::any_of(result.overhangs.begin(), result.overhangs.end(), [](const auto& p) { return !p.empty(); }));
        REQUIRE(result.cantilever);
        REQUIRE(result.max_cantilever > scale_(3.));
        return result;
    };
    for (bool critical_only : {false, true}) {
        const auto reference = detect(1, critical_only, "serial");
        for (int repeat = 0; repeat < 2; ++repeat) {
            const auto parallel = detect(4, critical_only, repeat == 0 ? "parallel" : "validate");
            CHECK(parallel.overhangs == reference.overhangs);
            CHECK(parallel.cantilevers == reference.cantilevers);
            CHECK(parallel.types == reference.types);
            CHECK(parallel.sharp_tail == reference.sharp_tail);
            CHECK(parallel.cantilever == reference.cantilever);
            CHECK(parallel.max_cantilever == reference.max_cantilever);
        }
    }
    detect(4, false, "parallel", true);
}
