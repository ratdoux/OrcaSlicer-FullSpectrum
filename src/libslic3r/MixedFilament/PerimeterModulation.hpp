#ifndef slic3r_MixedFilament_PerimeterModulation_hpp_
#define slic3r_MixedFilament_PerimeterModulation_hpp_

#include "../ExPolygon.hpp"
#include "../Polyline.hpp"

namespace Slic3r {

// Surface-bias values are stored as insets: negative values expose more of the
// active filament. This helper converts that inset into an outward/inward
// external-perimeter centerline shift without changing extrusion width.
bool apply_mixed_filament_perimeter_modulation(
    Polyline& polyline, const ExPolygons& layer_slices, float surface_inset_mm, float max_abs_shift_mm, float max_boundary_distance_mm);

} // namespace Slic3r

#endif
