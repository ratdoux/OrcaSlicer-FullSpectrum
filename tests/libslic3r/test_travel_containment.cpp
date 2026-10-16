#include <catch2/catch_test_macros.hpp>
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/GCode/RetractWhenCrossingPerimeters.hpp"
#include "libslic3r/Layer.hpp"
#include <random>

using namespace Slic3r;
namespace {
struct TravelTestLayer : Layer
{
    TravelTestLayer() : Layer(1, nullptr, 0.2, 0.4, 0.3) {}
    ~TravelTestLayer() override = default;
};
} // namespace

TEST_CASE("Indexed travel containment preserves clipping at holes and boundaries", "[CPUSlicing][TravelContainment]")
{
    TravelTestLayer layer, other;
    ExPolygon       island(Polygon{{{0, 0}, {10000000, 0}, {10000000, 10000000}, {0, 10000000}}});
    island.holes.emplace_back(Points{{2000000, 2000000}, {2000000, 8000000}, {8000000, 8000000}, {8000000, 2000000}});
    layer.add_region(nullptr)->slices.surfaces.emplace_back(stInternal, island);
    ExPolygon far = island;
    far.translate(1000000000, 0); // Outside the bounded predicate's coordinate range.
    other.add_region(nullptr)->slices.surfaces.emplace_back(stInternal, far);
    RetractWhenCrossingPerimeters cache;
    auto                          check = [&](const Layer& current, const ExPolygon& shape, const Points& points) {
        Polyline    travel(points);
        BoundingBox bbox = get_extents(travel);
        bbox.offset(SCALED_EPSILON);
        const bool reference = diff_pl(travel, ClipperUtils::clip_clipper_polygons_with_subject_bbox(shape, bbox)).empty();
        CHECK(cache.travel_inside_internal_regions(current, travel) == reference);
    };
    for (const Points& points : std::vector<Points>{{{0, 0}, {10000000, 0}},
                                                    {{1, 1}, {9999999, 1}},
                                                    {{3, 3}, {9999997, 3}},
                                                    {{1000000, 1000000}, {9000000, 9000000}},
                                                    {{3000000, 3000000}, {7000000, 7000000}},
                                                    {{1000000, 1000000}, {1000000, 9000000}, {9000000, 9000000}},
                                                    {{100, 100}, {100, 100}},
                                                    {{-1000, -1000}, {-500, -500}},
                                                    {{2000000, 2000000}, {8000000, 2000000}}})
        check(layer, island, points);
    std::mt19937                       random(43123);
    std::uniform_int_distribution<int> coordinate(-1000000, 11000000);
    for (size_t i = 0; i < 2000; ++i) {
        Points points;
        for (size_t j = 0; j < 2 + i % 4; ++j)
            points.emplace_back(coordinate(random), coordinate(random));
        check(layer, island, points);
    }
    check(other, far, {{1000000100, 100}, {1000000200, 200}});
    check(layer, island, {{100, 100}, {200, 200}});
}
