#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/MaterialBand/Production.hpp"
#include "libslic3r/MaterialBand/ClippedPlanner.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/Tesselate.hpp"

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <tbb/task_arena.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <set>

using namespace Slic3r;

namespace {
double length_mm(const Polylines& lines)
{
    double result = 0.;
    for (const auto& line : lines)
        result += line.length() * SCALING_FACTOR;
    return result;
}

void svg_lines(std::ostream& out, const Polylines& lines, const char* color, double width)
{
    for (const auto& line : lines) {
        out << "<polyline fill='none' stroke='" << color << "' stroke-width='" << width << "' points='";
        for (const auto& p : line.points)
            out << p.x() * SCALING_FACTOR << ',' << -p.y() * SCALING_FACTOR << ' ';
        out << "'/>\n";
    }
}
} // namespace

// Opt-in local corpus runner. Source projects stay outside the repository and
// are never rewritten. The Python driver runs each project in a separate process
// with a deadline, retaining both failed runs and partial reports.
TEST_CASE("Material band local project geometry audit", "[.material_band_real_models]")
{
    const char* source = std::getenv("MATERIAL_BAND_REAL_MODEL");
    const char* output = std::getenv("MATERIAL_BAND_REAL_MODEL_OUTPUT");
    REQUIRE(source);
    REQUIRE(output);
    const boost::filesystem::path directory(output);
    boost::filesystem::create_directories(directory / "backup/Metadata/fullspectrum");
    boost::filesystem::create_directories(directory / "backup/3D/Objects");
    nlohmann::json report{{"source", source}, {"stage", "loading"}};
    const auto     save = [&] { std::ofstream((directory / "report.json").string()) << report.dump(2); };
    save();
    auto config = DynamicPrintConfig::full_print_config();
    // Match preset initialization: static enum-vector defaults do not carry
    // DynamicConfig's string lookup table, which JSON migration requires.
    for (const auto& key : config.keys()) {
        if (config.option(key)->type() != coEnums)
            continue;
        auto* initialized = config.def()->get(key)->create_default_option();
        initialized->set(config.option(key));
        config.set_key_value(key, initialized);
    }
    config.apply(PresetBundle{}.project_config);
    Model model;
    model.set_backup_path((directory / "backup").string());
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    PlateDataPtrs             plates;
    std::vector<Preset*>      presets;
    bool                      is_bbs = false;
    Semver                    version;
    report["stage"] = "importing_3mf";
    save();
    const auto progress = [&](int stage, int current, int total, bool&) {
        std::cerr << "Import " << stage << ' ' << current << '/' << total << std::endl;
    };
    const bool loaded = load_bbs_3mf(source, &config, &substitutions, &model, &plates, &presets, &is_bbs, &version, progress,
                                     LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence);
    release_PlateData_list(plates);
    for (auto* preset : presets)
        delete preset;
    REQUIRE(loaded);
    report["objects"]              = model.objects.size();
    report["triangles"]            = 0;
    report["paint_stream_bits"]    = 0;
    report["image_mapped_volumes"] = 0;
    for (const auto* object : model.objects)
        for (const auto* volume : object->volumes) {
            report["triangles"]            = report["triangles"].get<size_t>() + volume->mesh().its.indices.size();
            report["image_mapped_volumes"] = report["image_mapped_volumes"].get<size_t>() + size_t(volume->has_image_map_data());
            report["paint_stream_bits"]    = report["paint_stream_bits"].get<size_t>() +
                                          volume->mmu_segmentation_facets.get_data().bitstream.size();
        }
    for (const char* key :
         {"gcode_flavor", "layer_height", "wall_loops", "sparse_infill_density", "sparse_infill_pattern", "internal_solid_infill_pattern",
          "wall_generator", "filament_diameter", "enable_support", "brim_width", "brim_type", "print_sequence", "prime_volume",
          "enable_prime_tower", "initial_layer_print_height", "mixed_filament_height_lower_bound", "mixed_filament_height_upper_bound",
          "initial_layer_speed", "initial_layer_infill_speed"})
        if (const auto* option = config.option(key))
            report["settings"][key] = option->serialize();
    const char* engine = std::getenv("MATERIAL_BAND_REAL_MODEL_ENGINE");
    config.set_deserialize_strict("local_z_engine", engine ? engine : "material_bands");
    report["engine_requested"] = engine ? engine : "material_bands";
    Print print;
    print.apply(model, config);
    const bool plan_only = std::getenv("MATERIAL_BAND_REAL_MODEL_PLAN_ONLY") != nullptr;
    try {
        report["stage"] = "planning_material_bands";
        save();
        if (!plan_only) {
            const auto begin = std::chrono::steady_clock::now();
            tbb::task_arena(2).execute([&] { print.process(); });
            report["slice_seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        }
        auto production = print.material_band_result();
        if (!production)
            production = MaterialBand::plan_production_print(print, {1, 2, 3, 4, 5, 6, 7});
        report["material_band_accepted"] = production->used_material_bands();
        report["unsupported_settings"]   = production->unsupported_settings;
        report["planning_ms"]            = {{"snapshot", production->snapshot_ms},
                                            {"paths", production->paths_ms},
                                            {"finalize", production->finalize_ms}};
        if (production->plan) {
            report["cycles"]            = production->plan->cycles.size();
            report["operations"]        = production->plan->operations.size();
            report["object_volume_mm3"] = production->plan->object_volume_mm3;
            report["brim_volume_mm3"]   = production->plan->brim_volume_mm3;
            report["purge_volume_mm3"]  = production->plan->purge_volume_mm3;
            for (const auto& object : production->plan->schedule->objects) {
                if (object.object == std::numeric_limits<uint64_t>::max())
                    continue;
                for (size_t material = 0; material < MaterialBand::MAX_MATERIALS; ++material) {
                    const auto& coverage = object.paths->coverage[material];
                    report["path_coverage"].push_back({{"object", object.object},
                                                       {"material", material + 1},
                                                       {"target_mm3", coverage.target_mm3},
                                                       {"deposited_mm3", coverage.deposited_mm3},
                                                       {"missing_mm3", coverage.missing_mm3},
                                                       {"excess_mm3", coverage.excess_mm3}});
                }
            }
        }
        if (production->fallback) {
            report["fallback"]  = production->fallback->detail;
            const auto& failure = *production->fallback;
            if (failure.band)
                report["failure_band"] = {{"domain", failure.band->domain},
                                          {"cycle", failure.band->macro_cycle},
                                          {"material", failure.band->material.value}};
            if (failure.witness_mm)
                report["failure_witness"] = {failure.witness_mm->x(), failure.witness_mm->y(), failure.witness_mm->z()};
        }
        report["stage"] = "loaded";
        save();
        if (plan_only)
            return;
        report["stage"] = "auditing";
        save();
        size_t     total_layers = 0, total_paths = 0, error_layers = 0, holes = 0;
        uint64_t   geometry_hash         = 14695981039346656037ull;
        uint64_t   ordered_geometry_hash = 14695981039346656037ull;
        const auto hash_value            = [&](uint64_t value) {
            for (unsigned byte = 0; byte < 8; ++byte) {
                geometry_hash ^= (value >> (8 * byte)) & 255;
                geometry_hash *= 1099511628211ull;
            }
        };
        double perimeter_length = 0., fill_length = 0., outside_perimeter = 0., outside_fill = 0.;
        if (const auto accepted = print.material_band_result(); accepted && accepted->used_material_bands()) {
            report["geometry_source"]   = "canonical_material_band_paths";
            report["object_volume_mm3"] = accepted->plan->object_volume_mm3;
            report["brim_volume_mm3"]   = accepted->plan->brim_volume_mm3;
            report["purge_volume_mm3"]  = accepted->plan->purge_volume_mm3;
            size_t sparse_segments      = 0;
            double sparse_length = 0., intentional_void = 0.;
            for (const auto& scheduled : accepted->plan->schedule->objects) {
                const PrintObject* source_object = nullptr;
                Transform3d        matrix        = Transform3d::Identity();
                for (const auto* candidate : print.objects())
                    for (const auto& instance : candidate->instances())
                        if (instance.model_instance->id().id == scheduled.object) {
                            source_object = candidate;
                            matrix        = instance.model_instance->get_matrix();
                        }
                if (!source_object)
                    continue; // Auxiliary tower has its own validated footprint.
                REQUIRE(source_object->model_object()->volumes.size() == 1);
                const auto*  volume = source_object->model_object()->volumes.front();
                TriangleMesh mesh   = volume->mesh();
                matrix.translation() -= print.get_plate_origin();
                mesh.transform(matrix * volume->get_matrix(), true);
                struct Section
                {
                    double    bottom = 1e30, top = -1e30;
                    Polylines walls, fills, brims;
                };
                std::map<uint64_t, Section> sections;
                intentional_void += scheduled.paths->intentional_void_mm3;
                for (const auto& path : scheduled.paths->paths) {
                    auto&    section = sections[path.band.macro_cycle];
                    Polyline line;
                    for (const auto& p : path.vertices)
                        line.points.emplace_back(scale_(p.x()), scale_(p.y()));
                    if (path.role == MaterialBand::PathRole::Brim) {
                        section.brims.push_back(std::move(line));
                        continue;
                    }
                    for (const auto& segment : path.segments)
                        for (const auto& cell : segment.cells)
                            for (size_t i = 0; i < 3; ++i) {
                                section.bottom = std::min(section.bottom, cell.lower_z[i]);
                                section.top    = std::max(section.top, cell.upper[i].z());
                            }
                    if (path.role == MaterialBand::PathRole::SparseFill) {
                        sparse_length += line.length() * SCALING_FACTOR;
                        sparse_segments += path.segments.size();
                    }
                    (path.role == MaterialBand::PathRole::Perimeter ? section.walls : section.fills).push_back(std::move(line));
                }
                std::vector<float> heights;
                for (const auto& entry : sections)
                    heights.push_back(float((entry.second.bottom + entry.second.top) / 2.));
                MeshSlicingParamsEx params;
                params.resolution   = .005;
                const auto outlines = slice_mesh_ex(mesh.its, heights, params, [] {});
                size_t     index    = 0;
                for (const auto& entry : sections) {
                    const auto&  section = entry.second;
                    const auto&  outline = outlines[index];
                    const auto   allowed = offset_ex(outline, float(scale_(.06)));
                    const double wl = length_mm(section.walls), fl = length_mm(section.fills);
                    const double wo = length_mm(diff_pl(section.walls, allowed)), fo = length_mm(diff_pl(section.fills, allowed));
                    ++total_layers;
                    total_paths += section.walls.size() + section.fills.size();
                    for (const auto& polygon : outline)
                        holes += polygon.holes.size();
                    perimeter_length += wl;
                    fill_length += fl;
                    outside_perimeter += wo;
                    outside_fill += fo;
                    if (index == 0 || index == sections.size() / 4 || index == sections.size() / 2 || index == 3 * sections.size() / 4 ||
                        index + 1 == sections.size()) {
                        report["samples"].push_back({{"object", scheduled.object},
                                                     {"cycle", entry.first},
                                                     {"bottom", section.bottom},
                                                     {"top", section.top},
                                                     {"islands", outline.size()},
                                                     {"wall_mm", wl},
                                                     {"fill_mm", fl},
                                                     {"outside_wall_mm", wo},
                                                     {"outside_fill_mm", fo}});
                        Points bounds_points = to_points(outline);
                        for (const auto& brim : section.brims)
                            bounds_points.insert(bounds_points.end(), brim.points.begin(), brim.points.end());
                        const BoundingBox bounds(bounds_points);
                        const Vec2d       lo = bounds.min.cast<double>() * SCALING_FACTOR, hi = bounds.max.cast<double>() * SCALING_FACTOR;
                        std::ofstream     svg(
                            (directory / ("kernel-" + std::to_string(scheduled.object) + "-cycle-" + std::to_string(entry.first) + ".svg"))
                                .string());
                        svg << "<svg xmlns='http://www.w3.org/2000/svg' width='1000' height='1000' viewBox='" << lo.x() - 1 << ' '
                            << -hi.y() - 1 << ' ' << hi.x() - lo.x() + 2 << ' ' << hi.y() - lo.y() + 2 << "'>\n";
                        Polylines boundaries;
                        for (const auto& polygon : to_polygons(outline))
                            boundaries.push_back(polygon.split_at_first_point());
                        svg_lines(svg, boundaries, "#a0a0a0", .06);
                        svg_lines(svg, section.brims, "#36a557", .1);
                        svg_lines(svg, section.fills, "#d08025", .1);
                        svg_lines(svg, section.walls, "#2255a0", .13);
                        svg << "</svg>\n";
                    }
                    ++index;
                }
            }
            report["sparse_segments"]      = sparse_segments;
            report["sparse_infill_mm"]     = sparse_length;
            report["intentional_void_mm3"] = intentional_void;
        }
        for (size_t oi = 0; oi < print.objects().size(); ++oi) {
            const auto& layers = print.objects()[oi]->layers();
            if (layers.empty())
                continue;
            const std::set<size_t> samples{0, layers.size() / 4, layers.size() / 2, 3 * layers.size() / 4, layers.size() - 1};
            for (size_t li = 0; li < layers.size(); ++li) {
                const auto* layer = layers[li];
                ++total_layers;
                error_layers += layer->slicing_errors;
                for (const auto& polygon : layer->lslices)
                    holes += polygon.holes.size();
                Polylines walls, fills;
                for (const auto* region : layer->regions()) {
                    region->perimeters.collect_polylines(walls);
                    region->fills.collect_polylines(fills);
                }
                total_paths += walls.size() + fills.size();
                hash_value(oi);
                hash_value(li);
                hash_value(uint64_t(std::llround(layer->print_z / SCALING_FACTOR)));
                for (const auto* paths : {&walls, &fills}) {
                    std::vector<std::array<int64_t, 4>> segments;
                    for (const auto& line : *paths) {
                        for (const auto& point : line.points)
                            for (uint64_t value : {uint64_t(point.x()), uint64_t(point.y())})
                                for (unsigned byte = 0; byte < 8; ++byte) {
                                    ordered_geometry_hash ^= (value >> (8 * byte)) & 255;
                                    ordered_geometry_hash *= 1099511628211ull;
                                }
                        for (size_t i = 1; i < line.points.size(); ++i) {
                            Point a = line.points[i - 1], b = line.points[i];
                            if (std::tie(a.x(), a.y()) > std::tie(b.x(), b.y()))
                                std::swap(a, b);
                            segments.push_back({a.x(), a.y(), b.x(), b.y()});
                        }
                    }
                    std::sort(segments.begin(), segments.end());
                    hash_value(segments.size());
                    for (const auto& segment : segments)
                        for (int64_t value : segment)
                            hash_value(uint64_t(value));
                }
                const double wl = length_mm(walls), fl = length_mm(fills);
                perimeter_length += wl;
                fill_length += fl;
                // Check entire segments, including crossings over cavities. The
                // 0.06 mm tolerance accommodates the configured path resolution;
                // it is recorded rather than silently treating errors as passes.
                const auto   allowed = offset_ex(layer->lslices, float(scale_(.06)));
                const double wo = length_mm(diff_pl(walls, allowed)), fo = length_mm(diff_pl(fills, allowed));
                outside_perimeter += wo;
                outside_fill += fo;
                if (samples.count(li)) {
                    report["samples"].push_back({{"object", oi},
                                                 {"layer", li},
                                                 {"z", layer->print_z},
                                                 {"islands", layer->lslices.size()},
                                                 {"wall_mm", wl},
                                                 {"fill_mm", fl},
                                                 {"outside_wall_mm", wo},
                                                 {"outside_fill_mm", fo}});
                    const auto points = to_points(layer->lslices);
                    if (points.empty())
                        continue;
                    const BoundingBox bounds(points);
                    const Vec2d       lo   = bounds.min.cast<double>() * SCALING_FACTOR;
                    const Vec2d       hi   = bounds.max.cast<double>() * SCALING_FACTOR;
                    const std::string name = "object-" + std::to_string(oi) + "-layer-" + std::to_string(li) + ".svg";
                    std::ofstream     svg((directory / name).string());
                    svg << "<svg xmlns='http://www.w3.org/2000/svg' width='1000' height='1000' viewBox='" << lo.x() - 1 << ' '
                        << -hi.y() - 1 << ' ' << hi.x() - lo.x() + 2 << ' ' << hi.y() - lo.y() + 2 << "'>\n";
                    Polylines boundaries;
                    for (const auto& polygon : to_polygons(layer->lslices))
                        boundaries.push_back(polygon.split_at_first_point());
                    svg_lines(svg, boundaries, "#a0a0a0", .06);
                    svg_lines(svg, fills, "#dd7a16", .13);
                    svg_lines(svg, walls, "#2255a0", .13);
                    svg << "</svg>\n";
                }
            }
        }
        report["geometry"] = {{"layers", total_layers},
                              {"paths", total_paths},
                              {"slicing_error_layers", error_layers},
                              {"hole_sections", holes},
                              {"perimeter_mm", perimeter_length},
                              {"infill_mm", fill_length},
                              {"outside_perimeter_mm", outside_perimeter},
                              {"outside_infill_mm", outside_fill},
                              {"centerline_tolerance_mm", .06},
                              {"sorted_segment_fnv1a64", std::to_string(geometry_hash)},
                              {"ordered_point_fnv1a64", std::to_string(ordered_geometry_hash)}};
        if (std::getenv("MATERIAL_BAND_REAL_MODEL_EXPORT")) {
            report["stage"] = "exporting";
            save();
            GCodeProcessorResult parsed;
            tbb::task_arena(2).execute([&] { print.export_gcode((directory / "slice.gcode").string(), &parsed); });
            size_t extrusions = 0, invalid = 0;
            double deposited = 0.;
            for (const auto& move : parsed.moves) {
                invalid += !move.position.allFinite() || !std::isfinite(move.delta_extruder) || !std::isfinite(move.feedrate);
                if (move.type != EMoveType::Extrude)
                    continue;
                ++extrusions;
                invalid += move.width <= 0. || move.height <= 0. || move.mm3_per_mm <= 0.;
                deposited += std::max(0.f, move.delta_extruder);
            }
            report["gcode"] = {{"moves", parsed.moves.size()},
                               {"extrusions", extrusions},
                               {"invalid_moves", invalid},
                               {"positive_filament_mm", deposited},
                               {"material_band_cycles", parsed.material_band_cycles.size()}};
            CHECK(extrusions > 0);
            CHECK(invalid == 0);
        }
        report["stage"] = "complete";
        save();
        CHECK(total_layers > 0);
        CHECK(total_paths > 0);
        CHECK(error_layers == 0);
        CHECK(outside_perimeter < 1e-6);
        CHECK(outside_fill < 1e-6);
    } catch (const std::exception& error) {
        report["error"] = error.what();
        save();
        FAIL(error.what());
    }
}

namespace {
void check_captured_schedule(const char* source, bool verify_fixed_order = false)
{
    using namespace MaterialBand;
    REQUIRE(source);
    std::ifstream stream(source);
    REQUIRE(stream.good());
    nlohmann::json data;
    stream >> data;
    const auto  vector     = [](const nlohmann::json& p) { return Vec3d(p[0].get<double>(), p[1].get<double>(), p[2].get<double>()); };
    auto        plan       = std::make_shared<PathPlan>();
    auto&       m          = plan->machine;
    const auto& machine    = data["machine"];
    m.material_count       = machine[0];
    m.clearance_mm         = machine[1];
    m.travel_lift_mm       = machine[2];
    m.max_z_mm_s           = machine[3];
    m.max_volume_mm3_s     = machine[4];
    m.max_segment_mm       = machine[5];
    m.deposition_mm_s      = machine[6];
    m.shared_nozzle        = machine[7].get<int>() != 0;
    m.coplanar_tip_contact = machine[8].get<int>() != 0;
    for (size_t i = 0; i < MAX_MATERIALS; ++i)
        m.filament_diameter_mm[i] = data["diameters"][i];
    for (const auto& s : data["sections"])
        m.sections.push_back({s[0], s[1], s[2]});
    plan->solid_min       = vector(data["minimum"]);
    plan->solid_max       = vector(data["maximum"]);
    plan->layered_process = plan->ordered_region_paths = true;
    for (const auto& item : data["paths"]) {
        BandPath    p;
        const auto& band = item["band"];
        p.band           = {band[0], band[1], {band[2]}, band[3]};
        p.role           = PathRole(item["role"].get<int>());
        p.overhang_mm    = item["overhang"];
        p.bridge_span_mm = item["bridge"];
        p.split_reason   = item["reason"];
        for (const auto& point : item["vertices"])
            p.vertices.push_back(vector(point));
        for (const auto& s : item["segments"]) {
            PathSegment segment;
            const auto& values      = s["values"];
            segment.volume_mm3      = values[0];
            segment.speed_mm_s      = values[1];
            segment.width_mm        = values[2];
            segment.height_start_mm = values[3];
            segment.height_end_mm   = values[4];
            for (const auto& c : s["cells"]) {
                BeadCell cell;
                for (size_t i = 0; i < 3; ++i) {
                    cell.upper[i]   = Vec3d(c[i * 4].get<double>(), c[i * 4 + 1].get<double>(), c[i * 4 + 2].get<double>());
                    cell.lower_z[i] = c[i * 4 + 3];
                }
                segment.cells.push_back(cell);
            }
            p.segments.push_back(std::move(segment));
        }
        plan->paths.push_back(std::move(p));
    }
    std::stable_sort(plan->paths.begin(), plan->paths.end(), [](const BandPath& a, const BandPath& b) {
        return std::make_tuple(a.band.macro_cycle, std::llround(a.vertices.front().z() * 1e8), a.band.material.value, a.band.domain) <
               std::make_tuple(b.band.macro_cycle, std::llround(b.vertices.front().z() * 1e8), b.band.material.value, b.band.domain);
    });
    if (std::getenv("MATERIAL_BAND_REPLAY_FIT_CAPS")) {
        ExPolygons preceding;
        for (size_t first = 0; first < plan->paths.size();) {
            size_t last = first + 1;
            while (last < plan->paths.size() && plan->paths[last].band.macro_cycle == plan->paths[first].band.macro_cycle)
                ++last;
            PathPlan cycle;
            cycle.machine = m;
            cycle.paths.assign(plan->paths.begin() + first, plan->paths.begin() + last);
            double bottom = std::numeric_limits<double>::max(), top = -bottom;
            for (const auto& path : cycle.paths)
                for (const auto& segment : path.segments) {
                    cycle.volume_mm3[path.band.stack_slot] += segment.volume_mm3;
                    for (const auto& cell : segment.cells)
                        for (size_t i = 0; i < 3; ++i) {
                            bottom = std::min(bottom, cell.lower_z[i]);
                            top    = std::max(top, cell.upper[i].z());
                        }
                }
            auto fitted = fit_planar_overhang_caps(cycle, preceding, bottom);
            REQUIRE(std::holds_alternative<std::shared_ptr<const PathPlan>>(fitted));
            const auto& completed = *std::get<std::shared_ptr<const PathPlan>>(fitted);
            std::copy(completed.paths.begin(), completed.paths.end(), plan->paths.begin() + first);
            Polygons pieces;
            for (const auto& path : completed.paths)
                for (const auto& segment : path.segments)
                    for (const auto& cell : segment.cells) {
                        if (std::abs(cell.upper.front().z() - top) > 1e-10)
                            continue;
                        Points points;
                        for (const auto& p : cell.upper)
                            points.emplace_back(scale_(p.x()), scale_(p.y()));
                        pieces.emplace_back(std::move(points));
                    }
            preceding = union_ex(pieces);
            first     = last;
        }
    }
    for (size_t i = 0; i < plan->paths.size(); ++i)
        plan->paths[i].id = i + 1;
    ScheduleInput input;
    input.machine                 = m;
    input.objects                 = {{1, plan}};
    input.initial_position        = Vec3d(.5, 1., plan->solid_max.z() + 2.);
    input.allow_planar_reordering = data.value("allow_planar_reordering", false);
    if (data.contains("occupied"))
        for (const auto& c : data["occupied"]) {
            BeadCell cell;
            for (size_t i = 0; i < 3; ++i) {
                cell.upper[i]   = Vec3d(c[i * 4].get<double>(), c[i * 4 + 1].get<double>(), c[i * 4 + 2].get<double>());
                cell.lower_z[i] = c[i * 4 + 3];
            }
            input.occupied.push_back(cell);
        }
    ScheduleLimits limits;
    limits.max_paths           = 50000;
    limits.max_cells           = 500000;
    limits.max_candidates      = 100000000;
    limits.max_index_visits    = 500000000;
    limits.max_temporary_bytes = size_t(1024) * 1024 * 1024;
    if (data.value("diagnostic_full", false)) {
        limits.max_paths           = 500000;
        limits.max_cells           = 4000000;
        limits.max_index_nodes     = 16000000;
        limits.max_edges           = 4000000;
        limits.max_index_visits    = 2000000000;
        limits.max_temporary_bytes = size_t(3) * 1024 * 1024 * 1024;
    }
    if (data.contains("finalize_profile")) {
        // Diagnostic replay of captured geometry, separate from qualification
        // of a complete original project through the production adapter.
        const auto&     setup = data.at("finalize_profile");
        FinalizeProfile profile;
        profile.adapter                         = MachineAdapter::SnapmakerU1;
        profile.home                            = false;
        profile.process.macro_height_mm         = setup.at("macro_height_mm");
        profile.process.bead_width_mm           = setup.at("bead_width_mm");
        profile.process.min_active_thickness_mm = setup.at("minimum_mm");
        profile.process.max_active_thickness_mm = setup.at("maximum_mm");
        profile.bed_min                         = Vec2d(.5, 1.);
        profile.bed_max                         = Vec2d(270.5, 271.);
        profile.max_print_height_mm             = 270.05;
        profile.purge.enabled                   = true;
        profile.purge.origin                    = Vec2d(setup.at("tower_x").get<double>(), setup.at("tower_y").get<double>());
        profile.purge.width_mm                  = setup.at("tower_width_mm");
        profile.purge.prime_mm3.fill(setup.at("prime_mm3").get<double>());
        const double step      = std::min(profile.process.macro_height_mm, profile.process.max_active_thickness_mm);
        input.initial_position = Vec3d(5., 5., std::ceil(plan->solid_max.z() / step - 1e-8) * step + 2.);
        FinalizeLimits final_limits;
        final_limits.schedule                  = limits;
        final_limits.paths.max_paths           = 500000;
        final_limits.paths.max_segments        = 2000000;
        final_limits.paths.max_temporary_bytes = size_t(1024) * 1024 * 1024;
        final_limits.max_operations            = 4000000;
        final_limits.max_temporary_bytes       = size_t(2) * 1024 * 1024 * 1024;
        const auto final                       = finalize_print(input, profile, final_limits);
        if (const auto* failure = std::get_if<Failure>(&final))
            FAIL(failure->detail);
        REQUIRE(std::holds_alternative<std::shared_ptr<const FinalPrintPlan>>(final));
        const auto& complete = *std::get<std::shared_ptr<const FinalPrintPlan>>(final);
        std::cout << "Captured final plan paths=" << complete.schedule->order.size()
                  << " dependencies=" << complete.schedule->dependencies.size() << " index_visits=" << complete.schedule->index_visits
                  << " operations=" << complete.operations.size() << std::endl;
        return;
    }
    const auto result = schedule_paths(input, limits);
    if (const auto* failure = std::get_if<Failure>(&result))
        FAIL(failure->detail);
    REQUIRE(std::holds_alternative<std::shared_ptr<const Schedule>>(result));
    if (verify_fixed_order) {
        const auto&           schedule = *std::get<std::shared_ptr<const Schedule>>(result);
        std::vector<BandPath> ordered;
        for (const auto& item : schedule.order) {
            ordered.push_back(plan->paths[item.operation.path - 1]);
            ordered.back().id = ordered.size();
        }
        plan->paths                   = std::move(ordered);
        input.allow_planar_reordering = false;
        const auto fixed              = schedule_paths(input, limits);
        if (const auto* failure = std::get_if<Failure>(&fixed))
            FAIL(failure->detail);
        REQUIRE(std::holds_alternative<std::shared_ptr<const Schedule>>(fixed));
    }
}
} // namespace

TEST_CASE("Material band captured cycle scheduling replay", "[.material_band_schedule_replay]")
{
    check_captured_schedule(std::getenv("MATERIAL_BAND_SCHEDULE_REPLAY"));
}

TEST_CASE("Material Benchy curved hull retains grounded inner-wall support", "[material_band][layer_process][schedule]")
{
    const std::string fixture = std::string(TEST_DATA_DIR) + "/material_band/" +
                                GENERATE("benchy-wall-46.json", "benchy-wall-57.json", "benchy-wall-61.json");
    check_captured_schedule(fixture.c_str(), true);
}

TEST_CASE("Material expanded support retains narrow actual floor caps", "[material_band][layer_process][schedule]")
{
    const std::string fixture = std::string(TEST_DATA_DIR) + "/material_band/benchy-gyroid-floor-52.json";
    check_captured_schedule(fixture.c_str());
}

TEST_CASE("Material painted cap tolerance cannot retain a detached neighboring deposit", "[material_band][layer_process][clipped_paths]")
{
    using namespace MaterialBand;
    PathPlan geometry;
    geometry.layered_process              = true;
    geometry.solid_min                    = Vec3d::Zero();
    geometry.solid_max                    = Vec3d(2., .5, .2);
    geometry.machine.sections             = {{0., 140., 72.5}};
    geometry.machine.coplanar_tip_contact = true;
    geometry.machine.max_segment_mm       = 3.;
    BandPath source;
    source.id       = 1;
    source.band     = {1, 0, {1}, 0};
    source.vertices = {Vec3d(0., .2, .2), Vec3d(2., .2, .2)};
    PathSegment segment;
    segment.width_mm        = .4;
    segment.height_start_mm = segment.height_end_mm = .2;
    segment.speed_mm_s                              = 10.;
    segment.coverage_triangles                      = {{{Vec2d(0., 0.), Vec2d(2., 0.), Vec2d(2., .4)}},
                                                       {{Vec2d(0., 0.), Vec2d(2., .4), Vec2d(0., .4)}},
                                                       {{Vec2d(.1, .41), Vec2d(.1002, .41), Vec2d(.1, .413)}}};
    for (const auto& triangle : segment.coverage_triangles) {
        const Vec2d a = triangle[1] - triangle[0], b = triangle[2] - triangle[0];
        segment.volume_mm3 += .5 * (a.x() * b.y() - a.y() * b.x()) * .2;
        BeadCell cell;
        for (size_t i = 0; i < 3; ++i) {
            cell.upper[i]   = Vec3d(triangle[i].x(), triangle[i].y(), .2);
            cell.lower_z[i] = 0.;
        }
        segment.cells.push_back(cell);
    }
    source.segments.push_back(segment);
    geometry.paths.push_back(source);
    PlanarMaterialBand band;
    band.id        = {1, 0, {1}, 0};
    band.top_mm    = .2;
    band.footprint = {
        ExPolygon(Polygon({Point::new_scale(0., 0.), Point::new_scale(2., 0.), Point::new_scale(2., .4), Point::new_scale(0., .4)}))};
    const auto result = plan_partitioned_cycle(geometry, {band}, {}, {}, {});
    if (const auto* failure = std::get_if<Failure>(&result))
        FAIL(failure->detail);
    const auto& plan = *std::get<std::shared_ptr<const PathPlan>>(result);
    REQUIRE_FALSE(plan.paths.empty());
    double outside = 0.;
    for (const auto& path : plan.paths)
        for (const auto& s : path.segments)
            for (const auto& cell : s.cells) {
                Points points;
                for (const auto& p : cell.upper)
                    points.emplace_back(scale_(p.x()), scale_(p.y()));
                for (const auto& piece : diff_ex(Polygons{Polygon(points)}, band.footprint))
                    outside += piece.area() * SCALING_FACTOR * SCALING_FACTOR;
            }
    CHECK(outside < 1e-12);
    CHECK(plan.volume_mm3[0] == Approx(.16));
}

TEST_CASE("Material Benchy wall seams start on preceding material", "[material_band][layer_process][schedule]")
{
    using namespace MaterialBand;
    const std::string fixture = GENERATE("benchy-seam-182.json", "benchy-seam-28.json", "benchy-seam-39.json");
    CAPTURE(fixture);
    std::ifstream input(std::string(TEST_DATA_DIR) + "/material_band/" + fixture);
    REQUIRE(input.good());
    nlohmann::json data;
    input >> data;
    const bool relocate = GENERATE(false, true);
    CAPTURE(relocate);
    MachineEnvelope machine;
    machine.material_count       = data["machine"][0];
    machine.clearance_mm         = data["machine"][1];
    machine.travel_lift_mm       = data["machine"][2];
    machine.max_z_mm_s           = data["machine"][3];
    machine.max_volume_mm3_s     = data["machine"][4];
    machine.max_segment_mm       = data["machine"][5];
    machine.deposition_mm_s      = data["machine"][6];
    machine.shared_nozzle        = data["machine"][7].get<int>() != 0;
    machine.coplanar_tip_contact = data["machine"][8].get<int>() != 0;
    for (const auto& section : data["sections"])
        machine.sections.push_back({section[0], section[1], section[2]});
    const auto triangle = [](const nlohmann::json& cell) {
        Points points;
        for (size_t i = 0; i < 3; ++i)
            points.emplace_back(scale_(cell[i * 4].get<double>()), scale_(cell[i * 4 + 1].get<double>()));
        return Polygon(std::move(points));
    };
    Polygons      footprint, floor_faces;
    ScheduleInput scheduling;
    scheduling.machine                 = machine;
    scheduling.allow_planar_reordering = true;
    for (const auto& source : data["occupied"]) {
        floor_faces.push_back(triangle(source));
        BeadCell cell;
        for (size_t i = 0; i < 3; ++i) {
            cell.upper[i]   = Vec3d(source[i * 4], source[i * 4 + 1], source[i * 4 + 2]);
            cell.lower_z[i] = source[i * 4 + 3];
        }
        scheduling.occupied.push_back(cell);
    }
    std::vector<ContourRun> runs;
    for (const auto& path : data["paths"]) {
        Points points;
        for (const auto& vertex : path["vertices"])
            points.emplace_back(scale_(vertex[0].get<double>()), scale_(vertex[1].get<double>()));
        if (points.front() != points.back())
            points.push_back(points.front());
        runs.push_back({Polyline(std::move(points)), .42, PathRole::Perimeter});
        for (const auto& segment : path["segments"])
            for (const auto& cell : segment["cells"])
                footprint.push_back(triangle(cell));
    }
    const auto preceding = union_ex(floor_faces);
    if (relocate) {
        auto original = runs;
        CHECK(anchor_perimeter_seams(runs, preceding) > 0);
        for (size_t i = 0; i < runs.size(); ++i) {
            CHECK(runs[i].line.points.front() == runs[i].line.points.back());
            auto before = original[i].line.points, after = runs[i].line.points;
            before.pop_back();
            after.pop_back();
            const auto less = [](const Point& a, const Point& b) { return std::tie(a.x(), a.y()) < std::tie(b.x(), b.y()); };
            std::sort(before.begin(), before.end(), less);
            std::sort(after.begin(), after.end(), less);
            CHECK(before == after);
        }
        CHECK(anchor_perimeter_seams(runs, preceding) == 0);
    }
    ProcessConstraints process;
    process.bead_width_mm           = .42;
    process.min_active_thickness_mm = .06;
    const double top                = data["paths"][0]["vertices"][0][2];
    const double bottom             = data["paths"][0]["segments"][0]["cells"][0][3];
    const auto   outline            = union_ex(footprint);
    auto         result             = plan_contour_cycle(outline, runs, {2046, 182, {1}, 0}, bottom, top, {}, process, {}, machine);
    if (const auto* failure = std::get_if<Failure>(&result))
        FAIL(failure->detail);
    auto   plan     = std::make_shared<PathPlan>(*std::get<std::shared_ptr<const PathPlan>>(result));
    double intended = 0.;
    for (const auto& polygon : outline)
        intended += polygon.area() * SCALING_FACTOR * SCALING_FACTOR * (top - bottom);
    CHECK(plan->volume_mm3[0] >= intended * .99);
    CHECK(plan->volume_mm3[0] <= intended * 1.0001 + 1e-6);
    for (auto& path : plan->paths) {
        path.overhang_mm    = .21;
        path.bridge_span_mm = 20.;
    }
    scheduling.objects          = {{1, plan}};
    scheduling.initial_position = Vec3d(.5, 1., top + 2.);
    ScheduleLimits limits;
    limits.max_candidates   = 100000000;
    limits.max_index_visits = 500000000;
    const auto scheduled    = schedule_paths(scheduling, limits);
    if (relocate) {
        if (const auto* failure = std::get_if<Failure>(&scheduled))
            FAIL(failure->detail);
        REQUIRE(std::holds_alternative<std::shared_ptr<const Schedule>>(scheduled));
        const auto&           ordered = *std::get<std::shared_ptr<const Schedule>>(scheduled);
        std::vector<BandPath> paths;
        for (const auto& item : ordered.order) {
            paths.push_back(plan->paths.at(item.operation.path - 1));
            paths.back().id = paths.size();
        }
        plan->paths                = std::move(paths);
        plan->ordered_region_paths = true;
        for (size_t i = 1; i < ordered.order.size(); ++i)
            scheduling.dependencies.push_back({{1, i}, {1, i + 1}, DependencyKind::Explicit});
        scheduling.allow_planar_reordering = false;
        const auto fixed                   = schedule_paths(scheduling, limits);
        if (const auto* failure = std::get_if<Failure>(&fixed))
            FAIL(failure->detail);
        REQUIRE(std::holds_alternative<std::shared_ptr<const Schedule>>(fixed));
    } else {
        REQUIRE(std::holds_alternative<Failure>(scheduled));
        CHECK(std::get<Failure>(scheduled).detail == "unsupported_bead_footprint");
    }
}

TEST_CASE("Material band Benchy cap repair preserves existing walls", "[material_band][layer_process][clipped_paths]")
{
    using namespace MaterialBand;
    const char*       source  = std::getenv("MATERIAL_BAND_PARTITION_REPLAY");
    const std::string fixture = source ? std::string(source) :
                                         std::string(TEST_DATA_DIR) + "/material_band/" +
                                             GENERATE("benchy-cap-83.json", "benchy-cap-109.json", "benchy-cap-239.json",
                                                      "benchy-cap-242.json", "benchy-cap-normalization.json", "benchy-foundation-157.json");
    CAPTURE(fixture);
    std::ifstream input(fixture);
    REQUIRE(input.good());
    nlohmann::json data;
    input >> data;
    const auto vector = [](const nlohmann::json& p) { return Vec3d(p[0].get<double>(), p[1].get<double>(), p[2].get<double>()); };
    PathPlan   geometry;
    geometry.layered_process              = true;
    geometry.solid_min                    = vector(data["minimum"]);
    geometry.solid_max                    = vector(data["maximum"]);
    geometry.machine.material_count       = data["machine"][0];
    geometry.machine.max_segment_mm       = data["machine"][1];
    geometry.machine.max_volume_mm3_s     = data["machine"][2];
    geometry.machine.deposition_mm_s      = data["machine"][3];
    geometry.machine.sections             = {{0., 140., 72.5}};
    geometry.machine.coplanar_tip_contact = true;
    ProcessConstraints process;
    process.bead_width_mm           = data["process"][0];
    process.min_active_thickness_mm = data["process"][1];
    process.max_active_thickness_mm = data["process"][2];
    process.macro_height_mm         = data["process"][3];
    for (const auto& item : data["segments"]) {
        BandPath path;
        path.id       = geometry.paths.size() + 1;
        path.band     = {1, 0, {1}, 0};
        path.role     = PathRole(item["role"].get<int>());
        path.vertices = {vector(item["a"]), vector(item["b"])};
        PathSegment segment;
        segment.volume_mm3      = item["volume"];
        segment.width_mm        = item["width"];
        segment.speed_mm_s      = item["speed"];
        segment.height_start_mm = segment.height_end_mm = geometry.solid_max.z() - geometry.solid_min.z();
        for (const auto& tri : item["triangles"]) {
            std::array<Vec2d, 3> triangle;
            BeadCell             cell;
            for (size_t i = 0; i < 3; ++i) {
                triangle[i]     = Vec2d(tri[i][0].get<double>(), tri[i][1].get<double>());
                cell.upper[i]   = Vec3d(triangle[i].x(), triangle[i].y(), geometry.solid_max.z());
                cell.lower_z[i] = geometry.solid_min.z();
            }
            segment.coverage_triangles.push_back(triangle);
            segment.cells.push_back(cell);
        }
        path.segments.push_back(std::move(segment));
        geometry.paths.push_back(std::move(path));
    }
    PlanarMaterialBand band;
    const uint32_t     material = data["band"][2];
    band.id                     = {data["band"][0].get<uint64_t>(), data["band"][1].get<uint64_t>(), {material}, material - 1};
    band.bottom_mm              = data["band"][3];
    band.top_mm                 = data["band"][4];
    const auto read_polygons    = [](const nlohmann::json& polygons) {
        ExPolygons result;
        for (const auto& polygon : polygons) {
            ExPolygon piece;
            for (size_t r = 0; r < polygon.size(); ++r) {
                Points points;
                for (const auto& p : polygon[r])
                    points.emplace_back(p[0].get<coord_t>(), p[1].get<coord_t>());
                if (r == 0)
                    piece.contour = Polygon(std::move(points));
                else
                    piece.holes.emplace_back(std::move(points));
            }
            result.push_back(std::move(piece));
        }
        return result;
    };
    band.footprint = read_polygons(data["domain"]);
    std::vector<PlanarMaterialBand> bands{band};
    ExPolygons                      preceding;
    if (data.contains("bands")) {
        bands.clear();
        for (const auto& b : data["bands"])
            bands.push_back({{b[0], b[1], {b[2]}, b[3]}, read_polygons(b[8]), b[4], b[5], b[6], b[7]});
        preceding = read_polygons(data["preceding"]);
    }
    CoverageLimits coverage;
    coverage.max_candidate_cells = 4000000;
    const auto result = plan_partitioned_cycle(geometry, bands, process, {}, {}, coverage, {}, {}, preceding, data.value("contact_z", 0.),
                                               data.value("common_foundation", 0.));
    if (const auto* failure = std::get_if<Failure>(&result))
        FAIL(failure->detail);
    const auto& plan = *std::get<std::shared_ptr<const PathPlan>>(result);
    REQUIRE_FALSE(plan.paths.empty());
    if (data.contains("bands")) {
        MaterialFractions intended{};
        for (const auto& part : bands)
            for (const auto& polygon : part.footprint)
                intended[part.id.stack_slot] += polygon.area() * SCALING_FACTOR * SCALING_FACTOR * (part.top_mm - part.bottom_mm);
        for (size_t m = 0; m < MAX_MATERIALS; ++m) {
            CHECK(plan.volume_mm3[m] >= intended[m] * .99 - 1e-6);
            CHECK(plan.volume_mm3[m] <= intended[m] * 1.0001 + 1e-6);
        }
        for (const auto& path : plan.paths)
            for (const auto& segment : path.segments) {
                CHECK(segment.height_start_mm >= process.min_active_thickness_mm - 1e-9);
                CHECK(segment.height_start_mm <= process.max_active_thickness_mm + 1e-9);
            }
        auto ordered = std::make_shared<PathPlan>(plan);
        std::stable_sort(ordered->paths.begin(), ordered->paths.end(), [](const BandPath& a, const BandPath& b) {
            return std::make_tuple(std::llround(a.vertices.front().z() * 1e8), a.band.material.value, a.band.domain) <
                   std::make_tuple(std::llround(b.vertices.front().z() * 1e8), b.band.material.value, b.band.domain);
        });
        for (size_t i = 0; i < ordered->paths.size(); ++i)
            ordered->paths[i].id = i + 1;
        ScheduleInput scheduling;
        scheduling.machine                 = plan.machine;
        scheduling.objects                 = {{1, ordered}};
        scheduling.allow_planar_reordering = true;
        scheduling.initial_position        = Vec3d(.5, 1., plan.solid_max.z() + 2.);
        const double contact_z             = data["contact_z"];
        const auto   floor                 = triangulate_expolygons_3d(preceding, contact_z);
        for (size_t i = 0; i < floor.size(); i += 3) {
            if ((floor[i + 1] - floor[i]).cross(floor[i + 2] - floor[i]).z() <= 1e-12)
                continue;
            scheduling.occupied.push_back({{{floor[i], floor[i + 1], floor[i + 2]}}, {{contact_z - .1, contact_z - .1, contact_z - .1}}});
        }
        ScheduleLimits limits;
        limits.max_paths           = 50000;
        limits.max_cells           = 500000;
        limits.max_candidates      = 100000000;
        limits.max_index_visits    = 500000000;
        limits.max_temporary_bytes = size_t(1024) * 1024 * 1024;
        const auto scheduled       = schedule_paths(scheduling, limits);
        if (const auto* failure = std::get_if<Failure>(&scheduled))
            FAIL(failure->detail);
        REQUIRE(std::holds_alternative<std::shared_ptr<const Schedule>>(scheduled));
        return;
    }
    Polygons footprints;
    double   summed_area = 0.;
    for (const auto& path : plan.paths)
        for (const auto& segment : path.segments)
            for (const auto& cell : segment.cells) {
                Points points;
                for (const auto& p : cell.upper)
                    points.emplace_back(scale_(p.x()), scale_(p.y()));
                Polygon triangle(std::move(points));
                summed_area += triangle.area() * SCALING_FACTOR * SCALING_FACTOR;
                footprints.push_back(std::move(triangle));
            }
    const auto covered      = union_ex(footprints);
    double     covered_area = 0., excess = 0.;
    for (const auto& piece : covered)
        covered_area += piece.area() * SCALING_FACTOR * SCALING_FACTOR;
    for (const auto& piece : diff_ex(covered, band.footprint))
        excess += piece.area() * SCALING_FACTOR * SCALING_FACTOR;
    CHECK(summed_area - covered_area < 1e-4);
    CHECK(excess < 1e-4);
    double target_volume = 0.;
    for (const auto& piece : band.footprint)
        target_volume += piece.area() * SCALING_FACTOR * SCALING_FACTOR * (band.top_mm - band.bottom_mm);
    CHECK(plan.volume_mm3[material - 1] >= target_volume * .99);
    CHECK(plan.volume_mm3[material - 1] <= target_volume + 1e-5);
    ScheduleInput scheduling;
    scheduling.machine          = plan.machine;
    scheduling.bed_z_mm         = band.bottom_mm;
    scheduling.initial_position = Vec3d(0., 0., band.top_mm + 2.);
    scheduling.objects          = {{1, std::get<std::shared_ptr<const PathPlan>>(result)}};
    const auto scheduled        = schedule_paths(scheduling);
    if (const auto* failure = std::get_if<Failure>(&scheduled))
        FAIL(failure->detail);
    REQUIRE(std::holds_alternative<std::shared_ptr<const Schedule>>(scheduled));
}
