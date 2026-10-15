#include <catch2/catch_test_macros.hpp>
#include "libslic3r/PerimeterGenerator.hpp"
#include "libslic3r/ClipperUtils.hpp"
using namespace Slic3r;
TEST_CASE("Classic walls reuse nozzle offsets for distinct wall widths", "[CPUSlicing][WallSupport]")
{
    for (float nozzle : {0.25f, 0.4f, 0.6f, 0.8f}) {
        PrintConfig       print;
        PrintRegionConfig region;
        PrintObjectConfig object;
        print.nozzle_diameter.values = {double(nozzle)};
        region.wall_filament.value   = 1;
        SurfaceCollection         slices, fill;
        ExtrusionEntityCollection loops, gaps;
        ExPolygons                no_overlap;
        ExPolygon                 lower(Polygon{Points{{0, 0}, {10000000, 0}, {10000000, 10000000}, {0, 10000000}}});
        lower.holes.push_back(Polygon{Points{{2000000, 2000000}, {2000000, 8000000}, {8000000, 8000000}, {8000000, 2000000}}});
        ExPolygons         below{lower};
        LayerRegionPtrs compatible;
        PerimeterGenerator generator(&slices, &compatible, 0.1, 0.15, Flow(nozzle * 1.1f, 0.1f, nozzle), &region, &object, &print, false,
                                     &loops, &gaps, &fill, &no_overlap);
        generator.layer_id           = 1;
        generator.lower_slices       = &below;
        generator.ext_perimeter_flow = Flow(nozzle * 1.2f, 0.1f, nozzle);
        generator.process_classic();
        const auto expected = offset(below, float(scale_(float(0.5 * nozzle))));
        REQUIRE(generator.m_lower_polygons_series.size() == 2);
        CHECK(generator.m_lower_polygons_series.back() == expected);
        CHECK(generator.m_external_lower_polygons_series.back() == expected);
        CHECK(generator.m_smaller_external_lower_polygons_series.back() == expected);
    }
}
