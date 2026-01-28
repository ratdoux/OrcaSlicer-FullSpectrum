#ifndef slic3r_Moonraker_hpp_
#define slic3r_Moonraker_hpp_

#include <string>
#include <wx/string.h>
#include <boost/optional.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/TimeoutMap.hpp"

class MqttClient;

namespace Slic3r {

class DynamicPrintConfig;
class Http;
// Base class for communicating with Moonraker API
class Moonraker : public PrintHost
{
public:
    // Constructor takes printer configuration
    Moonraker(DynamicPrintConfig *config);
    ~Moonraker() override = default;

    // Get name of this print host type
    const char* get_name() const override;

    // Test connection to printer
    virtual bool test(wxString &curl_msg) const override;
    wxString get_test_ok_msg () const override;
    wxString get_test_failed_msg (wxString &msg) const override;

    // Upload file to printer
    bool upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const override;

    // Send G-code commands to printer
    bool send_gcodes(const std::vector<std::string>& codes, std::string& extraInfo) override;

    // Get printer information
    bool get_machine_info(const std::vector<std::pair<std::string, std::vector<std::string>>>& targets, nlohmann::json& response) override;

    // Configuration getters
    bool has_auto_discovery() const override { return true; }
    bool can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    std::string get_host() const override { return m_host; }
    const std::string& get_apikey() const { return m_apikey; }
    const std::string& get_cafile() const { return m_cafile; }

    // set auth info
    virtual void set_auth_info(const nlohmann::json& info) override {};

    // Connect/disconnect from printer
    virtual bool connect(wxString& msg, const nlohmann::json& params) override;
    virtual bool disconnect(wxString& msg, const nlohmann::json& params) override { return true; }

    // Async printer information methods
    virtual void async_get_system_info(std::function<void(const nlohmann::json& response)> callback) override {}
    virtual void async_server_files_get_status(std::function<void(const nlohmann::json& response)> callback) override {}
    virtual void async_get_machine_info(const std::vector<std::pair<std::string, std::vector<std::string>>>& targets, std::function<void(const nlohmann::json& response)>) override  {}
    virtual void async_get_device_info(std::function<void(const nlohmann::json& response)>) override  {}
    virtual void async_subscribe_machine_info(const std::string& hash, std::function<void(const nlohmann::json&)>) override {}
    virtual void async_get_machine_objects(std::function<void(const nlohmann::json& response)>)override {}
    virtual void async_set_machine_subscribe_filter(const std::vector<std::pair<std::string, std::vector<std::string>>>& targets,
                                                    std::function<void(const nlohmann::json& response)>                  callback) override {}
    virtual void async_unsubscribe_machine_info(const std::string& hash, std::function<void(const nlohmann::json&)>) override {}
    virtual void async_send_gcodes(const std::vector<std::string>& scripts, std::function<void(const nlohmann::json&)>) override{}

    virtual void async_start_print_job(const std::string& filename, std::function<void(const nlohmann::json&)>) override{}

    virtual void async_pause_print_job(std::function<void(const nlohmann::json&)>) override{}

    virtual void async_resume_print_job(std::function<void(const nlohmann::json&)>) override{}

    virtual void async_cancel_print_job(std::function<void(const nlohmann::json&)>) override{}

    virtual void test_async_wcp_mqtt_moonraker(const nlohmann::json& mqtt_request_params, std::function<void(const nlohmann::json&)>) override {}

    virtual bool check_sn_arrived() override { return false; }

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


    // new  

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

    virtual void async_defect_detaction_config(const nlohmann::json& targets, std::function<void(const nlohmann::json& response)>) {}

    virtual void async_get_userdata_space(const nlohmann::json& targets, std::function<void(const nlohmann::json& response)>) {}

protected:
    // Internal upload implementations
#ifdef WIN32
    virtual bool upload_inner_with_resolved_ip(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn, const boost::asio::ip::address& resolved_addr) const;
#endif
    virtual bool validate_version_text(const boost::optional<std::string> &version_text) const;
    virtual bool upload_inner_with_host(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const;

    // Connection parameters
    std::string m_host;
    std::string m_apikey;
    std::string m_cafile;
    bool        m_ssl_revoke_best_effort;

    // Helper methods
    virtual void set_auth(Http &http) const;
    std::string make_url(const std::string &path) const;

    std::string make_url_8080(const std::string& path) const;

private:
#ifdef WIN32
    bool test_with_resolved_ip(wxString& curl_msg) const;
#endif
};

// Extended Moonraker class that uses MQTT for communication
class Moonraker_Mqtt : public Moonraker
{
public:
    class SequenceGenerator {
    private:
        int64_t connection_id_;
        uint32_t seq_id_low_bits_;
        static const uint32_t MAX_SEQ_ID_INCREASED_LIMIT = 0xFFFFFFFF; // 32位掩码

        void init_connection_id() {
            seq_id_low_bits_ = 0;
            // 获取当前时间戳并取低31位
            auto now = std::chrono::system_clock::now();
            auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()
            ).count();
            connection_id_ = millis & 0x7FFFFFFF;
        }

    public:
        SequenceGenerator() : seq_id_low_bits_(0), connection_id_(-1) {
            init_connection_id();
        }

        void set_connection_id(int64_t connection_id) {
            connection_id_ = connection_id;
        }

        int64_t generate_seq_id() {
            if (connection_id_ == -1) {
                throw std::runtime_error("Connection not initialized.");
            }
            // 递增并限制在32位
            seq_id_low_bits_ = (seq_id_low_bits_ + 1) & MAX_SEQ_ID_INCREASED_LIMIT;
            // 组合高31位和低32位，确保符号位为0
            return (connection_id_ << 32) | seq_id_low_bits_;
        }
    };
    // Constructor
    Moonraker_Mqtt(DynamicPrintConfig* config, bool change_engine =  true);

    // set auth info
    virtual void set_auth_info(const nlohmann::json& info) override;

    // get auth info
    virtual nlohmann::json get_auth_info() override;

    // Override connection methods
    virtual bool connect(wxString& msg, const nlohmann::json& params) override;
    virtual bool disconnect(wxString& msg, const nlohmann::json& params) override;

    // Override async information methods
    virtual void async_get_system_info(std::function<void(const nlohmann::json& response)> callback) override;
    virtual void async_server_files_get_status(std::function<void(const nlohmann::json& response)> callback) override;
    virtual void async_get_machine_info(const std::vector<std::pair<std::string, std::vector<std::string>>>& targets, std::function<void(const nlohmann::json& response)> callback) override;
    virtual void async_get_device_info(std::function<void(const nlohmann::json& response)> callback) override;
    virtual void async_subscribe_machine_info(const std::string& hash, std::function<void(const nlohmann::json&)>) override;
    virtual void async_get_machine_objects(std::function<void(const nlohmann::json& response)> callback) override;
    virtual void async_set_machine_subscribe_filter(const std::vector<std::pair<std::string, std::vector<std::string>>>& targets,
                                                    std::function<void(const nlohmann::json& response)>                  callback) override;
    virtual void async_unsubscribe_machine_info(const std::string& hash, std::function<void(const nlohmann::json&)>) override;
    virtual void async_send_gcodes(const std::vector<std::string>& scripts, std::function<void(const nlohmann::json&)>) override;
    virtual void async_get_printer_info(std::function<void(const nlohmann::json& response)> callback) override;

    virtual void async_start_print_job(const std::string& filename, std::function<void(const nlohmann::json&)> cb) override;

    virtual void async_pause_print_job(std::function<void(const nlohmann::json&)> cb) override;

    virtual void async_resume_print_job(std::function<void(const nlohmann::json&)> cb) override;

    virtual void async_cancel_print_job(std::function<void(const nlohmann::json&)> cb) override;

    virtual void test_async_wcp_mqtt_moonraker(const nlohmann::json& mqtt_request_params, std::function<void(const nlohmann::json&)>) override;

    virtual bool check_sn_arrived() override;

    virtual void async_machine_files_roots(std::function<void(const nlohmann::json& response)>) override;

    virtual void async_machine_files_metadata(const std::string& filename, std::function<void(const nlohmann::json& response)>) override;

    virtual void async_machine_files_thumbnails(const std::string& filename, std::function<void(const nlohmann::json& response)>) override;
    
    virtual void async_server_client_manager_set_userinfo(const nlohmann::json& user, std::function<void(const nlohmann::json& response)>) override;

    virtual void async_machine_files_directory(const std::string& path, bool extend, std::function<void(const nlohmann::json& response)>) override;

    virtual void async_camera_start(const std::string& domain, int interval, bool expect_pw, std::function<void(const nlohmann::json& response)>) override;

    virtual void async_canmera_stop(const std::string& domain, std::function<void(const nlohmann::json& response)>) override;

    virtual void async_delete_machine_file(const std::string& path, std::function<void(const nlohmann::json& response)>) override;

    virtual void async_pull_cloud_file(const nlohmann::json& targets, std::function<void(const nlohmann::json& response)>) override;

    virtual void async_start_cloud_print(const nlohmann::json& targets, std::function<void(const nlohmann::json& response)>) override;

    virtual void async_cancel_pull_cloud_file(std::function<void(const nlohmann::json& response)>) override;

    virtual void async_upload_camera_timelapse(const nlohmann::json& targets, std::function<void(const nlohmann::json& response)>) override;

    virtual void async_delete_camera_timelapse(const nlohmann::json& targets, std::function<void(const nlohmann::json& response)>) override;

    virtual void async_get_timelapse_instance(const nlohmann::json& targets, std::function<void(const nlohmann::json& response)>) override;

    virtual void async_get_userdata_space(const nlohmann::json& targets, std::function<void(const nlohmann::json& response)>) override;

    virtual void async_defect_detaction_config(const nlohmann::json& targets, std::function<void(const nlohmann::json& response)>) override;

    void set_connection_lost(std::function<void()> callback) override;

    std::string get_sn() override;

    // new
    virtual void async_set_device_name(const std::string& device_name, std::function<void(const nlohmann::json& response)>) override;

    virtual void async_control_led(const std::string& name, int white, std::function<void(const nlohmann::json& response)>) override;

    virtual void async_control_print_speed(int percentage, std::function<void(const nlohmann::json& response)>) override;

    virtual void async_bedmesh_abort_probe_mesh(std::function<void(const nlohmann::json& response)>) override;

    virtual void async_controlPurifier(int fan_speed, int delay_time, int work_time, std::function<void(const nlohmann::json& response)>) override;

    virtual void async_control_main_fan(int speed, std::function<void(const nlohmann::json& response)>) override;

    virtual void async_control_generic_fan(const std::string& name, int speed, std::function<void(const nlohmann::json& response)>) override;

    virtual void async_control_bed_temp(int temp, std::function<void(const nlohmann::json& response)>) override;

    virtual void async_control_extruder_temp(int temp, int index, int map, std::function<void(const nlohmann::json& response)>) override;

    virtual void async_files_thumbnails_base64(const std::string& path, std::function<void(const nlohmann::json& response)>) override;

    virtual void async_exception_query(std::function<void(const nlohmann::json& response)>) override;

    virtual void async_get_file_page_list(const std::string& root, int files_per_page, int page_number, std::function<void(const nlohmann::json& response)>)  override;

public:
    // MQTT message handler
    void on_mqtt_message_arrived(const std::string& topic, const std::string& payload);

    // MQTTS message handler
    void on_mqtt_tls_message_arrived(const std::string& topic, const std::string& payload);

private:
    // Helper methods for MQTT communication
    bool send_to_request(const std::string& method,
                        const nlohmann::json& params,
                        bool need_response,
                        std::function<void(const nlohmann::json& response)> callback,
                        std::function<void()> timeout_callback);

    bool add_response_target(int64_t id,
                           std::function<void(const nlohmann::json&)> callback,
                           std::function<void()> timeout_callback = nullptr,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds(80000));

    std::function<void(const nlohmann::json& response)> get_request_callback(int64_t id);
    void delete_response_target(int64_t id);

    bool wait_for_sn(int timeout_seconds = 6);

    // MQTTs message handlers
    void on_response_arrived(const std::string& payload);
    void on_status_arrived(const std::string& payload);
    void on_notification_arrived(const std::string& payload);

    // MQTT message handlers
    void on_auth_arrived(const std::string& payload);

    // Ask for TLS info
    bool ask_for_tls_info(const nlohmann::json& params);

public:
    // set engine
    bool set_engine(const std::shared_ptr<MqttClient>& engine, std::string& msg);

    // Callback structure for MQTT requests
    struct RequestCallback {
        std::function<void(const nlohmann::json&)> success_cb;  // Success callback
        std::function<void()> timeout_cb;                       // Timeout callback

        RequestCallback(
            std::function<void(const nlohmann::json&)> success,
            std::function<void()> timeout = nullptr)
            : success_cb(std::move(success))
            , timeout_cb(std::move(timeout))
        {}
    };

private:
    // Static MQTT client and related variables
    static std::shared_ptr<MqttClient> m_mqtt_client;
    static std::shared_ptr<MqttClient> m_mqtt_client_tls;
    static TimeoutMap<int64_t, RequestCallback> m_request_cb_map;
    static std::unordered_map<std::string, std::function<void(const nlohmann::json&)>> m_status_cbs;
    static std::unordered_map<std::string, std::function<void(const nlohmann::json&)>> m_notification_cbs;

    // MQTT topics
    static std::string m_auth_topic;
    static std::string m_auth_req_topic;

    // MQTTs topics
    static std::string m_response_topic;
    static std::string m_status_topic;
    static std::string m_notification_topic;
    static std::string m_request_topic;

public:
    // Printer serial number
    static std::string m_sn;
    static std::mutex m_sn_mtx;

    // Auth info
    static nlohmann::json m_auth_info;

public:
    std::string m_user_name = "";
    std::string m_password = "";
    std::string m_ca = "";
    std::string m_cert = "";
    std::string m_key = "";
    std::string m_client_id = "";
    int m_port = 8883;

    SequenceGenerator m_seq_generator;
};

}

#endif
