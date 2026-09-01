#pragma once

#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Slic3r {

class AllowlistManager
{
public:
    using StringList = std::list<std::string>;

    static AllowlistManager& instance();
    static void uninit();

    StringList get_list(const std::string& section, const std::string& key) const;

private:
    enum class State
    {
        Uninitialized,
        Initialized,
        Failed,
        Terminated
    };

    using Section = std::unordered_map<std::string, StringList>;
    using Sections = std::unordered_map<std::string, Section>;

    AllowlistManager() = default;
    ~AllowlistManager() = default;

    AllowlistManager(const AllowlistManager&) = delete;
    AllowlistManager& operator=(const AllowlistManager&) = delete;

    static AllowlistManager& raw_instance();

    void init();
    void release();

private:
    mutable std::mutex m_mutex;
    State              m_state{ State::Uninitialized };
    Sections           m_sections;
};

} // namespace Slic3r
