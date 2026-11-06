#include <catch2/catch_test_macros.hpp>

#include "libslic3r/LocalZOrderOptimizer.hpp"
#include "libslic3r/LocalZPassScheduler.hpp"

using namespace Slic3r;

TEST_CASE("Local-Z scheduling shares deterministic tool phases across independent chains", "[LocalZ][GCode]")
{
    // The input is deliberately out of dependency order. Object 1's group 1
    // is independent of object 0's group 1, and prints with two tools.
    const std::vector<LocalZSchedulePass> passes{{0, 1, 1, 1, 0.2, {0}},
                                                 {0, 2, 0, 2, 0.1, {1}},
                                                 {0, 1, 0, 0, 0.1, {1}},
                                                 {1, 1, 0, 0, 0.1, {0, 1}}};
    const auto                            schedule = schedule_local_z_passes(passes, 1);
    REQUIRE(schedule.has_value());
    REQUIRE(schedule->size() == 3);
    CHECK((*schedule)[0].extruder_id == 1);
    CHECK((*schedule)[0].pass_state_indices == std::vector<size_t>{2, 1, 3});
    CHECK((*schedule)[1].extruder_id == 0);
    CHECK((*schedule)[1].pass_state_indices == std::vector<size_t>{3});
    CHECK((*schedule)[2].extruder_id == 0);
    CHECK((*schedule)[2].pass_state_indices == std::vector<size_t>{0});
    CHECK_FALSE(schedule_local_z_passes({{0, 1, 0, 0, 0.1, {}}}, 0).has_value());
}

TEST_CASE("Local-Z bucket ordering starts with the active extruder when possible", "[LocalZ][GCode]")
{
    const std::vector<unsigned int> ordered =
        LocalZOrderOptimizer::order_bucket_extruders({1, 2}, 2, 1);

    REQUIRE(ordered == std::vector<unsigned int>{2, 1});
}

TEST_CASE("Local-Z pass groups keep a matching active extruder bucket first", "[LocalZ][GCode]")
{
    const std::vector<size_t> ordered =
        LocalZOrderOptimizer::order_pass_group({{1}, {2}}, 2);

    REQUIRE(ordered == std::vector<size_t>{1, 0});
}

TEST_CASE("Local-Z pass groups keep original order when no bucket matches the active extruder", "[LocalZ][GCode]")
{
    const std::vector<size_t> ordered =
        LocalZOrderOptimizer::order_pass_group({{1}, {2}, {3}}, 0);

    REQUIRE(ordered == std::vector<size_t>{0, 1, 2});
}
