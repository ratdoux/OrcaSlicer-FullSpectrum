#include <catch2/catch_test_macros.hpp>
#include <test_utils.hpp>

#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/AABBTreeIndirect.hpp>

using namespace Slic3r;

TEST_CASE("Building a tree over a box, ray caster and closest query", "[AABBIndirect]")
{
    TriangleMesh tmesh = make_cube(1., 1., 1.);

    auto tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(tmesh.its.vertices, tmesh.its.indices);
    REQUIRE(! tree.empty());

    igl::Hit hit;
	bool intersected = AABBTreeIndirect::intersect_ray_first_hit(
		tmesh.its.vertices, tmesh.its.indices,
		tree,
		Vec3d(0.5, 0.5, -5.),
		Vec3d(0., 0., 1.),
		hit);

    REQUIRE(intersected);
    REQUIRE_THAT(hit.t, WithinRel(5., 0.001));

    std::vector<igl::Hit> hits;
	bool intersected2 = AABBTreeIndirect::intersect_ray_all_hits(
		tmesh.its.vertices, tmesh.its.indices,
		tree,
        Vec3d(0.3, 0.5, -5.),
		Vec3d(0., 0., 1.),
		hits);
    REQUIRE(intersected2);
    REQUIRE(hits.size() == 2);
    REQUIRE_THAT(hits.front().t, WithinRel(5., 0.001));
    REQUIRE_THAT(hits.back().t, WithinRel(6., 0.001));

    size_t hit_idx;
    Vec3d  closest_point;
    double squared_distance = AABBTreeIndirect::squared_distance_to_indexed_triangle_set(
		tmesh.its.vertices, tmesh.its.indices,
		tree,
        Vec3d(0.3, 0.5, -5.),
		hit_idx, closest_point);
    REQUIRE_THAT(squared_distance, WithinRel(5. * 5., 0.001));
    REQUIRE_THAT(closest_point.x(), WithinRel(0.3, 0.001));
    REQUIRE_THAT(closest_point.y(), WithinRel(0.5, 0.001));
    REQUIRE_THAT(closest_point.z(), WithinRel(0., 0.001));

    squared_distance = AABBTreeIndirect::squared_distance_to_indexed_triangle_set(
		tmesh.its.vertices, tmesh.its.indices,
		tree,
        Vec3d(0.3, 0.5, 5.),
		hit_idx, closest_point);
    REQUIRE_THAT(squared_distance, WithinRel(4. * 4., 0.001));
    REQUIRE_THAT(closest_point.x(), WithinRel(0.3, 0.001));
    REQUIRE_THAT(closest_point.y(), WithinRel(0.5, 0.001));
    REQUIRE_THAT(closest_point.z(), WithinRel(1., 0.001));
}
