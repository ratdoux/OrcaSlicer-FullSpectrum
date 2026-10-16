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
