#include <catch2/catch_test_macros.hpp>
#include "material_band_fixtures.hpp"
#include "libslic3r/MaterialBand/Production.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include <boost/filesystem.hpp>
#include <tbb/task_arena.h>
#include <chrono>
#include <fstream>
#include <numeric>
#include <set>

using namespace Slic3r;
using namespace Slic3r::MaterialBand;
using Json = nlohmann::json;
namespace {
struct PaintedFront
{
    std::vector<Vec2d>                         vertices;
    std::vector<Vec3i32>                       faces;
    std::vector<int>                           labels;
    std::map<std::pair<int64_t, int64_t>, int> ids;
    int                                        vertex(Vec2d p)
    {
        const auto key = std::make_pair(std::llround(p.x() * 1e9), std::llround(p.y() * 1e9));
        const auto it  = ids.emplace(key, int(vertices.size()));
        if (it.second)
            vertices.push_back(p);
        return it.first->second;
    }
    void triangle(Vec2d a, Vec2d b, Vec2d c, int label)
    {
        if ((b - a).x() * (c - a).y() - (b - a).y() * (c - a).x() < 0.)
            std::swap(b, c);
        faces.emplace_back(vertex(a), vertex(b), vertex(c));
        labels.push_back(label);
    }
    void quad(double x0, double z0, double x1, double z1, int label)
    {
        triangle({x0, z0}, {x1, z0}, {x1, z1}, label);
        triangle({x0, z0}, {x1, z1}, {x0, z1}, label);
    }
    void refine()
    {
        const auto old_faces  = std::move(faces);
        const auto old_labels = std::move(labels);
        faces.clear();
        labels.clear();
        for (size_t i = 0; i < old_faces.size(); ++i) {
            const auto  f = old_faces[i];
            const Vec2d a = vertices[f[0]], b = vertices[f[1]], c = vertices[f[2]];
            const Vec2d ab = (a + b) / 2., bc = (b + c) / 2., ca = (c + a) / 2.;
            const int   label = old_labels[i];
            triangle(a, ab, ca, label);
            triangle(ab, b, bc, label);
            triangle(ca, bc, c, label);
            triangle(ab, bc, ca, label);
        }
    }
};

MaterialBandFixtures::Fixture make_stress(const Json& spec, Json& metrics)
{
    const size_t      count = spec.at("regions"), recipes = spec.at("recipes");
    const unsigned    physical = spec.at("physical");
    const std::string topology = spec.at("topology"), change = spec.at("height_change");
    PaintedFront      front;
    const auto        label = [&](size_t patch) { return int(1 + patch % recipes); };
    if (count == 0)
        front.quad(0., 0., 12., .48, 0);
    else if (topology == "stripes" || topology == "slivers") {
        const double pitch = 12. / count, width = topology == "slivers" ? .02 * pitch : .6 * pitch;
        for (size_t i = 0; i < count; ++i) {
            const double x = i * pitch, a = x + (pitch - width) / 2., b = a + width;
            front.quad(x, 0., a, .48, 0);
            front.quad(a, 0., b, .48, label(i));
            front.quad(b, 0., x + pitch, .48, 0);
        }
    } else if (topology == "islands") {
        const size_t cols = size_t(std::ceil(std::sqrt(double(count)))), rows = (count + cols - 1) / cols;
        for (size_t y = 0; y < rows * 3; ++y)
            for (size_t x = 0; x < cols * 3; ++x) {
                const size_t patch = (y / 3) * cols + x / 3;
                front.quad(12. * x / (cols * 3), .48 * y / (rows * 3), 12. * (x + 1) / (cols * 3), .48 * (y + 1) / (rows * 3),
                           x % 3 == 1 && y % 3 == 1 && patch < count ? label(patch) : 0);
            }
    } else if (topology == "nested") {
        const auto ring = [](double t) {
            return std::array<Vec2d, 4>{
                {{12. * t, .48 * t}, {12. * (1. - t), .48 * t}, {12. * (1. - t), .48 * (1. - t)}, {12. * t, .48 * (1. - t)}}};
        };
        for (size_t k = 0; k < 2 * count; ++k) {
            const auto a = ring(.45 * k / (2 * count)), b = ring(.45 * (k + 1) / (2 * count));
            for (size_t j = 0; j < 4; ++j) {
                front.triangle(a[j], a[(j + 1) % 4], b[(j + 1) % 4], k % 2 ? label(k / 2) : 0);
                front.triangle(a[j], b[(j + 1) % 4], b[j], k % 2 ? label(k / 2) : 0);
            }
        }
        front.quad(12. * .45, .48 * .45, 12. * .55, .48 * .55, 0);
    } else {
        // Radial sectors on the front rectangular face. Insert all corners,
        // so even low sector counts retain an exact box, not a bounding-box proxy.
        const size_t     sectors = topology == "multiway" ? count : 2 * count;
        std::set<double> angles;
        for (size_t i = 0; i < sectors; ++i)
            angles.insert(2. * PI * i / sectors);
        for (double a : {PI / 4., 3. * PI / 4., 5. * PI / 4., 7. * PI / 4.})
            angles.insert(a);
        const std::vector<double> values(angles.begin(), angles.end());
        const auto                point = [](double a) {
            const double x = std::cos(a), y = std::sin(a), d = std::max(std::abs(x), std::abs(y));
            return Vec2d(6. + 6. * x / d, .24 + .24 * y / d);
        };
        for (size_t i = 0; i < values.size(); ++i) {
            const double end    = i + 1 < values.size() ? values[i + 1] : 2. * PI;
            const size_t sector = std::min(sectors - 1, size_t((values[i] + end) / 2. * sectors / (2. * PI)));
            front.triangle({6., .24}, point(values[i]), point(end),
                           topology == "multiway" ? label(sector) :
                           sector % 2             ? label(sector / 2) :
                                                    0);
        }
    }
    // Refinement is conforming, exact on these planar faces, and independent of
    // recipe count. Height edits alter paint only, never the closed model solid.
    for (int i = 0; i < spec.at("resolution").get<int>(); ++i)
        front.refine();
    for (size_t i = 0; i < front.faces.size(); ++i)
        if (front.labels[i]) {
            const auto  f      = front.faces[i];
            const Vec2d center = (front.vertices[f[0]] + front.vertices[f[1]] + front.vertices[f[2]]) / 3.;
            if (change == "gradient")
                front.labels[i] = label(size_t(center.y() / .48 * recipes) % recipes);
            if (change == "birth_death" && (center.y() < .08 || center.y() > .4))
                front.labels[i] = 0;
            if (change == "component_count" && center.y() > .24 && center.x() > 5. && center.x() < 7.)
                front.labels[i] = 0;
        }
    std::map<std::pair<int, int>, std::vector<size_t>> edges;
    for (size_t i = 0; i < front.faces.size(); ++i)
        for (int j = 0; j < 3; ++j) {
            const int a = front.faces[i][j], b = front.faces[i][(j + 1) % 3];
            edges[{std::min(a, b), std::max(a, b)}].push_back(i);
        }
    std::vector<size_t> parent(front.faces.size());
    std::iota(parent.begin(), parent.end(), 0);
    const auto root = [&](size_t i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i         = parent[i];
        }
        return i;
    };
    for (const auto& entry : edges)
        if (entry.second.size() == 2) {
            const auto a = entry.second[0], b = entry.second[1];
            if (front.labels[a] && front.labels[a] == front.labels[b])
                parent[root(a)] = root(b);
        }
    std::set<size_t> components;
    for (size_t i = 0; i < front.faces.size(); ++i)
        if (front.labels[i])
            components.insert(root(i));
    metrics["connected_recipe_components"] = components.size();
    indexed_triangle_set mesh;
    const int            n = int(front.vertices.size());
    for (double y : {0., 8.})
        for (const auto& p : front.vertices)
            mesh.vertices.emplace_back(float(p.x()), float(y), float(p.y()));
    std::vector<int> channels;
    for (size_t i = 0; i < front.faces.size(); ++i) {
        mesh.indices.push_back(front.faces[i]);
        channels.push_back(front.labels[i]);
    }
    for (const auto& f : front.faces) {
        mesh.indices.emplace_back(n + f[2], n + f[1], n + f[0]);
        channels.push_back(0);
    }
    for (const auto& entry : edges)
        if (entry.second.size() == 1) {
            const auto& f = front.faces[entry.second[0]];
            for (int j = 0; j < 3; ++j) {
                const int a = f[j], b = f[(j + 1) % 3];
                if (std::min(a, b) != entry.first.first || std::max(a, b) != entry.first.second)
                    continue;
                mesh.indices.emplace_back(a, n + a, n + b);
                mesh.indices.emplace_back(a, n + b, b);
                channels.insert(channels.end(), 2, 0);
            }
        }
    // Independent oriented-edge and signed-volume validation before slicing.
    std::map<std::pair<int, int>, std::pair<int, int>> solid_edges;
    double                                             volume = 0.;
    for (const auto& f : mesh.indices) {
        const Vec3d a = mesh.vertices[f[0]].cast<double>(), b = mesh.vertices[f[1]].cast<double>(), c = mesh.vertices[f[2]].cast<double>();
        REQUIRE((b - a).cross(c - a).norm() > 1e-12);
        volume += a.dot(b.cross(c)) / 6.;
        for (int j = 0; j < 3; ++j) {
            const int x = f[j], y = f[(j + 1) % 3];
            auto&     e = solid_edges[{std::min(x, y), std::max(x, y)}];
            ++e.first;
            e.second += x < y ? 1 : -1;
        }
    }
    for (const auto& e : solid_edges) {
        REQUIRE(e.second.first == 2);
        REQUIRE(e.second.second == 0);
    }
    REQUIRE(volume == Approx(12. * 8. * .48).epsilon(1e-6));
    metrics["mesh_triangles"]          = mesh.indices.size();
    metrics["source_approximation_mm"] = 0.;
    std::set<double> x_stations;
    for (const auto& v : front.vertices)
        x_stations.insert(v.x());
    metrics["paint_candidate_upper_bound"] = 2 * front.faces.size() * x_stations.size();
    auto fixture                           = MaterialBandFixtures::make_fixture("MB01");
    MaterialBandFixtures::enable_material_bands(fixture);
    fixture.config.set_num_filaments(physical);
    fixture.config.set_num_extruders(physical);
    fixture.config.option<ConfigOptionFloats>("filament_diameter")->values.assign(physical, 1.75);
    auto& purge = fixture.config.option<ConfigOptionFloats>("flush_volumes_matrix")->values;
    purge.assign(physical * physical, 1.);
    for (size_t m = 0; m < physical; ++m)
        purge[m * physical + m] = 0.;
    std::vector<std::string> colors{"#FF0000", "#0000FF", "#00FF00", "#FFFFFF"};
    colors.resize(physical);
    fixture.config.option<ConfigOptionStrings>("filament_colour")->values = colors;
    std::vector<MixedFilamentDefinition> definitions(recipes + 1);
    for (size_t i = 0; i < definitions.size(); ++i) {
        auto& d                   = definitions[i];
        d.identity.stable_id      = 20001 + i;
        d.source.kind             = MixedFilamentSourceKind::Custom;
        const int a               = i == 0 ? 50 : 25 + int(i) - 1;
        d.recipe.blend.components = {{{1}, a}, {{2}, 100 - a}};
        d.behavior.distribution   = MixedFilamentDistributionMode::LayerCycle;
        d.behavior.layer_cadence  = {1, 1};
    }
    MixedFilamentManager manager;
    manager.set_mixed_filament_definitions(definitions, colors);
    fixture.config.set("mixed_filament_definitions", manager.serialize_custom_entries());
    auto* object = fixture.model.objects.front();
    object->delete_volume(0);
    object->config.set("extruder", int(physical + 1));
    auto* part = object->add_volume(TriangleMesh(std::move(mesh)), ModelVolumeType::MODEL_PART, false);
    part->config.set("extruder", int(physical + 1));
    TriangleSelector selector(part->mesh());
    for (size_t i = 0; i < channels.size(); ++i)
        if (channels[i])
            selector.set_facet(int(i), static_cast<EnforcerBlockerType>(physical + 1 + channels[i]));
    part->mmu_segmentation_facets.set(selector);
    const std::string process = spec.at("process"), state = spec.at("state");
    if (process == "arachne")
        fixture.config.set_deserialize_strict("wall_generator", "arachne");
    if (process == "narrow_bead")
        fixture.config.set_key_value("line_width", new ConfigOptionFloatOrPercent(.3, false));
    if (process == "thickness_bounds")
        fixture.config.set("mixed_filament_height_lower_bound", .05);
    if (state == "translation")
        object->instances.front()->set_offset(Vec3d(30., 25., 0.));
    if (state == "rotation")
        object->instances.front()->set_rotation(Z, PI / 2.);
    if (state == "copies")
        object->add_instance()->set_offset(Vec3d(100., 20., 0.));
    if (state == "insertion_order") {
        // Reversing triangle insertion preserves the exact geometry and paint.
        auto reversed = part->mesh().its;
        std::reverse(reversed.indices.begin(), reversed.indices.end());
        object->delete_volume(0);
        part = object->add_volume(TriangleMesh(std::move(reversed)), ModelVolumeType::MODEL_PART, false);
        part->config.set("extruder", int(physical + 1));
        TriangleSelector reversed_selector(part->mesh());
        std::reverse(channels.begin(), channels.end());
        for (size_t i = 0; i < channels.size(); ++i)
            if (channels[i])
                reversed_selector.set_facet(int(i), static_cast<EnforcerBlockerType>(physical + 1 + channels[i]));
        part->mmu_segmentation_facets.set(reversed_selector);
    }
    fixture.manifest = spec;
    if (physical == 2 && process != "arachne" && count) {
        if (state == "rotation")
            metrics["expected"] = "end_wall_paint_mismatch";
        else if (2 * front.faces.size() * x_stations.size() > 2000000)
            metrics["expected"] = "paint_domain_candidate_limit";
    }
    fixture.manifest["expected"] = metrics.at("expected");
    return fixture;
}

void save_bundle(const boost::filesystem::path& directory, MaterialBandFixtures::Fixture& fixture)
{
    boost::filesystem::create_directories(directory);
    std::ofstream(directory.string() + "/manifest.json") << fixture.manifest.dump(2) << '\n';
    Json profile;
    for (const auto& key : fixture.config.keys())
        profile[key] = fixture.config.opt_serialize(key);
    std::ofstream(directory.string() + "/profile.json") << profile.dump(2) << '\n';
    fixture.model.set_backup_path(directory.string());
    const std::string filename = (directory / "input.3mf").string();
    StoreParams       store;
    store.path     = filename.c_str();
    store.model    = &fixture.model;
    store.config   = &fixture.config;
    store.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipStatic | SaveStrategy::SkipAuxiliary;
    REQUIRE(store_bbs_3mf(store));
}
} // namespace

TEST_CASE("Material band fixed seeded paint and state stress corpus", "[material_band][stress]")
{
    std::ifstream stream(std::string(TEST_DATA_DIR) + "/material_band/stress.json");
    REQUIRE(stream.good());
    const auto manifest = Json::parse(stream);
    REQUIRE(manifest.at("cases").size() == 64);
    size_t accepted_count = 0;
    for (const auto& spec : manifest.at("cases")) {
        CAPTURE(spec.at("id"), spec.at("seed"));
        Json        metrics   = spec;
        auto        fixture   = make_stress(spec, metrics);
        const char* output    = std::getenv("MATERIAL_BAND_STRESS_DIR");
        const auto  directory = boost::filesystem::path(output ? output : boost::filesystem::temp_directory_path().string()) /
                               spec.at("id").get<std::string>();
        if (output)
            save_bundle(directory, fixture);
        Print print;
        print.apply(fixture.model, fixture.config);
        const auto started         = std::chrono::steady_clock::now();
        auto       result          = tbb::task_arena(spec.at("threads").get<int>()).execute([&] {
            return plan_production_print(print, {1, 2, 3, 4, 5, 6, 7});
        });
        metrics["wall_ms"]         = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        metrics["snapshot_ms"]     = result->snapshot_ms;
        metrics["paths_ms"]        = result->paths_ms;
        metrics["finalize_ms"]     = result->finalize_ms;
        metrics["engine_used"]     = result->used_material_bands() ? "material_bands" : "legacy";
        metrics["fallback"]        = result->fallback ? result->fallback->detail : "";
        metrics["geometry_reuses"] = result->geometry_reuses;
        if (output)
            std::ofstream((directory / "metrics.json").string()) << metrics.dump(2) << '\n';
        const std::string expected = fixture.manifest.at("expected");
        if (expected == "material_bands") {
            if (result->fallback)
                FAIL(result->fallback->detail);
            REQUIRE(result->plan);
            ++accepted_count;
            const double copies = spec.at("state") == "copies" ? 2. : 1.;
            for (size_t m = 0; m < MAX_MATERIALS; ++m)
                CHECK(result->plan->object_volume_mm3[m] == Approx(m < 2 ? 12. * 8. * .48 * .5 * copies : 0.).margin(.0001));
            if (spec.at("state") == "edit_revert") {
                auto*      instance = fixture.model.objects.front()->instances.front();
                const auto original = instance->get_offset();
                instance->set_offset(original + Vec3d(5., 0., 0.));
                print.apply(fixture.model, fixture.config);
                const auto edited = plan_production_print(print, {1, 3, 3, 4, 5, 6, 8});
                REQUIRE(edited->plan);
                instance->set_offset(original);
                print.apply(fixture.model, fixture.config);
                const auto reverted = plan_production_print(print, {1, 4, 3, 4, 5, 6, 9});
                REQUIRE(reverted->plan);
                REQUIRE(result->plan->operations.size() == reverted->plan->operations.size());
                for (size_t i = 0; i < result->plan->operations.size(); ++i)
                    CHECK(result->plan->operations[i].position == reverted->plan->operations[i].position);
            }
            if (output) {
                GCodeWriter   writer;
                std::ofstream code((directory / "output.gcode").string());
                emit_print(*result->plan, writer, [&](const std::string& s) { code << s; });
            }
        } else {
            REQUIRE(result->fallback);
            CHECK_FALSE(result->plan);
            CHECK(result->fallback->detail == expected);
        }
    }
    CHECK(accepted_count >= 3);
}

TEST_CASE("Material band dedicated benchmark capture", "[.][material_band][benchmark]")
{
    const char* input  = std::getenv("MATERIAL_BAND_BENCHMARK_INPUT");
    const char* output = std::getenv("MATERIAL_BAND_BENCHMARK_OUTPUT");
    REQUIRE(input);
    REQUIRE(output);
    std::ifstream     spec_stream(input);
    const auto        spec    = Json::parse(spec_stream);
    Json              metrics = spec;
    const std::string shape   = spec.at("shape");
    auto              fixture = shape == "stress" ?
                                    make_stress(spec, metrics) :
                                shape.compare(0, 2, "MB") == 0 ?
                                    MaterialBandFixtures::make_fixture(shape) :
                                    MaterialBandFixtures::make_axial_fixture(shape, spec.value("resolution", 64), spec.value("height", .48));
    if (shape != "stress")
        MaterialBandFixtures::enable_material_bands(fixture);
    if (spec.contains("infill_pattern")) {
        for (const char* key : {"internal_solid_infill_pattern", "top_surface_pattern", "bottom_surface_pattern"})
            fixture.config.set_deserialize_strict(key, spec.at("infill_pattern").get<std::string>());
        fixture.config.set("solid_infill_direction", spec.value("infill_angle", 0.));
    }
    if (shape == "MB02-transition" || shape == "MB03") {
        fixture.config.set_deserialize_strict("material_band_boundary_policy", "allow_transition");
        fixture.config.set_deserialize_strict("material_band_transition_width", "2");
        fixture.config.set_deserialize_strict("material_band_transition_error", "25%");
    }
    if (shape.compare(0, 2, "MB") == 0)
        fixture.model.objects.front()->volumes.front()->set_scaling_factor(Vec3d(1., 1., spec.value("height", .48) / 2.4));
    if (spec.contains("width"))
        fixture.config.set_key_value("line_width", new ConfigOptionFloatOrPercent(spec.at("width"), false));
    if (shape == "frustum") {
        fixture.config.set("layer_height", .12);
        fixture.config.set("initial_layer_print_height", .12);
    }
    if (spec.contains("copies"))
        for (size_t i = 1; i < spec.at("copies").get<size_t>(); ++i)
            fixture.model.objects.front()->add_instance()->set_offset(Vec3d(20. + 20. * i, 20., 0.));
    const auto directory = boost::filesystem::path(output).parent_path();
    save_bundle(directory, fixture);
    Print print;
    print.apply(fixture.model, fixture.config);
    const auto started         = std::chrono::steady_clock::now();
    const auto result          = tbb::task_arena(2).execute([&] { return plan_production_print(print, {1, 2, 3, 4, 5, 6, 7}); });
    metrics["wall_ms"]         = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    metrics["snapshot_ms"]     = result->snapshot_ms;
    metrics["paths_ms"]        = result->paths_ms;
    metrics["finalize_ms"]     = result->finalize_ms;
    metrics["geometry_reuses"] = result->geometry_reuses;
    metrics["fallback"]        = result->fallback ? result->fallback->detail : "";
    if (result->plan) {
        size_t cells = 0, segments = 0, faces = 0;
        for (const auto& object : result->plan->schedule->objects) {
            for (const auto& patch : object.paths->interfaces)
                faces += patch.surface.triangles.size();
            for (const auto& path : object.paths->paths) {
                segments += path.segments.size();
                for (const auto& segment : path.segments)
                    cells += segment.cells.size();
            }
        }
        metrics["cells"]           = cells;
        metrics["segments"]        = segments;
        metrics["interface_faces"] = faces;
        metrics["paths"]           = result->plan->schedule->order.size();
        metrics["dependencies"]    = result->plan->schedule->dependencies.size();
        metrics["candidates"]      = result->plan->schedule->candidate_pairs;
        metrics["index_visits"]    = result->plan->schedule->index_visits;
        metrics["tool_changes"]    = result->plan->transitions.size();
        metrics["logical_cycles"]  = result->plan->cycles.size();
        GCodeWriter   writer;
        size_t        bytes = 0;
        std::ofstream code((directory / "output.gcode").string());
        const auto    export_start = std::chrono::steady_clock::now();
        emit_print(*result->plan, writer, [&](const std::string& s) {
            code << s;
            bytes += s.size();
        });
        metrics["export_ms"]   = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - export_start).count();
        metrics["gcode_bytes"] = bytes;
        if (spec.value("edit_revert", false)) {
            tbb::task_arena(2).execute([&] { print.process(); });
            const auto cache       = print.material_band_result();
            const auto cache_start = std::chrono::steady_clock::now();
            tbb::task_arena(2).execute([&] { print.process(); });
            metrics["cache_hit_ms"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cache_start).count();
            REQUIRE(print.material_band_result() == cache);
            const std::string    original_recipes = fixture.config.opt_string("mixed_filament_definitions");
            const auto           colors           = fixture.config.option<ConfigOptionStrings>("filament_colour")->values;
            MixedFilamentManager manager;
            manager.auto_generate(colors);
            manager.load_custom_entries(original_recipes, colors);
            auto definitions = manager.mixed_filament_definitions(2);
            REQUIRE_FALSE(definitions.empty());
            definitions.front().recipe.blend.components = {{{1}, 26}, {{2}, 74}};
            manager.set_mixed_filament_definitions(definitions, colors);
            fixture.config.set("mixed_filament_definitions", manager.serialize_custom_entries());
            print.apply(fixture.model, fixture.config);
            const auto edit_start = std::chrono::steady_clock::now();
            tbb::task_arena(2).execute([&] { print.process(); });
            metrics["edit_ms"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - edit_start).count();
            REQUIRE(print.material_band_result()->plan);
            fixture.config.set("mixed_filament_definitions", original_recipes);
            print.apply(fixture.model, fixture.config);
            tbb::task_arena(2).execute([&] { print.process(); });
            const auto reverted = print.material_band_result();
            REQUIRE(reverted->plan);
            REQUIRE(cache->plan->operations.size() == reverted->plan->operations.size());
            for (size_t i = 0; i < cache->plan->operations.size(); ++i)
                CHECK(cache->plan->operations[i].position == reverted->plan->operations[i].position);
        }
    }
    std::ofstream(output) << metrics.dump(2) << '\n';
    if (spec.contains("expected"))
        CHECK(metrics["fallback"] == spec.at("expected"));
}
