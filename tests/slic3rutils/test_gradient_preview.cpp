#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/MFDGradientPreview.hpp"

#include <array>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace Slic3r;
using namespace Slic3r::GUI;

namespace {
using Pixel = std::array<unsigned char, 3>;
Pixel pixel(const wxImage& image, int x, int y) { return {image.GetRed(x, y), image.GetGreen(x, y), image.GetBlue(x, y)}; }
void  save_artifact(const wxImage& image, const std::string& name)
{
    if (const char* directory = std::getenv("GRADIENT_PREVIEW_ARTIFACT_DIR")) {
        std::ofstream file(std::string(directory) + "/" + name + ".ppm", std::ios::binary);
        REQUIRE(file.good());
        file << "P6\n" << image.GetWidth() << ' ' << image.GetHeight() << "\n255\n";
        file.write(reinterpret_cast<const char*>(image.GetData()), image.GetWidth() * image.GetHeight() * 3);
    }
}
} // namespace

TEST_CASE("Gradient rasters fill every pixel without brush seams", "[GradientPreview][GUI]")
{
    // More samples than pixels reproduces the old overlapping-brush rendering.
    std::vector<wxColour> colors;
    for (int value = 0; value <= 256; ++value)
        colors.emplace_back(std::min(value, 255), std::min(value, 255), std::min(value, 255));
    for (const int extent : {36, 120, 210, 551}) {
        CAPTURE(extent);
        const wxImage horizontal = make_mixed_gradient_raster(colors, wxSize(extent, 5), false);
        const wxImage vertical   = make_mixed_gradient_raster(colors, wxSize(5, extent), true);
        REQUIRE(horizontal.IsOk());
        REQUIRE(vertical.IsOk());
        CHECK(pixel(horizontal, 0, 0) == Pixel{0, 0, 0});
        CHECK(pixel(horizontal, extent - 1, 0) == Pixel{255, 255, 255});
        for (int x = 0; x < extent; ++x) {
            const auto color = pixel(horizontal, x, 0);
            if (x > 0)
                CHECK(color[0] >= pixel(horizontal, x - 1, 0)[0]);
            for (int cross = 0; cross < 5; ++cross) {
                CHECK(pixel(horizontal, x, cross) == color);
                CHECK(pixel(vertical, cross, extent - 1 - x) == color);
            }
        }
    }
    CHECK_FALSE(make_mixed_gradient_raster({}, wxSize(10, 10), false).IsOk());
    CHECK_FALSE(make_mixed_gradient_raster(colors, wxSize(0, 10), false).IsOk());
}

TEST_CASE("Gradient preview preserves solid zones and physical layer spacing", "[GradientPreview][GUI]")
{
    MixedFilamentDefinition definition;
    definition.behavior.gradient.enabled        = true;
    definition.recipe.blend.components          = {{{1}, 1}, {{2}, 1}, {{3}, 1}};
    definition.behavior.gradient.stop_positions = {0.f, .435f, .536f, .636f, 1.f};
    definition.behavior.gradient.solid_widths   = {0.f, .099f, 0.f};
    MixedFilamentDisplayContext context;
    context.num_physical      = 3;
    context.physical_colors   = {"#000000", "#80804E", "#FFFFFF"};
    context.max_layer_heights = {.3, .3, .3};
    struct EngineGuard
    {
        MixedFilamentColorEngine previous = MixedFilamentManager::color_engine();
        ~EngineGuard() { MixedFilamentManager::set_color_engine(previous); }
    } guard;
    std::string prefix = "filament-mixer-";
    SECTION("FilamentMixer") { MixedFilamentManager::set_color_engine(MixedFilamentColorEngine::FilamentMixer); }
    SECTION("KM-KS")
    {
        MixedFilamentManager::set_color_engine(MixedFilamentColorEngine::FullSpectrumKSPairResidual);
        prefix = "ks-";
    }
    std::vector<wxColour> samples;
    for (int i = 0; i <= 256; ++i) {
        const auto  mix = sample_mixed_gradient(definition, 3, i / 256.0, .03);
        const auto& a   = context.physical_colors[mix.component_a - 1];
        const auto& b   = context.physical_colors[mix.component_b - 1];
        samples.emplace_back(
            mix.component_a == mix.component_b ? a : MixedFilamentManager::blend_color(a, b, 100 - mix.mix_b_percent, mix.mix_b_percent));
    }
    const wxImage smooth = make_mixed_gradient_raster(samples, wxSize(268, 210), true);
    const wxImage bar    = make_mixed_gradient_raster(samples, wxSize(551, 70), false);
    const wxImage layers = make_mixed_gradient_layer_raster(definition, context, wxSize(268, 210), true);
    REQUIRE(smooth.IsOk());
    REQUIRE(layers.IsOk());
    CHECK(pixel(smooth, 0, 0) == Pixel{255, 255, 255});
    CHECK(pixel(smooth, 0, 209) == Pixel{0, 0, 0});
    CHECK(pixel(smooth, 0, 98) == Pixel{128, 128, 78});
    for (int y = 0; y < 210; ++y) {
        for (int x = 1; x < 268; ++x) {
            CHECK(pixel(smooth, x, y) == pixel(smooth, 0, y));
            CHECK(pixel(layers, x, y) == pixel(layers, 0, y));
        }
        const auto color = pixel(layers, 0, y);
        CHECK((color == Pixel{0, 0, 0} || color == Pixel{128, 128, 78} || color == Pixel{255, 255, 255}));
    }
    // Raster orientation must not change the cadence or blend.
    const wxImage horizontal_layers = make_mixed_gradient_layer_raster(definition, context, wxSize(210, 5), false);
    for (int x = 0; x < 210; ++x)
        CHECK(pixel(horizontal_layers, x, 0) == pixel(layers, 0, 209 - x));
    save_artifact(smooth, prefix + "gradient-blended");
    save_artifact(bar, prefix + "gradient-bar");
    save_artifact(layers, prefix + "gradient-layered");
}
