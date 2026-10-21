#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/LocalZClipper.hpp"
#include "libslic3r/LocalZPathRouter.hpp"

#include <algorithm>
#include <random>
#include <stdexcept>
#include <tbb/parallel_for.h>

using namespace Slic3r;

TEST_CASE("Local-Z direct reuse preserves owned geometry and component volume", "[LocalZ][LocalZReuse]")
{
    const ExPolygons    masks{ExPolygon(Points{{0, 0}, {10000000, 0}, {10000000, 10000000}, {0, 10000000}})};
    const LocalZClipper clipper(masks);
    ExtrusionPath       source(erExternalPerimeter, 0.08, 0.4f, 0.2f);
    source.polyline  = Polyline(Points{{1000000, 1000000}, {9000000, 1000000}, {9000000, 9000000}, {1000000, 1000000}});
    source.inset_idx = 3;
    source.set_reverse();
    source.set_force_no_extrusion(true);
    source.polyline.fitting_result.emplace_back(); // Existing clipping discards fitted arcs.
    LocalZPathRouter router(true);
    double           total_volume = 0.;
    for (double height : {0.04, 0.16}) {
        std::vector<ExtrusionPath> result;
        router.append(source, &clipper, nullptr, height, result);
        REQUIRE(result.size() == 1);
        const auto& path = result.front();
        CHECK(path.polyline.points == source.polyline.points); // Includes the authored seam and direction.
        CHECK(path.polyline.fitting_result.empty());
        CHECK(path.mm3_per_mm == Catch::Approx(source.mm3_per_mm * height / source.height));
        CHECK(path.height == Catch::Approx(height));
        CHECK(path.width == source.width);
        CHECK(path.role() == source.role());
        CHECK(path.inset_idx == source.inset_idx);
        CHECK_FALSE(path.can_reverse());
        CHECK(path.is_force_no_extrusion());
        total_volume += path.total_volume();
    }
    CHECK(total_volume == Catch::Approx(source.total_volume()).epsilon(1e-6));
    CHECK(source.height == 0.2f);
    CHECK(source.mm3_per_mm == 0.08);
    CHECK(source.polyline.fitting_result.size() == 1);
    CHECK(router.statistics().reused == 2);
    CHECK(router.statistics().clipped == 0);
}

TEST_CASE("Local-Z ownership requires all segments and both pass masks", "[LocalZ][LocalZReuse]")
{
    ExPolygon ring(Points{{0, 0}, {1000, 0}, {1000, 1000}, {0, 1000}});
    ring.holes.emplace_back(Points{{400, 400}, {400, 600}, {600, 600}, {600, 400}});
    const ExPolygons    masks{ring};
    const ExPolygons    exclusion{ExPolygon(Points{{700, 0}, {1000, 0}, {1000, 1000}, {700, 1000}})};
    const LocalZClipper clipper(masks), exclude(exclusion);
    CHECK(clipper.can_reuse_whole_path(Polyline(Points{{100, 100}, {900, 100}})));
    CHECK_FALSE(clipper.can_reuse_whole_path(Polyline(Points{{100, 500}, {900, 500}}))); // Crosses hole.
    CHECK_FALSE(clipper.can_reuse_whole_path(Polyline(Points{{0, 100}, {0, 900}})));
    CHECK_FALSE(clipper.can_reuse_whole_path(Polyline(Points{{1, 100}, {1, 900}})));
    CHECK_FALSE(clipper.can_reuse_whole_path(Polyline(Points{{100, 100}, {100, 100}})));
    CHECK_FALSE(clipper.can_reuse_whole_path(Polyline(Points{{1 << 30, 0}, {(1 << 30) + 10, 20}})));
    for (const Points& points :
         {Points{{100, 100}, {900, 100}}, Points{{100, 500}, {900, 500}}, Points{{0, 100}, {0, 900}}, Points{{1100, 100}, {1200, 100}}}) {
        ExtrusionPath source(erInternalInfill, 0.07, 0.42f, 0.2f);
        source.polyline = Polyline(points);
        LocalZPathRouter           router(true), reference(false);
        std::vector<ExtrusionPath> result, expected;
        router.append(source, &clipper, &exclude, 0.04, result);
        reference.append(source, &clipper, &exclude, 0.04, expected);
        CHECK(router.statistics().reused == 0);
        REQUIRE(result.size() == expected.size());
        for (size_t i = 0; i < result.size(); ++i) {
            CHECK(result[i].polyline.points == expected[i].polyline.points);
            CHECK(result[i].height == expected[i].height);
            CHECK(result[i].mm3_per_mm == expected[i].mm3_per_mm);
        }
    }
    LocalZPathRouter router(true);
    ExtrusionPath    base(erPerimeter, 0.08, 0.4f, 0.2f);
    base.polyline = Polyline(Points{{1100, 100}, {1200, 100}});
    std::vector<ExtrusionPath> result;
    router.append(base, nullptr, &clipper, 0., result);
    REQUIRE(result.size() == 1);
    CHECK(router.statistics().reused == 1);
    CHECK(result.front().height == base.height);
    CHECK(result.front().mm3_per_mm == base.mm3_per_mm);
}

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

TEST_CASE("Local-Z reused paths match independent full-mask coverage and volume", "[LocalZ][LocalZReuse]")
{
    ExPolygon ring = box(0, 0, 10000, 10000);
    ring.holes.push_back(box(3500, 3500, 3000, 3000).contour);
    ring.holes.back().reverse();
    const ExPolygons                   masks{ring, box(8000, 0, 5000, 10000), box(-15000, 0, 5000, 10000)};
    const LocalZClipper                clipper(masks);
    std::mt19937                       random(518429);
    std::uniform_int_distribution<int> coordinate(-16000, 14000);
    LocalZPathRouter                   router(true);
    for (size_t i = 0; i < 2000; ++i) {
        ExtrusionPath source(erInternalInfill, 0.08, 0.4f, 0.2f);
        source.polyline = Polyline(Points{{coordinate(random), coordinate(random)}, {coordinate(random), coordinate(random)}});
        if (i % 4 == 0)
            source.polyline.points.emplace_back(coordinate(random), coordinate(random));
        const bool                 difference = i % 2 != 0;
        const size_t               reused     = router.statistics().reused;
        std::vector<ExtrusionPath> result;
        router.append(source, difference ? nullptr : &clipper, difference ? &clipper : nullptr, 0.05, result);
        const Polylines input{source.polyline};
        const Polylines expected      = difference ? diff_pl(input, masks) : intersection_pl(input, masks);
        double          result_length = 0., expected_length = 0., volume = 0.;
        for (const auto& path : result) {
            result_length += path.length();
            volume += path.total_volume();
        }
        for (const auto& path : expected)
            expected_length += path.length();
        CHECK(result_length == Catch::Approx(expected_length).margin(1e-6));
        CHECK(volume == Catch::Approx(unscale<double>(expected_length) * source.mm3_per_mm * 0.05 / source.height).margin(1e-12));
        if (router.statistics().reused != reused) {
            REQUIRE(result.size() == 1);
            CHECK(result.front().polyline.points == source.polyline.points);
            CHECK((difference ? intersection_pl(input, masks) : diff_pl(input, masks)).empty());
        }
    }
    CHECK(router.statistics().reused > 50);
    CHECK(router.statistics().clipped > 50);
}

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

TEST_CASE("Local-Z indexed empty paths preserve winding, holes and rounding guards", "[LocalZ][CPUSlicing]")
{
    ExPolygons masks;
    for (coord_t i = 0; i < 12; ++i)
        masks.push_back(box(i * 1000, 0, 800, 800));
    masks.front().holes.push_back(box(200, 200, 400, 400).contour);
    masks.front().holes.back().reverse();
    // Opposite winding cancels under Clipper's nonzero fill rule.
    masks.push_back(masks[2]);
    masks.back().contour.reverse();
    const Polylines subjects{Polyline(Points{{100, 100}, {150, 100}, {150, 150}}),
                             Polyline(Points{{300, 300}, {400, 400}}),
                             Polyline(Points{{100, 400}, {700, 400}}), // Endpoints inside, but crosses a hole.
                             Polyline(Points{{2100, 100}, {2700, 700}}),
                             Polyline(Points{{-100, -100}, {-50, -50}}),
                             Polyline(Points{{1, 10}, {1, 100}}),
                             Polyline(Points{{2, 10}, {2, 100}}),
                             Polyline(Points{{3, 10}, {3, 100}}),
                             Polyline(Points{{0, 10}, {0, 100}}),
                             Polyline(Points{{100, 100}, {100, 100}, {150, 150}}),
                             Polyline(Points{{100, 100}, {150, 100}, {150, 150}, {100, 100}})};
    check_paths(masks, subjects);
    const LocalZClipper prepared(masks);
    tbb::parallel_for(size_t(0), size_t(64), [&](size_t i) {
        const Polylines input{subjects[i % subjects.size()]};
        // Catch assertions are not thread-safe; compare after joining.
        if (!same_paths(prepared.difference(input), diff_pl(input, masks)))
            throw std::runtime_error("Concurrent Local-Z empty classification mismatch");
    });
    for (ExPolygon& mask : masks) {
        mask.translate(Point((1 << 29) - 6000, 0));
    }
    Polylines translated = subjects;
    for (Polyline& line : translated)
        line.translate(Point((1 << 29) - 6000, 0));
    check_paths(masks, translated); // One ineligible mask disables the entire index.
}

TEST_CASE("Local-Z repeated masks cache exact ordered results without aliasing", "[LocalZ][CPUSlicing][LocalZCache]")
{
    ExPolygon ring = box(0, 0, 100, 100);
    ring.holes.push_back(box(20, 20, 60, 60).contour);
    ring.holes.back().reverse();
    const ExPolygons masks{ring, box(200, 0, 100, 100)}, duplicate = masks;
    ExPolygons       changed = masks;
    changed.front().holes.clear();
    ExPolygons reordered = masks;
    std::reverse(reordered.begin(), reordered.end());
    LocalZClipCache     cache;
    const LocalZClipper first(masks, &cache), second(duplicate, &cache), other(changed, &cache), ordered(reordered, &cache);
    CHECK(cache.statistics().masks == 3);
    const Polylines subjects{Polyline(Points{{-10, 50}, {310, 50}}),
                             Polyline(Points{{310, 50}, {-10, 50}}),
                             Polyline(Points{{30, 30}, {70, 70}}),
                             Polyline(Points{{0, 0}, {100, 0}, {100, 100}, {0, 0}}),
                             Polyline(Points{{-10, -10}, {10, 10}, {10, 10}, {50, 50}, {310, 30}}),
                             Polyline(Points{{400, 10}, {450, 10}, {450, 10}}),
                             Polyline(Points{}),
                             Polyline(Points{{0, 0}})};
    for (const Polyline& path : subjects) {
        const Polylines expected = intersection_pl(Polylines{path}, masks);
        Polylines       result   = first.intersection(path);
        REQUIRE(same_paths(result, expected));
        result.clear(); // A caller cannot modify the cached value.
        const size_t hits = cache.statistics().hits;
        CHECK(same_paths(second.intersection(path), expected));
        CHECK(cache.statistics().hits == hits + 1);
        CHECK(same_paths(other.intersection(path), intersection_pl(Polylines{path}, changed)));
        CHECK(same_paths(ordered.intersection(path), intersection_pl(Polylines{path}, reordered)));
        CHECK(same_paths(first.difference(Polylines{path}), diff_pl(Polylines{path}, masks)));
        CHECK(same_paths(second.difference(Polylines{path}), diff_pl(Polylines{path}, masks)));
    }
    CHECK(same_paths(first.difference(subjects), diff_pl(subjects, masks)));
    CHECK(same_paths(second.difference(subjects), diff_pl(subjects, masks)));
    Polylines reverse = subjects;
    std::reverse(reverse.begin(), reverse.end());
    CHECK(same_paths(second.difference(reverse), diff_pl(reverse, masks)));
    std::vector<Polylines> results(64);
    tbb::parallel_for(size_t(0), results.size(), [&](size_t i) { results[i] = second.intersection(subjects[i % subjects.size()]); });
    for (size_t i = 0; i < results.size(); ++i)
        CHECK(same_paths(results[i], intersection_pl(Polylines{subjects[i % subjects.size()]}, masks)));
}

TEST_CASE("Local-Z clipping cache falls back when its storage budget is exhausted", "[LocalZ][CPUSlicing][LocalZCache]")
{
    const ExPolygons masks{box(0, 0, 100, 100)};
    for (size_t budget : {size_t(0), size_t(1024)}) {
        LocalZClipCache     cache(budget);
        const LocalZClipper first(masks, &cache), second(masks, &cache);
        for (coord_t y = 1; y < 90; ++y) {
            const Polyline  path(Points{{-10, y}, {110, y}});
            const Polylines expected = intersection_pl(Polylines{path}, masks);
            CHECK(same_paths(first.intersection(path), expected));
            CHECK(same_paths(second.intersection(path), expected));
            CHECK(cache.statistics().bytes <= budget);
        }
        CHECK(cache.statistics().entries < 10);
        if (budget)
            CHECK(cache.statistics().hits > 0);
        else
            CHECK(cache.statistics().entries == 0);
    }
}

TEST_CASE("FullSpectrum reuse respects role-specific exclusions and preserves loop input", "[LocalZ][LocalZReuse]")
{
    const ExPolygons    include_masks{box(0, 0, 1000, 1000)};
    const ExPolygons    external_masks{box(100, 100, 800, 800)};
    const ExPolygons    non_external_masks{box(300, 300, 400, 400)};
    const LocalZClipper include(include_masks), external(external_masks), non_external(non_external_masks);
    ExtrusionPath       source(erExternalPerimeter, .08, .4f, .2f);
    source.polyline = Polyline(Points{{200, 200}, {800, 200}, {800, 800}, {200, 800}, {200, 200}});
    source.polyline.fitting_result.emplace_back();
    const ExtrusionLoop loop(source);
    LocalZPathRouter    router(true), reference(false);
    // Intact loops take only the proof; FullSpectrum still clones the loop and
    // places its seam later. The proof must not mutate arcs, vertices or flow.
    CHECK(router.can_reuse(loop.paths.front().polyline, &include, nullptr));
    CHECK_FALSE(reference.can_reuse(loop.paths.front().polyline, &include, nullptr));
    CHECK(loop.paths.front().polyline.points == source.polyline.points);
    CHECK(loop.paths.front().polyline.fitting_result.size() == 1);
    CHECK(loop.paths.front().height == source.height);
    // One geometric path can belong to the adaptive external pass but remain
    // in the ordinary base pass for another role. Never share that decision.
    CHECK_FALSE(router.can_reuse(source.polyline, nullptr, &external));
    CHECK(router.can_reuse(source.polyline, nullptr, &non_external));
    std::vector<ExtrusionPath> removed, retained;
    router.append(source, nullptr, &external, 0., removed);
    source.set_extrusion_role(erPerimeter);
    router.append(source, nullptr, &non_external, 0., retained);
    CHECK(removed.empty());
    REQUIRE(retained.size() == 1);
    CHECK(retained.front().polyline.points == source.polyline.points);
    CHECK(retained.front().role() == erPerimeter);
    CHECK(retained.front().mm3_per_mm == source.mm3_per_mm);
    CHECK(retained.front().height == source.height);
}
