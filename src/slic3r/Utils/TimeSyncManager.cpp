#include "TimeSyncManager.hpp"
#include <boost/log/trivial.hpp>
#include <mutex>
namespace Slic3r {

int64_t TimeSyncManager::getCurrentTimeSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

void TimeSyncManager::addTimeFields(nlohmann::json& request) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    int64_t current_time = getCurrentTimeSec();

    // Detect clock rollback
    if (last_cli_time_ > 0 && current_time < last_cli_time_) {
        BOOST_LOG_TRIVIAL(warning) << "[TimeSyncManager] Clock rollback detected, resetting sync state";
        clock_offset_ = 0;
        rtt_ = 0;
        last_cli_time_ = 0;
        is_synced_ = false;
        sync_fail_count_ = 0;
    }

    int64_t dev_time_value = is_synced_ ? calculateDevTime() : -1;
    request["cli_time"] = current_time;
    request["dev_time"] = dev_time_value;

    BOOST_LOG_TRIVIAL(info) << "[TimeSyncManager] addTimeFields: cli_time=" << current_time
                            << ", dev_time=" << dev_time_value
                            << ", is_synced=" << is_synced_;

    last_cli_time_ = current_time;
}

void TimeSyncManager::updateFromResponse(const nlohmann::json& response) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    // Check required fields
    if (!response.contains("cli_time") || !response.contains("dev_time")) {
        BOOST_LOG_TRIVIAL(warning) << "[TimeSyncManager] Response missing time fields";
        sync_fail_count_++;
        if (sync_fail_count_ >= 3) {
            BOOST_LOG_TRIVIAL(error) << "[TimeSyncManager] 3 consecutive sync failures, resetting";
            is_synced_ = false;
            sync_fail_count_ = 0;
        }
        return;
    }

    // Check field types
    if (!response["cli_time"].is_number() || !response["dev_time"].is_number()) {
        BOOST_LOG_TRIVIAL(error) << "[TimeSyncManager] Invalid time field types";
        sync_fail_count_++;
        if (sync_fail_count_ >= 3) {
            is_synced_ = false;
            sync_fail_count_ = 0;
        }
        return;
    }

    // NTP-style time synchronization algorithm:
    // cli_time1 = client time when the request was sent (recorded by last addTimeFields call)
    // dev_time  = device time when processing the request (carried in response, unit: seconds)
    // cli_time2 = client time when the response was received
    // RTT = cli_time2 - cli_time1
    // One-way delay estimate = RTT / 2
    // clock_offset = dev_time - (cli_time1 + RTT / 2)
    // Estimated current device time = current_cli_time + clock_offset

    int64_t cli_time1 = last_cli_time_;
    int64_t cli_time2 = getCurrentTimeSec();
    int64_t dev_time = response["dev_time"].get<int64_t>();

    rtt_ = cli_time2 - cli_time1;
    clock_offset_ = dev_time - (cli_time1 + rtt_ / 2);
    is_synced_ = true;
    sync_fail_count_ = 0;

    BOOST_LOG_TRIVIAL(info) << "[TimeSyncManager] Sync updated: rtt=" << rtt_
                            << "s, offset=" << clock_offset_ << "s"
                            << ", cli_time1=" << cli_time1
                            << ", cli_time2=" << cli_time2
                            << ", dev_time=" << dev_time;
}

void TimeSyncManager::reset() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    clock_offset_ = 0;
    rtt_ = 0;
    last_cli_time_ = 0;
    is_synced_ = false;
    sync_fail_count_ = 0;
    BOOST_LOG_TRIVIAL(info) << "[TimeSyncManager] Sync state reset";
}

bool TimeSyncManager::isSynced() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return is_synced_;
}

int64_t TimeSyncManager::calculateDevTime() const {
    return getCurrentTimeSec() + clock_offset_;
}

} // namespace Slic3r
