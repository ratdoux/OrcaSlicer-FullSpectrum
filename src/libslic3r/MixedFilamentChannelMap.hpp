#ifndef slic3r_MixedFilamentChannelMap_hpp_
#define slic3r_MixedFilamentChannelMap_hpp_

#include "MixedFilament.hpp"
#include <vector>

namespace Slic3r {

// Resolve on the calling thread: even const manager queries may synchronize a
// borrowed legacy cache. Workers consume this immutable snapshot instead.
// Segmentation channel 0 is the parent region; other channels are filament IDs.
inline std::vector<int> mixed_filament_channel_rows(const MixedFilamentManager& manager, size_t physical_count, size_t channel_count)
{
    std::vector<int> rows(channel_count, -1);
    for (size_t channel = 0; channel < channel_count; ++channel)
        rows[channel] = manager.mixed_index_from_filament_id(unsigned(channel), physical_count);
    return rows;
}

} // namespace Slic3r
#endif
