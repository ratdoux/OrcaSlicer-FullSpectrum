#include "DownloadManager.hpp"
#include "GUI_App.hpp"
#include "libslic3r/Utils.hpp"
#include "HttpServer.hpp"
#include "slic3r/Utils/FileDecrypt.hpp"
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/log/trivial.hpp>
#include <boost/format.hpp>
#include <vector>
#include <ctime>

namespace Slic3r { namespace GUI {

// ============================================================================
// Helper Functions
// ============================================================================

std::string DownloadManager::get_unique_file_path(const boost::filesystem::path& file_path)
{
    // file_path should be the complete absolute path: directory + filename
    std::string original_path = file_path.string();
    BOOST_LOG_TRIVIAL(debug) << boost::format("DownloadManager::get_unique_file_path: Checking path '%1%'") % original_path;
    
    // Check if file exists, if not return original path
    if (!boost::filesystem::exists(file_path)) {
        BOOST_LOG_TRIVIAL(debug) << boost::format("DownloadManager::get_unique_file_path: File does not exist, returning original path '%1%'") % original_path;
        return original_path;
    }
    
    BOOST_LOG_TRIVIAL(debug) << boost::format("DownloadManager::get_unique_file_path: File exists, generating unique name");
    
    boost::filesystem::path parent_dir = file_path.parent_path();
    std::string filename = file_path.filename().string();
    std::string extension = file_path.extension().string();

    std::string name_without_ext;
    if (extension.empty()) {
        name_without_ext = filename;
        BOOST_LOG_TRIVIAL(debug) << boost::format("DownloadManager::get_unique_file_path: No extension found, filename='%1%'") % filename;
    } else {
        name_without_ext = filename.substr(0, filename.size() - extension.size());
        BOOST_LOG_TRIVIAL(debug) << boost::format("DownloadManager::get_unique_file_path: filename='%1%', extension='%2%', name_without_ext='%3%'")
            % filename % extension % name_without_ext;
    }
    
    // Generate unique filename with Windows-style numbering: filename(1).ext, filename(2).ext, etc.
    size_t version = 1;
    boost::filesystem::path unique_path;
    do {
        std::string new_filename;
        if (extension.empty()) {
            // No extension: filename(1), filename(2), etc.
            new_filename = name_without_ext + "(" + std::to_string(version) + ")";
        } else {
            // Has extension: filename(1).ext, filename(2).ext, etc.
            new_filename = name_without_ext + "(" + std::to_string(version) + ")" + extension;
        }
        unique_path = parent_dir / new_filename;
        BOOST_LOG_TRIVIAL(debug) << boost::format("DownloadManager::get_unique_file_path: Trying version %1%: '%2%'") % version % unique_path.string();
        version++;
    } while (boost::filesystem::exists(unique_path) && version < 10000);  // Safety limit
    
    if (version >= 10000) {
        // If we hit the limit, log a warning and return a timestamp-based name
        BOOST_LOG_TRIVIAL(warning) << boost::format("DownloadManager::get_unique_file_path: Too many duplicate files for '%1%', using timestamp-based name")
            % original_path;
        std::string timestamp = std::to_string(std::time(nullptr));
        std::string new_filename;
        if (extension.empty()) {
            new_filename = name_without_ext + "_" + timestamp;
        } else {
            new_filename = name_without_ext + "_" + timestamp + extension;
        }
        unique_path = parent_dir / new_filename;
    }

#ifdef _WIN32
    std::string result = boost::nowide::narrow(unique_path.wstring());
#else
    std::string result = unique_path.string();
#endif
    BOOST_LOG_TRIVIAL(debug) << boost::format("DownloadManager::get_unique_file_path: Final unique path: '%1%'") % result;
    return result;
}

// ============================================================================
// WCP Download Interface (for Web-to-PC communication)
// ============================================================================
size_t DownloadManager::start_wcp_download(const std::string& file_url,
                                             const std::string& file_name,
                                             std::shared_ptr<SSWCP_Instance> wcp_instance,
                                             bool use_original_event_id) {
    return start_wcp_download(file_url, file_name, wcp_instance, use_original_event_id, false, "");
}

size_t DownloadManager::start_wcp_download(const std::string& file_url,
                                             const std::string& file_name,
                                             std::shared_ptr<SSWCP_Instance> wcp_instance,
                                             bool use_original_event_id,
                                             bool need_decrypt,
                                             const std::string& sn) {
    
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    size_t task_id = m_next_task_id++;
    
    auto downloadPath = wxGetApp().app_config->get("download_path");
#ifdef _WIN32
    boost::filesystem::path dest_folder(boost::nowide::widen(downloadPath));
#else
    boost::filesystem::path dest_folder(downloadPath);
#endif
    boost::filesystem::create_directories(dest_folder);
    boost::filesystem::path dest_file = dest_folder / file_name;
    
    // Generate unique file path if file already exists
    std::string dest_path = get_unique_file_path(dest_file);
    
    // Update file_name if it was changed due to duplicate
    std::string actual_file_name = boost::filesystem::path(dest_path).filename().string();
    
    auto task = std::make_shared<DownloadTask>(task_id, 
                                                file_url, 
                                                actual_file_name, 
                                                dest_path, 
                                                wcp_instance,
                                                use_original_event_id,
                                                need_decrypt,
                                                sn);

    m_tasks[task_id] = task;

    task->state = DownloadTaskState::Downloading;

    start_download_impl(task);
    return task_id;
}

// ============================================================================
// Internal Download Interface (for PC internal use)
// ============================================================================
size_t DownloadManager::start_internal_download(const std::string& file_url,
                                                 const std::string& file_name,
                                                 const std::string& dest_path,
                                                 DownloadCallbacks callbacks) {
    
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    size_t task_id = m_next_task_id++;

#ifdef _WIN32
    boost::filesystem::path dest_path_obj(boost::nowide::widen(dest_path));
#else
    boost::filesystem::path dest_path_obj(dest_path);
#endif
    
    boost::filesystem::path dest_file_path;
    if (boost::filesystem::is_directory(dest_path_obj) || dest_path_obj.filename().empty()) {
        // dest_path is a directory, need to append file_name
        BOOST_LOG_TRIVIAL(debug) << boost::format("DownloadManager::start_internal_download: dest_path '%1%' is a directory, appending file_name '%2%'")
            % dest_path % file_name;
        dest_file_path = dest_path_obj / file_name;
    } else {
        // dest_path is already a complete file path (directory + filename)
        BOOST_LOG_TRIVIAL(debug) << boost::format("DownloadManager::start_internal_download: dest_path '%1%' is a complete file path")
            % dest_path;
        dest_file_path = dest_path_obj;
    }
    
    boost::filesystem::create_directories(dest_file_path.parent_path());
    
    std::string unique_dest_path = get_unique_file_path(dest_file_path);

    auto task = std::make_shared<DownloadTask>(task_id, 
                                                file_url, 
                                                file_name, 
                                                unique_dest_path, 
                                                std::move(callbacks));

    m_tasks[task_id] = task;

    task->state = DownloadTaskState::Downloading;

    start_download_impl(task);
    
    return task_id;
}

size_t DownloadManager::start_internal_download(const std::string& file_url,
                                                 const std::string& file_name,
                                                 DownloadCallbacks callbacks) {
    // Get default download path
    auto downloadPath = wxGetApp().app_config->get("download_path");
#ifdef _WIN32
    boost::filesystem::path dest_folder(boost::nowide::widen(downloadPath));
#else
    boost::filesystem::path dest_folder(downloadPath);
#endif
    boost::filesystem::create_directories(dest_folder);
    
    boost::filesystem::path dest_file = dest_folder / file_name;
    
    // Generate unique file path if file already exists
    std::string dest_path = get_unique_file_path(dest_file);
    
    return start_internal_download(file_url, file_name, dest_path, std::move(callbacks));
}

size_t DownloadManager::start_internal_download(const std::string& file_url,
                                                 const std::string& file_name,
                                                 DownloadCallbacks callbacks,
                                                 bool need_decrypt,
                                                 const std::string& sn) {
    // Get default download path
    auto downloadPath = wxGetApp().app_config->get("download_path");
#ifdef _WIN32
    boost::filesystem::path dest_folder(boost::nowide::widen(downloadPath));
#else
    boost::filesystem::path dest_folder(downloadPath);
#endif
    if (!sn.empty()) {
        dest_folder /= sn;
    }
    boost::filesystem::create_directories(dest_folder);
    
    boost::filesystem::path dest_file = dest_folder / file_name;
    
    // Generate unique file path if file already exists
    std::string dest_path = get_unique_file_path(dest_file);
    
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    size_t task_id = m_next_task_id++;
    
    auto task = std::make_shared<DownloadTask>(task_id, 
                                                file_url, 
                                                file_name, 
                                                dest_path, 
                                                std::move(callbacks),
                                                need_decrypt,
                                                sn);

    m_tasks[task_id] = task;

    task->state = DownloadTaskState::Downloading;
    start_download_impl(task);

    return task_id;
}

void DownloadManager::start_download_impl(std::shared_ptr<DownloadTask> task) {
    
    wxGetApp().CallAfter([this, task]() {
        try {
            // Step 1: Create Http object
            Http http = Http::get(task->file_url);
            
            http.timeout_max(0);

            // Step 2: Set progress callback
            http.on_progress([this, task](Http::Progress progress, bool& cancel) {
                // Check if task is canceled or already cleaned up
                {
                    std::lock_guard<std::mutex> lock(m_tasks_mutex);
                    if (m_tasks.find(task->task_id) == m_tasks.end()) {
                        // Task has been cleaned up, cancel the download
                        cancel = true;
                        return;
                    }
                }
                
                if (task->state == DownloadTaskState::Canceled) {
                    cancel = true;
                    return;
                }
                
                int percent = 0;
                if (progress.dltotal > 0) {
                    percent = (int)(progress.dlnow * 100 / progress.dltotal);
                }

                std::lock_guard<std::mutex> lock(m_tasks_mutex);

                // Double-check task still exists after acquiring lock
                if (m_tasks.find(task->task_id) == m_tasks.end()) {
                    cancel = true;
                    return;
                }

                auto& last_pct = m_last_percent[task->task_id];
                auto& last_upd = m_last_update[task->task_id];

                // Monotonic progress: weak networks can report dlnow fluctuation
                // (retry/buffer) that would otherwise make the bar jump backwards.
                if (percent < last_pct) {
                    percent = last_pct;
                }
                task->percent = percent;

                // Throttle progress updates: update every 5% or every second
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
                        // Check if task still exists before sending update
                        std::lock_guard<std::mutex> lock(m_tasks_mutex);
                        if (m_tasks.find(task->task_id) != m_tasks.end() && 
                            task->state != DownloadTaskState::Canceled) {
                            send_progress_update(task, percent, progress.dlnow, progress.dltotal);
                        }
                    });
                }
            });
            
            // Step 3: Set complete callback
            http.on_complete([this, task](std::string body, unsigned status) {
                wxGetApp().CallAfter([this, task, body]() {
                    // Check if task still exists and is not canceled (without lock to avoid deadlock with cleanup_task)
                    if (task->state == DownloadTaskState::Canceled) {
                        // Task has been canceled, ignore completion
                        BOOST_LOG_TRIVIAL(debug) << "DownloadManager: Ignoring complete callback for canceled task " << task->task_id;
                        return;
                    }

                    if (task->callbacks.on_should_write && !task->callbacks.on_should_write()) {
                        cleanup_task(task->task_id);
                        return;
                    }

                    try {
                        // Save file
                        boost::nowide::ofstream file(task->dest_path, std::ios::binary);
                        if (!file.is_open()) {
                            std::string error_msg = "Failed to open file for writing: " + task->dest_path;
                            BOOST_LOG_TRIVIAL(error) << "DownloadManager: " << error_msg;
                            // Flush logs immediately for critical errors
                            Slic3r::flush_logs();
                            send_error_update(task, error_msg);
                            cleanup_task(task->task_id);
                            return;
                        }
                        
                        file.write(body.c_str(), body.size());
                        if (file.fail()) {
                            std::string error_msg = "Failed to write file: " + task->dest_path;
                            BOOST_LOG_TRIVIAL(error) << "DownloadManager: " << error_msg << ", body size: " << body.size();
                            // Flush logs immediately for critical errors
                            Slic3r::flush_logs();
                            file.close();
                            send_error_update(task, error_msg);
                            cleanup_task(task->task_id);
                            return;
                        }
                        file.close();

                        std::string final_path = task->dest_path;

                        if (task->need_decrypt && !task->sn.empty()) {
                            const std::string enc_path = task->dest_path;
                            const std::string tmp_path = enc_path + ".tmp";

                            if (!decrypt_file_aes_cbc(enc_path, task->sn, Slic3r::PBKDF2_ITERATIONS, tmp_path)) {
                                send_error_update(task, "File decryption failed");
                                cleanup_task(task->task_id);
                                return;
                            }

                            boost::filesystem::remove(enc_path);
                            boost::filesystem::rename(tmp_path, enc_path);

                            task->decrypt_path = enc_path;
                        }

                        task->state = DownloadTaskState::Completed;
                        task->percent = 100;
                        send_complete_update(task, final_path);
                        cleanup_task(task->task_id);
                    } catch (std::exception& e) {
                        std::string error_msg = std::string("File write exception: ") + e.what();
                        BOOST_LOG_TRIVIAL(error) << "DownloadManager: " << error_msg << ", file: " << task->dest_path;
                        // Flush logs immediately for critical errors
                        Slic3r::flush_logs();
                        send_error_update(task, error_msg);
                        cleanup_task(task->task_id);
                    }
                });
            });
            
            // Step 4: Set error callback
            http.on_error([this, task](std::string body, std::string error, unsigned status) {
                wxGetApp().CallAfter([this, task, error, status]() {
                    // Check if task was canceled (without lock to avoid deadlock with cleanup_task)
                    if (task->state == DownloadTaskState::Canceled) {
                        // Task was canceled, ignore error callback (cancel already handled cleanup)
                        BOOST_LOG_TRIVIAL(debug) << "DownloadManager: Ignoring error callback for canceled task " << task->task_id;
                        return;
                    }
                    
                    std::string error_msg = boost::str(boost::format("HTTP error: %1% (status: %2%)") % error % status);
                    BOOST_LOG_TRIVIAL(error) << "DownloadManager: " << error_msg 
                        << ", URL: " << task->file_url 
                        << ", file: " << task->file_name
                        << ", dest: " << task->dest_path;
                    // Flush logs immediately for critical errors to ensure they are written
                    Slic3r::flush_logs();
                    task->state = DownloadTaskState::Error;
                    task->error_message = error;
                    send_error_update(task, error);
                    cleanup_task(task->task_id);
                });
            });
            
            // Step 5: Start download and save Http::Ptr for cancellation
            task->http_object = http.perform();
            
        } catch (std::exception& e) {
            std::string error_msg = std::string("Download exception: ") + e.what();
            BOOST_LOG_TRIVIAL(error) << "DownloadManager: " << error_msg 
                << ", URL: " << task->file_url 
                << ", file: " << task->file_name
                << ", dest: " << task->dest_path;
            task->state = DownloadTaskState::Error;
            task->error_message = e.what();
            send_error_update(task, e.what());
            cleanup_task(task->task_id);
        }
    });
}

//this function not currently in use
bool DownloadManager::cancel_download(size_t task_id) {
    std::shared_ptr<SSWCP_Instance> wcp_to_destroy;
    
    {
        std::lock_guard<std::mutex> lock(m_tasks_mutex);
        
        auto it = m_tasks.find(task_id);
        if (it == m_tasks.end()) {
            return false;
        }
        
        auto task = it->second;
        if (task->state == DownloadTaskState::Downloading) {
            task->state = DownloadTaskState::Canceled;
            if (task->http_object) {
                task->http_object->cancel();
            }
            
            // Only for WCP downloads
            if (task->is_wcp_download()) {
                wcp_to_destroy = task->wcp_instance.lock();
            } else {
                task->callbacks.on_error = nullptr;
                task->callbacks.on_progress = nullptr;
                task->callbacks.on_complete = nullptr;
            }

            std::string partial_path = task->dest_path;
            // Cleanup task directly (already holding the lock, don't call cleanup_task)
            m_tasks.erase(task_id);
            m_last_percent.erase(task_id);
            m_last_update.erase(task_id);
            if (!partial_path.empty()) {
                boost::system::error_code ec;
                boost::filesystem::remove(partial_path, ec);
            }
        } else {
            return false;
        }
    }

    if (wcp_to_destroy) {
        wcp_to_destroy->finish_job();
    }
    
    return true;
}

// this function not currently in use
bool DownloadManager::pause_download(size_t task_id) {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    auto it = m_tasks.find(task_id);
    if (it != m_tasks.end() && it->second->state == DownloadTaskState::Downloading) {
        it->second->state = DownloadTaskState::Paused;        
        return true;
    }
    return false;
}

// this function not currently in use
bool DownloadManager::resume_download(size_t task_id) {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    auto it = m_tasks.find(task_id);
    if (it != m_tasks.end() && it->second->state == DownloadTaskState::Paused) {
        return false; 
    }
    return false;
}

// this function not currently in use
DownloadTaskState DownloadManager::get_task_state(size_t task_id) {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    auto it = m_tasks.find(task_id);
    if (it != m_tasks.end()) {
        return it->second->state;
    }
    return DownloadTaskState::Error;
}

// this function not currently in use
std::shared_ptr<DownloadTask> DownloadManager::get_task(size_t task_id) {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    auto it = m_tasks.find(task_id);
    if (it != m_tasks.end()) {
        return it->second;
    }
    return nullptr;
}

// this function not currently in use
std::vector<std::shared_ptr<DownloadTask>> DownloadManager::get_all_tasks() {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    std::vector<std::shared_ptr<DownloadTask>> result;
    result.reserve(m_tasks.size());
    for (const auto& pair : m_tasks) {
        result.push_back(pair.second);
    }
    return result;
}

// ============================================================================
// Progress/Complete/Error Update Handlers
// ============================================================================
void DownloadManager::send_progress_update(std::shared_ptr<DownloadTask> task, 
                                               int percent, 
                                               size_t downloaded, 
                                               size_t total) {
    if (task->is_wcp_download()) {
        send_wcp_progress_update(task, percent, downloaded, total);
    } else {
        call_internal_progress_callback(task, percent, downloaded, total);
    }
}

void DownloadManager::send_wcp_progress_update(std::shared_ptr<DownloadTask> task, 
                                                  int percent, 
                                                  size_t downloaded, 
                                                  size_t total) {    
    if (!task->use_original_event_id) {
        return;
    }
    
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
                
        json header;
        if (task->use_original_event_id) {
            header["event_id"] = wcp->m_event_id;
        } else {
            header["event_id"] = wcp->m_event_id + "_progress";
        }

        header["command"] = "download_progress";
        wcp->m_header = header;
        
        wcp->send_to_js();
    }
}

void DownloadManager::call_internal_progress_callback(std::shared_ptr<DownloadTask> task,
                                                      int percent,
                                                      size_t downloaded,
                                                      size_t total) {

    if (task->callbacks.on_progress && task->state != DownloadTaskState::Canceled) {
        task->callbacks.on_progress(task->task_id, percent, downloaded, total);
    }
}

void DownloadManager::send_complete_update(std::shared_ptr<DownloadTask> task, 
                                               const std::string& file_path) {
    if (task->is_wcp_download()) {
        send_wcp_complete_update(task, file_path);
    } else {
        call_internal_complete_callback(task, file_path);
    }
}

void DownloadManager::send_wcp_complete_update(std::shared_ptr<DownloadTask> task, 
                                                 const std::string& file_path) {
    
    if (!task->use_original_event_id) {
        return;
    }

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
        wcp->finish_job();
    }
}

void DownloadManager::call_internal_complete_callback(std::shared_ptr<DownloadTask> task,
                                                       const std::string& file_path) {

    if (task->callbacks.on_complete && task->state != DownloadTaskState::Canceled) {
        task->callbacks.on_complete(task->task_id, file_path);
    }
}

void DownloadManager::send_error_update(std::shared_ptr<DownloadTask> task, 
                                           const std::string& error) {
    if (task->is_wcp_download()) {
        send_wcp_error_update(task, error);
    } else {
        call_internal_error_callback(task, error);
    }
}

void DownloadManager::send_wcp_error_update(std::shared_ptr<DownloadTask> task, 
                                              const std::string& error) {
    
    if (!task->use_original_event_id) {
        return;
    }
    
    if (auto wcp = task->wcp_instance.lock()) {
        json error_data;
        error_data["task_id"] = task->task_id;
        error_data["error"] = error;
        error_data["state"] = "error";
        
        wcp->m_res_data = error_data;
        wcp->m_status = -1;
        wcp->m_msg = error;

        wcp->send_to_js();
        wcp->finish_job();
    }
}

void DownloadManager::call_internal_error_callback(std::shared_ptr<DownloadTask> task,
                                                    const std::string& error) {

    if (task->callbacks.on_error && task->state != DownloadTaskState::Canceled) {
        task->callbacks.on_error(task->task_id, error);
    }
}

void DownloadManager::cleanup_task(size_t task_id) {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    m_tasks.erase(task_id);
    m_last_percent.erase(task_id);
    m_last_update.erase(task_id);
}

}} // namespace Slic3r::GUI

