// Process-only x64 sampling fallback when ETW recording is unavailable.
// Build with MSVC: cl /std:c++17 /O2 /EHsc profile_slicing_windows.cpp /link dbghelp.lib
// Usage: profiler PID OUTPUT_PREFIX SYMBOL_DIRECTORY [period_ms]
// Only attach to a disposable CLI slice, never to the user's interactive process.
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <dbghelp.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {
using QuerySystem = NTSTATUS(NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);

struct Snapshot
{
    DWORD                      tid   = 0;
    ULONG                      state = 0;
    CONTEXT                    context{};
    DWORD64                    stack_start = 0;
    std::vector<unsigned char> stack;
};

const Snapshot* current_snapshot = nullptr;

BOOL CALLBACK read_memory(HANDLE process, DWORD64 address, PVOID buffer, DWORD count, LPDWORD read)
{
    const Snapshot& snapshot = *current_snapshot;
    if (address >= snapshot.stack_start && address - snapshot.stack_start < snapshot.stack.size()) {
        const size_t offset    = size_t(address - snapshot.stack_start);
        const size_t available = std::min<size_t>(count, snapshot.stack.size() - offset);
        memcpy(buffer, snapshot.stack.data() + offset, available);
        *read = DWORD(available);
        return available == count;
    }
    SIZE_T     actual = 0;
    const BOOL result = ReadProcessMemory(process, reinterpret_cast<void*>(address), buffer, count, &actual);
    *read             = DWORD(actual);
    return result;
}

double seconds(FILETIME time) { return double((ULONGLONG(time.dwHighDateTime) << 32) | time.dwLowDateTime) / 10000000.; }

double unix_time()
{
    FILETIME time;
    GetSystemTimePreciseAsFileTime(&time);
    return seconds(time) - 11644473600.;
}

std::string json_string(const char* value)
{
    std::string result = "\"";
    for (const unsigned char ch : std::string(value ? value : "")) {
        if (ch == '\\' || ch == '"')
            result += '\\';
        if (ch >= 32)
            result += char(ch);
    }
    return result + '"';
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 4 || argc > 5) {
        fprintf(stderr, "Usage: profiler PID OUTPUT_PREFIX SYMBOL_DIRECTORY [period_ms]\n");
        return 2;
    }
    const DWORD pid     = strtoul(argv[1], nullptr, 10);
    const DWORD period  = argc == 5 ? std::max(10UL, strtoul(argv[4], nullptr, 10)) : 20;
    HANDLE      process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid);
    if (!process) {
        fprintf(stderr, "OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }
    auto              query    = reinterpret_cast<QuerySystem>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    const std::string prefix   = argv[2];
    FILE*             samples  = fopen((prefix + "_stacks.csv").c_str(), "wb");
    FILE*             timeline = fopen((prefix + "_timeline.csv").c_str(), "wb");
    FILE*             symbols  = fopen((prefix + "_symbols.jsonl").c_str(), "wb");
    if (!query || !samples || !timeline || !symbols)
        return 1;
    setvbuf(samples, nullptr, _IOFBF, 1024 * 1024);
    fprintf(samples, "unix_time,tid,state,addresses_leaf_first\n");
    fprintf(timeline, "unix_time,user_seconds,kernel_seconds,running,ready,waiting,batch_seconds\n");
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_NO_PROMPTS | SYMOPT_FAIL_CRITICAL_ERRORS);
    if (!SymInitialize(process, argv[3], TRUE)) {
        fprintf(stderr, "SymInitialize failed: %lu\n", GetLastError());
        return 1;
    }
    std::vector<unsigned char>       system_info(4 * 1024 * 1024);
    std::map<DWORD, HANDLE>          threads;
    std::set<DWORD64>                addresses;
    std::array<unsigned char, 65536> stack_buffer;
    unsigned long long               sample_count = 0, capture_failures = 0, max_depth_count = 0;
    double                           suspended_seconds = 0, max_suspension = 0;
    double                           last_refresh = 0;
    unsigned                         random_state = 0x71b341;
    printf("Sampling PID %lu at approximately %lu ms; output %s\n", pid, period, prefix.c_str());
    fflush(stdout);
    const double started = unix_time();
    while (WaitForSingleObject(process, 0) == WAIT_TIMEOUT && unix_time() - started < 7200) {
        const double batch_start = unix_time();
        if (batch_start - last_refresh > 2) {
            SymRefreshModuleList(process);
            last_refresh = batch_start;
        }
        ULONG    required = 0;
        NTSTATUS status   = query(SystemProcessInformation, system_info.data(), ULONG(system_info.size()), &required);
        if (status == NTSTATUS(0xc0000004L)) {
            system_info.resize(required + 1024 * 1024);
            continue;
        }
        if (status < 0) {
            fprintf(stderr, "NtQuerySystemInformation failed: %lx\n", status);
            break;
        }
        auto* entry = reinterpret_cast<SYSTEM_PROCESS_INFORMATION*>(system_info.data());
        while (DWORD(ULONG_PTR(entry->UniqueProcessId)) != pid && entry->NextEntryOffset)
            entry = reinterpret_cast<SYSTEM_PROCESS_INFORMATION*>(reinterpret_cast<unsigned char*>(entry) + entry->NextEntryOffset);
        if (DWORD(ULONG_PTR(entry->UniqueProcessId)) != pid)
            break;
        auto*                 thread_info = reinterpret_cast<SYSTEM_THREAD_INFORMATION*>(entry + 1);
        const double          timestamp   = unix_time();
        unsigned              running = 0, ready = 0, waiting = 0;
        std::vector<Snapshot> batch;
        batch.reserve(entry->NumberOfThreads);
        // Capture all selected threads before doing any symbol/unwind work.
        for (ULONG i = 0; i < entry->NumberOfThreads; ++i) {
            const auto& info = thread_info[i];
            running += info.ThreadState == 2;
            ready += info.ThreadState == 1 || info.ThreadState == 3;
            waiting += info.ThreadState == 5;
            if (info.ThreadState != 2 && info.ThreadState != 1 && info.ThreadState != 3)
                continue;
            const DWORD tid    = DWORD(ULONG_PTR(info.ClientId.UniqueThread));
            HANDLE&     thread = threads[tid];
            if (!thread)
                thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
            if (!thread)
                continue;
            Snapshot snapshot;
            snapshot.tid                  = tid;
            snapshot.state                = info.ThreadState;
            snapshot.context.ContextFlags = CONTEXT_FULL;
            const double suspend_start    = unix_time();
            if (SuspendThread(thread) == DWORD(-1)) {
                ++capture_failures;
                continue;
            }
            // No allocation, symbol loading or stack unwinding while suspended.
            bool                     captured = GetThreadContext(thread, &snapshot.context) != FALSE;
            MEMORY_BASIC_INFORMATION memory{};
            SIZE_T                   bytes = 0;
            if (captured) {
                snapshot.stack_start = snapshot.context.Rsp;
                captured             = VirtualQueryEx(process, reinterpret_cast<void*>(snapshot.stack_start), &memory, sizeof(memory)) != 0;
                if (captured) {
                    const SIZE_T count = std::min<SIZE_T>(stack_buffer.size(),
                                                          DWORD64(memory.BaseAddress) + memory.RegionSize - snapshot.stack_start);
                    captured = ReadProcessMemory(process, reinterpret_cast<void*>(snapshot.stack_start), stack_buffer.data(), count,
                                                 &bytes) != FALSE;
                }
            }
            const DWORD resumed = ResumeThread(thread);
            if (resumed == DWORD(-1)) {
                fprintf(stderr, "ResumeThread failed for %lu: %lu\n", tid, GetLastError());
                return 1;
            }
            const double pause = unix_time() - suspend_start;
            suspended_seconds += pause;
            max_suspension = std::max(max_suspension, pause);
            if (!captured) {
                ++capture_failures;
                continue;
            }
            snapshot.stack.assign(stack_buffer.begin(), stack_buffer.begin() + bytes);
            batch.push_back(std::move(snapshot));
        }
        for (auto& snapshot : batch) {
            current_snapshot = &snapshot;
            STACKFRAME64 frame{};
            frame.AddrPC    = {snapshot.context.Rip, 0, AddrModeFlat};
            frame.AddrFrame = {snapshot.context.Rbp, 0, AddrModeFlat};
            frame.AddrStack = {snapshot.context.Rsp, 0, AddrModeFlat};
            fprintf(samples, "%.6f,%lu,%lu,", timestamp, snapshot.tid, snapshot.state);
            DWORD64  previous_pc = 0, previous_sp = 0;
            unsigned depth = 0;
            for (; depth < 96 && frame.AddrPC.Offset; ++depth) {
                if (frame.AddrPC.Offset != previous_pc || frame.AddrStack.Offset != previous_sp) {
                    fprintf(samples, "%s%llx", depth ? ";" : "", frame.AddrPC.Offset);
                    addresses.insert(frame.AddrPC.Offset);
                }
                previous_pc = frame.AddrPC.Offset;
                previous_sp = frame.AddrStack.Offset;
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, threads[snapshot.tid], &frame, &snapshot.context, read_memory,
                                 SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                    break;
                if (depth > 0 && frame.AddrPC.Offset == previous_pc && frame.AddrStack.Offset == previous_sp)
                    break;
            }
            max_depth_count += depth == 96;
            fprintf(samples, "\n");
            ++sample_count;
        }
        FILETIME creation, exit_time, kernel, user;
        if (GetProcessTimes(process, &creation, &exit_time, &kernel, &user))
            fprintf(timeline, "%.6f,%.6f,%.6f,%u,%u,%u,%.6f\n", unix_time(), seconds(user), seconds(kernel), running, ready, waiting,
                    unix_time() - batch_start);
        random_state              = random_state * 1664525U + 1013904223U;
        const double remaining_ms = double(period) + int(random_state % 7) - 3 - 1000. * (unix_time() - batch_start);
        if (remaining_ms > 0)
            Sleep(DWORD(remaining_ms));
    }
    fclose(samples);
    fclose(timeline);
    printf("Captured %llu stacks, %zu PCs, %llu failures, %llu depth limits. Suspend total %.3fs, max %.6fs. Resolving symbols...\n",
           sample_count, addresses.size(), capture_failures, max_depth_count, suspended_seconds, max_suspension);
    fflush(stdout);
    for (const DWORD64 address : addresses) {
        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
        auto*                     symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
        symbol->SizeOfStruct             = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen               = MAX_SYM_NAME;
        DWORD64         displacement     = 0;
        const bool      resolved         = SymFromAddr(process, address, &displacement, symbol) != FALSE;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct       = sizeof(line);
        DWORD line_displacement = 0;
        SymGetLineFromAddr64(process, address, &line_displacement, &line);
        IMAGEHLP_MODULE64 module{};
        module.SizeOfStruct = sizeof(module);
        SymGetModuleInfo64(process, address, &module);
        fprintf(symbols, "{\"pc\":\"%llx\",\"name\":%s,\"module\":%s,\"file\":%s,\"line\":%lu,\"displacement\":%llu}\n", address,
                json_string(resolved ? symbol->Name : "[unresolved]").c_str(), json_string(module.ModuleName).c_str(),
                json_string(line.FileName).c_str(), line.LineNumber, displacement);
    }
    fclose(symbols);
    for (auto [tid, thread] : threads)
        if (thread)
            CloseHandle(thread);
    SymCleanup(process);
    CloseHandle(process);
    printf("Finished.\n");
    return 0;
}
