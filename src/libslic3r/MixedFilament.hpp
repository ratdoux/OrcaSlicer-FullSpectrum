#ifndef slic3r_MixedFilament_hpp_
#define slic3r_MixedFilament_hpp_

#include <string>
#include <vector>
#include <algorithm>
#include <cstdint>

namespace Slic3r {

// Represents a virtual "mixed" filament created by alternating layers of two
// physical filaments.  The display colour is an additive RGB blend so that,
// for example, Red + Green previews as Yellow.
struct MixedFilament
{
    // 1-based physical filament IDs that are combined.
    unsigned int component_a = 1;
    unsigned int component_b = 2;

    // Layer-alternation ratio.  With ratio_a = 2, ratio_b = 1 the cycle is
    // A, A, B, A, A, B, ...
    int ratio_a = 1;
    int ratio_b = 1;

    // Whether this mixed filament is enabled (available for assignment).
    bool enabled = true;

    // Computed display colour as "#RRGGBB".
    std::string display_color;

    bool operator==(const MixedFilament &rhs) const
    {
        return component_a == rhs.component_a &&
               component_b == rhs.component_b &&
               ratio_a     == rhs.ratio_a     &&
               ratio_b     == rhs.ratio_b     &&
               enabled     == rhs.enabled;
    }
    bool operator!=(const MixedFilament &rhs) const { return !(*this == rhs); }
};

// ---------------------------------------------------------------------------
// MixedFilamentManager
//
// Owns the list of mixed filaments and provides helpers used by the slicing
// pipeline to resolve virtual IDs back to physical extruders.
//
// Virtual filament IDs are numbered starting at (num_physical + 1).  For a
// 4-extruder printer the first mixed filament has ID 5, the second 6, etc.
// ---------------------------------------------------------------------------
class MixedFilamentManager
{
public:
    MixedFilamentManager() = default;

    // ---- Auto-generation ------------------------------------------------

    // Rebuild the mixed-filament list from the current set of physical
    // filament colours.  Generates all C(N,2) pairwise combinations.
    // Previous ratio/enabled state is preserved when a combination still
    // exists.
    void auto_generate(const std::vector<std::string> &filament_colours);

    // Remove a physical filament (1-based ID) from the mixed list.
    // Any mixed filament that contains the removed component is deleted.
    // Remaining component IDs are shifted down to stay aligned with physical IDs.
    void remove_physical_filament(unsigned int deleted_filament_id);

    // ---- Queries --------------------------------------------------------

    // True when `filament_id` (1-based) refers to a mixed filament.
    bool is_mixed(unsigned int filament_id, size_t num_physical) const
    {
        return filament_id > num_physical && index_of(filament_id, num_physical) < m_mixed.size();
    }

    // Resolve a mixed filament ID to a physical extruder (1-based) for the
    // given `layer_index`.  Returns `filament_id` unchanged when it is not a
    // mixed filament.
    unsigned int resolve(unsigned int filament_id, size_t num_physical, int layer_index) const;

    // Compute a display colour by additively blending the two component
    // colours.  `filament_colours` contains the physical colours only.
    static std::string blend_color(const std::string &color_a,
                                   const std::string &color_b,
                                   int ratio_a, int ratio_b);

    // ---- Accessors ------------------------------------------------------

    const std::vector<MixedFilament> &mixed_filaments() const { return m_mixed; }
    std::vector<MixedFilament>       &mixed_filaments()       { return m_mixed; }

    size_t enabled_count() const;

    // Total filament count = num_physical + number of *enabled* mixed filaments.
    size_t total_filaments(size_t num_physical) const { return num_physical + enabled_count(); }

    // Return the display colours of all enabled mixed filaments (in order).
    std::vector<std::string> display_colors() const;

private:
    // Convert a 1-based virtual ID to a 0-based index into m_mixed.
    size_t index_of(unsigned int filament_id, size_t num_physical) const
    {
        return static_cast<size_t>(filament_id - num_physical - 1);
    }

    std::vector<MixedFilament> m_mixed;
};

} // namespace Slic3r

#endif /* slic3r_MixedFilament_hpp_ */
