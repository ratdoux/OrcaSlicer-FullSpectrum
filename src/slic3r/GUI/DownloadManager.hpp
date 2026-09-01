#ifndef slic3r_DownloadManager_hpp_
#define slic3r_DownloadManager_hpp_

#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <functional>
#include "../Utils/Http.hpp"
#include "SSWCP.hpp"
#include <boost/filesystem/path.hpp>
#include "nlohmann/json.hpp"

namespace Slic3r { namespace GUI {

// Download task state (renamed to avoid conflict with Downloader::DownloadState)
enum class DownloadTaskState {
    Pending,
    Downloading,
    Paused,
    Completed,
    Error,
    Canceled
};

// Download callback interface for internal downloads
struct DownloadCallbacks {
    std::function<void(size_t task_id, int percent, size_t downloaded, size_t total)> on_progress;
    std::function<void(size_t task_id, const std::string& file_path)> on_complete;
    std::function<void(size_t task_id, const std::string& error)> on_error;
    // Called right before writing the downloaded body to disk.
    std::function<bool()> on_should_write;
    
    DownloadCallbacks() = default;
    DownloadCallbacks(
        std::function<void(size_t, int, size_t, size_t)> progress,
        std::function<void(size_t, const std::string&)> complete,
        std::function<void(size_t, const std::string&)> error)
        : on_progress(std::move(progress))
        , on_complete(std::move(complete))
        , on_error(std::move(error))
    {}
};

// Download task information
struct DownloadTask {
    size_t task_id;
    std::string file_url;
    std::string file_name;
    std::string dest_path;
    
    std::weak_ptr<SSWCP_Instance> wcp_instance;
    
    DownloadCallbacks callbacks;
    
    Http::Ptr http_object;  
    DownloadTaskState state;
    int percent;
    std::string error_message;

    bool need_decrypt;
    std::string sn;
    std::string decrypt_path;

    bool auto_finish_job;
    bool use_original_event_id;

    // Constructor for WCP downloads
    DownloadTask(size_t id, const std::string& url, const std::string& name,
                 const std::string& path, std::shared_ptr<SSWCP_Instance> instance,
                 bool use_original_event = false, bool decrypt = true,
                 const std::string& sn_param = "")
        : task_id(id), file_url(url), file_name(name), dest_path(path)
        , wcp_instance(instance), state(DownloadTaskState::Pending), percent(0)
        , need_decrypt(decrypt), sn(sn_param)
        , auto_finish_job(false), use_original_event_id(use_original_event)
    {}
    
    // Constructor for internal downloads
    DownloadTask(size_t id, const std::string& url, const std::string& name,
                 const std::string& path, DownloadCallbacks cb)
        : task_id(id), file_url(url), file_name(name), dest_path(path)
        , callbacks(std::move(cb)), state(DownloadTaskState::Pending), percent(0)
        , need_decrypt(false), auto_finish_job(false), use_original_event_id(false)
    {}

    // Constructor for internal downloads with encryption support
    DownloadTask(size_t id, const std::string& url, const std::string& name,
                 const std::string& path, DownloadCallbacks cb,
                 bool decrypt, const std::string& sn_param)
        : task_id(id), file_url(url), file_name(name), dest_path(path)
        , callbacks(std::move(cb)), state(DownloadTaskState::Pending), percent(0)
        , need_decrypt(decrypt), sn(sn_param)
        , auto_finish_job(false), use_original_event_id(false)
    {}
    
    // Check if this is a WCP download
    bool is_wcp_download() const {
        return !wcp_instance.expired();
    }
};


class DownloadManager {
public:
    static DownloadManager& getInstance() {
        static DownloadManager instance;
        return instance;
    }
    
    // ============================================================================
    // WCP Download Interface (for Web-to-PC communication)
    // ============================================================================
    size_t start_wcp_download(const std::string& file_url,
                              const std::string& file_name,
                              std::shared_ptr<SSWCP_Instance> wcp_instance,
                              bool use_original_event_id = false);

    size_t start_wcp_download(const std::string& file_url,
                              const std::string& file_name,
                              std::shared_ptr<SSWCP_Instance> wcp_instance,
                              bool use_original_event_id,
                              bool need_decrypt,
                              const std::string& sn);
    
    // ============================================================================
    // Internal Download Interface (for PC internal use)
    // ============================================================================
    size_t start_internal_download(const std::string& file_url,
                                    const std::string& file_name,
                                    const std::string& dest_path,
                                    DownloadCallbacks callbacks);

    size_t start_internal_download(const std::string& file_url,
                                    const std::string& file_name,
                                    DownloadCallbacks callbacks);

    size_t start_internal_download(const std::string& file_url,
                                    const std::string& file_name,
                                    DownloadCallbacks callbacks,
                                    bool need_decrypt,
                                    const std::string& sn);

    // ============================================================================
    // Common Interface (works for both WCP and internal downloads)
    // ============================================================================
    // Cancel a download task
    bool cancel_download(size_t task_id);
    
    // Pause a download task (if needed)
    bool pause_download(size_t task_id);
    
    // Resume a download task (if needed)
    bool resume_download(size_t task_id);
    
    // Get task state
    DownloadTaskState get_task_state(size_t task_id);
    
    // Get task information
    std::shared_ptr<DownloadTask> get_task(size_t task_id);
    
    // Get all active tasks
    std::vector<std::shared_ptr<DownloadTask>> get_all_tasks();
    
private:
    DownloadManager() = default;
    ~DownloadManager() = default;
    DownloadManager(const DownloadManager&) = delete;
    DownloadManager& operator=(const DownloadManager&) = delete;
    
    std::mutex m_tasks_mutex;
    std::unordered_map<size_t, std::shared_ptr<DownloadTask>> m_tasks;
    std::atomic<size_t> m_next_task_id{1};
    
    // Track last progress update for throttling
    std::unordered_map<size_t, int> m_last_percent;
    std::unordered_map<size_t, std::chrono::steady_clock::time_point> m_last_update;
    
    // ============================================================================
    // Internal Implementation
    // ============================================================================
    
    // Common download implementation (used by both WCP and internal downloads)
    void start_download_impl(std::shared_ptr<DownloadTask> task);
    
    // Send progress update (handles both WCP and internal modes)
    void send_progress_update(std::shared_ptr<DownloadTask> task,
                              int percent, 
                              size_t downloaded,
                              size_t total);
    
    // Send completion message (handles both WCP and internal modes)
    void send_complete_update(std::shared_ptr<DownloadTask> task,
                              const std::string& file_path);
    
    // Send error message (handles both WCP and internal modes)
    void send_error_update(std::shared_ptr<DownloadTask> task,
                           const std::string& error);
    
    // WCP-specific: Send progress update via WCP instance
    void send_wcp_progress_update(std::shared_ptr<DownloadTask> task, 
                                  int percent,
                                  size_t downloaded,
                                  size_t total);
    
    // WCP-specific: Send completion via WCP instance
    void send_wcp_complete_update(std::shared_ptr<DownloadTask> task,
                                  const std::string& file_path);
    
    // WCP-specific: Send error via WCP instance
    void send_wcp_error_update(std::shared_ptr<DownloadTask> task,
                               const std::string& error);
    
    // Internal-specific: Call progress callback
    void call_internal_progress_callback(std::shared_ptr<DownloadTask> task,
                                         int percent,
                                         size_t downloaded,
                                         size_t total);
    
    // Internal-specific: Call complete callback
    void call_internal_complete_callback(std::shared_ptr<DownloadTask> task,
                                         const std::string& file_path);
    
    // Internal-specific: Call error callback
    void call_internal_error_callback(std::shared_ptr<DownloadTask> task,
                                      const std::string& error);
    
    // Clean up completed task
    void cleanup_task(size_t task_id);
    
    // Generate unique file path if file already exists
    // Returns path like "file(1).zip", "file(2).zip" etc.
    static std::string get_unique_file_path(const boost::filesystem::path& file_path);
};

}} // namespace Slic3r::GUI

#endif // slic3r_DownloadManager_hpp_

