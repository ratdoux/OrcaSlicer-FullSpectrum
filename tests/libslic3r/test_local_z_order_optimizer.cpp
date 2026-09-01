#include <catch2/catch_test_macros.hpp>

#include "libslic3r/LocalZOrderOptimizer.hpp"

using namespace Slic3r;

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
