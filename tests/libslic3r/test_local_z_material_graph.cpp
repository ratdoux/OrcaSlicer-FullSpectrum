#include <catch2/catch_test_macros.hpp>
#include "libslic3r/LocalZMaterialGraph.hpp"

using namespace Slic3r;

namespace {
ExPolygon rectangle(double x, double y, double w, double h)
{
    return ExPolygon(
        Polygon({Point::new_scale(x, y), Point::new_scale(x + w, y), Point::new_scale(x + w, y + h), Point::new_scale(x, y + h)}));
}
LocalZMaterialGraph::Region region(unsigned source, ExPolygons masks, size_t config = 0)
{
    return {source, config, {{1, .4, {.06, .32}}, {2, .6, {.06, .32}}}, std::move(masks)};
}
LocalZMaterialGraph::Slice slice(size_t index, std::vector<LocalZMaterialGraph::Region> regions)
{
    return {index, index * .08, (index + 1) * .08, (index + .5) * .08, {}, std::move(regions)};
}
} // namespace

TEST_CASE("LocalZ graph keeps disconnected recipes independent", "[LocalZ][LocalZMaterialGraph]")
{
    std::vector<LocalZMaterialGraph::Slice> samples;
    for (size_t i = 0; i < 4; ++i)
        samples.push_back(slice(i, {region(5, {rectangle(0, 0, 5, 5), rectangle(10, 0, 5, 5)})}));
    const auto graph = LocalZMaterialGraph::build(samples);
    REQUIRE(graph.domains.size() == 2);
    CHECK(graph.contacts.empty());
    for (const auto& domain : graph.domains) {
        CHECK(domain.nodes.size() == 4);
        CHECK(domain.births == 1);
        CHECK(domain.deaths == 1);
        CHECK(domain.splits == 0);
        CHECK(domain.merges == 0);
    }
}

TEST_CASE("LocalZ graph records splits merges and changing recipe targets", "[LocalZ][LocalZMaterialGraph]")
{
    auto samples                           = std::vector<LocalZMaterialGraph::Slice>{slice(0, {region(5, {rectangle(0, 0, 10, 5)})}),
                                                                                     slice(1, {region(5, {rectangle(0, 0, 4, 5), rectangle(6, 0, 4, 5)})}),
                                                                                     slice(2, {region(5, {rectangle(0, 0, 10, 5)})})};
    samples[1].regions[0].recipe[0].weight = .6;
    samples[1].regions[0].recipe[1].weight = .4;
    const auto graph                       = LocalZMaterialGraph::build(samples);
    REQUIRE(graph.domains.size() == 1);
    CHECK(graph.domains[0].splits == 1);
    CHECK(graph.domains[0].merges == 1);
    CHECK(graph.nodes[0].above.size() == 2);
    CHECK(graph.nodes.back().below.size() == 2);
}

TEST_CASE("LocalZ graph represents lateral and vertical material handovers", "[LocalZ][LocalZMaterialGraph]")
{
    const auto graph = LocalZMaterialGraph::build({slice(0, {region(5, {rectangle(0, 0, 5, 5)}), region(6, {rectangle(5, 0, 5, 5)})}),
                                                   slice(1, {region(7, {rectangle(0, 0, 10, 5)})})});
    REQUIRE(graph.domains.size() == 3);
    REQUIRE(graph.contacts.size() == 3);
    size_t lateral = 0, vertical = 0;
    for (const auto& contact : graph.contacts) {
        lateral += contact.slices.size();
        vertical += contact.vertical_slices.size();
    }
    CHECK(lateral == 1);
    CHECK(vertical == 2);
    const auto configurations = LocalZMaterialGraph::build(
        {slice(0, {region(5, {rectangle(0, 0, 5, 5)}, 0)}), slice(1, {region(5, {rectangle(0, 0, 5, 5)}, 1)})});
    CHECK(configurations.domains.size() == 2);
    REQUIRE(configurations.contacts.size() == 1);
    CHECK(configurations.contacts[0].vertical_slices.size() == 1);
}

TEST_CASE("LocalZ graph rejects invalid samples and exposes real gaps", "[LocalZ][LocalZMaterialGraph]")
{
    auto first = slice(0, {region(5, {rectangle(0, 0, 5, 5)})});
    auto third = slice(2, first.regions);
    auto graph = LocalZMaterialGraph::build({first, third});
    CHECK(graph.domains.size() == 2);
    CHECK(graph.contacts.empty());
    first.sample_z = 1.;
    CHECK_THROWS_AS(LocalZMaterialGraph::build({first}), std::invalid_argument);
}
