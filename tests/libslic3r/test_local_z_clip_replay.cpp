#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Compute/LocalZClipDecision.hpp"
#include "libslic3r/LocalZClipper.hpp"
#include "libslic3r/LocalZClipAcceleration.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <optional>

using namespace Slic3r;
namespace {
using Json  = nlohmann::json;
using Clock = std::chrono::steady_clock;
double elapsed_ms(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
bool   same(const Polylines& a, const Polylines& b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](const Polyline& x, const Polyline& y) { return x.points == y.points; });
}
Points read_points(const Json& value)
{
    Points points;
    for (const auto& point : value)
        points.emplace_back(point[0].get<coord_t>(), point[1].get<coord_t>());
    return points;
}
Polylines read_lines(const Json& value)
{
    Polylines lines;
    for (const auto& line : value)
        lines.emplace_back(read_points(line));
    return lines;
}
struct ReplayCase
{
    uint64_t                         id;
    bool                             difference;
    double                           captured_ms;
    ExPolygons                       masks;
    Polylines                        input, reference;
    std::vector<ClipSegmentQuery>    queries;
    std::unique_ptr<LocalZClipper>   clipper;
    std::unique_ptr<LocalZClipIndex> index;
    bool                             eligible = true, proven_empty = false;
};
bool bounded(const Point& p) { return p.x() >= -(1 << 29) && p.x() <= (1 << 29) && p.y() >= -(1 << 29) && p.y() <= (1 << 29); }

struct LocalZSettingsGuard
{
    bool                       preference = LocalZClipAcceleration::preference_enabled();
    std::optional<std::string> environment;
    LocalZSettingsGuard()
    {
        if (const char* value = std::getenv("ORCA_LOCAL_Z_CLIP"))
            environment = value;
        set_environment(nullptr);
    }
    ~LocalZSettingsGuard()
    {
        set_environment(environment ? environment->c_str() : nullptr);
        LocalZClipAcceleration::set_preference_enabled(preference);
    }
    static void set_environment(const char* value)
    {
#ifdef _WIN32
        _putenv_s("ORCA_LOCAL_Z_CLIP", value ? value : "");
#else
        if (value)
            setenv("ORCA_LOCAL_Z_CLIP", value, 1);
        else
            unsetenv("ORCA_LOCAL_Z_CLIP");
#endif
    }
};
} // namespace

TEST_CASE("Local-Z GPU preference is sampled per export and respects explicit overrides", "[LocalZ][SlicingPreferences]")
{
    LocalZSettingsGuard settings;
    LocalZClipAcceleration::set_preference_enabled(false);
    CHECK_FALSE(LocalZClipAcceleration::from_settings());
    LocalZClipAcceleration::set_preference_enabled(true);
    auto active_export = LocalZClipAcceleration::from_settings();
    CHECK(bool(active_export) == VulkanBatch::compiled());
    LocalZClipAcceleration::set_preference_enabled(false);
    CHECK_FALSE(LocalZClipAcceleration::from_settings());
    if (active_export)
        CHECK_FALSE(active_export->validation()); // An existing export keeps its sampled settings.

    settings.set_environment("cpu");
    LocalZClipAcceleration::set_preference_enabled(true);
    CHECK_FALSE(LocalZClipAcceleration::from_settings());
    settings.set_environment("vulkan");
    LocalZClipAcceleration::set_preference_enabled(false);
    auto forced = LocalZClipAcceleration::from_settings();
    REQUIRE(forced);
    CHECK_FALSE(forced->validation());
    settings.set_environment("validate");
    auto validating = LocalZClipAcceleration::from_settings();
    REQUIRE(validating);
    CHECK(validating->validation());
    settings.set_environment("indexed");
    auto indexed = LocalZClipAcceleration::from_settings();
    REQUIRE(indexed);
    CHECK_FALSE(indexed->validation());
    settings.set_environment("indexed_validate");
    indexed = LocalZClipAcceleration::from_settings();
    REQUIRE(indexed);
    CHECK(indexed->validation());
    settings.set_environment("invalid");
    CHECK_THROWS_AS(LocalZClipAcceleration::from_settings(), std::runtime_error);
}

TEST_CASE("Local-Z empty decisions preserve boundaries and holes", "[LocalZ][VulkanCompute]")
{
    const std::vector<ClipMaskEdge> edges{{0, 0, 100, 0},   {100, 0, 100, 100}, {100, 100, 0, 100}, {0, 100, 0, 0},
                                          {30, 30, 30, 70}, {30, 70, 70, 70},   {70, 70, 70, 30},   {70, 30, 30, 30}};
    auto                            decision = [&](std::array<int32_t, 4> p, bool difference) {
        return local_z_segment_proven_empty(edges, {p, 0, uint32_t(edges.size()), uint32_t(difference), 0});
    };
    CHECK(decision({10, 10, 20, 20}, true) == 1);
    CHECK(decision({40, 40, 60, 60}, false) == 1); // Hole.
    CHECK(decision({110, 50, 200, 50}, false) == 1);
    CHECK(decision({-10, 50, 110, 50}, false) == 0);
    CHECK(decision({0, 10, 0, 90}, true) == 0);
    CHECK(decision({1, 10, 1, 90}, true) == 0); // Rounding guard.
    CHECK(decision({40, 40, 40, 40}, false) == 0);
    CHECK(decision({1 << 30, 0, (1 << 30) + 10, 10}, false) == 0);
}

TEST_CASE("Indexed export separates empty rings from an adjacent packed mask", "[LocalZ][ClipIndex]")
{
    LocalZSettingsGuard settings;
    settings.set_environment("indexed");
    auto                acceleration = LocalZClipAcceleration::from_settings();
    const ExPolygons    empty_rings{ExPolygon{}};
    const ExPolygons    solid{ExPolygon(Points{{0, 0}, {100, 0}, {100, 100}, {0, 100}})};
    const LocalZClipper empty_mask(empty_rings), solid_mask(solid);
    ExtrusionPath       path(erPerimeter, 0.08, 0.4f, 0.2f);
    path.polyline = Polyline(Points{{20, 20}, {80, 20}});
    ExtrusionEntityCollection collection;
    collection.append(path);
    acceleration->begin_layer();
    acceleration->add(collection, empty_mask, false);
    acceleration->add(collection, empty_mask, true);
    acceleration->add(collection, solid_mask, false);
    acceleration->add(collection, solid_mask, true);
    acceleration->prepare([] {});
    const auto& entity = *collection.entities.front();
    CHECK(acceleration->proven_empty(entity, &empty_mask, nullptr));
    CHECK_FALSE(acceleration->proven_empty(entity, nullptr, &empty_mask));
    CHECK_FALSE(acceleration->proven_empty(entity, &solid_mask, nullptr));
    CHECK(acceleration->proven_empty(entity, nullptr, &solid_mask));
    CHECK_FALSE(acceleration->proven_empty(entity, &solid_mask, &empty_mask));
}

TEST_CASE("Vulkan Local-Z decisions match bounded integer reference", "[.][LocalZVulkanHardware]")
{
    const std::vector<ClipMaskEdge> edges{{0, 0, 500000000, 0},
                                          {500000000, 0, 500000000, 500000000},
                                          {500000000, 500000000, 0, 500000000},
                                          {0, 500000000, 0, 0},
                                          {100000000, 100000000, 100000000, 200000000},
                                          {100000000, 200000000, 200000000, 200000000},
                                          {200000000, 200000000, 200000000, 100000000},
                                          {200000000, 100000000, 100000000, 100000000}};
    const char*                     device = std::getenv("ORCA_VULKAN_DEVICE");
    std::string                     error;
    auto                            backend = VulkanLocalZClipDecision::create(edges, device ? device : "", error);
    INFO(error);
    REQUIRE(backend);
    std::mt19937                           engine(43871);
    std::uniform_int_distribution<int32_t> position(-536870912, 536870912);
    for (size_t count : {size_t(1), size_t(1003), size_t(VulkanBatch::BATCH_CAPACITY)}) {
        std::vector<ClipSegmentQuery> queries;
        for (size_t i = 0; i < count; ++i)
            queries.push_back(
                {{position(engine), position(engine), position(engine), position(engine)}, 0, 8, uint32_t(i % 2), uint32_t((i / 2) % 2)});
        queries.front().endpoints = {0, 0, 500000000, 0}; // Coincident boundary.
        if (count > 1) {
            queries[1].edge_start = 9;
            queries[2].edge_count = 100;
            queries[3].endpoints  = {0, 0, 0, 0};
            queries[4].endpoints  = {INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX};
            queries[5].endpoints  = {1, 20, 1, 5000};
            queries[6].endpoints  = {499999999, 20, 499999999, 5000};
        }
        std::vector<uint32_t> decisions;
        REQUIRE(backend->compute(queries, decisions, error));
        REQUIRE(decisions.size() == queries.size());
        for (size_t i = 0; i < queries.size(); ++i)
            CHECK(decisions[i] == local_z_segment_decision(edges, queries[i]));
    }
    std::vector<uint32_t> results;
    REQUIRE(backend->compute({}, results, error));
    CHECK(results.empty());
    CHECK_FALSE(backend->compute(std::vector<ClipSegmentQuery>(VulkanBatch::BATCH_CAPACITY + 1), results, error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(VulkanLocalZClipDecision::create(edges, "no-such-vulkan-gpu", error));
    REQUIRE(backend->update_edges(std::vector<ClipMaskEdge>(edges.begin(), edges.begin() + 4), error));
    const std::vector<ClipMaskEdge>     solid(edges.begin(), edges.begin() + 4);
    const std::vector<ClipSegmentQuery> updated{{{150000000, 150000000, 160000000, 160000000}, 0, 4, 1, 0}};
    REQUIRE(backend->compute(updated, results, error));
    REQUIRE(results.size() == 1);
    CHECK(results[0] == local_z_segment_proven_empty(solid, updated[0]));
    CHECK(results[0] == 1);
    CHECK_FALSE(backend->update_edges(std::vector<ClipMaskEdge>(edges.size() + 1), error));
    CHECK_FALSE(backend->update_edges({}, error));
}

TEST_CASE("Indexed Local-Z decisions exactly preserve the reference predicates", "[LocalZ][VulkanCompute][ClipIndex]")
{
    std::mt19937                           engine(73195);
    std::uniform_int_distribution<int32_t> coordinate(-20000, 20000);
    std::vector<ClipMaskEdge>              edges{{INT32_MIN, 0, INT32_MAX, 0}}; // Outside the indexed range.
    for (int i = 0; i < 100; ++i) {
        const int32_t x = coordinate(engine), y = coordinate(engine);
        edges.insert(edges.end(), {{x, y, x + 200, y}, {x + 200, y, x + 200, y + 300}, {x + 200, y + 300, x, y + 300}, {x, y + 300, x, y}});
    }
    edges.insert(edges.end(), {{-100, -100, 100, -100},
                               {100, -100, 100, 100},
                               {100, 100, -100, 100},
                               {-100, 100, -100, -100},
                               {-30, -30, -30, 30},
                               {-30, 30, 30, 30},
                               {30, 30, 30, -30},
                               {30, -30, -30, -30},
                               {5, 5, 5, 5}});
    const uint32_t        count = uint32_t(edges.size() - 1);
    const LocalZClipIndex index(edges, 1, count);
    auto                  compare = [&](const std::array<int32_t, 4>& p) {
        for (uint32_t difference : {0u, 1u})
            for (uint32_t keep : {0u, 1u}) {
                ClipSegmentQuery query{p, 1, count, difference, keep};
                CHECK(index.classify(query) == local_z_segment_decision(edges, query));
                CHECK(local_z_segment_proven_empty(edges, query) == uint32_t(local_z_segment_decision(edges, query) == 1));
            }
    };
    for (int i = 0; i < 12000; ++i)
        compare({coordinate(engine), coordinate(engine), coordinate(engine), coordinate(engine)});
    for (const auto& e : edges)
        for (int delta = -3; delta <= 3; ++delta) {
            if (e[0] == INT32_MIN)
                continue;
            compare({e[0] + delta, e[1], e[2] + delta, e[3]});
            compare({e[0], e[1] + delta, e[2], e[3] + delta});
        }
    compare({0, 0, 0, 0});
    compare({INT32_MIN, 0, INT32_MAX, 1});
    const int32_t                   bound = 1 << 29;
    const std::vector<ClipMaskEdge> extremes{{-bound, -bound, bound, -bound},
                                             {bound, -bound, bound, bound},
                                             {bound, bound, -bound, bound},
                                             {-bound, bound, -bound, -bound}};
    const LocalZClipIndex           extreme_index(extremes, 0, 4);
    for (int delta = 0; delta < 5; ++delta) {
        ClipSegmentQuery q{{-bound + delta, -bound + delta, bound - delta, bound - delta}, 0, 4, 1, 1};
        CHECK(extreme_index.classify(q) == local_z_segment_decision(extremes, q));
    }
    const LocalZClipIndex invalid(edges, 0, uint32_t(edges.size()));
    CHECK(invalid.classify({{1000, 1000, 1001, 1001}, 0, uint32_t(edges.size()), 0, 1}) == 0);
    const LocalZClipIndex empty({}, 0, 0);
    CHECK(empty.classify({{1, 1, 2, 2}, 0, 0, 0, 1}) == 1);
    CHECK(empty.classify({{1, 1, 2, 2}, 0, 0, 1, 1}) == 2);
    CHECK(index.classify({{1, 1, 2, 2}, 0, count, 0, 1}) == 0);
    const LocalZClipIndex invalid_range(edges, uint32_t(edges.size()) + 1, 1);
    CHECK(invalid_range.classify({{1, 1, 2, 2}, uint32_t(edges.size()) + 1, 1, 0, 1}) == 0);
}

TEST_CASE("Conservative empty decisions agree with polygon clipping", "[LocalZ][VulkanCompute]")
{
    std::mt19937                       engine(307117);
    std::uniform_int_distribution<int> coordinate(-10000, 10000), size(20, 1000);
    size_t                             checked_empty = 0;
    for (size_t iteration = 0; iteration < 400; ++iteration) {
        ExPolygons                polygons;
        std::vector<ClipMaskEdge> edges;
        auto                      add_ring = [&](const Points& points) {
            for (size_t i = 0; i < points.size(); ++i) {
                const auto& a = points[i];
                const auto& b = points[(i + 1) % points.size()];
                edges.push_back({int32_t(a.x()), int32_t(a.y()), int32_t(b.x()), int32_t(b.y())});
            }
        };
        for (size_t m = 0; m < 6; ++m) {
            const int x = coordinate(engine), y = coordinate(engine), width = size(engine), height = size(engine);
            ExPolygon polygon(Points{{x, y}, {x + width, y}, {x + width + 5, y + height}, {x, y + height / 2}});
            if (m % 2 == 0) {
                polygon.holes.emplace_back(Points{{x + 3, y + 3}, {x + 3, y + 8}, {x + 8, y + 8}, {x + 8, y + 3}});
                add_ring(polygon.holes.back().points);
            }
            add_ring(polygon.contour.points);
            polygons.push_back(std::move(polygon));
        }
        LocalZClipper clipper(polygons);
        for (size_t path = 0; path < 20; ++path) {
            Points points;
            for (size_t i = 0; i < 2 + path % 5; ++i)
                points.emplace_back(coordinate(engine), coordinate(engine));
            const Polyline line(points);
            for (bool difference : {false, true}) {
                bool empty = true;
                for (size_t i = 1; i < points.size(); ++i)
                    empty = empty && local_z_segment_proven_empty(edges, {{int32_t(points[i - 1].x()), int32_t(points[i - 1].y()),
                                                                           int32_t(points[i].x()), int32_t(points[i].y())},
                                                                          0,
                                                                          uint32_t(edges.size()),
                                                                          uint32_t(difference),
                                                                          0});
                if (empty) {
                    const auto clipped = difference ? clipper.difference(Polylines{line}) : clipper.intersection(line);
                    REQUIRE(clipped.empty());
                    ++checked_empty;
                }
            }
        }
    }
    CHECK(checked_empty > 1000);
}

// Captured inputs are deliberately external, bounded development artifacts.
// Replay reproduces ordered integer output before reporting any opportunity.
TEST_CASE("Replay captured Local-Z export clipping", "[.][LocalZReplay]")
{
    const char* capture = std::getenv("ORCA_LOCAL_Z_REPLAY");
    REQUIRE(capture != nullptr);
    std::ifstream stream(std::filesystem::u8path(capture));
    REQUIRE(stream.good());
    std::vector<ReplayCase>   cases;
    std::vector<ClipMaskEdge> edges;
    std::string               text;
    while (std::getline(stream, text)) {
        const auto row = Json::parse(text);
        REQUIRE(row.at("version") == 1);
        ReplayCase c;
        c.id                    = row.at("id");
        c.difference            = row.at("operation") == "difference";
        c.captured_ms           = row.at("elapsed_ms");
        c.input                 = read_lines(row.at("subject"));
        c.reference             = read_lines(row.at("result"));
        const size_t edge_start = edges.size();
        for (const auto& polygon : row.at("masks")) {
            ExPolygon mask(read_points(polygon.at(0)));
            for (size_t i = 1; i < polygon.size(); ++i)
                mask.holes.emplace_back(read_points(polygon[i]));
            for (const auto& ring : polygon) {
                const Points points = read_points(ring);
                for (size_t i = 0; i < points.size(); ++i) {
                    const auto& a = points[i];
                    const auto& b = points[(i + 1) % points.size()];
                    if (!bounded(a) || !bounded(b))
                        c.eligible = false;
                    edges.push_back({int32_t(a.x()), int32_t(a.y()), int32_t(b.x()), int32_t(b.y())});
                }
            }
            c.masks.push_back(std::move(mask));
        }
        for (const Polyline& line : c.input) {
            if (line.points.size() < 2)
                c.eligible = false;
            for (size_t i = 1; i < line.points.size(); ++i) {
                const auto& a = line.points[i - 1];
                const auto& b = line.points[i];
                if (!bounded(a) || !bounded(b))
                    c.eligible = false;
                c.queries.push_back({{int32_t(a.x()), int32_t(a.y()), int32_t(b.x()), int32_t(b.y())},
                                     uint32_t(edge_start),
                                     uint32_t(edges.size() - edge_start),
                                     uint32_t(c.difference),
                                     1});
            }
        }
        if (c.queries.empty())
            c.eligible = false;
        cases.emplace_back(std::move(c));
    }
    REQUIRE_FALSE(cases.empty());
    for (auto& c : cases)
        c.clipper = std::make_unique<LocalZClipper>(c.masks);
    const auto index_start = Clock::now();
    size_t     index_bytes = 0;
    for (auto& c : cases) {
        if (c.queries.empty())
            continue;
        c.index = std::make_unique<LocalZClipIndex>(edges, c.queries[0].edge_start, c.queries[0].edge_count);
        index_bytes += c.index->memory_bytes();
    }
    const double          index_build_ms = elapsed_ms(index_start);
    std::vector<uint32_t> reference_decisions;
    double                reference_ms = 0, captured_ms = 0, saved_reference_ms = 0, saved_captured_ms = 0;
    size_t                empty = 0, rejected = 0, queries = 0;
    const auto            classify_start = Clock::now();
    for (auto& c : cases) {
        c.proven_empty = c.eligible;
        for (const auto& query : c.queries) {
            ++queries;
            const uint32_t decision = local_z_segment_decision(edges, query);
            reference_decisions.push_back(decision);
            if (decision != 1)
                c.proven_empty = false;
        }
    }
    const double classify_ms = elapsed_ms(classify_start);
    size_t       unchanged = 0, changed = 0, keep_admitted = 0, keep_mismatches = 0;
    for (const auto& c : cases) {
        INFO(c.id);
        const auto   start  = Clock::now();
        const auto   result = c.difference ? c.clipper->difference(c.input) : c.clipper->intersection(c.input.at(0));
        const double ms     = elapsed_ms(start);
        REQUIRE(same(result, c.reference));
        reference_ms += ms;
        captured_ms += c.captured_ms;
        empty += result.empty();
        unchanged += !result.empty() && same(result, c.input);
        changed += !result.empty() && !same(result, c.input);
        bool keep = c.eligible;
        for (const auto& q : c.queries)
            keep = keep && c.index->classify(q) == 2;
        if (keep) {
            ++keep_admitted;
            keep_mismatches += !same(result, c.input);
        }
        if (c.proven_empty) {
            REQUIRE(result.empty());
            ++rejected;
            saved_reference_ms += ms;
            saved_captured_ms += c.captured_ms;
        }
    }
    Json report          = {{"cases", cases.size()},
                            {"edges", edges.size()},
                            {"segments", queries},
                            {"empty_results", empty},
                            {"unchanged_results", unchanged},
                            {"changed_results", changed},
                            {"shadow_keep_admitted", keep_admitted},
                            {"shadow_keep_ordered_mismatches", keep_mismatches},
                            {"cpu_index_build_ms", index_build_ms},
                            {"cpu_index_accounted_bytes", index_bytes},
                            {"proven_empty", rejected},
                            {"reference_ms", reference_ms},
                            {"classify_cpu_ms", classify_ms},
                            {"saved_reference_ms", saved_reference_ms},
                            {"captured_ms", captured_ms},
                            {"saved_captured_ms", saved_captured_ms}};
    Json cpu_repetitions = Json::array();
    auto run_index       = [&]() {
        const auto            start = Clock::now();
        std::vector<uint32_t> decisions;
        decisions.reserve(queries);
        for (const auto& c : cases)
            for (const auto& query : c.queries)
                decisions.push_back(c.index->classify(query));
        const double ms = elapsed_ms(start);
        REQUIRE(decisions == reference_decisions);
        cpu_repetitions.push_back(ms);
    };
    if (std::getenv("ORCA_LOCAL_Z_REPLAY_GPU")) {
        const char* device = std::getenv("ORCA_VULKAN_DEVICE");
        std::string error;
        const auto  setup_start = Clock::now();
        auto        backend     = VulkanLocalZClipDecision::create(edges, device ? device : "", error);
        INFO(error);
        REQUIRE(backend);
        report["gpu_device"]          = backend->device_name();
        report["gpu_setup_upload_ms"] = elapsed_ms(setup_start);
        Json repetitions              = Json::array();
        for (size_t repetition = 0; repetition < 5; ++repetition) {
            if (repetition % 2 == 0)
                run_index();
            const auto                    start = Clock::now();
            std::vector<uint32_t>         decisions, batch_results;
            std::vector<ClipSegmentQuery> batch;
            batch.reserve(VulkanBatch::BATCH_CAPACITY);
            decisions.reserve(queries);
            for (const auto& c : cases)
                for (const auto& query : c.queries) {
                    batch.push_back(query);
                    if (batch.size() == VulkanBatch::BATCH_CAPACITY) {
                        REQUIRE(backend->compute(batch, batch_results, error));
                        decisions.insert(decisions.end(), batch_results.begin(), batch_results.end());
                        batch.clear();
                    }
                }
            if (!batch.empty()) {
                REQUIRE(backend->compute(batch, batch_results, error));
                decisions.insert(decisions.end(), batch_results.begin(), batch_results.end());
            }
            std::vector<bool> discard(cases.size());
            size_t            index = 0;
            for (size_t i = 0; i < cases.size(); ++i) {
                discard[i] = cases[i].eligible;
                for (size_t q = 0; q < cases[i].queries.size(); ++q)
                    if (decisions[index++] != 1)
                        discard[i] = false;
            }
            const double gpu_ms = elapsed_ms(start);
            REQUIRE(index == decisions.size());
            index = 0;
            for (size_t i = 0; i < cases.size(); ++i) {
                INFO(cases[i].id);
                REQUIRE(discard[i] == cases[i].proven_empty);
                for (const auto& query : cases[i].queries)
                    REQUIRE(decisions[index++] == local_z_segment_decision(edges, query));
            }
            double fallback_ms = 0;
            for (size_t i = 0; i < cases.size(); ++i) {
                const auto& c              = cases[i];
                const auto  fallback_start = Clock::now();
                const auto  result         = discard[i] ? Polylines{} :
                                                          (c.difference ? c.clipper->difference(c.input) : c.clipper->intersection(c.input.at(0)));
                fallback_ms += elapsed_ms(fallback_start);
                REQUIRE(same(result, c.reference));
            }
            repetitions.push_back(
                {{"gpu_classification_ms", gpu_ms}, {"cpu_fallback_ms", fallback_ms}, {"combined_ms", gpu_ms + fallback_ms}});
            if (repetition % 2 != 0)
                run_index();
        }
        report["gpu_repetitions"] = std::move(repetitions);
    } else {
        for (int i = 0; i < 5; ++i)
            run_index();
    }
    report["indexed_cpu_query_ms"] = std::move(cpu_repetitions);
    std::cout << "Local-Z replay " << report.dump() << '\n';
    if (const char* output = std::getenv("ORCA_LOCAL_Z_REPLAY_REPORT")) {
        std::ofstream out(std::filesystem::u8path(output));
        out << report.dump(2) << '\n';
        REQUIRE(out.good());
    }
}
