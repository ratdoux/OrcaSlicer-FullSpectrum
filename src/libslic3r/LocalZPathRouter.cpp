#include "LocalZPathRouter.hpp"
#include "LocalZClipper.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace Slic3r {
bool LocalZPathRouter::enabled_from_environment()
{
    static const bool enabled = [] {
        const char* value = std::getenv("ORCA_LOCAL_Z_REUSE");
        if (!value || std::string(value) == "1")
            return true;
        if (std::string(value) == "0")
            return false;
        throw std::runtime_error("ORCA_LOCAL_Z_REUSE must be 0 or 1");
    }();
    return enabled;
}

bool LocalZPathRouter::can_reuse(const Polyline& subject, const LocalZClipper* include, const LocalZClipper* exclude) const
{
    return m_reuse && subject.is_valid() && (!include || include->empty() || include->can_reuse_whole_path(subject)) &&
           (!exclude || exclude->empty() || exclude->can_reuse_whole_path(subject, true));
}

void LocalZPathRouter::append(const ExtrusionPath&        source,
                              const LocalZClipper*        include,
                              const LocalZClipper*        exclude,
                              double                      flow_height,
                              std::vector<ExtrusionPath>& destination)
{
    const bool reusable = can_reuse(source.polyline, include, exclude);
    Polylines  paths;
    if (reusable) {
        // Match clipping's linear geometry contract: stale fitted arcs must not
        // survive. Keep the authored direction and vertices, including seam cuts.
        paths.emplace_back(source.polyline.points);
        ++m_statistics.reused;
        m_statistics.reused_points += source.polyline.points.size();
    } else {
        paths = LocalZClipper::clip_path(source.polyline, include, exclude);
        ++m_statistics.clipped;
    }
    for (Polyline& line : paths) {
        if (!line.is_valid())
            continue;
        ExtrusionPath result(std::move(line), source);
        if (flow_height > EPSILON) {
            if (result.height > EPSILON)
                result.mm3_per_mm *= flow_height / result.height;
            result.height = float(flow_height);
        }
        destination.emplace_back(std::move(result));
    }
}
} // namespace Slic3r
