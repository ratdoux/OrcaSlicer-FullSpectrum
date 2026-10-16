#ifndef slic3r_SlicingAcceleration_hpp_
#define slic3r_SlicingAcceleration_hpp_

#include <cstdlib>
#include <string>

namespace Slic3r {
// Developer controls for isolated reference comparisons. Normal builds use the
// validated defaults; no project or preference setting is required.
inline bool slicing_acceleration(const char* name)
{
    static const std::string modes = [] {
        const char* value = std::getenv("ORCA_SLICE_EXPERIMENTS");
        return std::string(",") + (value ? value : "all") + ",";
    }();
    return modes == ",all," || modes.find(std::string(",") + name + ",") != std::string::npos;
}
inline bool slicing_acceleration_validate()
{
    static const bool enabled = [] {
        const char* value = std::getenv("ORCA_SLICE_VALIDATE");
        return value && std::string(value) == "1";
    }();
    return enabled;
}
} // namespace Slic3r
#endif
