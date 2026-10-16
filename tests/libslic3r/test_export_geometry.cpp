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
