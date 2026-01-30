#ifndef slic3r_WCPDownloadManager_hpp_
#define slic3r_WCPDownloadManager_hpp_

#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <chrono>
#include "../Utils/Http.hpp"
#include "SSWCP.hpp"
#include <boost/filesystem/path.hpp>
#include "nlohmann/json.hpp"

namespace Slic3r { namespace GUI {

// Download task state
enum class WCPDownloadState {
    Pending,
    Downloading,
    Paused,
    Completed,
    Error,
    Canceled
};

// Download task information
struct WCPDownloadTask {
    size_t task_id;
    std::string file_url;
    std::string file_name;
    std::string dest_path;
    std::weak_ptr<SSWCP_Instance> wcp_instance;  // Associated WCP instance
    Http::Ptr http_object;  // HTTP object for cancellation
    WCPDownloadState state;
    int percent;
    std::string error_message;
    
    WCPDownloadTask(size_t id, const std::string& url, const std::string& name, 
                   const std::string& path, std::shared_ptr<SSWCP_Instance> instance)
        : task_id(id), file_url(url), file_name(name), dest_path(path),
          wcp_instance(instance), state(WCPDownloadState::Pending), percent(0) {}
};

// WCP Download Manager
class WCPDownloadManager {
public:
    static WCPDownloadManager& getInstance() {
        static WCPDownloadManager instance;
        return instance;
    }
    
    // Start a download task
    size_t start_download(const std::string& file_url, 
                         const std::string& file_name,
                         std::shared_ptr<SSWCP_Instance> wcp_instance);
    
    // Cancel a download task
    bool cancel_download(size_t task_id);
    
    // Pause a download task (if needed)
    bool pause_download(size_t task_id);
    
    // Resume a download task (if needed)
    bool resume_download(size_t task_id);
    
    // Get task state
    WCPDownloadState get_task_state(size_t task_id);
    
    // Get task information
    std::shared_ptr<WCPDownloadTask> get_task(size_t task_id);
    
private:
    WCPDownloadManager() = default;
    ~WCPDownloadManager() = default;
    WCPDownloadManager(const WCPDownloadManager&) = delete;
    WCPDownloadManager& operator=(const WCPDownloadManager&) = delete;
    
    std::mutex m_tasks_mutex;
    std::unordered_map<size_t, std::shared_ptr<WCPDownloadTask>> m_tasks;
    std::atomic<size_t> m_next_task_id{1};
    
    // Track last progress update for throttling
    std::unordered_map<size_t, int> m_last_percent;
    std::unordered_map<size_t, std::chrono::steady_clock::time_point> m_last_update;
    
    // Send progress update to WCP
    void send_progress_update(std::shared_ptr<WCPDownloadTask> task, int percent, 
                              size_t downloaded, size_t total);
    
    // Send completion message to WCP
    void send_complete_update(std::shared_ptr<WCPDownloadTask> task, const std::string& file_path);
    
    // Send error message to WCP
    void send_error_update(std::shared_ptr<WCPDownloadTask> task, const std::string& error);
    
    // Clean up completed task
    void cleanup_task(size_t task_id);
};

}} // namespace Slic3r::GUI

#endif // slic3r_WCPDownloadManager_hpp_

