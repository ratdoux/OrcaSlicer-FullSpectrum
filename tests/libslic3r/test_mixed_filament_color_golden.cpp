#include <catch2/catch_test_macros.hpp>
#include "libslic3r/filament_mixer.h"
#include "libslic3r/filament_mixer_model.h"
#include <array>
#include <cmath>
#include <random>
#include <vector>

// Keep the imported regression coefficients pinned independently of the
// application's endpoint correction. Do not silently refit the model.

TEST_CASE("Raw FilamentMixer documented example remains unchanged", "[mixed_filament][golden]")
{
    // Documented in filament_mixer_model.h: blue(0,33,133) + yellow(252,211,0)
    // @ t=0.5 -> green(47,141,56).
    unsigned char r = 0, g = 0, b = 0;
    filament_mixer::lerp(0, 33, 133, 252, 211, 0, 0.5f, &r, &g, &b);
    REQUIRE(static_cast<int>(r) == 47);
    REQUIRE(static_cast<int>(g) == 141);
    REQUIRE(static_cast<int>(b) == 56);
}

TEST_CASE("filament_mixer_lerp endpoint invariants", "[mixed_filament][golden]")
{
    unsigned char r = 0, g = 0, b = 0;
    auto          check = [&](float t, int er, int eg, int eb) {
        Slic3r::filament_mixer_lerp(10, 20, 30, 200, 210, 220, t, &r, &g, &b);
        REQUIRE(static_cast<int>(r) == er);
        REQUIRE(static_cast<int>(g) == eg);
        REQUIRE(static_cast<int>(b) == eb);
    };
    // Pure endpoints and out-of-range ratios retain the input colors.
    check(0.0f, 10, 20, 30);    // pure A
    check(1.0f, 200, 210, 220); // pure B
    // Out-of-range t is clamped to the nearest endpoint.
    check(-0.5f, 10, 20, 30);   // clamp low  -> A
    check(1.5f, 200, 210, 220); // clamp high -> B
}

namespace {
using Color = std::array<unsigned char, 3>;
Color corrected_mix(const Color& a, const Color& b, float t)
{
    Color result{};
    Slic3r::filament_mixer_lerp(a[0], a[1], a[2], b[0], b[1], b[2], t, &result[0], &result[1], &result[2]);
    return result;
}
} // namespace

TEST_CASE("FilamentMixer approaches both endpoints across the RGB gamut", "[mixed_filament][GradientPort]")
{
    std::vector<Color> colors;
    for (int r : {0, 128, 255})
        for (int g : {0, 128, 255})
            for (int b : {0, 128, 255})
                colors.push_back({static_cast<unsigned char>(r), static_cast<unsigned char>(g), static_cast<unsigned char>(b)});
    auto check_pair = [](const Color& a, const Color& b) {
        CHECK(corrected_mix(a, b, 0.f) == a);
        CHECK(corrected_mix(a, b, .00001f) == a);
        CHECK(corrected_mix(a, b, .99999f) == b);
        CHECK(corrected_mix(a, b, 1.f) == b);
    };
    for (const auto& a : colors)
        for (const auto& b : colors)
            check_pair(a, b);
    std::mt19937 rng(17092026);
    for (int i = 0; i < 4096; ++i) {
        Color a{}, b{};
        for (int c = 0; c < 3; ++c) {
            a[c] = static_cast<unsigned char>(rng() & 255);
            b[c] = static_cast<unsigned char>(rng() & 255);
        }
        check_pair(a, b);
        for (float t : {.01f, .25f, .5f, .75f, .99f})
            CHECK(corrected_mix(a, a, t) == a);
    }
}

TEST_CASE("FilamentMixer green to white has no pink fringe", "[mixed_filament][GradientPort]")
{
    const Color green{0, 255, 0}, white{255, 255, 255};
    Color       previous = green;
    for (int percent = 1; percent <= 100; ++percent) {
        const Color result = corrected_mix(green, white, percent / 100.f);
        CHECK(result[1] >= result[0]);
        CHECK(result[1] >= result[2]);
        for (int c = 0; c < 3; ++c)
            CHECK(std::abs(int(result[c]) - int(previous[c])) <= 4);
        previous = result;
    }
    const Color expected{244, 255, 238};
    CHECK(corrected_mix(green, white, .95f) == expected);
}

TEST_CASE("Endpoint-corrected FilamentMixer retains pigment mixing", "[mixed_filament][GradientPort]")
{
    const auto green = corrected_mix({0, 33, 133}, {252, 211, 0}, .5f);
    // Anchoring changes the old fit but preserves blue + yellow -> green,
    // unlike ordinary RGB interpolation (126, 122, 67).
    CHECK(green[1] > green[0] + 50);
    CHECK(green[1] > green[2] + 50);
    const Color expected{73, 143, 52};
    CHECK(green == expected);
}
