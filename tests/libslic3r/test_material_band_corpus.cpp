#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "material_band_fixtures.hpp"
#include "libslic3r/MaterialBand/Production.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <tbb/task_arena.h>
#include <cmath>
#include <map>

using namespace Slic3r;
using namespace Slic3r::MaterialBand;

namespace {
// Independently generated source solids. These do not call the band clipper.
TriangleMesh round_cap(double radius, double center_z, double top, double center_x = 0.)
{
    constexpr int        angles = 48, rings = 24;
    indexed_triangle_set mesh;
    for (int row = 0; row < rings; ++row) {
        const double z = top * row / rings;
        const double r = std::sqrt(radius * radius - (z - center_z) * (z - center_z));
        for (int col = 0; col < angles; ++col) {
            const double a = 2. * PI * col / angles;
            mesh.vertices.emplace_back(float(center_x + r * std::cos(a)), float(r * std::sin(a)), float(z));
        }
    }
    const int tip = int(mesh.vertices.size());
    mesh.vertices.emplace_back(float(center_x), 0.f, float(top));
    for (int col = 0; col < angles; ++col) {
        const int next = (col + 1) % angles;
        for (int row = 0; row + 1 < rings; ++row) {
            const int a = row * angles + col, b = row * angles + next;
            mesh.indices.emplace_back(a, b, b + angles);
            mesh.indices.emplace_back(a, b + angles, a + angles);
        }
        mesh.indices.emplace_back((rings - 1) * angles + col, (rings - 1) * angles + next, tip);
        if (col > 0 && next != 0)
            mesh.indices.emplace_back(0, next, col);
    }
    return TriangleMesh(std::move(mesh));
}

TriangleMesh saddle()
{
    constexpr int        n = 16, stride = n + 1, plane = stride * stride;
    indexed_triangle_set mesh;
    for (int top = 0; top < 2; ++top)
        for (int row = 0; row <= n; ++row)
            for (int col = 0; col <= n; ++col) {
                const double x = -5. + 10. * col / n, y = -5. + 10. * row / n;
                mesh.vertices.emplace_back(float(x), float(y), top ? float(4. + .02 * (x * x - y * y)) : 0.f);
            }
    for (int row = 0; row < n; ++row)
        for (int col = 0; col < n; ++col) {
            const int a = row * stride + col, b = a + 1, c = b + stride, d = a + stride;
            mesh.indices.emplace_back(a, c, b);
            mesh.indices.emplace_back(a, d, c);
            mesh.indices.emplace_back(a + plane, b + plane, c + plane);
            mesh.indices.emplace_back(a + plane, c + plane, d + plane);
        }
    std::vector<int> perimeter;
    for (int i = 0; i < n; ++i)
        perimeter.push_back(i);
    for (int i = 0; i < n; ++i)
        perimeter.push_back(i * stride + n);
    for (int i = n; i > 0; --i)
        perimeter.push_back(n * stride + i);
    for (int i = n; i > 0; --i)
        perimeter.push_back(i * stride);
    for (size_t i = 0; i < perimeter.size(); ++i) {
        const int a = perimeter[i], b = perimeter[(i + 1) % perimeter.size()];
        mesh.indices.emplace_back(a, b, b + plane);
        mesh.indices.emplace_back(a, b + plane, a + plane);
    }
    return TriangleMesh(std::move(mesh));
}

double check_source(const TriangleMesh& mesh)
{
    std::map<std::pair<int, int>, std::pair<int, int>> edges;
    double                                             volume = 0.;
    for (const auto& f : mesh.its.indices) {
        const Vec3d a = mesh.its.vertices[f[0]].cast<double>(), b = mesh.its.vertices[f[1]].cast<double>(),
                    c = mesh.its.vertices[f[2]].cast<double>();
        REQUIRE(a.allFinite());
        INFO("Source triangle: " << a.transpose() << "; " << b.transpose() << "; " << c.transpose());
        REQUIRE((b - a).cross(c - a).squaredNorm() > 1e-20);
        volume += a.dot(b.cross(c)) / 6.;
        for (int i = 0; i < 3; ++i) {
            const int x = f[i], y = f[(i + 1) % 3];
            auto&     edge = edges[{std::min(x, y), std::max(x, y)}];
            ++edge.first;
            edge.second += x < y ? 1 : -1;
        }
    }
    for (const auto& edge : edges) {
        REQUIRE(edge.second.first == 2);
        REQUIRE(edge.second.second == 0);
    }
    REQUIRE(volume > 0.);
    return volume;
}

MaterialBandFixtures::Fixture make_core(const std::string& id)
{
    using namespace MaterialBandFixtures;
    if (id == "MB01" || id == "MB02-edge" || id == "MB02-transition" || id == "MB03") {
        auto f = make_fixture(id);
        enable_material_bands(f);
        if (id == "MB02-transition" || id == "MB03") {
            f.config.set_deserialize_strict("material_band_boundary_policy", "allow_transition");
            f.config.set_deserialize_strict("material_band_transition_width", "2");
            f.config.set_deserialize_strict("material_band_transition_error", "25%");
        }
        return f;
    }
    auto  f      = make_fixture("MB02-edge");
    auto* object = f.model.objects.front();
    if (id == "MB04" || id == "MB05" || id == "MB06" || id == "MB09") {
        const auto axial = make_axial_fixture(id == "MB04" ? "cylinder" :
                                              id == "MB05" ? "annulus" :
                                              id == "MB06" ? "frustum" :
                                                             "hole_plate",
                                              64, id == "MB06" ? 4.8 : 2.4);
        object->delete_volume(0);
        object->add_volume(TriangleMesh(axial.model.objects.front()->volumes.front()->mesh()), ModelVolumeType::MODEL_PART, false);
    } else if (id == "MB07" || id == "MB08" || id == "MB10") {
        TriangleMesh mesh = id == "MB08" ? saddle() : id == "MB07" ? round_cap(6., 0., 6.) : round_cap(3., 2.5, 5.5, -2.);
        if (id == "MB10") {
            MeshBoolean::cgal::plus(mesh, round_cap(3., 2.5, 5.5, 2.));
            // Symmetric intersections can differ by 1e-16 mm. Normalize the
            // fixture at a declared 1e-6 mm source quantum before welding.
            for (auto& vertex : mesh.its.vertices)
                for (int axis = 0; axis < 3; ++axis)
                    vertex[axis] = float(std::round(double(vertex[axis]) * 1e6) / 1e6);
            its_merge_vertices(mesh.its);
            its_remove_degenerate_faces(mesh.its);
        }
        const double source_volume = check_source(mesh);
        CHECK(source_volume == Approx(id == "MB08" ? 400. : id == "MB07" ? 144. * PI : 65.25 * PI).epsilon(.02));
        object->delete_volume(0);
        object->add_volume(std::move(mesh), ModelVolumeType::MODEL_PART, false);
    } else if (id == "MB12" || id == "MB12-edit-revert") {
        f      = make_fixture("MB01");
        object = f.model.objects.front();
        object->volumes.front()->set_scaling_factor(Vec3d(1., 1., .2));
        auto  second = make_axial_fixture("annulus", 64, .48);
        auto* added  = f.model.add_object(*second.model.objects.front());
        added->instances.front()->set_offset(Vec3d(40., 20., 0.));
        f.config.set("wall_loops", 1);
        enable_material_bands(f);
        return f;
    }
    enable_material_bands(f);
    if (id == "MB11") {
        f.config.set_num_filaments(3);
        f.config.set_num_extruders(3);
        f.config.option<ConfigOptionFloats>("flush_volumes_matrix")->values = {0., 1., 1., 1., 0., 1., 1., 1., 0.};
        f.config.option<ConfigOptionStrings>("filament_colour")->values     = {"#FF0000", "#0000FF", "#00FF00"};
        f.config.option<ConfigOptionFloats>("filament_diameter")->values    = {1.75, 1.75, 1.75};
        std::vector<MixedFilamentDefinition> recipes(3);
        recipes[0].recipe.blend.components = {{{1}, 50}, {{2}, 50}};
        recipes[1].recipe.blend.components = {{{1}, 34}, {{2}, 33}, {{3}, 33}};
        recipes[2].recipe.blend.components = {{{2}, 50}, {{3}, 50}};
        for (size_t i = 0; i < recipes.size(); ++i) {
            recipes[i].identity.stable_id = 20001 + i;
            recipes[i].source.kind        = MixedFilamentSourceKind::Custom;
        }
        MixedFilamentManager manager;
        manager.set_mixed_filament_definitions(recipes, f.config.option<ConfigOptionStrings>("filament_colour")->values);
        f.config.set("mixed_filament_definitions", manager.serialize_custom_entries());
        auto*            volume = object->volumes.front();
        TriangleSelector selector(volume->mesh());
        for (size_t i = 0; i < volume->mesh().its.indices.size(); ++i)
            selector.set_facet(int(i), static_cast<EnforcerBlockerType>(std::array<int, 5>{{1, 4, 5, 6, 3}}[i % 5]));
        volume->mmu_segmentation_facets.set(selector);
    } else if (id == "MB11-reversed" || id == "MB11-below-minimum") {
        f = make_fixture("MB01");
        enable_material_bands(f);
        MixedFilamentDefinition recipe;
        recipe.identity.stable_id = 10001;
        recipe.source.kind        = MixedFilamentSourceKind::Custom;
        if (id == "MB11-reversed")
            recipe.recipe.blend.components = {{{2}, 50}, {{1}, 50}};
        else
            recipe.recipe.blend.components = {{{1}, 1}, {{2}, 99}};
        recipe.behavior.layer_cadence = {1, 1};
        MixedFilamentManager manager;
        manager.set_mixed_filament_definitions({recipe}, {"#FF0000", "#0000FF"});
        f.config.set("mixed_filament_definitions", manager.serialize_custom_entries());
    } else {
        auto*            volume = object->volumes.front();
        TriangleSelector selector(volume->mesh());
        for (size_t i = 0; i < volume->mesh().its.indices.size(); ++i) {
            const auto& face = volume->mesh().its.indices[i];
            Vec3f       c    = Vec3f::Zero();
            for (int k = 0; k < 3; ++k)
                c += volume->mesh().its.vertices[face[k]] / 3.f;
            // Angular sectors/meridians; MB06 also changes its sector with Z.
            selector.set_facet(int(i), static_cast<EnforcerBlockerType>((c.x() >= 0.f) != (id == "MB06" && c.z() > 2.4f) ? 3 : 4));
        }
        volume->mmu_segmentation_facets.set(selector);
    }
    return f;
}
} // namespace

TEST_CASE("Material band core corpus declares all sixteen enabled outcomes", "[material_band][integration][corpus]")
{
    const std::string id = GENERATE("MB01", "MB02-edge", "MB02-transition", "MB03", "MB04", "MB05", "MB06", "MB07", "MB08", "MB09", "MB10",
                                    "MB11", "MB11-reversed", "MB11-below-minimum", "MB12", "MB12-edit-revert");
    CAPTURE(id);
    auto fixture = make_core(id);
    for (const auto* object : fixture.model.objects)
        check_source(object->volumes.front()->mesh());
    Print print;
    print.apply(fixture.model, fixture.config);
    const auto        result   = tbb::task_arena(2).execute([&] { return plan_production_print(print, {1, 2, 3, 4, 5, 6, 7}); });
    const auto        data     = MaterialBandFixtures::corpus();
    const std::string expected = data.at("core_acceptance").at(id).at("fallback");
    if (!expected.empty()) {
        REQUIRE(result->fallback);
        CHECK(result->fallback->detail == expected);
        CHECK_FALSE(result->plan);
    } else {
        if (result->fallback)
            FAIL(result->fallback->detail);
        REQUIRE(result->used_material_bands());
        CHECK(result->paths_ms > 0.);
        CHECK(result->finalize_ms > 0.);
        if (id == "MB12" || id == "MB12-edit-revert")
            CHECK(result->object_count == 2);
        if (id == "MB12-edit-revert") {
            auto*      instance = fixture.model.objects.front()->instances.front();
            const auto original = instance->get_offset();
            instance->set_offset(original + Vec3d(1., 0., 0.));
            print.apply(fixture.model, fixture.config);
            tbb::task_arena(2).execute([&] { print.process(); });
            REQUIRE(print.material_band_result()->used_material_bands());
            instance->set_offset(original);
            print.apply(fixture.model, fixture.config);
            tbb::task_arena(2).execute([&] { print.process(); });
            const auto reverted = print.material_band_result();
            REQUIRE(reverted->used_material_bands());
            REQUIRE(reverted->plan->operations.size() == result->plan->operations.size());
            for (size_t i = 0; i < result->plan->operations.size(); ++i)
                CHECK(result->plan->operations[i].position == reverted->plan->operations[i].position);
        }
    }
}
