#include <catch2/catch_test_macros.hpp>
#include "libslic3r/Print.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/MultiMaterialSegmentation.hpp"
#include "libslic3r/PaintedLayerOutlineCache.hpp"
using namespace Slic3r;
namespace {
ExPolygon square(coord_t x, coord_t y, coord_t size)
{
    return ExPolygon(Polygon{Points{{x, y}, {x + size, y}, {x + size, y + size}, {x, y + size}}});
}
std::vector<ColoredLines> colors_for(const ExPolygons& input, bool mixed, bool different_hole)
{
    std::vector<ColoredLines> colors;
    for (size_t i = 0; i < input.size(); ++i) {
        const auto polygons = to_polygons(input[i]);
        for (size_t hole = 0; hole < polygons.size(); ++hole) {
            ColoredLines lines;
            const auto   edges = polygons[hole].lines();
            for (size_t j = 0; j < edges.size(); ++j)
                lines.push_back({edges[j],
                                 int(mixed && i == 0            ? j % 2 :
                                     different_hole && hole > 0 ? 2 :
                                                                  i % 3),
                                 int(colors.size()), int(j)});
            colors.push_back(std::move(lines));
        }
    }
    return colors;
}
} // namespace

TEST_CASE("Painted shell outline reuse preserves bounded and uncached chains", "[PaintPreparation][PaintIslands]")
{
    std::vector<ExPolygons> layers;
    for (coord_t layer = 0; layer < 31; ++layer) {
        ExPolygon polygon = square(layer * 10000, 0, 10000000 - layer * 20000);
        Polygon   hole    = square(2000000, 2000000 + layer * 10000, 2000000).contour;
        hole.reverse();
        polygon.holes.push_back(std::move(hole));
        layers.push_back({std::move(polygon)});
    }
    for (size_t point_limit : {size_t(0), size_t(16), size_t(32768)}) {
        PaintedLayerOutlineCache cache(layers, 15, point_limit, 3);
        for (size_t color = 0; color < 4; ++color)
            for (bool top : {false, true}) {
                ExPolygons expected = layers[15], scratch;
                for (size_t distance = 1; distance <= 5 + color; ++distance) {
                    expected = intersection_ex(expected, layers[top ? 15 - distance : 15 + distance]);
                    CHECK(cache.step(top, distance, scratch) == expected);
                }
            }
    }
}

TEST_CASE("Paint islands preserve separate colors and differently colored holes", "[CPUSlicing][PaintIslands]")
{
    for (bool mixed : {false, true}) {
        for (bool different_hole : {false, true}) {
            ExPolygons input{square(0, 0, 10000000), square(15000000, 0, 5000000)};
            Polygon    hole = square(2000000, 2000000, 6000000).contour;
            hole.reverse();
            input[0].holes.push_back(hole);
            // An independent island nested inside the first island's hole.
            input.push_back(square(3000000, 3000000, 1000000));
            const auto colors    = colors_for(input, mixed, different_hole);
            const auto reference = segment_painted_layer(input, colors, 3, false);
            const auto split     = segment_painted_layer(input, colors, 3, true);
            REQUIRE(split.size() == reference.size());
            for (size_t i = 0; i < split.size(); ++i) {
                CHECK(diff_ex(reference[i], split[i]).empty());
                CHECK(diff_ex(split[i], reference[i]).empty());
            }
        }
    }
}
