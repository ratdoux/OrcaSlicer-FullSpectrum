// Web communication protocol implementation for Slicer Studio
#ifndef SSWCP_HPP
#define SSWCP_HPP

#include <iostream>
#include <mutex>
#include <stack>
#include <wx/socket.h>

#include <wx/webview.h>
#include <unordered_set>
#include <unordered_map>
#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include "nlohmann/json.hpp"
#include "slic3r/Utils/Bonjour.hpp"
#include "slic3r/Utils/TimeoutMap.hpp"
#include "slic3r/Utils/PrintHost.hpp"
#include "slic3r/Utils/MQTT.hpp"


using namespace nlohmann;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;


//WCP Interface definition
#define UPDATE_PRIVACY_STATUS "sw_SubUserUpdatePrivacy"
#define UPLOAD_CAMERA_TIMELAPSE "sw_UploadCameraTimelapse"
#define DELETE_CAMERA_TIMELAPSE "sw_DeleteCameraTimelapse"
#define GETCAMERA_TIMELAPSE_INSTANCE "sw_GetCameraTimelapseInstance"
#define GET_DEVICEDATA_STORAGESPACE "sw_GetDeviceDataStorageSpace"

namespace Slic3r { namespace GUI {

class WCP_Logger
{
public:
    bool run();

    static WCP_Logger& getInstance();

    // Add a log message to the queue
    void add_log(const wxString& content, bool is_web, wxString time, wxString module, wxString level);

    void worker();

    bool set_level(wxString& level);

    int get_level() { return m_log_level; }

    std::unordered_map<wxString, int> m_log_level_map = {
        {"debug", 0},{"info", 1},{"warning", 2},{"error", 3},{"fatal", 4},
    };

private:
    WCP_Logger();
    ~WCP_Logger();
    std::mutex           m_log_mtx; // Mutex for thread-safe access
    std::queue<wxString> m_log_que;

    std::thread m_work_thread;

    std::mutex     m_end_mtx;
    bool m_end = false;
    wxSocketClient m_client;

    asio::io_context io_ctx;
    tcp::socket*      socket = nullptr;
    tcp::resolver*    resolver;

    bool inited = false;

    int                               m_log_level     = 0;

};

// Base class for handling web communication instances
class SSWCP_Instance : public std::enable_shared_from_this<SSWCP_Instance>
{
public:
    // Types of communication instances
    enum INSTANCE_TYPE {
        COMMON,             // Common instance type
        MACHINE_FIND,       // For machine discovery
        MACHINE_CONNECT,    // For machine connection
        MACHINE_OPTION,     // For machine options/settings
        SLICE_PROJECT,      // For homepage project business
        USER_LOGIN,         // For user login
        MACHINE_MANAGE,     // For homepage machine card
        MQTT_AGENT,         // For mqtt-agent
    };

public:
    // Constructor initializes instance with command and parameters
    SSWCP_Instance(std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview)
        : m_cmd(cmd), m_header(header), m_webview(webview), m_event_id(event_id), m_param_data(data)
    {}

    virtual ~SSWCP_Instance() {
        if (m_work_thread.joinable())
            m_work_thread.detach();
    }

    // Process the command
    virtual void process();

    // Send response back to JavaScript
    virtual void send_to_js();

    // Clean up after job completion
    virtual void finish_job();

    // General failure
    void handle_general_fail(int code = -1, const wxString& msg = "failure");

    // Get instance type
    virtual INSTANCE_TYPE getType() { return m_type; }

    // Check if instance is stopped
    virtual bool is_stop() { return false; }
    virtual void set_stop(bool stop) {}

    // Mark instance as illegal (invalid)
    virtual void set_Instance_illegal();
    virtual bool is_Instance_illegal();

    // Get associated webview
    wxWebView* get_web_view() const;
    void       set_web_view(wxWebView* view);

public:
    // Handle timeout event
    virtual void on_timeout();

    static void on_mqtt_msg_arrived(std::shared_ptr<SSWCP_Instance> obj, const json& response);

    static void on_mqtt_status_msg_arrived(std::shared_ptr<SSWCP_Instance> obj, const json& response);

    static std::unordered_map<std::string, json> m_wcp_cache;

private:
    // Test methods
    void sync_test();
    void async_test();

    void test_mqtt_request();

    // Cache methods
    void sw_SetCache();
    void sw_GetCache();
    void sw_RemoveCache();

    void sw_SubscribeCacheKey();
    void sw_UnsubscribeCacheKeys();

    // select tab
    void sw_SwitchTab();

    // unsubscribe
    void sw_Webview_Unsubscribe();
    void sw_UnsubscribeAll();
    void sw_Unsubscribe_Filter();

    // get file stream
    void sw_GetFileStream();
    void sw_GetActiveFile();

    // orca console
    void sw_LaunchConsole();
    void sw_Log();
    void sw_SetLogLevel();

    // mac quit
    void sw_Exit();

    // orca log
    void sw_FileLog();

    // open browser
    void sw_OpenBrowser();
    void sw_OpenOrcaWebview();

    // Sentry
    void sw_UploadEvent();

    // open network dialog
    void sw_OpenNetworkDialog();


public:
    // 抽象工具类函数
    void update_filament_info(const json& objects, bool send_message = false);

protected:
    std::thread m_work_thread; // Worker thread

public:
    std::string m_cmd;           // Command to execute
    json        m_header;        // Request header
    wxWebView*  m_webview;       // Associated webview
    std::string m_event_id;      // Event identifier
    json        m_param_data;    // Command parameters

    json        m_res_data;      // Response data
    int         m_status = 200;  // Response status code
    std::string m_msg = "OK";    // Response message

    static bool m_first_connected;

protected:
    INSTANCE_TYPE m_type = COMMON;  // Instance type

private:
    bool m_illegal = false;      // Invalid flag
    std::mutex m_illegal_mtx;    // Mutex for illegal flag
};

// Instance class for handling machine connection
class SSWCP_MachineConnect_Instance : public SSWCP_Instance
{
public:
    SSWCP_MachineConnect_Instance(std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview)
        : SSWCP_Instance(cmd, header, data, event_id, webview)
    {
        m_type = MACHINE_CONNECT;
    }

    ~SSWCP_MachineConnect_Instance()
    {
        if (m_work_thread.joinable())
            m_work_thread.detach();
    }

    void process() override;

private:
    // Connection test methods
    void sw_test_connect();
    void sw_connect();
    void sw_disconnect();

    void sw_get_connect_machine();

    void sw_connect_other_device();

    void sw_get_pin_code();

};

// mqtt-agent
class WebPresetDialog;
class SSWCP_MqttAgent_Instance : public SSWCP_Instance
{
public:
    SSWCP_MqttAgent_Instance(std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview)
        : SSWCP_Instance(cmd, header, data, event_id, webview)
    {
        m_type = MQTT_AGENT;
    }

    ~SSWCP_MqttAgent_Instance()
    {
        if (m_work_thread.joinable())
            m_work_thread.detach();
    }

    void process() override;

public:
    static std::unordered_map<wxWebView*, std::pair<std::string, std::shared_ptr<MqttClient>>> m_mqtt_engine_map; // (id, client)
    static std::mutex                                          m_engine_map_mtx;

    static std::map<std::pair<std::string, wxWebView*>, std::string> m_subscribe_map;          // ((event_id, webview), topic)
    static std::map<std::pair<std::string, wxWebView*>, std::weak_ptr<SSWCP_Instance>> m_subscribe_instance_map; // ((event_id, webview), instance)

    static WebPresetDialog* m_dialog;

public:
    bool validate_id(const std::string& id);
    std::shared_ptr<MqttClient> get_current_engine() {
        m_engine_map_mtx.lock();
        std::shared_ptr<MqttClient> ptr = nullptr;
        if (m_mqtt_engine_map.count(m_webview)) {
            ptr = m_mqtt_engine_map[m_webview].second;
        } 
        
        m_engine_map_mtx.unlock();

        return ptr;
    }
    bool set_current_engine(const std::pair<std::string, std::shared_ptr<MqttClient>>& target) {
        bool flag = true;
        m_engine_map_mtx.lock();
        m_mqtt_engine_map[m_webview] = target;
        
        m_engine_map_mtx.unlock();
        return flag;
    }

    void set_Instance_illegal() override;

private:

    void sw_create_mqtt_client();
    void sw_mqtt_connect();
    void sw_mqtt_disconnect();
    void sw_mqtt_subscribe();
    void sw_mqtt_unsubscribe();
    void sw_mqtt_publish();
    void sw_mqtt_set_engine();




private:
    void clean_current_engine();

    static void mqtt_msg_cb(const std::string& topic, const std::string& payload, void* client);

};

// Instance class for handling machine discovery
class SSWCP_MachineFind_Instance : public SSWCP_Instance
{
public:
    SSWCP_MachineFind_Instance(std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview)
        : SSWCP_Instance(cmd, header, data, event_id, webview)
    {
        m_type = MACHINE_FIND;
    }

    void process() override;

    bool is_stop() { return m_stop; }
    void set_stop(bool stop);

private:
    // Machine discovery methods
    void sw_GetMachineFindSupportInfo();
    void sw_StartMachineFind();
    void sw_StopMachineFind();
    void sw_WakeupFind();

private:
    void add_machine_to_list(const json& machine_info);
    void onOneEngineEnd();

private:
    std::mutex m_machine_list_mtx;  // Mutex for machine list
    std::mutex m_stop_mtx;          // Mutex for stop flag

private:
    std::unordered_map<std::string, json> m_machine_list;  // Found machines

private:
    int m_engine_end_count = 0;                         // Counter for finished discovery engines
    std::vector<std::shared_ptr<Bonjour>> m_engines;    // Discovery engines
    bool m_stop = false;                                // Stop flag
};

// Instance class for handling machine options
class SSWCP_MachineOption_Instance : public SSWCP_Instance
{
public:
    SSWCP_MachineOption_Instance(std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview)
        : SSWCP_Instance(cmd, header, data, event_id, webview)
    {
        m_type = MACHINE_OPTION;
    }

    ~SSWCP_MachineOption_Instance()
    {
        if (m_work_thread.joinable())
            m_work_thread.detach();
    }

    void process() override;

private:

    // Machine option methods
    void sw_SendGCodes();
    void sw_FileGetStatus();
    void sw_SystemGetDeviceInfo();
    void sw_GetMachineState();
    void sw_GetPrintInfo();
    void sw_SubscribeMachineState();
    void sw_UnSubscribeMachineState();
    void sw_GetMachineObjects();
    void sw_SetMachineSubscribeFilter();
    void sw_GetSystemInfo();
    void sw_MachinePrintStart();
    void sw_MachinePrintPause();
    void sw_MachinePrintResume();
    void sw_MachinePrintCancel();

    // SYSTEM
    void sw_MachineFilesRoots();
    void sw_MachineFilesMetadata();
    void sw_MachineFilesThumbnails();
    void sw_MachineFilesGetDirectory();
    void sw_CameraStartMonitor();
    void sw_CameraStopMonitor();
    void sw_DeleteMachineFile();

    // PrePrint
    void sw_GetFileFilamentMapping();
    void sw_SetFilamentMappingComplete();
    void sw_FinishFilamentMapping();

    // new
    void sw_SetDeviceName();
    void sw_ControlLed();
    void sw_ControlPrintSpeed();
    void sw_BedMesh_AbortProbeMesh();
    void sw_ControlPurifier();
    void sw_ControlMainFan();
    void sw_ControlGenericFan();
    void sw_ControlBedTemp();
    void sw_ControlExtruderTemp();
    void sw_FilesThumbnailsBase64();
    void sw_exception_query();
    void sw_GetFileListPage();
    void sw_UploadCameraTimelapse();
    void sw_DeleteCameraTimelapse();
    void sw_GetTimelapseInstance();

    void sw_DefectDetactionConfig();

    void CmdForwarding();

    // Download machine file
    void sw_DownloadMachineFile();

    // upload file to machine
    void sw_UploadFiletoMachine();

    // get is legal to send & print
    void sw_GetPrintLegal();

    // get 打印任务zip流
    void sw_GetPrintZip();

    // 结束预打印流程
    void sw_FinishPreprint();

    // 设置已绑定用户信息
    void sw_ServerClientManagerSetUserinfo();

    // 请求设备下载文件并打印
    void sw_PullCloudFile();

    // 请求设备取消下载文件
    void sw_CancelPullCloudFile();

    // 请求设备下载文件并打印
    void sw_StartCloudPrint();

    // 设备耗材同步
    void sw_UpdateMachineFilamentInfo();


};

// Instance class for Snapmaker machine manage
class SSWCP_MachineManage_Instance : public SSWCP_Instance
{
public:
    SSWCP_MachineManage_Instance(std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview)
        : SSWCP_Instance(cmd, header, data, event_id, webview)
    {
        m_type = MACHINE_MANAGE;
    }

    ~SSWCP_MachineManage_Instance() {}

    void process() override;

private:
    void sw_GetLocalDevices();
    
    void sw_AddDevice();

    void sw_SubscribeLocalDevices();

    void sw_RenameDevice();

    void sw_SwitchModel();

    void sw_DeleteDevices();
};

// Instance class for page state change subscription
class SSWCP_PageStateChange_Instance : public SSWCP_Instance
{
public:
    SSWCP_PageStateChange_Instance(std::string cmd, const json& header, const json& data,
                                    std::string event_id, wxWebView* webview)
        : SSWCP_Instance(cmd, header, data, event_id, webview)
    {
        m_type = COMMON;
    }

    ~SSWCP_PageStateChange_Instance() {}

    void process() override;

private:
    void sw_SubscribePageStateChange();
    void sw_UnsubscribePageStateChange();
};

// Instance class for Snapmaker user login
class SSWCP_UserLogin_Instance : public SSWCP_Instance
{
public:
    SSWCP_UserLogin_Instance(std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview)
        : SSWCP_Instance(cmd, header, data, event_id, webview)
    {
        m_type = USER_LOGIN;
    }

    ~SSWCP_UserLogin_Instance() {}

    void process() override;

private:
    void sw_UserLogin();

    void sw_UserLogout();

    void sw_GetUserLoginState();

    void sw_SubscribeUserLoginState();

    void sw_SubUserUpdatePrivacy();

};

// Instance class for homepage business
class SSWCP_SliceProject_Instance : public SSWCP_Instance
{
public:
    SSWCP_SliceProject_Instance(std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview)
        : SSWCP_Instance(cmd, header, data, event_id, webview)
    {
        m_type = SLICE_PROJECT;
    }

    ~SSWCP_SliceProject_Instance()
    {

    }

    void process() override;

private:
    void sw_NewProject();

    void sw_OpenProject();

    void sw_GetRecentProjects(); 

    void sw_OpenRecentFile();

    void sw_DeleteRecentFiles();

    void sw_SubscribeRecentFiles();
};

// Main SSWCP class for managing communication instances
class SSWCP
{
public:
    // Handle incoming web messages
    static void handle_web_message(std::string message, wxWebView* webview);

    // Create new SSWCP instance
    static std::shared_ptr<SSWCP_Instance> create_sswcp_instance(
        std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview);

    // Delete instance
    static void delete_target(SSWCP_Instance* target);

    // Stop machine discovery
    static void stop_machine_find();

    // Stop machine subscription
    static void stop_subscribe_machine();

    // Handle webview deletion
    static void on_webview_delete(wxWebView* webview);

    // query the info of the machine
    static bool query_machine_info(std::shared_ptr<PrintHost>& host, std::string& out_model, std::vector<std::string>& out_nozzle_diameters, std::string& device_name, int timeout_second = 5);

    // update the active file name
    static void update_active_filename(const std::string& filename);

    // update the display name
    static void update_display_filename(const std::string& display_name);

    // get the active file name
    static std::string get_active_filename();

    static std::string get_display_filename();

    static std::mutex m_file_size_mutex;
    static long long m_active_file_size;
    
    
    static std::unordered_map<std::string, int> m_tab_map; // for switching tab

private:
    static std::unordered_set<std::string> m_machine_find_cmd_list;     // Machine find commands
    static std::unordered_set<std::string> m_machine_option_cmd_list;   // Machine option commands
    static std::unordered_set<std::string> m_machine_connect_cmd_list;  // Machine connect commands
    static std::unordered_set<std::string> m_project_cmd_list; // homepage project commands
    static std::unordered_set<std::string> m_login_cmd_list; // homepage login commands
    static std::unordered_set<std::string> m_machine_manage_cmd_list; // homepage machine manage commands;
    static std::unordered_set<std::string> m_mqtt_agent_cmd_list; // mqtt-agent commands;
    static std::unordered_set<std::string> m_page_state_cmd_list; // page state change commands;

    static TimeoutMap<SSWCP_Instance*, std::shared_ptr<SSWCP_Instance>> m_instance_list;  // Active instances
    static constexpr std::chrono::milliseconds DEFAULT_INSTANCE_TIMEOUT{80000}; // Default timeout (8s)

    static std::string m_active_gcode_filename; // name of the file which is pretend to be upload and print
    static std::string m_display_gcode_filename; // name for display
}; 

class MachineIPType
{
public:
    static MachineIPType* getInstance();

    void add_instance(const std::string& ip, const std::string& machine_type)
    {
        m_map_mtx.lock();
        m_ip_type_map[ip] = machine_type;
        m_map_mtx.unlock();
    }

    bool get_machine_type(const std::string& ip, std::string& output)
    {
        bool res = true;
        m_map_mtx.lock();
        if (m_ip_type_map.count(ip)) {
            output = m_ip_type_map[ip];
        } else {
            res = false;
        }
        m_map_mtx.unlock();
        return res;
    }

private:
    std::mutex                                   m_map_mtx;
    std::unordered_map<std::string, std::string> m_ip_type_map;

};

}};

#endif
