#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/LocalZBridge.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

ExPolygon rectangle_mm(double x0, double y0, double x1, double y1)
{
    return ExPolygon(Points{Point::new_scale(x0, y0), Point::new_scale(x1, y0), Point::new_scale(x1, y1), Point::new_scale(x0, y1)});
}

// Slot interfaces of a stack that starts at `bottom`.
std::vector<double> stack_bounds(double bottom, const std::vector<double>& heights)
{
    std::vector<double> bounds{bottom};
    for (double height : heights)
        bounds.push_back(bounds.back() + height);
    return bounds;
}

// Left zone x in [0, 10], right zone x in [10, 20], both stacked from 0.2.
// A single-filament right zone takes the whole transition band.
LocalZBridgeField make_field(std::vector<unsigned int> slots,
                             std::vector<double>       left_heights,
                             std::vector<double>       right_heights,
                             double                    transition_width_mm,
                             bool                      right_is_single_filament = false)
{
    std::vector<LocalZBridgeField::Row> rows(2);
    rows[0].masks              = {rectangle_mm(0., 0., 10., 10.)};
    rows[0].bounds             = stack_bounds(0.20, left_heights);
    rows[1].masks              = {rectangle_mm(10., 0., 20., 10.)};
    rows[1].bounds             = stack_bounds(0.20, right_heights);
    rows[1].absorbs_transition = right_is_single_filament;
    return LocalZBridgeField(transition_width_mm, std::move(slots), std::move(rows));
}

// Cyan-heavy mix on the left, magenta-heavy mix on the right.
LocalZBridgeField make_two_mix_field(double transition_width_mm)
{
    return make_field({1, 2}, {0.15, 0.05}, {0.05, 0.15}, transition_width_mm);
}

ExtrusionPath horizontal_path(double x0, double x1)
{
    ExtrusionPath path(erPerimeter, 0.08, 0.45f, 0.15f);
    path.polyline = Polyline(Points{Point::new_scale(x0, 5.), Point::new_scale(x1, 5.)});
    return path;
}

// Every point stacks the whole layer, and interfaces never descend.
void check_closed_stack(const LocalZBridgeField& field, double x0, double x1)
{
    for (double x = x0; x <= x1 + 1e-9; x += 0.1) {
        const std::vector<double> heights = field.heights_at(Point::new_scale(x, 5.));
        double                    total   = 0.;
        for (double height : heights) {
            CHECK(height >= -1e-12);
            total += height;
        }
        CHECK_THAT(total, WithinAbs(0.20, 1e-9));
    }
}

} // namespace

TEST_CASE("Continuous mix bridge keeps row heights and blends across the boundary", "[LocalZ][LocalZBridge]")
{
    const LocalZBridgeField field = make_two_mix_field(2.0);
    REQUIRE(field.slot_count() == 2);
    REQUIRE(field.rows().size() == 2);

    const LocalZBridgeField::Span left   = field.slot_span_at(Point::new_scale(3., 5.), 0);
    const LocalZBridgeField::Span right  = field.slot_span_at(Point::new_scale(17., 5.), 0);
    const LocalZBridgeField::Span middle = field.slot_span_at(Point::new_scale(10., 5.), 0);
    CHECK_THAT(left.lower, WithinAbs(0.20, 1e-9));
    CHECK_THAT(left.upper, WithinAbs(0.35, 1e-9));
    CHECK_THAT(right.upper, WithinAbs(0.25, 1e-9));
    CHECK_THAT(middle.upper, WithinAbs(0.30, 1e-6));

    // The wall-side band behaves like the interior: the outer contour must not
    // pull the blend toward the boundary value.
    const LocalZBridgeField::Span left_wall = field.slot_span_at(Point::new_scale(3., 0.), 0);
    CHECK_THAT(left_wall.upper, WithinAbs(0.35, 1e-9));

    // The top slot always closes the nominal layer.
    for (double x : {1., 9.5, 10., 10.5, 19.}) {
        const LocalZBridgeField::Span top = field.slot_span_at(Point::new_scale(x, 5.), 1);
        CHECK_THAT(top.upper, WithinAbs(0.40, 1e-9));
        CHECK(top.height() >= 0.05 - 1e-9);
    }

    // The interface descends monotonically from the cyan-heavy to the magenta-heavy side.
    double previous = std::numeric_limits<double>::infinity();
    for (double x = 8.; x <= 12. + 1e-9; x += 0.25) {
        const double upper = field.slot_span_at(Point::new_scale(x, 5.), 0).upper;
        CHECK(upper <= previous + 1e-9);
        previous = upper;
    }

    const LocalZBridgeField::Span bounds = field.slot_bounds(0);
    CHECK_THAT(bounds.lower, WithinAbs(0.20, 1e-9));
    CHECK_THAT(bounds.upper, WithinAbs(0.35, 1e-9));
    CHECK_THAT(field.slot_max_height(1), WithinAbs(0.15, 1e-9));
}

TEST_CASE("Continuous mix bridge paths stay connected and flat outside the band", "[LocalZ][LocalZBridge]")
{
    const LocalZBridgeField field  = make_two_mix_field(2.0);
    const ExtrusionPath     source = horizontal_path(1., 19.);

    const std::vector<ExtrusionPathSloped> segments = field.modulate_path(source, 0, 0.);
    REQUIRE(segments.size() >= 3);

    // Flat ends keep only the source endpoints and the boundary of the band.
    CHECK(segments.front().polyline.points.front() == source.polyline.points.front());
    CHECK(segments.back().polyline.points.back() == source.polyline.points.back());
    CHECK(segments.front().is_flat());
    CHECK(segments.back().is_flat());
    CHECK_THAT(segments.front().absolute_z_begin, WithinAbs(0.35, 1e-9));
    CHECK_THAT(segments.back().absolute_z_end, WithinAbs(0.25, 1e-9));
    CHECK_THAT(double(segments.front().height), WithinAbs(0.15, 1e-6));
    CHECK_THAT(double(segments.back().height), WithinAbs(0.05, 1e-6));
    CHECK(unscale<double>(segments.front().polyline.points.back().x()) <= 9.0 + 0.21);

    double extruded_volume = 0.;
    double max_slope       = 0.;
    for (size_t idx = 0; idx < segments.size(); ++idx) {
        const ExtrusionPathSloped& segment = segments[idx];
        CHECK(segment.local_z_modulation);
        CHECK(segment.polyline.fitting_result.empty());
        CHECK(segment.height >= 0.05f - 1e-6f);
        CHECK(segment.height <= 0.15f + 1e-6f);
        // Flow scales with the local bead height; heights are stored as float.
        CHECK_THAT(segment.mm3_per_mm, WithinAbs(0.08 * double(segment.height) / double(source.height), 1e-6));
        if (idx > 0) {
            CHECK(segments[idx - 1].polyline.points.back() == segment.polyline.points.front());
            CHECK_THAT(segments[idx - 1].absolute_z_end, WithinAbs(segment.absolute_z_begin, 1e-12));
        }
        const double length = unscale<double>(segment.polyline.length());
        extruded_volume += segment.mm3_per_mm * length;
        if (length > EPSILON)
            max_slope = std::max(max_slope, std::abs(segment.absolute_z_end - segment.absolute_z_begin) / length);
    }
    // A 0.10 mm interface change over a 2 mm band peaks at 1.5x the mean slope.
    CHECK(max_slope <= 1.5 * 0.10 / 2.0 + 0.01);
    // Symmetric thicknesses around the boundary preserve the mean volume.
    CHECK_THAT(extruded_volume, WithinAbs(0.08 * (0.10 / 0.15) * 18., 1e-3));

    SECTION("z offset shifts every segment")
    {
        const std::vector<ExtrusionPathSloped> shifted = field.modulate_path(source, 0, 0.5);
        REQUIRE(shifted.size() == segments.size());
        for (size_t idx = 0; idx < shifted.size(); ++idx) {
            CHECK_THAT(shifted[idx].absolute_z_begin, WithinAbs(segments[idx].absolute_z_begin + 0.5, 1e-12));
            CHECK_THAT(shifted[idx].absolute_z_end, WithinAbs(segments[idx].absolute_z_end + 0.5, 1e-12));
        }
    }
}

TEST_CASE("Continuous mix bridge with zero transition width switches at the boundary", "[LocalZ][LocalZBridge]")
{
    const LocalZBridgeField field = make_two_mix_field(0.);
    CHECK_THAT(field.slot_span_at(Point::new_scale(9.9, 5.), 0).upper, WithinAbs(0.35, 1e-9));
    CHECK_THAT(field.slot_span_at(Point::new_scale(10.1, 5.), 0).upper, WithinAbs(0.25, 1e-9));

    const std::vector<ExtrusionPathSloped> segments = field.modulate_path(horizontal_path(1., 19.), 0, 0.);
    // Two flat runs joined by one short ramp across the boundary sample.
    REQUIRE(segments.size() == 3);
    CHECK(segments[0].is_flat());
    CHECK_FALSE(segments[1].is_flat());
    CHECK(segments[2].is_flat());
    CHECK(unscale<double>(segments[1].polyline.length()) <= 0.21);
}

TEST_CASE("Continuous mix bridge connects a mix to a single-filament zone", "[LocalZ][LocalZBridge]")
{
    // Cyan/magenta mix on the left, solid cyan on the right; cyan stacks first.
    const LocalZBridgeField field = make_field({1, 2}, {0.05, 0.15}, {0.20, 0.}, 2.0, true);

    // The mix keeps its exact ratio up to its boundary; the solid zone takes the band.
    CHECK_THAT(field.slot_span_at(Point::new_scale(9.9, 5.), 0).upper, WithinAbs(0.25, 1e-9));
    CHECK_THAT(field.slot_span_at(Point::new_scale(10., 5.), 1).height(), WithinAbs(0.15, 1e-6));

    // Inside the band magenta thins and cyan thickens; past it cyan fills the layer.
    const LocalZBridgeField::Span band_magenta = field.slot_span_at(Point::new_scale(11., 5.), 1);
    CHECK_THAT(band_magenta.height(), WithinAbs(0.075, 1e-6));
    CHECK_THAT(band_magenta.upper, WithinAbs(0.40, 1e-9));
    const LocalZBridgeField::Span solid_cyan = field.slot_span_at(Point::new_scale(12.5, 5.), 0);
    CHECK_THAT(solid_cyan.lower, WithinAbs(0.20, 1e-9));
    CHECK_THAT(solid_cyan.upper, WithinAbs(0.40, 1e-9));
    CHECK(field.slot_span_at(Point::new_scale(12.5, 5.), 1).height() <= 1e-9);
    check_closed_stack(field, 1., 19.);

    // Magenta tapers to nothing instead of ending at a thickness, leaving no pocket.
    double previous = std::numeric_limits<double>::infinity();
    for (double x = 10.; x <= 12. + 1e-9; x += 0.1) {
        const double magenta = field.slot_span_at(Point::new_scale(x, 5.), 1).height();
        CHECK(magenta <= previous + 1e-12);
        previous = magenta;
    }
    CHECK(previous <= 1e-9);

    // The cyan pass runs continuously from the mix into the solid zone.
    const std::vector<ExtrusionPathSloped> cyan = field.modulate_path(horizontal_path(1., 19.), 0, 0.);
    REQUIRE_FALSE(cyan.empty());
    CHECK(cyan.front().polyline.points.front() == Point::new_scale(1., 5.));
    CHECK(cyan.back().polyline.points.back() == Point::new_scale(19., 5.));
    for (size_t idx = 1; idx < cyan.size(); ++idx)
        CHECK(cyan[idx - 1].polyline.points.back() == cyan[idx].polyline.points.front());
    CHECK_THAT(cyan.back().absolute_z_end, WithinAbs(0.40, 1e-9));

    // The magenta pass ends where its taper reaches nothing, at the far edge of the band.
    const std::vector<ExtrusionPathSloped> magenta = field.modulate_path(horizontal_path(1., 19.), 1, 0.);
    REQUIRE_FALSE(magenta.empty());
    const double magenta_end_x = unscale<double>(magenta.back().polyline.points.back().x());
    CHECK(magenta_end_x > 11.5);
    CHECK(magenta_end_x <= 12. + 1e-9);

    // The magenta island covers the mix and the band; cyan's covers everything.
    const ExPolygons outline = {rectangle_mm(0., 0., 20., 10.)};
    CHECK_THAT(unscale<double>(get_extents(field.slot_island(1, outline)).max.x()), WithinAbs(12., 1e-3));
    CHECK_THAT(unscale<double>(get_extents(field.slot_island(0, outline)).max.x()), WithinAbs(20., 1e-3));
}

TEST_CASE("Continuous mix bridge keeps a thin painted shell at its own ratio", "[LocalZ][LocalZBridge]")
{
    // A 0.4 mm painted shell over a cyan body, as surface painting produces.
    std::vector<LocalZBridgeField::Row> rows(2);
    rows[0].masks              = {rectangle_mm(0., 0., 0.4, 10.)};
    rows[0].bounds             = stack_bounds(0.20, {0.05, 0.15});
    rows[1].masks              = {rectangle_mm(0.4, 0., 20., 10.)};
    rows[1].bounds             = stack_bounds(0.20, {0.20, 0.});
    rows[1].absorbs_transition = true;
    const LocalZBridgeField field(2.0, {1, 2}, std::move(rows));

    // The outer wall of the shell keeps the mix's exact sublayers.
    const std::vector<double> wall = field.heights_at(Point::new_scale(0.225, 5.));
    CHECK_THAT(wall[0], WithinAbs(0.05, 1e-9));
    CHECK_THAT(wall[1], WithinAbs(0.15, 1e-9));
}

TEST_CASE("Continuous mix bridge connects mixes with different filament sets", "[LocalZ][LocalZBridge]")
{
    // A cyan/magenta mix next to a cyan/magenta/yellow mix.
    const LocalZBridgeField field = make_field({1, 2, 3}, {0.10, 0.10, 0.}, {0.05, 0.05, 0.10}, 2.0);
    check_closed_stack(field, 1., 19.);
    for (double x = 1.; x <= 19. + 1e-9; x += 0.1) {
        const Point               point   = Point::new_scale(x, 5.);
        const std::vector<double> heights = field.heights_at(point);
        // The highest printed slot always ends at the layer top.
        for (size_t slot = heights.size(); slot-- > 0;)
            if (heights[slot] > 0.) {
                CHECK_THAT(field.slot_span_at(point, slot).upper, WithinAbs(0.40, 1e-9));
                break;
            }
    }

    // Cyan and magenta stay connected across the whole path.
    for (size_t slot : {size_t(0), size_t(1)}) {
        const std::vector<ExtrusionPathSloped> segments = field.modulate_path(horizontal_path(1., 19.), slot, 0.);
        REQUIRE_FALSE(segments.empty());
        CHECK(segments.front().polyline.points.front() == Point::new_scale(1., 5.));
        CHECK(segments.back().polyline.points.back() == Point::new_scale(19., 5.));
    }
    // Between two mixes the band is centered, so yellow tapers in over half a
    // band on the two-filament side.
    const std::vector<ExtrusionPathSloped> yellow = field.modulate_path(horizontal_path(1., 19.), 2, 0.);
    REQUIRE_FALSE(yellow.empty());
    const double yellow_start_x = unscale<double>(yellow.front().polyline.points.front().x());
    CHECK(yellow_start_x > 9.);
    CHECK(yellow_start_x < 9.5);
    const ExPolygons outline = {rectangle_mm(0., 0., 20., 10.)};
    CHECK_THAT(unscale<double>(get_extents(field.slot_island(2, outline)).min.x()), WithinAbs(9., 1e-3));
}

TEST_CASE("Continuous mix bridge connects stacks that start and end at different heights", "[LocalZ][LocalZBridge]")
{
    // The left mix stacks cyan 0.04 / magenta 0.12 from 0.16, the right mix a
    // whole 0.2 mm cyan layer from 0.20. Cyan is the connected slot.
    std::vector<LocalZBridgeField::Row> rows(2);
    rows[0].masks  = {rectangle_mm(0., 0., 10., 10.)};
    rows[0].bounds = {0.16, 0.20, 0.32};
    rows[1].masks  = {rectangle_mm(10., 0., 20., 10.)};
    rows[1].bounds = {0.20, 0.40, 0.40};
    const LocalZBridgeField field(2.0, {1, 2}, std::move(rows));

    // Each side keeps its own absolute interfaces away from the boundary.
    const LocalZBridgeField::Span left_cyan  = field.slot_span_at(Point::new_scale(3., 5.), 0);
    const LocalZBridgeField::Span right_cyan = field.slot_span_at(Point::new_scale(17., 5.), 0);
    CHECK_THAT(left_cyan.lower, WithinAbs(0.16, 1e-9));
    CHECK_THAT(left_cyan.upper, WithinAbs(0.20, 1e-9));
    CHECK_THAT(right_cyan.lower, WithinAbs(0.20, 1e-9));
    CHECK_THAT(right_cyan.upper, WithinAbs(0.40, 1e-9));

    // At the boundary every interface is the average of both sides.
    const LocalZBridgeField::Span boundary_cyan = field.slot_span_at(Point::new_scale(10., 5.), 0);
    CHECK_THAT(boundary_cyan.lower, WithinAbs(0.18, 1e-6));
    CHECK_THAT(boundary_cyan.upper, WithinAbs(0.30, 1e-6));

    // The cyan layer runs continuously from one side to the other.
    const std::vector<ExtrusionPathSloped> cyan = field.modulate_path(horizontal_path(1., 19.), 0, 0.);
    REQUIRE_FALSE(cyan.empty());
    CHECK(cyan.front().polyline.points.front() == Point::new_scale(1., 5.));
    CHECK(cyan.back().polyline.points.back() == Point::new_scale(19., 5.));
    for (size_t idx = 1; idx < cyan.size(); ++idx) {
        CHECK(cyan[idx - 1].polyline.points.back() == cyan[idx].polyline.points.front());
        CHECK_THAT(cyan[idx - 1].absolute_z_end, WithinAbs(cyan[idx].absolute_z_begin, 1e-12));
    }

    // Interfaces never descend.
    for (double x = 1.; x <= 19. + 1e-9; x += 0.1)
        for (double height : field.heights_at(Point::new_scale(x, 5.)))
            CHECK(height >= -1e-12);

    // Pass planes cover both stacks.
    CHECK_THAT(field.slot_bounds(0).lower, WithinAbs(0.16, 1e-9));
    CHECK_THAT(field.slot_bounds(1).upper, WithinAbs(0.40, 1e-9));
}

TEST_CASE("Continuous mix bridge tapers onto a mix that prints nothing on the layer", "[LocalZ][LocalZBridge]")
{
    // With independent heights a mix's cadence can skip a nominal layer. Its
    // stack then has zero height at its current top, 0.18, while the cyan body
    // next to it prints 0.20 to 0.28.
    auto make = [](std::vector<unsigned int> mix_filaments) {
        std::vector<LocalZBridgeField::Row> rows(2);
        rows[0].masks              = {rectangle_mm(0., 0., 10., 10.)};
        rows[0].bounds             = stack_bounds(0.20, {0.08});
        rows[0].absorbs_transition = true;
        rows[1].masks              = {rectangle_mm(10., 0., 20., 10.)};
        rows[1].bounds             = stack_bounds(0.18, {0.});
        rows[1].filaments          = std::move(mix_filaments);
        return LocalZBridgeField(2.0, {1}, std::move(rows));
    };

    const LocalZBridgeField field = make({1, 2});
    REQUIRE(field.rows().size() == 2);
    const LocalZBridgeField::Span inside = field.slot_span_at(Point::new_scale(5., 5.), 0);
    CHECK_THAT(inside.lower, WithinAbs(0.20, 1e-9));
    CHECK_THAT(inside.upper, WithinAbs(0.28, 1e-9));
    // The body's layer slopes down onto the mix's top across the band on its side.
    const LocalZBridgeField::Span middle = field.slot_span_at(Point::new_scale(9., 5.), 0);
    CHECK_THAT(middle.lower, WithinAbs(0.19, 1e-6));
    CHECK_THAT(middle.upper, WithinAbs(0.23, 1e-6));
    CHECK(field.slot_span_at(Point::new_scale(9.99, 5.), 0).height() < 1e-3);
    CHECK(field.slot_span_at(Point::new_scale(15., 5.), 0).height() <= 1e-9);

    const std::vector<ExtrusionPathSloped> cyan = field.modulate_path(horizontal_path(1., 19.), 0, 0.);
    REQUIRE_FALSE(cyan.empty());
    CHECK(cyan.back().polyline.points.back().x() <= Point::new_scale(10., 5.).x());
    CHECK(cyan.back().absolute_z_end < 0.20);

    // Without its component list the mix shares nothing on this layer, so the
    // body would end at full height and the next layer would overlap it.
    const LocalZBridgeField unshared = make({});
    CHECK_THAT(unshared.slot_span_at(Point::new_scale(9.9, 5.), 0).height(), WithinAbs(0.08, 1e-9));
}

TEST_CASE("Continuous mix bridge prints each part of a slot on the layer holding its top", "[LocalZ][LocalZBridge]")
{
    // Cyan's top falls from 0.35 on the left to 0.25 on the right and crosses
    // a nominal layer top at 0.30 in the middle of the band.
    const LocalZBridgeField field = make_two_mix_field(2.0);
    const ExtrusionPath     path  = horizontal_path(1., 19.);
    auto                    length_of = [](const std::vector<ExtrusionPathSloped>& segments) {
        double length = 0.;
        for (const ExtrusionPathSloped& segment : segments)
            length += unscale<double>(segment.polyline.length());
        return length;
    };

    const std::vector<ExtrusionPathSloped> whole = field.modulate_path(path, 0, 0.);
    const std::vector<ExtrusionPathSloped> lower = field.modulate_path(path, 0, 0., {0.20, 0.30});
    const std::vector<ExtrusionPathSloped> upper = field.modulate_path(path, 0, 0., {0.30, 0.40});
    REQUIRE_FALSE(lower.empty());
    REQUIRE_FALSE(upper.empty());
    // Together the two layers print the whole slot once.
    CHECK_THAT(length_of(lower) + length_of(upper), WithinAbs(length_of(whole), 1e-3));
    for (const ExtrusionPathSloped& segment : lower) {
        CHECK(segment.absolute_z_begin <= 0.30 + 1e-9);
        CHECK(segment.absolute_z_end <= 0.30 + 1e-9);
    }
    for (const ExtrusionPathSloped& segment : upper) {
        CHECK(segment.absolute_z_begin >= 0.30 - 1e-9);
        CHECK(segment.absolute_z_end >= 0.30 - 1e-9);
    }
    // The parts meet exactly where the top crosses 0.30.
    CHECK(upper.back().polyline.points.back() == lower.front().polyline.points.front());
    CHECK_THAT(upper.back().absolute_z_end, WithinAbs(0.30, 1e-9));
    CHECK_THAT(lower.front().absolute_z_begin, WithinAbs(0.30, 1e-9));
    // A layer above the slot prints none of it.
    CHECK(field.modulate_path(path, 0, 0., {0.40, 0.50}).empty());
}

TEST_CASE("Continuous mix bridge keeps zones that share no filament sharp", "[LocalZ][LocalZBridge]")
{
    // Solid cyan next to solid magenta: no shared filament, so no blend.
    const LocalZBridgeField field = make_field({1, 2}, {0.20, 0.}, {0., 0.20}, 2.0);
    const std::vector<double> left_weights  = field.row_weights_at(Point::new_scale(9.9, 5.));
    const std::vector<double> right_weights = field.row_weights_at(Point::new_scale(10.1, 5.));
    CHECK_THAT(left_weights[0], WithinAbs(1., 1e-12));
    CHECK_THAT(right_weights[1], WithinAbs(1., 1e-12));
    CHECK_THAT(field.slot_span_at(Point::new_scale(9.9, 5.), 0).upper, WithinAbs(0.40, 1e-9));
    CHECK(field.slot_span_at(Point::new_scale(10.1, 5.), 0).height() <= 1e-9);
}

TEST_CASE("Continuous mix bridge rejects invalid slots", "[LocalZ][LocalZBridge]")
{
    const LocalZBridgeField field = make_two_mix_field(2.0);
    CHECK(field.modulate_path(horizontal_path(1., 19.), 2, 0.).empty());

    ExtrusionPath single_point(erPerimeter, 0.08, 0.45f, 0.15f);
    single_point.polyline = Polyline(Points{Point::new_scale(1., 5.)});
    CHECK(field.modulate_path(single_point, 0, 0.).empty());
}
