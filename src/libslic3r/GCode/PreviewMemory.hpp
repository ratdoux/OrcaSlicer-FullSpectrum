#ifndef slic3r_GCode_PreviewMemory_hpp_
#define slic3r_GCode_PreviewMemory_hpp_

#include <cstddef>
#include <vector>

namespace Slic3r::GCodePreview {

// Use at completion/reset boundaries, not while appending moves or updating layer sliders.
// Keep small buffers and modest growth headroom to avoid repeated reallocations.
template<class T, class Allocator> void shrink_excess_capacity(std::vector<T, Allocator>& buffer)
{
    constexpr std::size_t MIN_UNUSED_BYTES = 1024 * 1024;
    const std::size_t     unused           = buffer.capacity() - buffer.size();
    if (unused >= (MIN_UNUSED_BYTES + sizeof(T) - 1) / sizeof(T) && unused > buffer.size() / 4)
        buffer.shrink_to_fit();
}

} // namespace Slic3r::GCodePreview

#endif
