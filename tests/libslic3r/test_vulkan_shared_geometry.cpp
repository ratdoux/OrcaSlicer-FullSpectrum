#include <catch2/catch_test_macros.hpp>
#include "libslic3r/Compute/SeamNeighbors.hpp"
#include "libslic3r/Compute/LocalZClipDecision.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/PolygonCompute.hpp"
#include "libslic3r/KDTreeIndirect.hpp"
#include <cstdlib>
#include <random>
#include <fstream>
#include <chrono>
#include <iostream>
#include <limits>
#include <cmath>
#include <nlohmann/json.hpp>
using namespace Slic3r;
namespace {
struct Coordinate
{
    const std::vector<Vec3f>& points;
    float                     operator()(size_t i, size_t d) const { return points[i][d]; }
};
std::vector<ClipMaskEdge> edges_of(const Polygons& polygons)
{
    std::vector<ClipMaskEdge> edges;
    for (const auto& p : polygons)
        for (size_t i = 0; i < p.points.size(); ++i) {
            const auto& a = p.points[i];
            const auto& b = p.points[(i + 1) % p.points.size()];
            edges.push_back({int32_t(a.x()), int32_t(a.y()), int32_t(b.x()), int32_t(b.y())});
        }
    return edges;
}
bool cpu_empty(const Polygons& subject, const Polygons& clip, bool difference)
{
    auto         edges = edges_of(subject);
    const size_t n     = edges.size();
    auto         other = edges_of(clip);
    edges.insert(edges.end(), other.begin(), other.end());
    const auto queries = closed_polygon_empty_queries(edges, n, difference);
    return !queries.empty() &&
           std::all_of(queries.begin(), queries.end(), [&](const auto& q) { return local_z_segment_proven_empty(edges, q) == 1; });
}
Polygon box(int x, int y, int w, int h) { return Polygon{{{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}}; }
} // namespace
TEST_CASE("Seam neighborhood admission keeps degenerate searches on CPU", "[VulkanCompute]")
{
    CHECK(VulkanSeamNeighbors::supports_query({1, 2, 3, 0.4f}));
    CHECK_FALSE(VulkanSeamNeighbors::supports_query({1, 2, 3, 0}));
    CHECK_FALSE(VulkanSeamNeighbors::supports_query({1, 2, 3, -1}));
    CHECK_FALSE(VulkanSeamNeighbors::supports_query({1, 2, 3, std::numeric_limits<float>::max()}));
    CHECK_FALSE(VulkanSeamNeighbors::supports_query({std::numeric_limits<float>::quiet_NaN(), 2, 3, 1}));
    CHECK_FALSE(VulkanSeamNeighbors::supports_query({1, 2, 3, std::numeric_limits<float>::infinity()}));
}
TEST_CASE("Closed polygon rejection handles enclosing islands and holes", "[VulkanCompute][PolygonCompute]")
{
    Polygons outer{box(0, 0, 1000, 1000)}, inner{box(100, 100, 100, 100)}, away{box(2000, 2000, 100, 100)};
    CHECK(cpu_empty(outer, away, false));
    CHECK_FALSE(cpu_empty(outer, inner, false));
    CHECK_FALSE(cpu_empty(inner, outer, false));
    CHECK(cpu_empty(inner, outer, true));
    CHECK_FALSE(cpu_empty(outer, inner, true));
    Polygon hole = box(50, 50, 800, 800);
    hole.reverse();
    Polygons donut = outer;
    donut.push_back(hole);
    CHECK(cpu_empty(inner, donut, false));
    CHECK_FALSE(cpu_empty(inner, donut, true));
    CHECK_FALSE(cpu_empty(outer, donut, true));
    CHECK_FALSE(cpu_empty(outer, {box(1000, 0, 100, 100)}, false));
    CHECK_FALSE(cpu_empty(outer, {box(999, 0, 100, 100)}, false));
    CHECK_FALSE(cpu_empty(outer, {box(1001, 0, 100, 100)}, false));
    std::mt19937 random(78192);
    for (size_t i = 0; i < 1500; ++i) {
        auto make = [&] {
            Polygons p{box(int(random() % 2500) - 1250, int(random() % 2500) - 1250, 50 + random() % 800, 50 + random() % 800)};
            if (random() % 2)
                p.push_back(box(int(random() % 2500) - 1250, int(random() % 2500) - 1250, 50 + random() % 800, 50 + random() % 800));
            return p;
        };
        const auto a = make(), b = make();
        for (bool difference : {false, true})
            if (cpu_empty(a, b, difference))
                CHECK((difference ? diff(a, b) : intersection(a, b)).empty());
    }
}
TEST_CASE("Vulkan seam neighborhoods preserve CPU traversal order", "[.][SharedVulkanHardware]")
{
    std::mt19937       random(91872);
    std::vector<Vec3f> points;
    for (size_t i = 0; i < 1500; ++i)
        points.emplace_back(float(int(random() % 2000) - 1000) / 30, float(int(random() % 2000) - 1000) / 30, float(random() % 3));
    KDTreeIndirect<3, float, Coordinate> tree(Coordinate{points}, points.size());
    std::vector<SeamNeighborNode>        nodes(tree.node_indices().size());
    for (size_t i = 0; i < nodes.size(); ++i)
        if (tree.node_indices()[i] != decltype(tree)::npos) {
            auto id           = tree.node_indices()[i];
            auto p            = points[id];
            nodes[i].position = {p.x(), p.y(), p.z(), 0};
            nodes[i].meta[0]  = uint32_t(id);
        }
    std::string error;
    const char* device = std::getenv("ORCA_VULKAN_DEVICE");
    auto        gpu    = VulkanSeamNeighbors::create(nodes.size(), device ? device : "", error);
    INFO(error);
    REQUIRE(gpu);
    REQUIRE(gpu->update(nodes, error));
    std::vector<SeamNeighborQuery> queries;
    for (size_t i = 0; i < 1003; ++i)
        queries.push_back(
            {float(int(random() % 2000) - 1000) / 30, float(int(random() % 2000) - 1000) / 30, 0, 0.1f + float(random() % 300) / 30});
    queries.push_back({0, 0, 0, 10000});
    queries.push_back({points[0].x() + 1, points[0].y(), points[0].z(), 1});
    std::vector<SeamNeighborResult> results;
    REQUIRE(gpu->compute(queries, results, error));
    REQUIRE(results.size() == queries.size());
    size_t checked = 0;
    for (size_t i = 0; i < queries.size(); ++i)
        if (!results[i].fallback) {
            const auto&         q        = queries[i];
            auto                expected = find_nearby_points(tree, Vec3f(q[0], q[1], q[2]), q[3]);
            std::vector<size_t> actual(results[i].indices.begin(), results[i].indices.begin() + results[i].count);
            CHECK(actual == expected);
            ++checked;
        }
    // Quantized coordinates intentionally produce radius-boundary fallbacks.
    CHECK(checked > 750);
    CHECK(results[1003].fallback != 0);
    CHECK(results[1004].fallback != 0);
    queries.assign(VulkanBatch::BATCH_CAPACITY + 1, {0, 0, 0, 1});
    CHECK_FALSE(gpu->compute(queries, results, error));
    CHECK(results.empty());
    gpu.reset();
    VulkanBatch::shutdown();
}
TEST_CASE("Replay captured shared polygon operations on Vulkan", "[.][SharedPolygonReplay]")
{
    const char* path = std::getenv("ORCA_POLYGON_REPLAY");
    REQUIRE(path);
    std::ifstream input(path);
    REQUIRE(input.good());
    std::string error;
    const char* device = std::getenv("ORCA_VULKAN_DEVICE");
    auto        gpu    = VulkanLocalZClipDecision::create(std::vector<ClipMaskEdge>(16384), device ? device : "", error);
    INFO(error);
    REQUIRE(gpu);
    std::string    line;
    size_t         cases = 0, rejected = 0;
    nlohmann::json timings;
    using Clock  = std::chrono::steady_clock;
    auto elapsed = [](Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); };
    auto decode  = [](const nlohmann::json& rings) {
        Polygons result;
        for (const auto& ring : rings) {
            Points points;
            for (const auto& p : ring)
                points.emplace_back(p[0].get<coord_t>(), p[1].get<coord_t>());
            result.emplace_back(std::move(points));
        }
        return result;
    };
    while (std::getline(input, line)) {
        auto         row     = nlohmann::json::parse(line);
        auto         subject = decode(row["subject"]), clip = decode(row["clip"]);
        bool         difference = row["operation"].get<int>() == int(ClipperLib::ctDifference);
        auto         start      = Clock::now();
        auto         reference  = difference ? diff(subject, clip) : intersection(subject, clip);
        const double cpu_ms     = elapsed(start);
        REQUIRE(reference.empty() == row["empty"].get<bool>());
        start              = Clock::now();
        auto         edges = edges_of(subject);
        const size_t n     = edges.size();
        auto         other = edges_of(clip);
        edges.insert(edges.end(), other.begin(), other.end());
        auto queries = closed_polygon_empty_queries(edges, n, difference);
        REQUIRE(!queries.empty());
        REQUIRE(gpu->update_edges(edges, error));
        std::vector<uint32_t> decisions;
        REQUIRE(gpu->compute(queries, decisions, error));
        const double gpu_ms = elapsed(start);
        for (size_t i = 0; i < queries.size(); ++i)
            REQUIRE(decisions[i] == local_z_segment_proven_empty(edges, queries[i]));
        const bool proven = std::all_of(decisions.begin(), decisions.end(), [](uint32_t v) { return v == 1; });
        if (proven) {
            CHECK(reference.empty());
            ++rejected;
        }
        auto& stage = timings[row["stage"].get<std::string>()];
        if (stage.is_null())
            stage = nlohmann::json::object();
        stage["cases"]        = stage.value("cases", 0) + 1;
        stage["proven"]       = stage.value("proven", 0) + int(proven);
        stage["cpu_ms"]       = stage.value("cpu_ms", 0.0) + cpu_ms;
        stage["gpu_ms"]       = stage.value("gpu_ms", 0.0) + gpu_ms;
        stage["saved_cpu_ms"] = stage.value("saved_cpu_ms", 0.0) + (proven ? cpu_ms : 0.0);
        ++cases;
    }
    INFO("Captured cases=" << cases << " rejected=" << rejected);
    CHECK(cases > 0);
    std::cout << "Polygon replay " << gpu->device_name() << ' ' << timings.dump() << '\n';
    gpu.reset();
    VulkanBatch::shutdown();
}

TEST_CASE("Shared polygon integration skips only empty complete operations", "[.][SharedPolygonIntegration]")
{
    const char* mode        = std::getenv("ORCA_POLYGON_COMPUTE");
    const char* report_path = std::getenv("ORCA_POLYGON_PROFILE");
    REQUIRE(mode);
    REQUIRE(std::string(mode) == "vulkan");
    REQUIRE(report_path);
    struct Cleanup
    {
        ~Cleanup()
        {
            PolygonComputeScope::shutdown();
            VulkanBatch::shutdown();
        }
    } cleanup;
    auto circle = [](int x, int radius) {
        Polygon p;
        for (size_t i = 0; i < 600; ++i) {
            const double angle = 2 * PI * double(i) / 600;
            p.points.emplace_back(x + coord_t(std::llround(radius * std::cos(angle))), coord_t(std::llround(radius * std::sin(angle))));
        }
        return p;
    };
    const Polygons a{circle(0, 10000000)}, b{circle(40000000, 10000000)}, outer{circle(0, 20000000)};
    const auto     nonempty_intersection = intersection_ex(a, outer);
    const auto     nonempty_difference   = diff_ex(outer, a);
    for (auto stage : {PolygonComputeScope::Stage::Walls, PolygonComputeScope::Stage::Shells, PolygonComputeScope::Stage::Paint}) {
        PolygonComputeScope scope(stage);
        CHECK(intersection(a, b).empty());
        CHECK(intersection_ex(a, b).empty());
        CHECK(diff(a, outer).empty());
        CHECK(diff_ex(a, outer).empty());
        CHECK(intersection_ex(a, outer) == nonempty_intersection);
        CHECK(diff_ex(outer, a) == nonempty_difference);
    }
    PolygonComputeScope::shutdown();
    std::ifstream input(report_path);
    REQUIRE(input.good());
    const auto report = nlohmann::json::parse(input);
    for (const char* stage : {"walls", "shells", "paint"}) {
        CHECK(report.at(stage).at("gpu").get<size_t>() == 6);
        CHECK(report.at(stage).at("skipped").get<size_t>() == 4);
    }
}
