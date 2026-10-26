#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "libslic3r/LocalZCadence.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

using namespace Slic3r::LocalZCadence;
using Catch::Approx;

namespace {
std::vector<Component> pair_recipe(double a, double minimum = .06, double maximum = .32)
{
    return {{1, a, {minimum, maximum}}, {2, 1. - a, {minimum, maximum}}};
}

void check_cycle(const Result& cycle, const std::vector<Component>& components, double quantum)
{
    REQUIRE(cycle);
    double height = 0.;
    for (const auto& bead : cycle.beads) {
        const auto component = std::find_if(components.begin(), components.end(), [&](const auto& c) { return c.tool == bead.tool; });
        REQUIRE(component != components.end());
        CHECK(bead.height >= component->limits.minimum - 1e-9);
        CHECK(bead.height <= component->limits.maximum + 1e-9);
        if (quantum > 0.)
            CHECK(bead.height / quantum == Approx(std::round(bead.height / quantum)).margin(1e-7));
        height += bead.height;
    }
    CHECK(height == Approx(cycle.period).margin(1e-9));
}
} // namespace

TEST_CASE("LocalZ chooses the minimum recipe before selecting a planning window", "[LocalZ][LocalZCadence]")
{
    for (const auto& example : std::vector<std::pair<double, double>>{{.5, .12}, {.4, .15}, {.33, .181818181818}, {.15, .4}}) {
        const auto recipe = pair_recipe(example.first);
        auto       cycle  = minimum(recipe);
        check_cycle(cycle, recipe, .001);
        CHECK(cycle.minimum_period == Approx(example.second));
        CHECK(cycle.period == Approx(std::ceil(example.second / .001 - 1e-8) * .001));
    }
    auto cycle = minimum(pair_recipe(.4, .08));
    REQUIRE(cycle);
    REQUIRE(cycle.beads.size() == 2);
    CHECK(cycle.period == Approx(.2));
    CHECK(cycle.beads[0].height == Approx(.08));
    CHECK(cycle.beads[1].height == Approx(.12));
}

TEST_CASE("LocalZ separates recipe periods from dominant component subdivisions", "[LocalZ][LocalZCadence]")
{
    const auto recipe = pair_recipe(.15);
    const auto cycle  = minimum(recipe);
    check_cycle(cycle, recipe, .001);
    REQUIRE(cycle.beads.size() == 3);
    CHECK(cycle.period == Approx(.4));
    CHECK(cycle.beads[0].height == Approx(.06));
    CHECK(cycle.beads[1].height == Approx(.17));
    CHECK(cycle.beads[2].height == Approx(.17));
    CHECK(cycle.beads[1].tool == cycle.beads[2].tool);
    Options options;
    options.maximum_beads = 2;
    CHECK(minimum(recipe, options).status == Status::pass_limit);
}

TEST_CASE("LocalZ cadence searches gaps between feasible bead counts", "[LocalZ][LocalZCadence]")
{
    Options options;
    options.z_quantum      = 0.;
    options.maximum_period = 2.;
    for (double weight : {.15, .25, .3, .4, .55, .8})
        for (double upper : {.08, .09, .11, .17}) {
            const std::vector<Component> recipe{{1, weight, {.06, .07}}, {2, 1. - weight, {.08, upper}}};
            double                       reference = std::numeric_limits<double>::infinity();
            // Independent exact enumeration of interval intersections.
            for (int a = 1; a <= 40; ++a)
                for (int b = 1; b <= 40; ++b) {
                    const double lo = std::max(a * .06 / weight, b * .08 / (1. - weight));
                    const double hi = std::min(a * .07 / weight, b * upper / (1. - weight));
                    if (lo <= hi + 1e-9 && lo <= options.maximum_period + 1e-9)
                        reference = std::min(reference, lo);
                }
            const auto result = minimum(recipe, options);
            if (std::isfinite(reference)) {
                check_cycle(result, recipe, 0.);
                CHECK(result.period == Approx(reference).margin(1e-8));
            } else
                CHECK_FALSE(result);
        }
}

TEST_CASE("LocalZ keeps the minimum cadence until the actual domain closure", "[LocalZ][LocalZCadence]")
{
    const auto recipe = pair_recipe(.4);
    const auto result = tile(recipe, 1.);
    REQUIRE(result);
    CHECK(result.minimum_period == Approx(.15));
    REQUIRE(result.cycles.size() == 6);
    CHECK(result.closure_adjusted);
    double              sum = 0.;
    std::vector<double> components(2, 0.);
    for (const auto& cycle : result.cycles) {
        check_cycle(cycle, recipe, .001);
        CHECK(cycle.period <= .25 + 1e-9);
        sum += cycle.period;
        for (const auto& bead : cycle.beads)
            components[bead.tool - 1] += bead.height;
    }
    CHECK(sum == Approx(1.));
    for (size_t i = 0; i < 5; ++i)
        CHECK(result.cycles[i].period == Approx(.15));
    CHECK(components[0] == Approx(.4).margin(.001));
    CHECK(components[1] == Approx(.6).margin(.001));
    CHECK_FALSE(tile(recipe, .1));
    CHECK(tile(recipe, 1.0005).status == Status::precision_limit);
}

TEST_CASE("LocalZ coordinate rounding preserves bead limits and local recipe debt", "[LocalZ][LocalZCadence]")
{
    const auto recipe = pair_recipe(.33);
    const auto result = tile(recipe, 3.64);
    REQUIRE(result);
    REQUIRE(result.cycles.size() == 20);
    double cyan = 0.;
    for (const auto& cycle : result.cycles) {
        check_cycle(cycle, recipe, .001);
        for (const auto& bead : cycle.beads)
            if (bead.tool == 1)
                cyan += bead.height;
    }
    CHECK(cyan == Approx(.33 * 3.64).margin(.001));
    CHECK_FALSE(at_period(recipe, .1815));
}

TEST_CASE("LocalZ limit resolution keeps minima and maxima independent", "[LocalZ][LocalZCadence]")
{
    auto limits = resolve_limits(.06, .08, .32, .4);
    CHECK(limits.minimum == Approx(.08));
    CHECK(limits.maximum == Approx(.32));
    limits = resolve_limits(.06, 0., 0., .4);
    CHECK(limits.minimum == Approx(.07));
    CHECK(limits.maximum == Approx(.3));
    limits = resolve_limits(.1, .08, .09, .4);
    CHECK(minimum({{1, .5, limits}, {2, .5, limits}}).status == Status::invalid_input);
}

TEST_CASE("LocalZ cadence distinguishes invalid inputs from resource limits", "[LocalZ][LocalZCadence]")
{
    CHECK(minimum({}).status == Status::invalid_input);
    CHECK(minimum({{0, 1., {.06, .32}}}).status == Status::invalid_input);
    CHECK(minimum({{1, 1., {.06, .32}}, {1, 1., {.06, .32}}}).status == Status::invalid_input);
    CHECK(minimum({{1, std::numeric_limits<double>::quiet_NaN(), {.06, .32}}}).status == Status::invalid_input);
    CHECK(minimum({{1, -1., {.06, .32}}}).status == Status::invalid_input);
    auto recipe = pair_recipe(.4);
    recipe.push_back({3, 0., {}});
    REQUIRE(minimum(recipe));
    Options options;
    options.maximum_period = .1;
    CHECK(minimum(recipe, options).status == Status::height_limit);
    options.maximum_period = 10.;
    options.search_budget  = 1;
    CHECK(minimum(recipe, options).status == Status::search_limit);
}
