#include <catch2/catch_test_macros.hpp>

#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/MixedFilamentLayerTracker.hpp"

using namespace Slic3r;

namespace {
ExPolygon rectangle(coord_t x0, coord_t y0, coord_t x1, coord_t y1)
{
    return ExPolygon(Polygon(Points{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}));
}
} // namespace

TEST_CASE("Mixed painted components retain cadence through splits and merges", "[CPUSlicing][MixedFilament]")
{
    // All layers remain alive, as in the segmentation pass that owns the masks.
    const std::vector<ExPolygons>       layers{{rectangle(0, 0, 100, 100)},
                                               {rectangle(0, 0, 40, 100), rectangle(60, 0, 100, 100), rectangle(200, 0, 300, 100)},
                                               {rectangle(0, 0, 300, 100)},
                                               {},
                                               {rectangle(0, 0, 100, 100)}};
    const std::vector<std::vector<int>> expected{{0}, {1, 1, 0}, {2}, {}, {0}};
    MixedFilamentLayerTracker           tracker;
    std::vector<int>                    indices;
    for (size_t i = 0; i < layers.size(); ++i) {
        tracker.track_layer(layers[i], int(i), indices);
        REQUIRE(indices == expected[i]);
    }
}

TEST_CASE("Mixed painted cadence uses polygon overlap including holes", "[CPUSlicing][MixedFilament]")
{
    ExPolygon ring = rectangle(0, 0, 100, 100);
    Polygon   hole = rectangle(20, 20, 80, 80).contour;
    hole.reverse();
    ring.holes.push_back(hole);
    const ExPolygons          first{ring};
    const ExPolygons          second{rectangle(30, 30, 70, 70), rectangle(5, 5, 15, 15), ExPolygon{}};
    MixedFilamentLayerTracker tracker;
    std::vector<int>          indices;
    tracker.track_layer(first, 5, indices);
    tracker.track_layer(second, 6, indices);
    REQUIRE(indices == std::vector<int>{0, 1, -1});
}

TEST_CASE("Mixed painted channels have independent activation runs", "[CPUSlicing][MixedFilament]")
{
    const ExPolygons          masks{rectangle(0, 0, 100, 100)};
    MixedFilamentLayerTracker first, second;
    std::vector<int>          first_indices, second_indices;
    first.track_layer(masks, 0, first_indices);
    second.track_layer(masks, 2, second_indices);
    first.track_layer(masks, 3, first_indices);
    second.track_layer(masks, 3, second_indices);
    REQUIRE(first_indices == std::vector<int>{3});
    REQUIRE(second_indices == std::vector<int>{1});
}

TEST_CASE("Read-only extrusion flattening borrows leaves without changing order", "[CPUSlicing][GCode]")
{
    ExtrusionPath path(erPerimeter, 0.08, 0.4f, 0.2f);
    path.polyline = Polyline(Points{{0, 0}, {100, 0}, {100, 100}});
    ExtrusionEntityCollection inner;
    inner.no_sort = true;
    inner.append(ExtrusionLoop(path));
    inner.append(ExtrusionMultiPath(path));
    ExtrusionEntityCollection source;
    source.append(path);
    source.append(inner);
    source.append(ExtrusionEntityCollection{});
    source.append(path);

    const auto  leaves = source.flatten_refs();
    const auto* nested = static_cast<const ExtrusionEntityCollection*>(source.entities[1]);
    REQUIRE(leaves == std::vector<const ExtrusionEntity*>{source.entities[0], nested->entities[0], nested->entities[1], source.entities[3]});
    const auto copied = source.flatten(false);
    REQUIRE(leaves.size() == copied.entities.size());
    for (size_t i = 0; i < leaves.size(); ++i) {
        CHECK(leaves[i] != copied.entities[i]);
        CHECK(leaves[i]->role() == copied.entities[i]->role());
        CHECK(leaves[i]->is_loop() == copied.entities[i]->is_loop());
        Points original_points, copied_points;
        leaves[i]->collect_points(original_points);
        copied.entities[i]->collect_points(copied_points);
        CHECK(original_points == copied_points);
    }
    REQUIRE(ExtrusionEntityCollection{}.flatten_refs().empty());
}
