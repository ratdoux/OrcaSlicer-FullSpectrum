#include "WCPDownloadManager.hpp"
#include "GUI_App.hpp"
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI {

size_t WCPDownloadManager::start_download(const std::string& file_url,
                                         const std::string& file_name,
                                         std::shared_ptr<SSWCP_Instance> wcp_instance) {

    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    
    size_t task_id = m_next_task_id++;
    
    // Get download path
    auto downloadPath = wxGetApp().app_config->get("download_path");
    boost::filesystem::path dest_folder(downloadPath);
    boost::filesystem::create_directories(dest_folder);
    
    boost::filesystem::path dest_file = dest_folder / file_name;
    std::string dest_path = dest_file.string();
    
    // Create task
    auto task = std::make_shared<WCPDownloadTask>(task_id, file_url, file_name, dest_path, wcp_instance);
    task->state = WCPDownloadState::Downloading;
    
    m_tasks[task_id] = task;
    
    // Start download
    wxGetApp().CallAfter([this, task]() {
        try {
            // Step 1: Create Http object
            Http http = Http::get(task->file_url);
            
            // Step 2: Set progress callback
            http.on_progress([this, task](Http::Progress progress, bool& cancel) {
                if (task->state == WCPDownloadState::Canceled) {
                    cancel = true;
                    return;
                }
                
                // Calculate progress
                int percent = 0;
                if (progress.dltotal > 0) {
                    percent = (int)(progress.dlnow * 100 / progress.dltotal);
                }
                
                task->percent = percent;
                
                // Throttle progress updates: update every 5% or every second
                std::lock_guard<std::mutex> lock(m_tasks_mutex);
                auto& last_pct = m_last_percent[task->task_id];
                auto& last_upd = m_last_update[task->task_id];
                
                auto now = std::chrono::steady_clock::now();
                bool should_update = false;
                
                if (percent - last_pct >= 5) {
                    should_update = true;
                    last_pct = percent;
                } else if (now - last_upd >= std::chrono::seconds(1)) {
                    should_update = true;
                }
                
                if (should_update) {
                    last_upd = now;
                    wxGetApp().CallAfter([this, task, percent, progress]() {
                        send_progress_update(task, percent, progress.dlnow, progress.dltotal);
                    });
                }
            });
            
            // Step 3: Set complete callback
            http.on_complete([this, task](std::string body, unsigned status) {
                wxGetApp().CallAfter([this, task, body]() {
                    try {
                        // Save file
                        boost::nowide::ofstream file(task->dest_path, std::ios::binary);
                        if (!file.is_open()) {
                            send_error_update(task, "Failed to open file for writing");
                            cleanup_task(task->task_id);
                            return;
                        }
                        
                        file.write(body.c_str(), body.size());
                        file.close();
                        
                        task->state = WCPDownloadState::Completed;
                        task->percent = 100;
                        send_complete_update(task, task->dest_path);
                        cleanup_task(task->task_id);
                    } catch (std::exception& e) {
                        send_error_update(task, e.what());
                        cleanup_task(task->task_id);
                    }
                });
            });
            
            // Step 4: Set error callback
            http.on_error([this, task](std::string body, std::string error, unsigned status) {
                wxGetApp().CallAfter([this, task, error, status]() {
                    task->state = WCPDownloadState::Error;
                    task->error_message = error;
                    send_error_update(task, error);
                    cleanup_task(task->task_id);
                });
            });
            
            // Step 5: Start download and save Http::Ptr for cancellation
            task->http_object = http.perform();
            
        } catch (std::exception& e) {
            task->state = WCPDownloadState::Error;
            task->error_message = e.what();
            send_error_update(task, e.what());
            cleanup_task(task->task_id);
        }
    });
    
    return task_id;
}

bool WCPDownloadManager::cancel_download(size_t task_id) {
    std::shared_ptr<SSWCP_Instance> wcp_to_destroy;
    
    {
        std::lock_guard<std::mutex> lock(m_tasks_mutex);
        
        auto it = m_tasks.find(task_id);
        if (it == m_tasks.end()) {
            return false;
        }
        
        auto task = it->second;
        if (task->state == WCPDownloadState::Downloading) {
            task->state = WCPDownloadState::Canceled;
            if (task->http_object) {
                task->http_object->cancel();
            }
            
            // Get WCP instance before cleanup (for destruction after lock release)
            wcp_to_destroy = task->wcp_instance.lock();
            
            cleanup_task(task_id);
        } else {
            return false;
        }
    }
    
    // Destroy WCP instance outside the lock to prevent deadlock
    // This is the WCP instance from the original download request (sw_DownloadFile)
    if (wcp_to_destroy) {
        wcp_to_destroy->finish_job();
    }
    
    return true;
}

bool WCPDownloadManager::pause_download(size_t task_id) {
    // Pause functionality can be implemented if needed
    // Current Http module may not support pause, need to implement resume from breakpoint
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    auto it = m_tasks.find(task_id);
    if (it != m_tasks.end() && it->second->state == WCPDownloadState::Downloading) {
        it->second->state = WCPDownloadState::Paused;
        // Note: Http module doesn't support pause directly, would need breakpoint resume
        return true;
    }
    return false;
}

bool WCPDownloadManager::resume_download(size_t task_id) {
    // Resume functionality can be implemented if needed
    // Would require breakpoint resume support in Http module
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    auto it = m_tasks.find(task_id);
    if (it != m_tasks.end() && it->second->state == WCPDownloadState::Paused) {
        // Would need to restart download with range header
        return false;  // Not implemented yet
    }
    return false;
}

WCPDownloadState WCPDownloadManager::get_task_state(size_t task_id) {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    auto it = m_tasks.find(task_id);
    if (it != m_tasks.end()) {
        return it->second->state;
    }
    return WCPDownloadState::Error;
}

std::shared_ptr<WCPDownloadTask> WCPDownloadManager::get_task(size_t task_id) {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    auto it = m_tasks.find(task_id);
    if (it != m_tasks.end()) {
        return it->second;
    }
    return nullptr;
}

void WCPDownloadManager::send_progress_update(std::shared_ptr<WCPDownloadTask> task, 
                                               int percent, 
                                               size_t downloaded, 
                                               size_t total) {
    if (auto wcp = task->wcp_instance.lock()) {
        json progress_data;
        progress_data["task_id"] = task->task_id;
        progress_data["percent"] = percent;
        progress_data["downloaded"] = downloaded;
        progress_data["total"] = total;
        progress_data["state"] = "downloading";
        
        wcp->m_res_data = progress_data;
        wcp->m_status = 0;
        wcp->m_msg = "Download progress";
        
        // Use progress event ID
        json header;
        header["event_id"] = wcp->m_event_id + "_progress";
        header["command"] = "download_progress";
        wcp->m_header = header;
        
        wcp->send_to_js();
    }
}

void WCPDownloadManager::send_complete_update(std::shared_ptr<WCPDownloadTask> task, 
                                               const std::string& file_path) {
    if (auto wcp = task->wcp_instance.lock()) {
        json complete_data;
        complete_data["task_id"] = task->task_id;
        complete_data["file_path"] = file_path;
        complete_data["file_name"] = task->file_name;
        complete_data["percent"] = 100;
        complete_data["state"] = "completed";
        
        wcp->m_res_data = complete_data;
        wcp->m_status = 0;
        wcp->m_msg = "Download completed";
        wcp->send_to_js();
        
        // Release WCP instance to prevent memory leak
        wcp->finish_job();
    }
}

void WCPDownloadManager::send_error_update(std::shared_ptr<WCPDownloadTask> task, 
                                           const std::string& error) {
    if (auto wcp = task->wcp_instance.lock()) {
        json error_data;
        error_data["task_id"] = task->task_id;
        error_data["error"] = error;
        error_data["state"] = "error";
        
        wcp->m_res_data = error_data;
        wcp->m_status = -1;
        wcp->m_msg = error;
        wcp->send_to_js();
        
        // Release WCP instance to prevent memory leak
        wcp->finish_job();
    }
}

void WCPDownloadManager::cleanup_task(size_t task_id) {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    m_tasks.erase(task_id);
    m_last_percent.erase(task_id);
    m_last_update.erase(task_id);
}

}} // namespace Slic3r::GUI

