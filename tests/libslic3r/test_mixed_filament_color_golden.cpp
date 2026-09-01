#include <catch2/catch_test_macros.hpp>
#include "libslic3r/filament_mixer.h"

// Golden baseline for the legacy Justin-Hayes polynomial mixer backend
// (Slic3r::filament_mixer_lerp -> filament_mixer::lerp in filament_mixer_model.h).
//
// Purpose: pin the CURRENT legacy filament_mixer backend output.
// Any refactor of filament_mixer* is immediately visible if it
// changes legacy behaviour. Integer-exact assertions (no hex-case ambiguity).

TEST_CASE("filament_mixer_lerp documented example", "[mixed_filament][golden]")
{
    // Documented in filament_mixer_model.h: blue(0,33,133) + yellow(252,211,0)
    // @ t=0.5 -> green(47,141,56).
    unsigned char r = 0, g = 0, b = 0;
    Slic3r::filament_mixer_lerp(0, 33, 133, 252, 211, 0, 0.5f, &r, &g, &b);
    REQUIRE(static_cast<int>(r) == 47);
    REQUIRE(static_cast<int>(g) == 141);
    REQUIRE(static_cast<int>(b) == 56);
}

TEST_CASE("filament_mixer_lerp endpoint invariants", "[mixed_filament][golden]")
{
    unsigned char r = 0, g = 0, b = 0;
    auto check = [&](float t, int er, int eg, int eb) {
        Slic3r::filament_mixer_lerp(10, 20, 30, 200, 210, 220, t, &r, &g, &b);
        REQUIRE(static_cast<int>(r) == er);
        REQUIRE(static_cast<int>(g) == eg);
        REQUIRE(static_cast<int>(b) == eb);
    };
    // Pure endpoints (filament_mixer::lerp returns the input colour verbatim
    // for t<=0 / t>=1, before any polynomial math runs).
    check(0.0f, 10, 20, 30);    // pure A
    check(1.0f, 200, 210, 220); // pure B
    // Out-of-range t is clamped to the nearest endpoint.
    check(-0.5f, 10, 20, 30);   // clamp low  -> A
    check(1.5f, 200, 210, 220); // clamp high -> B
}
