#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "libslic3r/MaterialBand/InputAdapter.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "material_band_fixtures.hpp"

using namespace Slic3r;
using namespace Slic3r::MaterialBand;

namespace {
InputContext context()
{
    InputContext result;
    result.revision                 = {100, 1, 2, 3, 4, 5, 6};
    result.source_revision          = "63117732a0683220530cf690f385055d1f3dd54d";
    result.phase.order              = {{{1}, {2}}};
    result.phase.first_active_cycle = 9;
    return result;
}

const KernelInput& snapshot(const InputResult& result)
{
    if (const auto* failure = std::get_if<Failure>(&result))
        FAIL(failure_name(failure->code) << ": " << failure->detail);
    const auto& input = std::get<std::shared_ptr<const KernelInput>>(result);
    REQUIRE(input);
    return *input;
}
} // namespace

TEST_CASE("Material band input snapshots preserve paint, physical identity and connected phase", "[material_band][integration]")
{
    const auto  id      = GENERATE("MB01", "MB02-edge", "MB03");
    auto        fixture = MaterialBandFixtures::make_fixture(id);
    const auto  options = context();
    const auto  result  = snapshot_input(*fixture.model.objects.front(), fixture.config, options);
    const auto& input   = snapshot(result);
    CHECK(input.revision == options.revision);
    REQUIRE(input.regions.size() == fixture.manifest.at("mesh_strips").get<size_t>() + 1);
    REQUIRE(input.regions.front().id == 0);
    CHECK(input.regions.front().scope == DepositionScope::WholeSolid);
    CHECK(input.regions.front().fraction_a == (std::string(id) == "MB01" ? .5 : 1.));
    size_t facets = 0;
    for (const auto& region : input.regions) {
        CHECK(region.phase.order[0].value == 1);
        CHECK(region.phase.order[1].value == 2);
        CHECK(region.phase.first_active_cycle == 9);
        if (region.id != 0) {
            CHECK(region.scope == DepositionScope::PaintedShell);
            CHECK(region.recipe == region.id - 3 + 10001);
            for (const auto& face : region.painted_surface.triangles) {
                double x = 0.;
                for (uint32_t index : face)
                    x += region.painted_surface.vertices[index].x() / 3.;
                const double expected = MaterialBandFixtures::target_fraction(fixture.manifest, x);
                CHECK(std::abs(region.fraction_a - expected) <=
                      fixture.manifest.at("production_recipe_quantization_fraction").get<double>() + 1e-7);
            }
        }
        facets += region.painted_surface.triangles.size();
    }
    CHECK(facets == input.solid.triangles.size());
    const auto saved_vertices = input.solid.vertices;
    fixture.model.objects.front()->volumes.front()->mmu_segmentation_facets.reset();
    fixture.model.objects.front()->volumes.front()->translate(Vec3d(100., 0., 0.));
    CHECK(input.solid.vertices == saved_vertices);
    REQUIRE(input.regions[1].painted_surface.triangles.size() > 0);
}

TEST_CASE("Material band input rejects unsupported intent before publishing a snapshot", "[material_band][integration]")
{
    auto              fixture  = MaterialBandFixtures::make_fixture("MB02-edge");
    auto              options  = context();
    FailureCode       expected = FailureCode::IncompatibleOrder;
    const std::string scenario = GENERATE("Cadence is not silently restarted", "Opposing stack order", "Invalid phase slot",
                                          "Input resource limit", "More than four physical materials",
                                          "Multiple solid parts require a domain adapter");
    CAPTURE(scenario);
    if (scenario == "Cadence is not silently restarted") {
        options.phase.cadence = {{2, 1}};
    }
    if (scenario == "Opposing stack order") {
        options.phase.order = {{{2}, {1}}};
    }
    if (scenario == "Invalid phase slot") {
        options.phase.slot = 2;
    }
    if (scenario == "Input resource limit") {
        options.max_triangles = 1;
        expected              = FailureCode::ResourceLimit;
    }
    if (scenario == "More than four physical materials") {
        fixture.config.option<ConfigOptionStrings>("filament_colour")->values.resize(5, "#00FF00");
        expected = FailureCode::UnsupportedProfile;
    }
    if (scenario == "Multiple solid parts require a domain adapter") {
        fixture.model.objects.front()->add_volume(make_cube(1., 1., 1.));
        expected = FailureCode::UnsupportedTopology;
    }
    const auto result = snapshot_input(*fixture.model.objects.front(), fixture.config, options);
    REQUIRE(std::holds_alternative<Failure>(result));
    CHECK(std::get<Failure>(result).code == expected);
    CHECK(std::get<Failure>(result).revision == options.revision);
}

TEST_CASE("Material band input cancellation discards partially copied paint", "[material_band][integration]")
{
    auto       fixture = MaterialBandFixtures::make_fixture("MB03");
    size_t     polls   = 0;
    const auto result  = snapshot_input(*fixture.model.objects.front(), fixture.config, context(), [&] { return ++polls > 20; });
    REQUIRE(std::holds_alternative<Failure>(result));
    CHECK(std::get<Failure>(result).code == FailureCode::Cancelled);
}

TEST_CASE("Material band input retains split painted facets and their surface area", "[material_band][integration]")
{
    auto             fixture = MaterialBandFixtures::make_fixture("MB02-edge");
    auto&            volume  = *fixture.model.objects.front()->volumes.front();
    TriangleSelector selector(volume.mesh());
    selector.deserialize(volume.mmu_segmentation_facets.get_data(), false);
    const auto transform = Transform3d::Identity();
    selector.select_patch(8,
                          TriangleSelector::SinglePointCursor::cursor_factory(Vec3f(2.f, 2.6f, 2.4f), Vec3f(2.f, 2.6f, 12.4f), .75f,
                                                                              TriangleSelector::SPHERE, transform,
                                                                              TriangleSelector::ClippingPlane()),
                          EnforcerBlockerType::Extruder4, transform, true);
    volume.mmu_segmentation_facets.set(selector);
    const auto  result = snapshot_input(*fixture.model.objects.front(), fixture.config, context());
    const auto& input  = snapshot(result);
    double      area   = 0.;
    size_t      facets = 0;
    for (const auto& region : input.regions)
        for (const auto& triangle : region.painted_surface.triangles) {
            const auto& vertices = region.painted_surface.vertices;
            area += (vertices[triangle[1]] - vertices[triangle[0]]).cross(vertices[triangle[2]] - vertices[triangle[0]]).norm() / 2.;
            ++facets;
        }
    REQUIRE(facets > input.solid.triangles.size());
    CHECK(area == Approx(2. * (12. * 8. + 12. * 2.4 + 8. * 2.4)).epsilon(1e-6));
}

TEST_CASE("Material band input does not infer physical identity from displayed colors", "[material_band][integration]")
{
    auto       fixture = MaterialBandFixtures::make_fixture("MB02-edge");
    const auto first   = snapshot_input(*fixture.model.objects.front(), fixture.config, context());
    fixture.config.option<ConfigOptionStrings>("filament_colour")->values = {"#777777", "#777777"};
    const auto  second = snapshot_input(*fixture.model.objects.front(), fixture.config, context());
    const auto& a      = snapshot(first);
    const auto& b      = snapshot(second);
    REQUIRE(a.regions.size() == b.regions.size());
    for (size_t i = 0; i < a.regions.size(); ++i) {
        CHECK(a.regions[i].recipe == b.regions[i].recipe);
        CHECK(a.regions[i].fraction_a == b.regions[i].fraction_a);
        CHECK(a.regions[i].phase.order[0].value == b.regions[i].phase.order[0].value);
        CHECK(a.regions[i].phase.order[1].value == b.regions[i].phase.order[1].value);
    }
}

TEST_CASE("Material band paint limits count decoded leaves rather than serialized color bits", "[material_band][input][paint_limits]")
{
    auto             fixture = MaterialBandFixtures::make_fixture("MB01");
    auto*            volume  = fixture.model.objects.front()->volumes.front();
    TriangleSelector selector(volume->mesh());
    for (size_t i = 0; i < volume->mesh().its.indices.size(); ++i)
        selector.set_facet(int(i), EnforcerBlockerType::Extruder2);
    volume->mmu_segmentation_facets.set(selector);
    auto options          = context();
    options.max_triangles = volume->mesh().its.indices.size();
    REQUIRE(volume->mmu_segmentation_facets.get_data().bitstream.size() > options.max_triangles);
    const auto  result = snapshot_input(*fixture.model.objects.front(), fixture.config, options);
    const auto& input  = snapshot(result);
    CHECK(input.solid.triangles.size() == options.max_triangles);
    size_t leaves = 0;
    for (const auto& region : input.regions)
        leaves += region.painted_surface.triangles.size();
    CHECK(leaves == options.max_triangles);
    --options.max_triangles;
    const auto rejected = snapshot_input(*fixture.model.objects.front(), fixture.config, options);
    REQUIRE(std::holds_alternative<Failure>(rejected));
    CHECK(std::get<Failure>(rejected).code == FailureCode::ResourceLimit);
}

TEST_CASE("Material band input drops zero area facets without shifting paint indices", "[material_band][integration][layer_process]")
{
    auto        fixture       = MaterialBandFixtures::make_fixture("MB02-edge");
    auto*       volume        = fixture.model.objects.front()->volumes.front();
    const auto  before_result = snapshot_input(*fixture.model.objects.front(), fixture.config, context());
    const auto& before        = snapshot(before_result);
    auto        mesh          = volume->mesh().its;
    const int   first         = int(mesh.vertices.size());
    mesh.vertices.emplace_back(0.f, 0.f, 0.f);
    mesh.vertices.emplace_back(1.f, 1.f, 1.f);
    mesh.vertices.emplace_back(2.f, 2.f, 2.f);
    mesh.indices.emplace_back(first, first + 1, first + 2);
    volume->set_mesh(TriangleMesh(std::move(mesh)));
    const auto  after_result = snapshot_input(*fixture.model.objects.front(), fixture.config, context());
    const auto& after        = snapshot(after_result);
    CHECK(after.discarded_degenerate_faces == 1);
    CHECK(after.solid.triangles == before.solid.triangles);
    REQUIRE(after.regions.size() == before.regions.size());
    for (size_t i = 0; i < before.regions.size(); ++i) {
        CHECK(after.regions[i].fractions == before.regions[i].fractions);
        CHECK(after.regions[i].painted_surface.triangles.size() == before.regions[i].painted_surface.triangles.size());
    }
}
