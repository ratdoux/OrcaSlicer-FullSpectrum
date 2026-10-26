#ifndef slic3r_GCode_PreviewLayerGrid_hpp_
#define slic3r_GCode_PreviewLayerGrid_hpp_

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r::GCodePreview {

// Spatial preview bands, independent of the order in which tools visit them.
// A move belongs to the band containing its endpoint; G-code is not modified.
struct LayerGrid
{
    double height       = 0.;
    double first_height = 0.;
    bool   enabled      = false;

    int index_at(double z) const
    {
        if (!enabled || !std::isfinite(z) || !std::isfinite(first_height) || !std::isfinite(height) || !(height > 0.))
            return -1;
        // G-code rounds Z to microns and preview coordinates are floats.
        return int(std::clamp(std::ceil((z - first_height - 1e-4) / height), 0., double(std::numeric_limits<int>::max())));
    }

    double top_z(int index) const { return first_height + std::max(0, index) * height; }

    bool contains(double z, double first_top, double last_top) const
    {
        const int index = index_at(z);
        return index >= 0 && index_at(first_top) <= index && index <= index_at(last_top);
    }
};

} // namespace Slic3r::GCodePreview

#endif
