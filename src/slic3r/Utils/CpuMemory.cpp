#include "CpuMemory.hpp"

// Platform headers must stay outside namespace Slic3r: including them inside
// a namespace puts their declarations into that namespace and breaks (or
// silently depends) on the SDK's internal structure.
#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __linux__
#include <unistd.h>
#include <sys/sysinfo.h>
#elif defined(__APPLE__)
#include <mach/mach_host.h>
#include <sys/sysctl.h>
#endif

namespace Slic3r {
#ifdef _WIN32
unsigned long long GetFreeMemoryWin()
{
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    GlobalMemoryStatusEx(&status);
    return status.ullAvailPhys;
}
#endif

#if defined(__linux__) || defined(__APPLE__)
unsigned long long GetFreMemoryUnix()
{
#ifdef __linux__
    struct sysinfo info;
    if (sysinfo(&info) == 0) {
        return info.freeram * info.mem_unit;
    }
#elif __APPLE__
    int      mib[2] = {CTL_HW, HW_MEMSIZE};
    uint64_t memsize;
    size_t   len = sizeof(memsize);
    if (sysctl(mib, 2, &memsize, &len, NULL, 0) == 0) {
        vm_size_t              page_size;
        mach_port_t            mach_port;
        mach_msg_type_number_t count;
        vm_statistics64_data_t vm_stats;

        mach_port = mach_host_self();
        count     = sizeof(vm_stats) / sizeof(natural_t);
        if (host_page_size(mach_port, &page_size) == KERN_SUCCESS && host_statistics64(mach_port, HOST_VM_INFO, (host_info64_t) &vm_stats, &count) == KERN_SUCCESS) {
            return (vm_stats.free_count + vm_stats.inactive_count) * page_size;
        }
    }
#endif
    return 0;
}
#endif
unsigned long long get_free_memory()
{
#ifdef _WIN32
    return GetFreeMemoryWin();
#elif defined(__linux__) || defined(__APPLE__)
    return GetFreMemoryUnix();
#else
    return 0;
#endif
}
bool CpuMemory::CurFreeMemoryLessThanSpecifySizeGb(int size)
{
    unsigned long long free_mem = get_free_memory();
    auto cur_size = free_mem / (1024.0 * 1024.0 * 1024.0);
    static bool first_debug_free_memory = true;
    static bool first_meet_size_gb      = true;
    if (first_debug_free_memory) {
        first_debug_free_memory = false;
    }
    if (cur_size < size) {
        if (first_meet_size_gb) {
            first_meet_size_gb = false;
        }
        return true;
    }
    return false;
}

}
