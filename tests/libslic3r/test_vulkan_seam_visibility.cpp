#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Compute/SeamVisibility.hpp"
#include "libslic3r/KDTreeIndirect.hpp"
#include "libslic3r/Point.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>

using namespace Slic3r;

TEST_CASE("Vulkan seam backend rejects invalid input or reports an unavailable build", "[VulkanCompute]")
{
    std::string error;
    CHECK_FALSE(VulkanSeamVisibility::create({}, 1.0f, "", error));
    CHECK_FALSE(error.empty());
}

// Explicit hardware tests: skipped by default, but fail if a requested GPU is
// unavailable. Run on each device via ORCA_VULKAN_DEVICE=NVIDIA / Intel.
TEST_CASE("Vulkan visibility matches CPU tree queries and weighting", "[.][VulkanComputeHardware]")
{
    struct Coordinates
    {
        const std::vector<Vec3f>* points;
        float                     operator()(size_t i, size_t d) const { return (*points)[i][d]; }
    };
    std::mt19937                          engine(31719);
    std::uniform_real_distribution<float> position(-10, 10);
    std::uniform_real_distribution<float> visibility(0, 2);
    std::vector<Vec3f>                    points, normals;
    std::vector<float>                    scores;
    for (size_t i = 0; i < 30000; ++i) {
        points.emplace_back(position(engine), position(engine), position(engine));
        Vec3f normal(position(engine), position(engine), position(engine));
        normals.push_back(normal.normalized());
        scores.push_back(visibility(engine));
    }
    KDTreeIndirect<3, float, Coordinates> tree(Coordinates{&points}, points.size());
    std::vector<SeamVisibilityNode>       nodes(tree.node_indices().size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        const size_t index = tree.node_indices()[i];
        if (index == size_t(-1))
            continue;
        const auto& p = points[index];
        const auto& n = normals[index];
        nodes[i]      = {{p.x(), p.y(), p.z(), scores[index]}, {n.x(), n.y(), n.z(), 1}};
    }
    const char* selected = std::getenv("ORCA_VULKAN_DEVICE");
    std::string error;
    const float radius  = 2;
    auto        backend = VulkanSeamVisibility::create(nodes, radius, selected ? selected : "", error);
    INFO(error);
    REQUIRE(backend);
    INFO(backend->device_name());
    std::vector<float> results;
    REQUIRE(backend->compute({}, results, error));
    CHECK(results.empty());
    size_t bit_differences = 0;
    float  max_error       = 0;
    for (size_t batch_size : {size_t(1), size_t(1003), size_t(VulkanSeamVisibility::BATCH_CAPACITY)}) {
        std::vector<SeamVisibilityQuery> queries;
        for (size_t i = 0; i < batch_size; ++i)
            queries.push_back({position(engine), position(engine), position(engine), 0});
        queries.back() = {100, 100, 100, 0}; // Empty neighbourhood.
        REQUIRE(backend->compute(queries, results, error));
        REQUIRE(results.size() == queries.size());
        for (size_t i = 0; i < queries.size(); ++i) {
            const Vec3f query(queries[i][0], queries[i][1], queries[i][2]);
            const auto  nearby   = find_nearby_points(tree, query, radius);
            float       weighted = 0, weight_sum = 0;
            for (size_t sample : nearby) {
                const Vec3f delta  = query - points[sample];
                float       weight = radius - std::abs(delta.dot(normals[sample]));
                weight += radius - delta.norm();
                weighted += weight * scores[sample];
                weight_sum += weight;
            }
            const float reference  = nearby.empty() ? 1.0f : weighted / weight_sum;
            const float difference = std::abs(reference - results[i]);
            if (std::memcmp(&reference, &results[i], sizeof(float)) != 0)
                ++bit_differences;
            max_error = std::max(max_error, difference);
            CHECK(std::isfinite(results[i]));
            CHECK(difference <= 2e-6f);
        }
    }
    WARN("Device=" << backend->device_name() << " max_error=" << max_error << " bit_differences=" << bit_differences);
    std::vector<SeamVisibilityQuery> too_large(VulkanSeamVisibility::BATCH_CAPACITY + 1);
    CHECK_FALSE(backend->compute(too_large, results, error));
    CHECK(results.empty());
    CHECK_FALSE(error.empty());
    CHECK_FALSE(VulkanSeamVisibility::create(nodes, radius, "no-such-test-gpu-31719", error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("Vulkan visibility preserves strict radius boundaries and duplicate nodes", "[.][VulkanComputeHardware]")
{
    const char* selected = std::getenv("ORCA_VULKAN_DEVICE");
    std::string error;
    // Valid preorder tree, three coincident samples with distinct scores.
    std::vector<SeamVisibilityNode> nodes   = {{{0, 0, 0, 0.2f}, {0, 0, 1, 1}},
                                               {{0, 0, 0, 0.4f}, {0, 0, 1, 1}},
                                               {{0, 0, 0, 0.6f}, {0, 0, 1, 1}},
                                               {}};
    auto                            backend = VulkanSeamVisibility::create(nodes, 1, selected ? selected : "", error);
    INFO(error);
    REQUIRE(backend);
    const float                      inside  = std::nextafter(1.0f, 0.0f);
    const float                      outside = std::nextafter(1.0f, 2.0f);
    std::vector<SeamVisibilityQuery> queries{{0, 0, 0, 0}, {1, 0, 0, 0}, {inside, 0, 0, 0}, {outside, 0, 0, 0}};
    std::vector<float>               results;
    REQUIRE(backend->compute(queries, results, error));
    REQUIRE(results.size() == queries.size());
    CHECK(std::abs(results[0] - 0.4f) <= 2e-6f);
    CHECK(results[1] == 1.0f);
    CHECK(std::abs(results[2] - 0.4f) <= 2e-6f);
    CHECK(results[3] == 1.0f);
    CHECK_FALSE(VulkanSeamVisibility::create(nodes, 0, "", error));
    CHECK_FALSE(VulkanSeamVisibility::create(nodes, std::numeric_limits<float>::infinity(), "", error));
}
