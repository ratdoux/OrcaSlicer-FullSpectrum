#ifndef slic3r_PreparationDiagnostics_hpp_
#define slic3r_PreparationDiagnostics_hpp_

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <boost/log/trivial.hpp>

namespace Slic3r {

// Developer experiments only. Read once before workers start; never mutate the
// process environment during a slice. No project settings or geometry tolerances.
inline bool preparation_experiment(const char* name)
{
    const std::string feature(name);
    if (feature == "ties" || feature == "shell_groups" || feature == "slab_order") {
        static const bool legacy = [] {
            const char* value = std::getenv("ORCA_PREP_LEGACY_THREADS");
            return value && std::string(value) == "1";
        }();
        return !legacy;
    }
    static const std::string modes = [] {
        const char* value = std::getenv("ORCA_PREP_EXPERIMENTS");
        return std::string(",") + (value ? value : "") + ",";
    }();
    return modes == ",all," || modes.find(std::string(",") + name + ",") != std::string::npos;
}

inline bool preparation_diagnostic(const char* name)
{
    const char* value = std::getenv(name);
    return value && std::string(value) == "1";
}

class PreparationProfile
{
public:
    explicit PreparationProfile(std::string label) : enabled(preparation_diagnostic("ORCA_PREP_PROFILE")), m_label(std::move(label)) {}
    ~PreparationProfile()
    {
        for (auto& [name, values] : m_times) {
            std::sort(values.begin(), values.end());
            double total = 0.;
            for (double value : values)
                total += value;
            BOOST_LOG_TRIVIAL(info) << "Preparation profile context=" << m_label << " stage=" << name << " calls=" << values.size()
                                    << " elapsed_sum_ms=" << total << " p95_ms=" << values[(values.size() - 1) * 95 / 100]
                                    << " max_ms=" << values.back();
        }
    }
    void add(const char* name, double milliseconds)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_times[name].push_back(milliseconds);
    }
    const bool enabled;

private:
    std::string                                m_label;
    std::mutex                                 m_mutex;
    std::map<std::string, std::vector<double>> m_times;
};

class PreparationTimer
{
public:
    PreparationTimer(PreparationProfile* profile, const char* name)
        : m_profile(profile && profile->enabled ? profile : nullptr), m_name(name)
    {
        if (m_profile)
            m_start = Clock::now();
    }
    ~PreparationTimer() { finish(); }
    void finish()
    {
        if (m_profile) {
            m_profile->add(m_name, std::chrono::duration<double, std::milli>(Clock::now() - m_start).count());
            m_profile = nullptr;
        }
    }

private:
    using Clock = std::chrono::steady_clock;
    PreparationProfile* m_profile;
    const char*         m_name;
    Clock::time_point   m_start;
};

} // namespace Slic3r
#endif
