#ifndef slic3r_PrintHost_hpp_
#define slic3r_PrintHost_hpp_

#include <memory>
#include <set>
#include <string>
#include <functional>
#include <boost/filesystem/path.hpp>

#include <wx/string.h>

#include <libslic3r/enum_bitmask.hpp>
#include "Http.hpp"
#include <map>
#include "nlohmann/json.hpp"

class wxArrayString;

namespace Slic3r {

class DynamicPrintConfig;

enum class PrintHostPostUploadAction {
    None,
    StartPrint,
    StartSimulation,
    QueuePrint
};
using PrintHostPostUploadActions = enum_bitmask<PrintHostPostUploadAction>;
ENABLE_ENUM_BITMASK_OPERATORS(PrintHostPostUploadAction);

struct PrintHostUpload
{
    bool use_3mf;
    boost::filesystem::path source_path;
    boost::filesystem::path upload_path;
    
    std::string group;
    std::string storage;

    PrintHostPostUploadAction post_action { PrintHostPostUploadAction::None };

    // Some extended parameters for different upload methods.
    std::map<std::string, std::string> extended_info;
};

class PrintHost
{
public:
    virtual ~PrintHost();

    typedef Http::ProgressFn ProgressFn;
    typedef std::function<void(wxString /* error */)> ErrorFn;
    typedef std::function<void(wxString /* tag */, wxString /* status */)> InfoFn;

    virtual const char* get_name() const = 0;

    virtual bool test(wxString &curl_msg) const = 0;
    virtual wxString get_test_ok_msg () const = 0;
    virtual wxString get_test_failed_msg (wxString &msg) const = 0;
    virtual bool upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const = 0;
    virtual bool has_auto_discovery() const = 0;
    virtual bool can_test() const = 0;
    virtual PrintHostPostUploadActions get_post_upload_actions() const = 0;
    // A print host usually does not support multiple printers, with the exception of Repetier server.
    virtual bool supports_multiple_printers() const { return false; }
    virtual std::string get_host() const = 0;

    // Support for Repetier server multiple groups & printers. Not supported by other print hosts.
    // Returns false if not supported. May throw HostNetworkError.
    virtual bool get_groups(wxArrayString & /* groups */) const { return false; }
    virtual bool get_printers(wxArrayString & /* printers */) const { return false; }
    // Support for PrusaLink uploading to different storage. Not supported by other print hosts.
    // Returns false if not supported or fail.
    virtual bool get_storage(wxArrayString& /*storage_path*/, wxArrayString& /*storage_name*/) const { return false; }

    static PrintHost* get_print_host(DynamicPrintConfig *config, bool change_engine = true);

    virtual bool send_gcodes(const std::vector<std::string>& codes, std::string& extraInfo) { return false; }

    virtual bool get_machine_info(const std::vector<std::pair<std::string, std::vector<std::string>>>& targets, nlohmann::json& response) { return false; }

    virtual void async_get_system_info(std::function<void(const nlohmann::json& response)> callback){}

    virtual void async_server_files_get_status(std::function<void(const nlohmann::json& response)> callback) {}

    virtual void async_get_machine_info(const std::vector<std::pair<std::string, std::vector<std::string>>>& targets, std::function<void(const nlohmann::json& response)>) {}

    virtual void async_get_device_info(std::function<void(const nlohmann::json& response)>) {}

    virtual void async_get_machine_objects(std::function<void(const nlohmann::json& response)>) {}

    virtual void async_get_printer_info(std::function<void(const nlohmann::json& response)>) {}

    virtual void async_set_machine_subscribe_filter(const std::vector<std::pair<std::string, std::vector<std::string>>>& targets,
                                                    std::function<void(const nlohmann::json& response)>                  callback) {}

    virtual void async_unsubscribe_machine_info(const std::string& hash, std::function<void(const nlohmann::json&)>) {}

    virtual void async_subscribe_machine_info(const std::string& hash, std::function<void(const nlohmann::json&)>) {}

    virtual void async_send_gcodes(const std::vector<std::string>& scripts, std::function<void(const nlohmann::json&)>) {}

    virtual void async_start_print_job(const std::string& filename, std::function<void(const nlohmann::json&)>) {}

    virtual void async_pause_print_job(std::function<void(const nlohmann::json&)>) {}

    virtual void async_resume_print_job(std::function<void(const nlohmann::json&)>) {}

    virtual void async_cancel_print_job(std::function<void(const nlohmann::json&)>) {}

    virtual void test_async_wcp_mqtt_moonraker(const nlohmann::json& mqtt_request_params, std::function<void(const nlohmann::json&)>) {}

    virtual bool connect(wxString& msg, const nlohmann::json& params) { return false; }

    virtual bool disconnect(wxString& msg, const nlohmann::json& params) { return true; }

    virtual bool check_sn_arrived() { return false; };

    virtual std::string get_sn() { return ""; }

    // system 
    virtual void async_machine_files_roots(std::function<void(const nlohmann::json& response)>) {}

    virtual void async_machine_files_metadata(const std::string& filename, std::function<void(const nlohmann::json& response)>) {}

    virtual void async_machine_files_thumbnails(const std::string& filename, std::function<void(const nlohmann::json& response)>) {}

    virtual void async_server_client_manager_set_userinfo(const nlohmann::json& user, std::function<void(const nlohmann::json& response)>) {}
    virtual void async_machine_files_directory(const std::string& path, bool extend, std::function<void(const nlohmann::json& response)>) {}

    virtual void async_camera_start(const std::string& domain, int interval, bool expect_pw, std::function<void(const nlohmann::json& response)>) {}

    virtual void async_canmera_stop(const std::string& domain, std::function<void(const nlohmann::json& response)>) {}

    virtual void async_delete_machine_file(const std::string& path, std::function<void(const nlohmann::json& response)>) {}

    virtual void async_pull_cloud_file(const nlohmann::json& targets, std::function<void(const nlohmann::json& response)>){}

    virtual void async_start_cloud_print(const nlohmann::json& targets, std::function<void(const nlohmann::json& response)>) {}

    virtual void async_cancel_pull_cloud_file(std::function<void(const nlohmann::json& response)>) {}

    virtual void async_set_device_name(const std::string& device_name, std::function<void(const nlohmann::json& response)>) {}

    virtual void async_control_led(const std::string& name, int white, std::function<void(const nlohmann::json& response)>) {}

    virtual void async_control_print_speed(int percentage, std::function<void(const nlohmann::json& response)>) {}

    virtual void async_bedmesh_abort_probe_mesh(std::function<void(const nlohmann::json& response)>) {}

    virtual void async_controlPurifier(int fan_speed, int delay_time, int work_time, std::function<void(const nlohmann::json& response)>) {}

    virtual void async_control_main_fan(int speed, std::function<void(const nlohmann::json& response)>) {}

    virtual void async_control_generic_fan(const std::string& name, int speed, std::function<void(const nlohmann::json& response)>) {}

    virtual void async_control_bed_temp(int temp, std::function<void(const nlohmann::json& response)>) {}

    virtual void async_control_extruder_temp(int temp, int index, int map, std::function<void(const nlohmann::json& response)>) {}

    virtual void async_files_thumbnails_base64(const std::string& path, std::function<void(const nlohmann::json& response)>) {}

    virtual void async_exception_query(std::function<void(const nlohmann::json& response)>) {}

    virtual void async_get_file_page_list(const std::string& root, int files_per_page, int page_number, std::function<void(const nlohmann::json& response)>){}

    virtual void async_upload_camera_timelapse(const nlohmann::json& targets, std::function<void(const nlohmann::json& response)>) {}

    virtual void async_delete_camera_timelapse(const nlohmann::json& targets, std::function<void(const nlohmann::json& response)>) {}

    virtual void async_get_timelapse_instance(const nlohmann::json& targets, std::function<void(const nlohmann::json& response)>) {}

    virtual void async_get_userdata_space(const nlohmann::json& targets, std::function<void(const nlohmann::json& response)>) {}
    
    virtual void async_defect_detaction_config(const nlohmann::json& targets, std::function<void(const nlohmann::json& response)>) {}

    //Support for cloud webui login
    virtual bool is_cloud() const { return false; }
    virtual bool is_logged_in() const { return false; }
    virtual void log_out() const {}
    virtual bool get_login_url(wxString& auth_url) const { return false; }

    virtual void set_connection_lost(std::function<void()> callback) { m_connection_lost_cb = callback; }

    // set auth info
    virtual void set_auth_info(const nlohmann::json& info){}

    virtual nlohmann::json get_auth_info() { return nlohmann::json::object(); }

protected:
    virtual wxString format_error(const std::string &body, const std::string &error, unsigned status) const;

private:
    std::function<void()> m_connection_lost_cb = nullptr;
};


struct PrintHostJob
{
    PrintHostUpload upload_data;
    std::unique_ptr<PrintHost> printhost;
    bool switch_to_device_tab{false};
    bool cancelled = false;

    PrintHostJob() {}
    PrintHostJob(const PrintHostJob&) = delete;
    PrintHostJob(PrintHostJob &&other)
        : upload_data(std::move(other.upload_data))
        , printhost(std::move(other.printhost))
        , switch_to_device_tab(other.switch_to_device_tab)
        , cancelled(other.cancelled)
    {}

    PrintHostJob(DynamicPrintConfig *config)
        : printhost(PrintHost::get_print_host(config, false))
    {}

    PrintHostJob& operator=(const PrintHostJob&) = delete;
    PrintHostJob& operator=(PrintHostJob &&other)
    {
        upload_data = std::move(other.upload_data);
        printhost   = std::move(other.printhost);
        switch_to_device_tab = other.switch_to_device_tab;
        cancelled = other.cancelled;
        return *this;
    }

    bool empty() const { return !printhost; }
    operator bool() const { return !!printhost; }
};


namespace GUI { class PrintHostQueueDialog; }

class PrintHostJobQueue
{
public:
    PrintHostJobQueue(GUI::PrintHostQueueDialog *queue_dialog);
    PrintHostJobQueue(const PrintHostJobQueue &) = delete;
    PrintHostJobQueue(PrintHostJobQueue &&other) = delete;
    ~PrintHostJobQueue();

    PrintHostJobQueue& operator=(const PrintHostJobQueue &) = delete;
    PrintHostJobQueue& operator=(PrintHostJobQueue &&other) = delete;

    void enqueue(PrintHostJob job);
    void cancel(size_t id);

private:
    struct priv;
    std::shared_ptr<priv> p;
};



}

#endif
