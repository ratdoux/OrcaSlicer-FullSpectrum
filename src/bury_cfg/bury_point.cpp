#include "bury_point.hpp"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

static std::atomic<bool> isAgreeSlice(true);

static std::atomic<bool> g_sentry_initialized(false);

bool get_sentry_flags() { return g_sentry_initialized; }

void set_sentry_flags(bool flags) { g_sentry_initialized = flags; }

bool get_privacy_policy() 
{
    return isAgreeSlice; 
}

void set_privacy_policy(bool isAgree) { 
    isAgreeSlice = isAgree; 
}

std::string get_timestamp_seconds()
{
    auto now = std::chrono::system_clock::now();

    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    std::ostringstream oss;
    oss << timestamp;
    auto strTime = oss.str();

    return strTime;
}

long long get_time_timestamp()
{
    auto now = std::chrono::system_clock::now();

    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    return timestamp;
}

std::string get_works_time(const long long& timestamp)
{ 
    long long hours        = timestamp / 3600000;
    long long remaining_ms = timestamp % 3600000;

    long long minutes = remaining_ms / 60000;
    remaining_ms     = remaining_ms % 60000;

    long long seconds = remaining_ms / 1000;
    long long ms      = remaining_ms % 1000;

    char buffer[32] = {0};
    std::snprintf(buffer, sizeof(buffer), "%02llu:%02llu:%02llu.%03llu", hours, minutes, seconds, ms);


    std::string works_time = std::string(buffer);
    
    return works_time;
}