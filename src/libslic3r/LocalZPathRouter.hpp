#ifndef slic3r_LocalZPathRouter_hpp_
#define slic3r_LocalZPathRouter_hpp_

#include "ExtrusionEntity.hpp"

namespace Slic3r {
class LocalZClipper;

// Export-layer-owned routing. Reuse only after proving the complete path belongs
// to this pass; preserve existing clipping for partial or ambiguous ownership.
// It never chooses a tool, Z plane, seam, dependency order or modulation recipe.
class LocalZPathRouter
{
public:
    struct Statistics
    {
        size_t reused = 0, clipped = 0, reused_points = 0;
    };
    static bool enabled_from_environment();
    explicit LocalZPathRouter(bool reuse = enabled_from_environment()) : m_reuse(reuse) {}
    // Also used before FullSpectrum's exact intact-loop check. This proof does
    // not flatten loops: their seam and modulation handling remains downstream.
    bool              can_reuse(const Polyline& subject, const LocalZClipper* include, const LocalZClipper* exclude) const;
    void              append(const ExtrusionPath&        source,
                             const LocalZClipper*        include,
                             const LocalZClipper*        exclude,
                             double                      flow_height,
                             std::vector<ExtrusionPath>& destination);
    const Statistics& statistics() const { return m_statistics; }

private:
    bool       m_reuse;
    Statistics m_statistics;
};
} // namespace Slic3r
#endif
