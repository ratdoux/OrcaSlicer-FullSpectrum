#ifndef slic3r_TimeSyncManager_hpp_
#define slic3r_TimeSyncManager_hpp_

#include <shared_mutex>
#include <chrono>
#include <nlohmann/json.hpp>

namespace Slic3r {

class TimeSyncManager {
public:
    TimeSyncManager() = default;
    ~TimeSyncManager() = default;

    // Add time fields to the request
    void addTimeFields(nlohmann::json& request);

    // Process response and update time sync state
    void updateFromResponse(const nlohmann::json& response);

    // Reset time sync state (used for reconnection)
    void reset();

    // Get sync status
    bool isSynced() const;

    // Get current system time (seconds)
    static int64_t getCurrentTimeSec();

private:
    mutable std::shared_mutex mutex_;
    int64_t clock_offset_ = 0;     // Device clock offset relative to client (seconds), dev = cli + offset
    int64_t rtt_ = 0;              // Latest round-trip time (seconds)
    int64_t last_cli_time_ = 0;    // Client time of the last request (seconds)
    bool is_synced_ = false;       // Whether the first sync has completed
    int sync_fail_count_ = 0;      // Consecutive sync failure count

    // Calculate device time
    int64_t calculateDevTime() const;
};

} // namespace Slic3r

#endif // slic3r_TimeSyncManager_hpp_
