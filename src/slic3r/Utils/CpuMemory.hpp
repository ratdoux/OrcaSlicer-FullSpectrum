#ifndef slic3r_CpuMemory_hpp_
#define slic3r_CpuMemory_hpp_

namespace Slic3r {

// Minimum free memory required for LOD rendering (in GB)
#define LOD_FREE_MEMORY_SIZE 5

class CpuMemory
{
public:
    // Returns true if current free memory is less than the specified size in GB
    static bool CurFreeMemoryLessThanSpecifySizeGb(int sizeGb);
};

} // namespace Slic3r

#endif
