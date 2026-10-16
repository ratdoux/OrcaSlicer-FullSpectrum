#include <catch2/catch_test_macros.hpp>
#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/GCode/ExtrusionProcessor.hpp"
#include <tbb/parallel_for.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>

using namespace Slic3r;

TEST_CASE("Replay quality geometry with and without unused curvature", "[.][QualityGeometryReplay]")
{
    const char* path = std::getenv("ORCA_QUALITY_REPLAY");
    REQUIRE(path);
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    using Clock        = std::chrono::steady_clock;
    auto   elapsed     = [](Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); };
    double original_ms = 0, accelerated_ms = 0;
    size_t paths = 0, input_points = 0, output_points = 0;
    for (;;) {
        uint32_t counts[3];
        input.read(reinterpret_cast<char*>(counts), sizeof(counts));
        if (input.eof())
            break;
        REQUIRE(input.good());
        REQUIRE(counts[0] <= 65536);
        REQUIRE(counts[1] <= 1000000);
        REQUIRE(counts[2] <= 1000000);
        Linesf lines;
        for (size_t i = 0; i < counts[0]; ++i) {
            double xy[4];
            input.read(reinterpret_cast<char*>(xy), sizeof(xy));
            lines.emplace_back(Vec2d(xy[0], xy[1]), Vec2d(xy[2], xy[3]));
        }
        Points points;
        for (size_t i = 0; i < counts[1]; ++i) {
            double xy[2];
            input.read(reinterpret_cast<char*>(xy), sizeof(xy));
            points.emplace_back(std::llround(xy[0] / SCALING_FACTOR), std::llround(xy[1] / SCALING_FACTOR));
        }
        std::vector<uint32_t> lengths(counts[2]);
        input.read(reinterpret_cast<char*>(lengths.data()), lengths.size() * sizeof(uint32_t));
        REQUIRE(input.good());
        AABBTreeLines::LinesDistancer<Linef> index(std::move(lines));
        size_t                               at = 0;
        for (size_t length : lengths) {
            REQUIRE(at + length <= points.size());
            Points path_points(points.begin() + at, points.begin() + at + length);
            at += length;
            auto start     = Clock::now();
            auto reference = estimate_points_properties<true, true, true, true>(path_points, index, .4f, -1, .04f, true);
            original_ms += elapsed(start);
            start     = Clock::now();
            auto fast = estimate_points_properties<true, true, true, true>(path_points, index, .4f, -1, .04f, false);
            accelerated_ms += elapsed(start);
            REQUIRE(reference.size() == fast.size());
            for (size_t i = 0; i < reference.size(); ++i) {
                REQUIRE(reference[i].position == fast[i].position);
                REQUIRE(reference[i].distance == fast[i].distance);
            }
            ++paths;
            input_points += length;
            output_points += reference.size();
        }
    }
    std::cout << "Quality geometry replay paths=" << paths << " input_points=" << input_points << " output_points=" << output_points
              << " reference_ms=" << original_ms << " no_curvature_ms=" << accelerated_ms << " width=0.4 min_distance=0.04\n";
    REQUIRE(paths > 0);
}

TEST_CASE("Layer distance batches retain exact queries and honor memory bounds", "[CPUSlicing][ExtrusionQuality]")
{
    struct TestLayer : Layer
    {
        TestLayer() : Layer(1, nullptr, .2, .4, .3) {}
        ~TestLayer() override = default;
    } layer;
    auto*         region = layer.add_region(nullptr);
    ExtrusionPath path(erPerimeter, .08, .4f, .2f);
    for (coord_t y = 0; y < 32; ++y)
        for (coord_t x = 0; x < 32; ++x)
            path.polyline.points.emplace_back(x * 300000, y * 300000);
    region->perimeters.entities.push_back(path.clone());
    Linesf                               lines{{{0, 0}, {10, 0}}, {{10, 0}, {10, 10}}, {{10, 10}, {0, 10}}, {{0, 10}, {0, 0}}};
    AABBTreeLines::LinesDistancer<Linef> index(lines);
    QualityDistanceCache                 cache;
    cache.prepare(layer, index, 65536);
    REQUIRE(cache.size() == 1024);
    Points points = path.polyline.points;
    points.emplace_back(-1234567, 3456789); // A new clipped/seam vertex falls back.
    const auto distances = cache.query(points, index);
    REQUIRE(distances.size() == points.size());
    for (size_t i = 0; i < points.size(); ++i)
        CHECK(distances[i] == index.distance_from_lines<true>(unscaled(points[i])));
    const auto reference = estimate_points_properties<true, true, true, true>(points, index, .4f, -1, .1f, false);
    const auto cached    = estimate_points_properties<true, true, true, true>(points, index, .4f, -1, .1f, false, &distances);
    REQUIRE(reference.size() == cached.size());
    for (size_t i = 0; i < reference.size(); ++i) {
        CHECK(reference[i].position == cached[i].position);
        CHECK(reference[i].distance == cached[i].distance);
    }
    cache.prepare(layer, index, 512);
    CHECK(cache.size() == 0);
    CHECK(cache.query(points, index).empty());
    cache.prepare(layer, index, 0);
    CHECK(cache.size() == 0);
}

TEST_CASE("Replay wall polygon pruning and independent unions", "[.][WallPolygonReplay]")
{
    const char* path = std::getenv("ORCA_POLYGON_REPLAY");
    REQUIRE(path);
    std::ifstream input(path);
    REQUIRE(input.good());
    using Clock  = std::chrono::steady_clock;
    auto elapsed = [](Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); };
    auto decode  = [](const nlohmann::json& rings) {
        Polygons result;
        for (const auto& ring : rings) {
            Points points;
            for (const auto& p : ring)
                points.emplace_back(p[0].get<coord_t>(), p[1].get<coord_t>());
            result.emplace_back(std::move(points));
        }
        return result;
    };
    nlohmann::json stats;
    std::string    line;
    while (std::getline(input, line)) {
        auto row = nlohmann::json::parse(line);
        if (row.value("fill", int(ClipperLib::pftNonZero)) != int(ClipperLib::pftNonZero))
            continue;
        auto      subject = decode(row["subject"]), clip = decode(row["clip"]);
        const int operation = row["operation"];
        if (operation != ClipperLib::ctUnion && operation != ClipperLib::ctIntersection && operation != ClipperLib::ctDifference)
            continue;
        auto run = [&](const Polygons& c) {
            if (operation == ClipperLib::ctUnion) {
                auto all = subject;
                append(all, c);
                return union_ex(all);
            }
            return operation == ClipperLib::ctDifference ? diff_ex(subject, c) : intersection_ex(subject, c);
        };
        double     ref_ms = 0, fast_ms = 0;
        ExPolygons reference, accelerated;
        size_t     removed = 0, components = 1;
        for (int repeat = 0; repeat < 3; ++repeat) {
            auto start = Clock::now();
            reference  = run(clip);
            ref_ms += elapsed(start);
            start = Clock::now();
            if (operation != ClipperLib::ctUnion) {
                auto     bounds = get_extents(subject);
                Polygons selected;
                removed = 0;
                for (const Polygon& p : clip) {
                    if (bounds.overlap(get_extents(p)))
                        selected.push_back(p);
                    else
                        ++removed;
                }
                accelerated = run(selected);
            } else {
                auto all = subject;
                append(all, clip);
                if (all.size() > 1024)
                    accelerated = run(clip);
                else {
                    std::vector<size_t> parent(all.size());
                    std::iota(parent.begin(), parent.end(), 0);
                    std::vector<BoundingBox> bounds;
                    for (const Polygon& p : all)
                        bounds.push_back(get_extents(p));
                    auto root = [&](size_t i) {
                        while (parent[i] != i) {
                            parent[i] = parent[parent[i]];
                            i         = parent[i];
                        }
                        return i;
                    };
                    for (size_t i = 0; i < all.size(); ++i)
                        for (size_t j = 0; j < i; ++j)
                            if (bounds[i].overlap(bounds[j]))
                                parent[root(i)] = root(j);
                    std::map<size_t, Polygons> groups;
                    for (size_t i = 0; i < all.size(); ++i)
                        groups[root(i)].push_back(all[i]);
                    components = groups.size();
                    accelerated.clear();
                    for (const auto& entry : groups)
                        append(accelerated, union_ex(entry.second));
                }
            }
            fast_ms += elapsed(start);
        }
        const bool ordered = reference == accelerated;
        if (!ordered) {
            CHECK(diff_ex(reference, accelerated).empty());
            CHECK(diff_ex(accelerated, reference).empty());
        }
        auto& s = stats[operation == ClipperLib::ctUnion ? "partition_union" : "prune_clip"];
        if (s.is_null())
            s = nlohmann::json::object();
        s["cases"]         = s.value("cases", 0) + 1;
        s["reference_ms"]  = s.value("reference_ms", 0.) + ref_ms / 3;
        s["experiment_ms"] = s.value("experiment_ms", 0.) + fast_ms / 3;
        s["changed_order"] = s.value("changed_order", 0) + int(!ordered);
        s["removed_rings"] = s.value("removed_rings", size_t(0)) + removed;
        s["split_cases"]   = s.value("split_cases", 0) + int(components > 1);
    }
    std::cout << "Wall polygon replay " << stats.dump() << '\n';
    if (const char* output = std::getenv("ORCA_POLYGON_REPLAY_RESULT"))
        std::ofstream(output) << stats.dump(2);
    REQUIRE(!stats.empty());
}

TEST_CASE("Export can omit curvature while retaining all geometry and distances", "[CPUSlicing][ExtrusionQuality]")
{
    Linesf                               lines{{{0, 0}, {10, 0}}, {{10, 0}, {10, 10}}, {{10, 10}, {0, 10}}, {{0, 10}, {0, 0}}};
    AABBTreeLines::LinesDistancer<Linef> index(lines);
    for (const Points& points : std::vector<Points>{{},
                                                    {{0, 0}},
                                                    {{1000000, 1000000}, {9000000, 1000000}, {9000000, 9000000}, {1000000, 1000000}},
                                                    {{-1000000, 1000000}, {11000000, 1000000}, {11000000, 11000000}},
                                                    {{1000000, 1000000}, {1000000, 1000000}}}) {
        const auto reference   = estimate_points_properties<true, true, true, true>(points, index, .4f, -1, .1f, true);
        const auto accelerated = estimate_points_properties<true, true, true, true>(points, index, .4f, -1, .1f, false);
        REQUIRE(accelerated.size() == reference.size());
        for (size_t i = 0; i < reference.size(); ++i) {
            CHECK(accelerated[i].position == reference[i].position);
            CHECK(accelerated[i].distance == reference[i].distance);
        }
    }
}
