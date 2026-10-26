#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/LocalZMaterialPlanner.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;
using Catch::Approx;

namespace {
ExPolygon box(double x, double width)
{
    return ExPolygon(
        Polygon({Point::new_scale(x, 0), Point::new_scale(x + width, 0), Point::new_scale(x + width, 10), Point::new_scale(x, 10)}));
}
LocalZMaterialGraph::Region material(unsigned source, double ratio, ExPolygon mask)
{
    return {source, 0, {{1, ratio, {.06, .32}}, {2, 1. - ratio, {.06, .32}}}, {std::move(mask)}};
}
LocalZMaterialGraph graph_for(const std::vector<LocalZMaterialGraph::Region>& regions)
{
    std::vector<LocalZMaterialGraph::Slice> slices;
    ExPolygons                              outline;
    for (const auto& region : regions)
        append(outline, region.masks);
    outline = union_ex(outline);
    for (size_t i = 0; i < 10; ++i)
        slices.push_back({i, i * .06, (i + 1) * .06, (i + .5) * .06, outline, regions});
    return LocalZMaterialGraph::build(std::move(slices));
}
void check_ownership(const LocalZMaterialGraph& graph, const LocalZMaterialPlanner::Plan& plan)
{
    // More than nominal midpoints: boundaries of independent cycles may cut
    // through a nominal slab. Check every emitted Z interval inside that slab.
    for (const auto& slice : graph.slices) {
        std::vector<double> events{slice.bottom, slice.top};
        for (const auto& cell : plan.cells)
            for (double z : {cell.geometry->z_lo, cell.geometry->z_hi})
                if (z > slice.bottom && z < slice.top)
                    events.push_back(z);
        std::sort(events.begin(), events.end());
        for (size_t event = 1; event < events.size(); ++event) {
            if (events[event] - events[event - 1] < 1e-7)
                continue;
            const double z = .5 * (events[event] + events[event - 1]);
            ExPolygons   claimed;
            for (const auto& cell : plan.cells) {
                const auto& geometry = *cell.geometry;
                if (z <= geometry.z_lo || z > geometry.z_hi)
                    continue;
                const auto mask = geometry.ownership(slice.layer_id);
                CHECK(area(intersection_ex(mask, claimed)) * SCALING_FACTOR * SCALING_FACTOR < 1e-6);
                append(claimed, mask);
            }
            CHECK(area(diff_ex(slice.outline, union_ex(claimed))) * SCALING_FACTOR * SCALING_FACTOR < 1e-6);
        }
    }
}
} // namespace

TEST_CASE("Material LocalZ keeps the minimum cadence of disconnected recipes", "[LocalZ][LocalZMaterialPlanner]")
{
    const auto graph = graph_for({material(5, .5, box(0, 10)), material(6, .4, box(20, 10))});
    const auto plan  = LocalZMaterialPlanner::build(graph, {});
    REQUIRE(plan.diagnostics.size() == 2);
    CHECK(plan.diagnostics[0].chosen_period == Approx(.12));
    CHECK(plan.diagnostics[1].chosen_period == Approx(.15));
    CHECK(plan.minimum_cycles == 9);
    CHECK(plan.closure_cycles == 0);
    CHECK(plan.nominal_cells == 0);
    check_ownership(graph, plan);
}

TEST_CASE("Material LocalZ joins equal periods with real same-material interfaces", "[LocalZ][LocalZMaterialPlanner]")
{
    const auto graph = graph_for({material(5, .4, box(0, 10)), material(6, .6, box(10, 10))});
    const auto plan  = LocalZMaterialPlanner::build(graph, {});
    REQUIRE(plan.contacts.size() == 1);
    CHECK(plan.contacts[0].kind == LocalZMaterialPlanner::ContactKind::equal_period_join);
    CHECK(plan.joined_cycles == 4);
    CHECK(plan.nominal_cells == 0);
    for (const auto& cell : plan.cells) {
        CHECK(cell.geometry->material_planner);
        CHECK_FALSE(cell.geometry->nominal_band);
        REQUIRE(cell.geometry->passes.size() == 2);
        const auto a = cell.geometry->surfaces(Point::new_scale(2, 5), 0);
        const auto b = cell.geometry->surfaces(Point::new_scale(18, 5), 0);
        CHECK(std::abs(a.second - b.second) == Approx(.03).margin(.001));
    }
    check_ownership(graph, plan);
}

TEST_CASE("Material LocalZ exposes unequal periods without coarsening them", "[LocalZ][LocalZMaterialPlanner]")
{
    const auto graph = graph_for({material(5, .5, box(0, 10)), material(6, .4, box(10, 10))});
    const auto plan  = LocalZMaterialPlanner::build(graph, {});
    REQUIRE(plan.contacts.size() == 1);
    CHECK(plan.contacts[0].kind == LocalZMaterialPlanner::ContactKind::unequal_period);
    CHECK(plan.minimum_cycles == 9);
    CHECK(plan.nominal_cells == 0);
    check_ownership(graph, plan);
}

TEST_CASE("Material LocalZ explicitly owns moving boundaries as nominal islands", "[LocalZ][LocalZMaterialPlanner]")
{
    std::vector<LocalZMaterialGraph::Slice> slices;
    for (size_t i = 0; i < 10; ++i) {
        const double edge = 10 + .1 * i;
        slices.push_back({i,
                          i * .08,
                          (i + 1) * .08,
                          (i + .5) * .08,
                          {box(0, 20)},
                          {material(5, .4, box(0, edge)), material(6, .6, box(edge, 20 - edge))}});
    }
    const auto graph = LocalZMaterialGraph::build(std::move(slices));
    const auto plan  = LocalZMaterialPlanner::build(graph, {});
    REQUIRE(plan.diagnostics.size() == 2);
    for (const auto& diagnostic : plan.diagnostics)
        CHECK(diagnostic.reason == "moving_material_boundary");
    CHECK(plan.minimum_cycles == 0);
    CHECK(plan.nominal_cells > 0);
    for (const auto& cell : plan.cells) {
        CHECK(cell.geometry->material_planner);
        CHECK(cell.geometry->planar_fallback);
        CHECK(cell.geometry->passes[0].flat_height == Approx(.08));
    }
    check_ownership(graph, plan);
}

TEST_CASE("Material LocalZ configuration is opt-in and serializable", "[LocalZ][LocalZMaterialPlanner]")
{
    auto  config = DynamicPrintConfig::full_print_config();
    auto* option = config.option<ConfigOptionEnum<LocalZPlanner>>("dithering_local_z_planner");
    REQUIRE(option);
    CHECK(option->value == LocalZPlanner::Legacy);
    CHECK(option->serialize() == "legacy");
    REQUIRE(option->deserialize("material_v1"));
    CHECK(option->value == LocalZPlanner::MaterialV1);
    CHECK(option->serialize() == "material_v1");
}

TEST_CASE("Material LocalZ swept ownership includes an expanding exterior", "[LocalZ][LocalZMaterialPlanner]")
{
    std::vector<LocalZMaterialGraph::Slice> slices;
    for (size_t i = 0; i < 10; ++i) {
        const auto mask = box(0, 10 + .03 * i);
        slices.push_back({i, i * .06, (i + 1) * .06, (i + .5) * .06, {mask}, {material(5, .4, mask)}});
    }
    const auto graph = LocalZMaterialGraph::build(std::move(slices));
    const auto plan  = LocalZMaterialPlanner::build(graph, {});
    CHECK(plan.nominal_cells == 0);
    CHECK(plan.minimum_volume == Approx(plan.mixed_volume));
    for (const auto& cell : plan.cells) {
        ExPolygons swept;
        for (const auto& zone : cell.geometry->zones)
            append(swept, zone.masks);
        for (const auto& footprint : cell.geometry->layer_footprints)
            CHECK(diff_ex(footprint.masks, union_ex(swept)).empty());
    }
    check_ownership(graph, plan);
}

TEST_CASE("Material LocalZ confines a moving boundary fallback to its Z run", "[LocalZ][LocalZMaterialPlanner]")
{
    std::vector<LocalZMaterialGraph::Slice> slices;
    for (size_t i = 0; i < 10; ++i) {
        const double edge = i < 8 ? 10. : 10.1;
        slices.push_back({i,
                          i * .08,
                          (i + 1) * .08,
                          (i + .5) * .08,
                          {box(0, 20)},
                          {material(5, .4, box(0, edge)), material(6, .6, box(edge, 20 - edge))}});
    }
    const auto graph = LocalZMaterialGraph::build(std::move(slices));
    REQUIRE(graph.domains.size() == 2);
    const auto plan = LocalZMaterialPlanner::build(graph, {});
    CHECK(plan.minimum_volume > 0.);
    CHECK(plan.nominal_volume < .4 * plan.mixed_volume);
    CHECK(plan.joined_cycles > 0);
    CHECK(plan.mixed_volume == Approx(160.));
    for (const auto& diagnostic : plan.diagnostics)
        CHECK(diagnostic.source_domain < 2);
    check_ownership(graph, plan);
}

TEST_CASE("Material LocalZ repairs failed contours without discarding distant cycles", "[LocalZ][LocalZMaterialPlanner]")
{
    double height = .06, ratio = .5;
    bool   aligned = true;
    SECTION("Cycle boundaries align with nominal slabs") {}
    SECTION("Repaired cycles do not contribute to emitted rounding debt")
    {
        height = .097;
        ratio  = .31;
    }
    SECTION("Unaligned repair expands to complete nominal slabs")
    {
        height  = .08;
        ratio   = .4;
        aligned = false;
    }
    std::vector<LocalZMaterialGraph::Slice> slices;
    for (size_t i = 0; i < 10; ++i) {
        const auto mask = box(0, i == 4 || i == 5 ? .05 : 10.);
        slices.push_back({i, i * height, (i + 1) * height, (i + .5) * height, {mask}, {material(5, ratio, mask)}});
    }
    const auto graph = LocalZMaterialGraph::build(std::move(slices));
    REQUIRE(graph.domains.size() == 1);
    const auto plan = LocalZMaterialPlanner::build(graph, {});
    CHECK(plan.nominal_volume > 0.);
    if (aligned) {
        CHECK(plan.minimum_volume > 0.);
        CHECK(plan.nominal_volume < plan.mixed_volume);
    } else {
        CHECK(plan.minimum_volume == 0.);
        CHECK(plan.nominal_volume == Approx(plan.mixed_volume));
    }
    for (const auto& cell : plan.cells)
        if (cell.geometry->nominal_band)
            CHECK(cell.geometry->passes.front().flat_height == Approx(height));
    double emitted_error = 0.;
    for (const auto& cell : plan.cells)
        if (!cell.geometry->nominal_band) {
            const auto& zone = cell.geometry->zones.front();
            emitted_error += ratio * (cell.geometry->z_hi - cell.geometry->z_lo);
            for (size_t i = 0; i < zone.component_ids.size(); ++i)
                if (zone.component_ids[i] == 1)
                    emitted_error -= zone.heights[i];
        }
    for (const auto& diagnostic : plan.diagnostics)
        if (!diagnostic.residual_debt.empty())
            CHECK(diagnostic.residual_debt.front() == Approx(emitted_error).margin(1e-8));
    check_ownership(graph, plan);
}

TEST_CASE("Material LocalZ retains the ideal cadence for a short feature", "[LocalZ][LocalZMaterialPlanner]")
{
    const auto mask  = box(0, 10);
    auto       graph = LocalZMaterialGraph::build({{0, 0., .08, .04, {mask}, {material(5, .4, mask)}}});
    const auto plan  = LocalZMaterialPlanner::build(graph, {});
    REQUIRE(plan.diagnostics.size() == 1);
    CHECK(plan.diagnostics.front().reason == "domain_too_short");
    CHECK(plan.diagnostics.front().ideal_period == Approx(.15));
    CHECK(plan.diagnostics.front().chosen_period == Approx(.15));
    check_ownership(graph, plan);
}
