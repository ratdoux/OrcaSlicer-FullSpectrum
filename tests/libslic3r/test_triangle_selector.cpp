#include <catch2/catch_test_macros.hpp>

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include <tbb/task_arena.h>

using namespace Slic3r;

TEST_CASE("Painted slab projections preserve face order across worker counts", "[TriangleSelector][PaintPreparation]")
{
    indexed_triangle_set patch;
    constexpr int        side = 24;
    for (int y = 0; y <= side; ++y)
        for (int x = 0; x <= side; ++x)
            patch.vertices.emplace_back(float(x), float(y), float((x % 4) + 2 * (y % 3)) * 0.125f);
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x) {
            if ((x + y) % 3 == 0)
                continue; // Open painted patches with holes and touching vertices.
            const int a = y * (side + 1) + x;
            patch.indices.emplace_back(a, a + 1, a + side + 1);
            patch.indices.emplace_back(a + 1, a + side + 2, a + side + 1);
        }
    const std::vector<float> heights{0.f, 0.125f, 0.25f, 0.5f, 0.75f, 1.f};
    for (bool mirror : {false, true}) {
        Transform3d transform = Transform3d::Identity();
        if (mirror)
            transform.scale(Vec3d(-1., 1., -1.));
        auto project = [&] {
            std::pair<std::vector<Polygons>, std::vector<Polygons>> out;
            slice_mesh_slabs(patch, heights, transform, &out.first, &out.second, nullptr, [] {});
            return out;
        };
        tbb::task_arena serial(1), parallel(8);
        const auto      expected = serial.execute(project);
        for (int repeat = 0; repeat < 3; ++repeat)
            CHECK(parallel.execute(project) == expected);
    }
}

TEST_CASE("Triangle selector round-trips painted states above sixteen", "[TriangleSelector][MMUPaint]")
{
    indexed_triangle_set its;
    its.vertices = {
        Vec3f(0.f, 0.f, 0.f),
        Vec3f(1.f, 0.f, 0.f),
        Vec3f(0.f, 1.f, 0.f),
    };
    its.indices = {
        stl_triangle_vertex_indices(0, 1, 2),
    };

    TriangleMesh mesh(its);
    TriangleSelector selector(mesh);

    constexpr int painted_state = 120;
    selector.set_facet(0, static_cast<EnforcerBlockerType>(painted_state));

    auto data = selector.serialize();
    REQUIRE_FALSE(data.triangles_to_split.empty());
    REQUIRE(data.used_states.size() > painted_state);
    CHECK(data.used_states[painted_state]);

    data.reset_used_states();
    data.update_used_states(size_t(data.triangles_to_split.front().bitstream_start_idx));
    CHECK(data.used_states[painted_state]);
    CHECK(TriangleSelector::has_facets(data, static_cast<EnforcerBlockerType>(painted_state)));

    TriangleSelector restored(mesh);
    restored.deserialize(data, true, static_cast<EnforcerBlockerType>(painted_state));
    CHECK(restored.has_facets(static_cast<EnforcerBlockerType>(painted_state)));
}

TEST_CASE("Facet string leaf encoding supports the complete painted-state range", "[TriangleSelector][MMUPaint]")
{
    CHECK(encode_enforcer_blocker_type_as_facet_string(EnforcerBlockerType::NONE) == "0");
    CHECK(encode_enforcer_blocker_type_as_facet_string(EnforcerBlockerType::Extruder1) == "4");
    CHECK(encode_enforcer_blocker_type_as_facet_string(EnforcerBlockerType::Extruder2) == "8");
    CHECK(encode_enforcer_blocker_type_as_facet_string(EnforcerBlockerType::Extruder3) == "0C");
    CHECK(encode_enforcer_blocker_type_as_facet_string(EnforcerBlockerType::Extruder16) == "DC");
    CHECK(encode_enforcer_blocker_type_as_facet_string(static_cast<EnforcerBlockerType>(17)) == "EC");
    CHECK_FALSE(encode_enforcer_blocker_type_as_facet_string(static_cast<EnforcerBlockerType>(120)).empty());
    CHECK_FALSE(encode_enforcer_blocker_type_as_facet_string(EnforcerBlockerType::ExtruderMax).empty());
}
