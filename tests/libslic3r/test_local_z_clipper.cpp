#include <catch2/catch_test_macros.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/LocalZClipper.hpp"

#include <algorithm>
#include <random>
#include <tbb/parallel_for.h>

using namespace Slic3r;

namespace {
ExPolygon box(coord_t x, coord_t y, coord_t width, coord_t height)
{
    return ExPolygon(Points{{x, y}, {x + width, y}, {x + width, y + height}, {x, y + height}});
}

bool same_paths(const Polylines& lhs, const Polylines& rhs)
{
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](const Polyline& a, const Polyline& b) { return a.points == b.points; });
}

void check_paths(const ExPolygons& masks, const Polylines& subjects)
{
    const LocalZClipper prepared(masks);
    for (const Polyline& subject : subjects) {
        const Polylines reference = intersection_pl(Polylines{subject}, masks);
        CHECK(same_paths(prepared.intersection(subject), reference));
        CHECK(same_paths(LocalZClipper::clip_path(subject, &prepared, nullptr), masks.empty() ? Polylines{subject} : reference));
        CHECK(same_paths(LocalZClipper::clip_path(subject, nullptr, &prepared),
                         masks.empty() ? Polylines{subject} : diff_pl(Polylines{subject}, masks)));
        CHECK(prepared.intersects(subject) ==
              std::any_of(reference.begin(), reference.end(), [](const Polyline& line) { return line.is_valid(); }));
        CHECK(same_paths(prepared.difference(Polylines{subject}), diff_pl(Polylines{subject}, masks)));
    }
    CHECK(same_paths(prepared.difference(subjects), diff_pl(subjects, masks)));
}
} // namespace

TEST_CASE("Local-Z spatial clipping preserves exact ordered paths and boundary behavior", "[LocalZ][CPUSlicing]")
{
    ExPolygon ring = box(0, 0, 100, 100);
    ring.holes.push_back(box(20, 20, 60, 60).contour);
    ring.holes.back().reverse();
    const ExPolygons masks{ring, box(200, 0, 100, 100), box(-100, 300, 50, 50), box(90, 90, 20, 20)};
    const Polylines  subjects{Polyline(Points{}),
                             Polyline(Points{{0, 0}}),
                             Polyline(Points{{0, 0}, {0, 0}}),
                             Polyline(Points{{-10, 50}, {310, 50}}),
                             Polyline(Points{{310, 50}, {-10, 50}}),
                             Polyline(Points{{30, 30}, {70, 70}}), // Fully inside a hole.
                             Polyline(Points{{-10, 0}, {110, 0}}),
                             Polyline(Points{{0, -10}, {0, 110}}),
                             Polyline(Points{{-10, -10}, {0, 0}, {-10, 10}}),                    // Corner touch only.
                             Polyline(Points{{20, 20}, {80, 20}, {80, 80}, {20, 80}, {20, 20}}), // Hole boundary.
                             Polyline(Points{{-10, -10}, {110, -10}, {110, 110}, {-10, 110}, {-10, -10}}),
                             Polyline(Points{{400, 20}, {500, 20}, {500, 20}, {450, -20}}), // Disjoint; Clipper normalizes this.
                             Polyline(Points{{0, 50}, {25, 50}, {25, 50}, {50, 50}, {90, 90}, {0, 50}})};
    check_paths(masks, subjects);
    check_paths(ExPolygons{}, subjects);
    check_paths(ExPolygons{ExPolygon{}}, subjects);
}

TEST_CASE("Local-Z spatial clipping matches full masks over deterministic complex paths", "[LocalZ][CPUSlicing]")
{
    std::mt19937                       rng(73124);
    std::uniform_int_distribution<int> position(-1000, 1000);
    std::uniform_int_distribution<int> size(1, 200);
    for (size_t iteration = 0; iteration < 800; ++iteration) {
        ExPolygons masks;
        for (size_t i = 0; i < 40; ++i) {
            const coord_t x = position(rng), y = position(rng), width = size(rng), height = size(rng);
            ExPolygon     mask = box(x, y, width, height);
            if (width > 10 && height > 10 && i % 3 == 0) {
                mask.holes.push_back(box(x + 2, y + 2, width - 4, height - 4).contour);
                mask.holes.back().reverse();
            }
            masks.push_back(std::move(mask));
        }
        Polylines subjects;
        for (size_t i = 0; i < 12; ++i) {
            Points points;
            for (size_t j = 0; j < 7; ++j)
                points.emplace_back(position(rng), position(rng));
            if (i % 2 == 0)
                points.push_back(points.front());
            subjects.emplace_back(std::move(points));
        }
        check_paths(masks, subjects);
    }
}

TEST_CASE("Local-Z prepared masks support independent concurrent queries", "[LocalZ][CPUSlicing]")
{
    const coord_t          origin = coord_t(2000000000);
    const ExPolygons       masks{box(origin, origin, 1000, 1000), box(-origin, -origin, 1000, 1000)};
    const LocalZClipper    prepared(masks);
    const Polyline         subject(Points{{origin - 100, origin + 500}, {origin + 1100, origin + 500}});
    const Polylines        expected = intersection_pl(Polylines{subject}, masks);
    std::vector<Polylines> results(128);
    tbb::parallel_for(size_t(0), results.size(), [&](size_t i) { results[i] = prepared.intersection(subject); });
    for (const Polylines& result : results)
        CHECK(same_paths(result, expected));
    check_paths(masks, Polylines{subject});
}
