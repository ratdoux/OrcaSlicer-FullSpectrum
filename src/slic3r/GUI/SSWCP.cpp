// Implementation of web communication protocol for Slicer Studio
#include "SSWCP.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "nlohmann/json.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "sentry_wrapper/SentryWrapper.hpp"
#include <algorithm>
#include <iterator>
#include <exception>
#include <cstdlib>
#include <regex>
#include <thread>
#include <string_view>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/asio/ip/host_name.hpp>

#include <slic3r/GUI/Widgets/WebView.hpp>
#include "NetworkTestDialog.hpp"

#include "MoonRaker.hpp"

#include "slic3r/GUI/WebPresetDialog.hpp"
#include <mutex>

#include "slic3r/GUI/SMPhysicalPrinterDialog.hpp"
#include "slic3r/GUI/WebUrlDialog.hpp"

#include "miniz/miniz.h"
#include "slic3r/Utils/MQTT.hpp"

namespace pt = boost::property_tree;

using namespace nlohmann;

namespace Slic3r { namespace GUI {


// WCP_Logger
WCP_Logger::WCP_Logger() {
    
}

bool WCP_Logger::run()
{
    if (inited) {
        return true; // Already initialized
    }

    // 创建IO上下文和TCP套接字
    socket = new tcp::socket(io_ctx);

    // 解析服务器地址（本地回环）
    resolver       = new tcp::resolver(io_ctx);
    auto endpoints = resolver->resolve("127.0.0.1", "50000"); // 端口与Python服务端一致

    // 连接服务器（同步连接，阻塞直到成功或失败）

    try {
        asio::connect(*socket, endpoints);
    } catch (std::exception& e) {
        return false;
    }

    m_work_thread = std::thread(&WCP_Logger::worker, this);

    inited = true;

    return inited;
}

WCP_Logger& WCP_Logger::getInstance()
{
    static WCP_Logger instance;
    return instance;
}

bool WCP_Logger::set_level(wxString& level)
{
    if (m_log_level_map.count(level)) {
        m_log_level = m_log_level_map[level];
        return true;
    } else {
        m_log_level = 0;
        return false;
    }
}


// Add a log message to the queue
void WCP_Logger::add_log(const wxString& content, bool is_web = false, wxString time = "", wxString module = "Default", wxString level = "debug")
{
    
    if (!inited) {
        return;
    }

    if (time == "") {
        // 获取当前时间
        wxDateTime now = wxDateTime::Now();

        // 格式化日期时间部分（年-月-日 时:分:秒）
        wxString dateTimePart = now.Format(_T("%Y-%m-%d %H:%M:%S"));

        // 获取毫秒并格式化为三位字符串（补零）
        int      milliseconds = now.GetMillisecond();
        wxString msPart       = wxString::Format(_T("%03d"), milliseconds);

        // 拼接完整时间字符串
        time = dateTimePart + _T(":") + msPart;
    }



    std::lock_guard<std::mutex> lock(m_log_mtx);
    m_log_que.push((time + " [ " + (is_web ? "Flutter" : "Native") + " ] [ " + level + " ] [ " + module + "] " + content + "\n").ToUTF8());
}

void WCP_Logger::worker()
{
    while (true) {
        m_log_mtx.lock();
        if (!m_log_que.empty()) {
            wxString log = m_log_que.front();
            m_log_que.pop();
            m_log_mtx.unlock();

            log += "\n";
            try {
                asio::write(*socket, asio::buffer(log.ToUTF8().data(), log.length() + 1));
            }
            catch (std::exception& e) {

            }
            
        } else {
            m_log_mtx.unlock();
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        bool end_flag = false;
        m_end_mtx.lock();
        end_flag = m_end;
        m_end_mtx.unlock();
        if (end_flag)
            break;
    }
}

WCP_Logger::~WCP_Logger()
{
    m_end_mtx.lock();
    m_end = true;
    m_end_mtx.unlock();

    if (m_work_thread.joinable())
        m_work_thread.join();

    try {
        if (socket != nullptr && socket->is_open()) {
            socket->close();
        }
    }
    catch (std::exception& e) {
        if (resolver)
            delete resolver;
        
        if (socket)
            delete socket;
        return;
    }
    
    if (resolver)
        delete resolver;
    
    if (socket)
        delete socket;
}

extern json m_ProfileJson;
extern std::mutex m_ProfileJson_mutex;

std::vector<std::string> load_thumbnails(const std::string& file, size_t image_count)
{
    std::vector<std::string> res;
    // Read a 64k block from the end of the G-code.
    boost::nowide::ifstream ifs(file);
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":  before parse_file %1%") % file.c_str();

    bool        begin_found = false;
    bool        end_found   = false;
    std::string line;

    int thumbnail_id = 0;
    while (std::getline(ifs, line)) {
        if (thumbnail_id == image_count) {
            break;
        }
        // 找到缩略图开始标记
        if (line.find("; THUMBNAIL_BLOCK_START") != std::string::npos) {
            std::string thumb_content = "";
            int         width         = 0;
            int         height        = 0;
            int         data_size     = 0;

            // 跳过空行
            std::getline(ifs, line);
            std::getline(ifs, line);

            // 读取缩略图信息行
            std::getline(ifs, line);
            if (line.find("; thumbnail begin") != std::string::npos) {
                // 解析宽度、高度和数据大小
                // 格式: "; thumbnail begin 48x48 1144"
                sscanf(line.c_str(), "; thumbnail begin %dx%d %d", &width, &height, &data_size);

                // 读取Base64编码的数据
                std::string base64_data;
                while (std::getline(ifs, line)) {
                    if (line.find("; thumbnail end") != std::string::npos) {
                        break;
                    }
                    // 移除行首的 "; "
                    if (line.substr(0, 2) == "; ") {
                        base64_data += line.substr(2);
                    }
                }
                thumb_content = base64_data;
                res.emplace_back(thumb_content);
                
                ++thumbnail_id;
            }

            // 读取到块结束标记
            while (std::getline(ifs, line)) {
                if (line.find("; THUMBNAIL_BLOCK_END") != std::string::npos) {
                    break;
                }
            }
        }
    }

    ifs.clear(); // 清除可能的 EOF 标志
    ifs.seekg(0);

    return std::move(res);

}

// Util
std::vector<char> create_zip_with_miniz(const std::string& name1, // 原文件路径（如 "c:/xxx/1.gcode"）
                                        const std::string& name2  // ZIP 内文件名（如 "target.gcode"）
)
{
    // 1. 读取原文件内容
    std::ifstream file(name1, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open source file: " + name1);
    }

    std::vector<char> file_content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    SSWCP::m_file_size_mutex.lock();
    SSWCP::m_active_file_size = file_content.size();
    SSWCP::m_file_size_mutex.unlock();

    // 2. 初始化 ZIP 写入器（内存模式）
    mz_zip_archive zip_archive;
    memset(&zip_archive, 0, sizeof(zip_archive));

    // 初始化 ZIP 写入到堆内存
    if (!mz_zip_writer_init_heap(&zip_archive, 0, 0)) {
        throw std::runtime_error("Failed to initialize ZIP writer");
    }

    // 3. 将文件内容添加到 ZIP（使用 name2 作为内部文件名）
    if (!mz_zip_writer_add_mem(&zip_archive,
                               name2.c_str(),         // ZIP 内文件名
                               file_content.data(),   // 文件内容指针
                               file_content.size(),   // 文件内容大小
                               MZ_DEFAULT_COMPRESSION // 压缩级别
                               )) {
        mz_zip_writer_end(&zip_archive);
        throw std::runtime_error("Failed to add file to ZIP");
    }

    // 4. 完成 ZIP 写入并获取内存数据
    void*  zip_data = nullptr;
    size_t zip_size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&zip_archive, &zip_data, &zip_size)) {
        mz_zip_writer_end(&zip_archive);
        throw std::runtime_error("Failed to finalize ZIP archive");
    }

    // 将 ZIP 数据复制到 vector（方便后续操作）
    std::vector<char> zip_stream(static_cast<char*>(zip_data), static_cast<char*>(zip_data) + zip_size);

    // 5. 清理资源
    mz_zip_writer_end(&zip_archive);
    mz_free(zip_data);

    return zip_stream;
}

static const std::string base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                        "abcdefghijklmnopqrstuvwxyz"
                                        "0123456789+/";

std::string base64_encode(const char* data, size_t len)
{
    std::string encoded;
    int         i = 0, j = 0;
    uint8_t     byte3[3], byte4[4];
    while (len--) {
        byte3[i++] = *(data++);
        if (i == 3) {
            byte4[0] = (byte3[0] & 0xfc) >> 2;
            byte4[1] = ((byte3[0] & 0x03) << 4) | ((byte3[1] & 0xf0) >> 4);
            byte4[2] = ((byte3[1] & 0x0f) << 2) | ((byte3[2] & 0xc0) >> 6);
            byte4[3] = byte3[2] & 0x3f;
            for (i = 0; i < 4; i++)
                encoded += base64_chars[byte4[i]];
            i = 0;
        }
    }
    if (i) {
        for (j = i; j < 3; j++)
            byte3[j] = 0;
        byte4[0] = (byte3[0] & 0xfc) >> 2;
        byte4[1] = ((byte3[0] & 0x03) << 4) | ((byte3[1] & 0xf0) >> 4);
        byte4[2] = ((byte3[1] & 0x0f) << 2) | ((byte3[2] & 0xc0) >> 6);
        byte4[3] = byte3[2] & 0x3f;
        for (j = 0; j < i + 1; j++)
            encoded += base64_chars[byte4[j]];
        while (i++ < 3)
            encoded += '=';
    }
    return encoded;
}

std::string generate_zip_path(const std::string& oriname, const std::string& targetname)
{
    // 解析 name1 的路径
    fs::path path1 = oriname;

    // 获取父目录（例如 "c:/xxx/xxx/xxx"）
    fs::path parent_dir = path1.parent_path();

    // 将 name2 作为基础文件名，追加 ".zip"（例如 "target.gcode" -> "target.gcode.zip"）
    fs::path new_filename = fs::path(targetname);
    new_filename += ".zip"; // 直接追加扩展名

    // 组合完整路径
    fs::path zip_path = parent_dir / new_filename;
    return zip_path.string();
}

// 检查文件是否存在并读取内容
bool read_existing_zip(const std::string& zip_path, std::vector<char>& out_data)
{
    if (!fs::exists(zip_path)) {
        return false;
    }
    std::ifstream file(zip_path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open existing ZIP file: " + zip_path);
    }
    out_data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

// 主逻辑函数
json get_or_create_zip_json(const std::string& name1,   // 原文件路径（如 "1.gcode"）
                            const std::string& name2,   // 目标 ZIP 文件名（如 "target.gcode"）
                            const std::string& zip_path // 要检查的 ZIP 文件路径（如 "output.zip"）
)
{
    std::vector<char> zip_stream;

    // 1. 检查同名 ZIP 是否存在
    if (read_existing_zip(zip_path, zip_stream)) {
        std::cout << "Reusing existing ZIP file: " << zip_path << std::endl;
    } else {
        // 2. 若不存在，创建新 ZIP 并写入文件
        std::cout << "Creating new ZIP file: " << zip_path << std::endl;
        zip_stream = create_zip_with_miniz(name1, name2);

        // 将新生成的 ZIP 写入文件（可选持久化）
        std::ofstream out_file(zip_path, std::ios::binary);
        out_file.write(zip_stream.data(), zip_stream.size());
        if (!out_file.good()) {
            throw std::runtime_error("Failed to write ZIP to: " + zip_path);
        }
    }

    //// 3. 编码为 Base64 并存入 JSON
    //std::string base64_str = base64_encode(zip_stream.data(), zip_stream.size());

    json j;
    j["zip_data"] = zip_stream;
    j["zip_name"] = name2 + ".zip";
    return j;
}

bool SSWCP_Instance::m_first_connected = true;

// Base SSWCP_Instance implementation
void SSWCP_Instance::process() {
    if (m_event_id != "") {
        json header;
        send_to_js();

        m_header.clear();
        m_header["event_id"] = m_event_id;
    }
    if (m_cmd == "test") {
        sync_test();
    } else if (m_cmd == "test_async"){
        async_test();
    } else if (m_cmd == "sw_test_mqtt_moonraker") {
        test_mqtt_request();
    } else if (m_cmd == "sw_SetCache") {
        sw_SetCache();
    } else if (m_cmd == "sw_GetCache") {
        sw_GetCache();
    } else if (m_cmd == "sw_RemoveCache") {
        sw_RemoveCache();
    } else if (m_cmd == "sw_SwitchTab") {
        sw_SwitchTab();
    } else if (m_cmd == "sw_Webview_Unsubscribe") {
        sw_Webview_Unsubscribe();
    } else if (m_cmd == "sw_UnsubscribeAll") {
        sw_UnsubscribeAll();
    } else if (m_cmd == "sw_Unsubscribe_Filter") {
        sw_Unsubscribe_Filter();
    } else if (m_cmd == "sw_GetFileStream") {
        sw_GetFileStream();
    } else if (m_cmd == "sw_GetActiveFile") {
        sw_GetActiveFile();
    } else if (m_cmd == "sw_Log") {
        sw_Log();
    } else if (m_cmd == "sw_SetLogLevel") {
        sw_SetLogLevel();
    } else if (m_cmd == "sw_LaunchConsole") {
        sw_LaunchConsole();
    } else if (m_cmd == "sw_Exit") {
        sw_Exit();
    } else  if(m_cmd == "sw_FileLog") {
        sw_FileLog();
    } else if (m_cmd == "sw_SubscribeCacheKey") {
        sw_SubscribeCacheKey();
    } else if (m_cmd == "sw_UnsubscribeCacheKeys") {
        sw_UnsubscribeCacheKeys();
    } else if (m_cmd == "sw_UploadEvent") {
        sw_UploadEvent();
    } else if (m_cmd == "sw_OpenOrcaWebview") {
        sw_OpenOrcaWebview();
    } else if (m_cmd == "sw_OpenBrowser") {
        sw_OpenBrowser();
    } else if (m_cmd == "sw_OpenNetworkDialog"){
        sw_OpenNetworkDialog();
    }
    else {
        handle_general_fail();
    }
}

void SSWCP_Instance::sw_UploadEvent() {
    try {
        if(!m_param_data.count("traceId")){
            handle_general_fail(-1, "param [traceId] required!");
            return;
        }

        if(!m_param_data.count("level")){
            handle_general_fail(-1, "param [level] required!");
            return;
        }

        if(!m_param_data.count("content")){
            handle_general_fail(-1, "param [content] required!");
            return;
        }

        std::string traceId = m_param_data["traceId"].get<std::string>();
        int level = m_param_data["level"].get<int>();

        std::string content = m_param_data["content"].get<std::string>();

        std::string funcModule = m_param_data.count("funcModule") ? m_param_data["funcModule"].get<std::string>() : "";
        std::string tagKey = m_param_data.count("tagKey") ? m_param_data["tagKey"].get<std::string>() : "";
        std::string tagValue = m_param_data.count("tagValue") ? m_param_data["tagValue"].get<std::string>() : "";

        
        sentryReportLog(SENTRY_LOG_LEVEL(level), content, funcModule, tagKey, tagValue, traceId);

        send_to_js();
        finish_job();
        
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}


void SSWCP_Instance::sw_OpenNetworkDialog() {
    try {
        send_to_js();
        finish_job();

        wxGetApp().CallAfter([]() {
            // Use shared_ptr to manage dialog lifetime
            auto dlg = std::make_shared<NetworkTestDialog>(wxGetApp().mainframe);
            dlg->ShowModal();

            // Keep dialog alive for 2 seconds after closing to allow background threads to finish
            // Use a timer to delay the destruction
            class DelayedReleaseTimer : public wxTimer
            {
                std::shared_ptr<NetworkTestDialog> m_dialog;

            public:
                DelayedReleaseTimer(std::shared_ptr<NetworkTestDialog> dlg) : m_dialog(std::move(dlg))
                {
                    StartOnce(5000); // 5 seconds delay
                }
                void Notify() override
                {
                    m_dialog.reset(); // Release the dialog
                    delete this;      // Delete the timer itself
                }
            };
            new DelayedReleaseTimer(dlg); 
        });
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}


void SSWCP_Instance::sw_OpenBrowser() {
    try {
        std::string url = m_param_data.count("url") ? m_param_data["url"].get<std::string>() : "";
        wxString wx_url  = wxString::FromUTF8(url);

        std::weak_ptr<SSWCP_Instance> weak_self = shared_from_this();
        wxGetApp().CallAfter([wx_url, weak_self]() {
            auto self = weak_self.lock();
            if (!self) {
                return;
            }
            bool res = wxLaunchDefaultBrowser(wx_url);
            if (!res) {
                self->handle_general_fail(-1, "Open browser failed");
            } else {
                self->send_to_js();
                self->finish_job();
            }
        });
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_Instance::sw_OpenOrcaWebview() {
    try {
        std::string url = m_param_data.count("url") ? m_param_data["url"].get<std::string>() : "";
        wxString wx_url = wxString::FromUTF8(url);

        std::weak_ptr<SSWCP_Instance> weak_self = shared_from_this();
        wxGetApp().CallAfter([wx_url, weak_self]() {
            auto self = weak_self.lock();
            if (!self) {
                return;
            }
            auto dialog = new WebUrlDialog();
            dialog->load_url(wx_url);
            self->send_to_js();
            self->finish_job();
            dialog->Show();
        });
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_Instance::sw_FileLog() {
    try {
        std::string level = m_param_data.count("level") ? m_param_data["level"].get<std::string>() : "debug";
        std::string content = m_param_data.count("content") ? m_param_data["content"].get<std::string>() : "";

        if(level == "debug") {
            BOOST_LOG_TRIVIAL(debug) << "[WCP] " << content;
        } else if(level == "info") {
            BOOST_LOG_TRIVIAL(info) << "[WCP] " << content;
        } else if(level == "warning") {
            BOOST_LOG_TRIVIAL(warning) << "[WCP] " << content;
        } else if(level == "error") {
            BOOST_LOG_TRIVIAL(error) << "[WCP] " << content;
        } else if(level == "fatal") {
            BOOST_LOG_TRIVIAL(fatal) << "[WCP] " << content;
        }

        send_to_js();
        finish_job();
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_Instance::sw_Exit() {
    try {
        wxGetApp().Exit();
    }
    catch (std::exception& e) {

    }
}

void SSWCP_Instance::sw_GetActiveFile()
{
    try {
        std::string file_path = SSWCP::get_active_filename();
        std::string file_name = SSWCP::get_display_filename();
        if (file_path == "" || file_name == "") {
            handle_general_fail();
            return;
        }
        bool iszip = false;
        if (m_param_data.count("is_zip")) {
            iszip = m_param_data["is_zip"].get<bool>();
        }

        if (iszip) {
            std::weak_ptr<SSWCP_Instance> weak_self = shared_from_this();

            if (m_work_thread.joinable())
                m_work_thread.join();

            m_work_thread          = std::thread([file_path, file_name, weak_self]() {
                auto        self       = weak_self.lock();
                std::string zipname    = generate_zip_path(file_path, file_name);
                json        res        = get_or_create_zip_json(file_path, file_name, zipname);
                size_t      name_index = file_name.find_last_of(".");
                size_t      path_index = file_path.find_last_of(".");
                if (!(name_index == std::string::npos || path_index == std::string::npos)) {
                    self->m_res_data["file_name"] = file_name.substr(0, name_index) + ".zip";
                    self->m_res_data["file_path"] = wxString(zipname).ToUTF8();
                    SSWCP::m_file_size_mutex.lock();
                    self->m_res_data["origin_size"] = SSWCP::m_active_file_size;
                    SSWCP::m_file_size_mutex.unlock();
                    
                    wxGetApp().CallAfter([weak_self]() {
                        if (weak_self.lock()) {
                            weak_self.lock()->send_to_js();
                            weak_self.lock()->finish_job();
                        }
                    });
                } else {
                    wxGetApp().CallAfter([weak_self]() {
                        if (weak_self.lock()) {
                            weak_self.lock()->handle_general_fail();
                        }
                    });
                    return;
                }
            });
            
        } else {
            m_res_data["file_name"] = file_name;
            m_res_data["file_path"] = file_path;
            send_to_js();
            finish_job();
        }

    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_Instance::sw_LaunchConsole() {
    try {
        bool res = WCP_Logger::getInstance().run();
        if (res) {
            m_msg = "Orca Console has been launched";
            send_to_js();
            finish_job();
        } else {
            handle_general_fail(-1, "Orca Console launched failed");
        }
    }
    catch (std::exception& e) {
        wxString reason = e.what();
        handle_general_fail(-1, "Exception caught: " + reason);
    }
}

void SSWCP_Instance::sw_SetLogLevel() {
    try {
        if (m_param_data.count("level")) {
            wxString level = m_param_data["level"].get<std::string>();
            bool res = WCP_Logger::getInstance().set_level(level);
            if (res) {
                m_msg = ("The log level has been set to " + level).ToUTF8();
                send_to_js();
                finish_job();
            } else {
                handle_general_fail(-1, "The param [level] is not legal, the log level will be set to debug");
            }
        } else {
            handle_general_fail(-1, "param [level] required!");
        }
    }
    catch (std::exception& e) {
        wxString reason = e.what();
        handle_general_fail(-1, "Exception caught: " + reason);
    }
}

void SSWCP_Instance::sw_Log()
{
    try {
        wxString time = m_param_data.count("time") ? m_param_data["time"].get<wxString>() : "";
        wxString level = m_param_data.count("level") ? m_param_data["level"].get<wxString>() : "";
        wxString module = m_param_data.count("module") ? m_param_data["module"].get<wxString>() : "";
        wxString content = m_param_data.count("content") ? m_param_data["content"].get<wxString>() : "";

        auto& logger = WCP_Logger::getInstance();

        if (!logger.m_log_level_map.count(level)) {
            // todo log:级别不对,转成debug,打原生log
            level = "debug";
        } 

        if (logger.m_log_level_map[level] >= logger.get_level()) {
            logger.add_log(content, true, time, module, level);
        }

        finish_job();

    }
    catch (std::exception& e) {
        finish_job();
    }
}

void SSWCP_Instance::sw_GetFileStream() {
    try {
        bool isZip = false;
        if (m_param_data.count("is_zip")) {
            isZip = m_param_data["is_zip"].get<bool>();
        } 
        std::string file_path = SSWCP::get_active_filename();

        std::weak_ptr<SSWCP_Instance> weak_self = shared_from_this();
        if (isZip) {
            auto oriname    = SSWCP::get_active_filename();
            auto targetname = SSWCP::get_display_filename();

            std::weak_ptr<SSWCP_Instance> weak_self = shared_from_this();
            if (m_work_thread.joinable())
                m_work_thread.join();
            m_work_thread                           = std::thread([oriname, targetname, weak_self]() {
                auto self = weak_self.lock();
                if (self) {
                    std::string zipname = generate_zip_path(oriname, targetname);
                    json        res     = get_or_create_zip_json(oriname, targetname, zipname);
                    wxGetApp().CallAfter([weak_self, res]() {
                        auto self = weak_self.lock();
                        if (self) {
                            self->m_res_data["name"]    = res["zip_name"];
                            self->m_res_data["content"] = res["zip_data"];

                            self->send_to_js();
                            self->finish_job();
                        }
                    });
                }
            });
        } else {
            if (m_work_thread.joinable())
                m_work_thread.join();
            m_work_thread = std::thread([file_path, weak_self]() {
                auto self = weak_self.lock();
                if (self) {
                    // 1. 读取原文件内容
                    std::ifstream file(file_path, std::ios::binary);
                    if (!file.is_open()) {
                        self->handle_general_fail();
                        return;
                    }
                    // 获取文件大小
                    file.seekg(0, std::ios::end);
                    std::streamsize file_size = file.tellg();
                    file.seekg(0, std::ios::beg);

                    // 预分配 std::string 空间
                    std::string content;
                    content.resize(file_size);

                    // 一次性读取整个文件
                    if (!file.read(&content[0], file_size)) {
                        std::cerr << "读取文件失败" << std::endl;
                        self->handle_general_fail();
                        return;
                    }

                    self->m_res_data["content"] = wxString(content).ToUTF8();

                    wxGetApp().CallAfter([weak_self]() {
                        auto self = weak_self.lock();
                        if (self) {
                            self->send_to_js();
                            self->finish_job();
                        }
                    });
                }
            });
        }
        
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_Instance::handle_general_fail(int code, const wxString& msg)
{
    try {
        m_status = code;
        m_msg    = msg.ToUTF8();
        send_to_js();
        finish_job();
    } catch (std::exception& e) {}
    
}

// Mark instance as invalid
void SSWCP_Instance::set_Instance_illegal()
{
    m_illegal_mtx.lock();
    m_illegal = true;
    m_illegal_mtx.unlock();
}

// Check if instance is invalid
bool SSWCP_Instance::is_Instance_illegal() {
    m_illegal_mtx.lock();
    bool res = m_illegal;
    m_illegal_mtx.unlock();
    
    return res;
}

// Get associated webview
wxWebView* SSWCP_Instance::get_web_view() const {
    return m_webview;
}

void SSWCP_Instance::set_web_view(wxWebView* view) {
    m_webview = view;
}

// Send response to JavaScript
void SSWCP_Instance::send_to_js()
{
    try {
        if (is_Instance_illegal()) {
            return;
        }

        json response, payload;
        response["header"] = m_header;

        payload["code"] = m_status;
        payload["message"]  = m_msg;
        payload["data"] = m_res_data;

        response["payload"] = payload;

        std::string json_str = response.dump(4, ' ', true);
        std::string str_res = "window.postMessage(JSON.stringify(" + json_str + "), '*');";

        WCP_Logger::getInstance().add_log(str_res, false, "", "WCP", "info");

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        wxGetApp().CallAfter([weak_self, str_res]() {
            try {
                auto self = weak_self.lock();
                if (self && self->m_webview && self->m_webview->GetRefData()) {
                    WebView::RunScript(self->m_webview, str_res);
                }
            } catch (std::exception& e) {
                WCP_Logger::getInstance().add_log(e.what(), false, "", "WCP", "info");
            }
        });
    } catch (std::exception& e) {}
}

// Clean up instance
void SSWCP_Instance::finish_job() {
    SSWCP::delete_target(this);
}

// Asynchronous test implementation
void SSWCP_Instance::async_test() {
    auto http = Http::get("http://172.18.1.69/");
    http.on_error([&](std::string body, std::string error, unsigned status) {

    })
    .on_complete([=](std::string body, unsigned) {
        json data;
        data["str"] = body;
        this->m_res_data = data;
        this->send_to_js();    
        finish_job();
    })
    .perform();
}

// Synchronous test implementation
std::unordered_map<std::string, json> SSWCP_Instance::m_wcp_cache;

void SSWCP_Instance::sync_test() {
    m_res_data = m_param_data;
    send_to_js();
    finish_job();
}

void SSWCP_Instance::test_mqtt_request() {
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail();
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->test_async_wcp_mqtt_moonraker(m_param_data, [weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });
        // host->async_get_printer_info([self](const json& response) { SSWCP_Instance::on_mqtt_msg_arrived(self, response); });
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_Instance::sw_SwitchTab() {
    try {
        
        if (m_param_data.count("target")) {
            std::string target_tab = m_param_data["target"].get<std::string>();
            if (SSWCP::m_tab_map.count(target_tab)) {
                wxGetApp().mainframe->request_select_tab(MainFrame::TabPosition(SSWCP::m_tab_map[target_tab]));
                send_to_js();
                finish_job();
                return;
            }
        }
        
        handle_general_fail();
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_Instance::sw_SetCache() {
    try {
        if (m_param_data.count("objects") && m_param_data["objects"].is_array() &&
            m_param_data["objects"].size() > 0) {
            json objects = m_param_data["objects"];
            for (size_t i = 0; i < objects.size(); ++i) {
                m_wcp_cache.insert({objects[i]["key"].get<std::string>(), objects[i]["value"]});
                wxGetApp().cache_notify(objects[i]["key"].get<std::string>(), objects[i]["value"]);
            }

            send_to_js();
            finish_job();
        } else {
            handle_general_fail();
        }
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
    
}

void SSWCP_Instance::sw_GetCache()
{
    try {
        if (m_param_data.count("keys") && m_param_data["keys"].is_array() && m_param_data["keys"].size() > 0) {
            json res_data;
            json keys = m_param_data["keys"];
            for (size_t i = 0; i < keys.size(); ++i) {
                std::string key = keys[i].get<std::string>();
                if (m_wcp_cache.count(key)) {
                    res_data.push_back(m_wcp_cache[key]);
                } else {
                    res_data.push_back(json::object());
                }
            }
            m_res_data = res_data;
            send_to_js();
            finish_job();
        } else {
            handle_general_fail();
        }
    } catch (std::exception& e) {
        handle_general_fail();
    }
}


void SSWCP_Instance::sw_RemoveCache()
{
    try {
        if (m_param_data.count("keys") && m_param_data["keys"].is_array()) {
            json keys = m_param_data["keys"];
            if (keys.size() == 0) {
                for (const auto& item : m_wcp_cache) {
                    wxGetApp().cache_notify(item.first, json::value_t::null);
                }
                m_wcp_cache.clear();
            } else {
                for (size_t i = 0; i < keys.size(); ++i) {
                    std::string key = keys[i].get<std::string>();
                    if (m_wcp_cache.count(key)) {
                        wxGetApp().cache_notify(key, json::value_t::null);
                        m_wcp_cache.erase(key);
                    }
                }
            }



            send_to_js();
            finish_job();
        } else {
            handle_general_fail();
        }
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_Instance::sw_SubscribeCacheKey()
{
    try {
        if (!m_param_data.count("key") || !m_param_data["key"].is_string()) {
            handle_general_fail(-1, "param [keys] required or wrong type");
            return;
        }

        std::string key       = m_param_data["key"].get<std::string>();
        auto& cache_map = wxGetApp().m_cache_subscribers;
        
        for (auto iter = cache_map.begin(); iter != cache_map.end();) {
            if (iter->first.first == m_webview && iter->second == key) {
                // 删除之前的订阅
                iter = cache_map.erase(iter);
            } else {
                ++iter;
            }
        }

        std::weak_ptr<SSWCP_Instance> weak_self = shared_from_this();
        cache_map[{m_webview, weak_self}]       = key;
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_Instance::sw_UnsubscribeCacheKeys()
{
    try {
        if (!m_param_data.count("keys") || !m_param_data["keys"].is_array()) {
            handle_general_fail(-1, "param [keys] required or wrong type!");
            return;
        }

        json keys = m_param_data["keys"];
        auto& cache_map = wxGetApp().m_cache_subscribers;
        if (keys.size() == 0) {
            for (auto iter = cache_map.begin(); iter != cache_map.end();) {
                if (iter->first.first == m_webview) {
                    iter = cache_map.erase(iter);
                } else {
                    iter++;
                }
            }
        } else {
            for (size_t i = 0; i < keys.size(); ++i) {
                std::string delete_key = keys[i].get<std::string>();
                for (auto iter = cache_map.begin(); iter != cache_map.end();) {
                    if (iter->first.first == m_webview && iter->second == delete_key) {
                        iter = cache_map.erase(iter);
                    } else {
                        iter++;
                    }
                }
            }
        }
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_Instance::on_mqtt_status_msg_arrived(std::shared_ptr<SSWCP_Instance> obj, const json& response)
{
    if (!obj) {
        return;
    }
    if (response.is_null()) {
        obj->m_status = -1;
        obj->m_msg    = "failure";
        obj->send_to_js();
    } else if (response.count("error")) {
        if (response["error"].is_string() && "timeout" == response["error"].get<std::string>()) {
            obj->on_timeout();
        } else {
            obj->m_res_data = response;
            obj->send_to_js();
        }
    } else {
        obj->m_res_data = response;
        obj->send_to_js();
    }
}

void SSWCP_Instance::on_mqtt_msg_arrived(std::shared_ptr<SSWCP_Instance> obj, const json& response) {
    if (!obj) {
        return;
    }
    if (response.is_null()) {
        obj->m_status = -1;
        obj->m_msg    = "failure";
        obj->send_to_js();
    } else if (response.count("error")) {
        if (response["error"].is_string() && "timeout" == response["error"].get<std::string>()) {
            obj->on_timeout();
        } else {
            obj->m_res_data = response;
            obj->send_to_js();
        }
    } else {
        obj->m_res_data = response;
        obj->send_to_js();
    }
    obj->finish_job();
}

void SSWCP_Instance::sw_UnsubscribeAll() {
    wxGetApp().m_device_card_subscribers.clear();
    wxGetApp().m_recent_file_subscribers.clear();
    wxGetApp().m_user_login_subscribers.clear();
    wxGetApp().m_cache_subscribers.clear();
    wxGetApp().m_user_update_privacy_subscribers.clear();

    send_to_js();
    finish_job();
}

void SSWCP_Instance::sw_Webview_Unsubscribe() {
    auto& device_map = wxGetApp().m_device_card_subscribers;
    for (auto iter = device_map.begin(); iter != device_map.end();) {
        if (iter->first == m_webview) {
            iter = device_map.erase(iter);
        } else {
            iter++;
        }
    }

    auto& login_map = wxGetApp().m_user_login_subscribers;
    for (auto iter = login_map.begin(); iter != login_map.end();) {
        if (iter->first == m_webview) {
            iter = login_map.erase(iter);
        } else {
            iter++;
        }
    }

    auto& recent_file_map = wxGetApp().m_recent_file_subscribers;
    for (auto iter = recent_file_map.begin(); iter != recent_file_map.end();) {
        if (iter->first == m_webview) {
            iter = recent_file_map.erase(iter);
        } else {
            iter++;
        }
    }

    auto& cache_map = wxGetApp().m_cache_subscribers;
    for (auto iter = cache_map.begin(); iter != cache_map.end();) {
        if (iter->first.first == m_webview) {
            iter = cache_map.erase(iter);
        } else {
            iter++;
        }
    }

    auto& privacy_map = wxGetApp().m_user_update_privacy_subscribers;
    for (auto iter = privacy_map.begin(); iter != privacy_map.end();) {
        if (iter->first == m_webview) {
            iter = privacy_map.erase(iter);
        } else {
            iter++;
        }
    }

    send_to_js();
    finish_job();
}

void SSWCP_Instance::sw_Unsubscribe_Filter() {
    try {
        std::string cmd = m_param_data.count("cmd") ? m_param_data["cmd"].get<std::string>() : "";
        std::string event_id = m_param_data.count("event_id") ? m_param_data["event_id"].get<std::string>() : "";
        if (cmd == "" || event_id == "") {
            send_to_js();
            finish_job();
            return;
        }

        auto&       device_map = wxGetApp().m_device_card_subscribers;
        auto&       login_map  = wxGetApp().m_user_login_subscribers;
        auto&       privacy_map     = wxGetApp().m_user_update_privacy_subscribers;
        auto&       recent_file_map = wxGetApp().m_recent_file_subscribers;
        auto&       cache_map       = wxGetApp().m_cache_subscribers;

        if (cmd == "") {
            for (auto iter = device_map.begin(); iter != device_map.end();) {
                if (iter->first == m_webview) {
                    auto ptr = iter->second.lock();
                    if (ptr) {
                        if (ptr->m_event_id == event_id) {
                            iter = device_map.erase(iter);
                        } else {
                            iter++;
                        }
                    } else {
                        iter = device_map.erase(iter);
                    }
                } else {
                    iter++;
                }
            }

            for (auto iter = login_map.begin(); iter != login_map.end();) {
                if (iter->first == m_webview) {
                    auto ptr = iter->second.lock();
                    if (ptr) {
                        if (ptr->m_event_id == event_id) {
                            iter = login_map.erase(iter);
                        } else {
                            iter++;
                        }
                    } else {
                        iter = login_map.erase(iter);
                    }
                } else {
                    iter++;
                }
            }

             for (auto iter = privacy_map.begin(); iter != privacy_map.end();) {
                if (iter->first == m_webview) {
                    auto ptr = iter->second.lock();
                    if (ptr) {
                        if (ptr->m_event_id == event_id) {
                            iter = privacy_map.erase(iter);
                        } else {
                            iter++;
                        }
                    } else {
                        iter = privacy_map.erase(iter);
                    }
                } else {
                    iter++;
                }
            }

            for (auto iter = recent_file_map.begin(); iter != recent_file_map.end();) {
                if (iter->first == m_webview) {
                    auto ptr = iter->second.lock();
                    if (ptr) {
                        if (ptr->m_event_id == event_id) {
                            iter = recent_file_map.erase(iter);
                        } else {
                            iter++;
                        }
                    } else {
                        iter = recent_file_map.erase(iter);
                    }
                } else {
                    iter++;
                }
            }

            for (auto iter = cache_map.begin(); iter != cache_map.end();) {
                if (iter->first.first == m_webview) {
                    auto ptr = iter->first.second.lock();
                    if (ptr) {
                        if (ptr->m_event_id == event_id) {
                            iter = cache_map.erase(iter);
                        } else {
                            iter++;
                        }
                    } else {
                        iter = cache_map.erase(iter);
                    }
                } else {
                    iter++;
                }
            }

        } else if (cmd == "sw_SubscribeRecentFiles") {
            for (auto iter = recent_file_map.begin(); iter != recent_file_map.end();) {
                if (iter->first == m_webview) {
                    auto ptr = iter->second.lock();
                    if (ptr) {
                        if (event_id == "" || (event_id != "" && event_id == ptr->m_event_id)) {
                            iter = recent_file_map.erase(iter);
                        } else {
                            iter++;
                        }
                    } else {
                        iter = recent_file_map.erase(iter);
                    }
                } else {
                    iter++;
                }
            }
        } else if (cmd == "sw_SubscribeUserLoginState") {
            for (auto iter = login_map.begin(); iter != login_map.end();) {
                if (iter->first == m_webview) {
                    auto ptr = iter->second.lock();
                    if (ptr) {
                        if (event_id == "" || (event_id != "" && event_id == ptr->m_event_id)) {
                            iter = login_map.erase(iter);
                        } else {
                            iter++;
                        }
                    } else {
                        iter = login_map.erase(iter);
                    }
                } else {
                    iter++;
                }
            }
        } else if (cmd == UPDATE_PRIVACY_STATUS) {

             for (auto iter = privacy_map.begin(); iter != privacy_map.end();) {
                if (iter->first == m_webview) {
                    auto ptr = iter->second.lock();
                    if (ptr) {
                        if (event_id == "" || (event_id != "" && event_id == ptr->m_event_id)) {
                            iter = privacy_map.erase(iter);
                        } else {
                            iter++;
                        }
                    } else {
                        iter = privacy_map.erase(iter);
                    }
                } else {
                    iter++;
                }
            }

        }else if (cmd == "sw_SubscribeLocalDevices") {
            for (auto iter = device_map.begin(); iter != device_map.end();) {
                if (iter->first == m_webview) {
                    auto ptr = iter->second.lock();
                    if (ptr) {
                        if (event_id == "" || (event_id != "" && event_id == ptr->m_event_id)) {
                            iter = device_map.erase(iter);
                        } else {
                            iter++;
                        }
                    } else {
                        iter = device_map.erase(iter);
                    }
                } else {
                    iter++;
                }
            }
        } else if (cmd == "sw_SubscribeCacheKey") {
            for (auto iter = cache_map.begin(); iter != cache_map.end();) {
                if (iter->first.first == m_webview) {
                    auto ptr = iter->first.second.lock();
                    if (ptr) {
                        if (event_id == "" || (event_id != "" && event_id == ptr->m_event_id)) {
                            iter = cache_map.erase(iter);
                        } else {
                            iter++;
                        }
                    } else {
                        iter = cache_map.erase(iter);
                    }
                } else {
                    iter++;
                }
            }
        } 

        send_to_js();
        finish_job();
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}


// Handle timeout event
void SSWCP_Instance::on_timeout() {
    handle_general_fail(-2, "time out");
    finish_job();
}

void SSWCP_Instance::update_filament_info(const json& objects, bool send_message)
{
    if (objects.size() < 1) {
        if (send_message)
            handle_general_fail(-1, "objects is empty!");
        return;
    }

    if (!objects[0].count("key") || !objects[0]["key"].is_string()) {
        if (send_message)
            handle_general_fail(-1, "param [key] required or wrong type!");
        return;
    }

    if (!objects[0].count("value") || !objects[0]["value"].is_string()) {
        if (send_message)
            handle_general_fail(-1, "param [value] required or wrong type!");
        return;
    }

    std::string sn    = objects[0]["key"].get<std::string>();
    std::string value = objects[0]["value"].get<std::string>();

    DeviceInfo info;
    if (!wxGetApp().app_config->get_device_info(sn, info)) {
        m_msg = "sn does not exist!";
        if (send_message) {
            handle_general_fail(-1, m_msg);
        }
        return;
    } else {
        if (!info.connected) {
            m_msg = "The machine is not connected!";
            if (send_message) {
                handle_general_fail(-1, m_msg);
            }
            return;
        } else {
            json j_value;
            try {
                j_value = json::parse(value);
            } catch (std::exception& e) {
                if (send_message)
                    handle_general_fail(-1, "value parse failed");
                return;
            }

            if (!j_value.count("filament_vendor") || !j_value["filament_vendor"].is_array() || !j_value.count("filament_type") ||
                !j_value["filament_type"].is_array() || !j_value.count("filament_sub_type") || !j_value["filament_sub_type"].is_array() ||
                ((!j_value.count("filament_color") || !j_value["filament_color"].is_array()) &&
                 (!j_value.count("filament_color_rgba") || !j_value["filament_color_rgba"].is_array())) ||
                !j_value.count("extruder_map_table") || !j_value["extruder_map_table"].is_array() || !j_value.count("filament_official") ||
                !j_value["filament_official"].is_array()) {
                if (send_message) {
                    handle_general_fail(-1, "value parse failed");
                }
                return;
            }

            // 存储耗材，并触发更新
            auto& filaments = wxGetApp().preset_bundle->machine_filaments;
            static auto tmp_filaments = filaments;

            if (m_first_connected) {
                tmp_filaments = filaments;
                m_first_connected = false;
            }

            filaments.clear();

            size_t count = 0;
            for (size_t i = 0; i < j_value["filament_official"].size(); ++i) {
                bool is_official = j_value["filament_official"][i].get<bool>();
                if (/*is_official*/ true) {
                    std::string vendor   = j_value["filament_vendor"][i].get<std::string>();
                    std::string type     = j_value["filament_type"][i].get<std::string>();
                    std::string sub_type = j_value["filament_sub_type"][i].get<std::string>();

                    std::string name = "";

                    // 名称特殊处理
                    if (type == "TPU") {
                        name = vendor + " " + type;
                    } else if (sub_type == "Support") {
                        name = vendor + " Support" + " For " + type;
                    } else {
                        name = vendor + " " + type + ((sub_type != "NONE" && sub_type != "") ? " " + sub_type : "");
                    }

                    int extruder = j_value["extruder_map_table"][i].get<int>();

                    if (j_value.count("filament_color_rgba") && j_value["filament_color_rgba"].is_array() &&
                        j_value["filament_color_rgba"].size() != 0) {
                        std::string str_color = "#" + j_value["filament_color_rgba"][i].get<std::string>();
                        filaments.insert({int(i), {name, str_color}});
                    } else {
                        if (j_value["filament_color"][i].is_number()) {
                            int                color = j_value["filament_color"][i].get<int>();
                            std::ostringstream oss;
                            oss << "#" << std::uppercase << std::setfill('0') << std::setw(6) << std::hex
                                << (color & 0x00FFFFFF); // 仅取低24位

                            std::string str_color = oss.str();
                            filaments.insert({int(i), {name, str_color}});
                        } else {
                            std::string str_color = "#" + j_value["filament_color"][i].get<std::string>();
                            filaments.insert({int(i), {name, str_color}});
                        }
                    }
                }
            }

            bool need_load_preset = false;

            if (filaments.size() == 0 && tmp_filaments.size() != 0)
                need_load_preset = true;

            for (auto iter = filaments.begin(); iter != filaments.end(); iter++) {
                if (tmp_filaments.count(iter->first)) {
                    auto pair     = iter->second;
                    auto tmp_pair = tmp_filaments[iter->first];
                    if (pair.first == tmp_pair.first && pair.second == pair.second) {
                        continue;
                    } else {
                        need_load_preset = true;
                        break;
                    }
                } else {
                    auto pair        = iter->second;
                    if (pair.first.find("NONE") != std::string::npos) {
                        continue;
                    }
                    need_load_preset = true;
                    break;
                }
            }
            if (need_load_preset) {
                tmp_filaments = filaments;
                wxGetApp().load_current_presets();
            }

            
            if (send_message) {
                send_to_js();
                finish_job();
            }
        }
    }
}

// SSWCP_MachineFind_Instance implementation

// Process machine find commands
void SSWCP_MachineFind_Instance::process() {
    if (m_event_id != "") {
        json header;
        send_to_js();

        m_header.clear();
        m_header["event_id"] = m_event_id;
    }

    if (m_cmd == "sw_StartMachineFind") {
        sw_StartMachineFind();
    } else if (m_cmd == "sw_GetMachineFindSupportInfo") {
        sw_GetMachineFindSupportInfo();
    } else if (m_cmd == "sw_StopMachineFind") {
        sw_StopMachineFind();
    } else if (m_cmd == "sw_WakeupFind") {
        sw_WakeupFind();
    } else {
        handle_general_fail();
    }
}

// Set stop flag for machine discovery
void SSWCP_MachineFind_Instance::set_stop(bool stop)
{
    m_stop_mtx.lock();
    m_stop = stop;
    m_stop_mtx.unlock();
}

// Get machine discovery support info
void SSWCP_MachineFind_Instance::sw_GetMachineFindSupportInfo()
{
    // 2.0.0 只支持 mdns - snapmaker
    json protocols = json::array();
    protocols.push_back("mdns");

    json mdns_service_names = json::array();
    mdns_service_names.push_back("snapmaker");

    json res;
    res["protocols"] = protocols;
    res["mdns_service_names"] = mdns_service_names;

    m_res_data = res;

    send_to_js();
}

void SSWCP_MachineFind_Instance::sw_WakeupFind()
{
    try {
        // 1) 广域服务浏览：触发 _services._dns-sd._udp.local.，维持约8秒
        //    此查询促使 AP/交换机为本机建立 224.0.0.251:5353 的组播转发表，并促使设备进行通告
        try {
            Bonjour::TxtKeys warmup_txt_keys = {};
            auto warmup = Bonjour("services._dns-sd")
                              .set_protocol("udp")
                              .set_txt_keys(std::move(warmup_txt_keys))
                              .set_retries(2)
                              .set_timeout(4) // 2*4 = ~8s
                              .on_reply([](BonjourReply&&){})
                              .on_complete([](){})
                              .lookup();
            (void)warmup;
        } catch (...) {
            // 预热失败不影响后续短查
        }

        // 2) 目标服务短查：对 snapmaker 做一次短查询（2 秒），帮助休眠设备快速回应
        try {
            auto kick = Bonjour("snapmaker")
                            .set_retries(1)
                            .set_timeout(2)
                            .on_reply([](BonjourReply&&){})
                            .on_complete([](){})
                            .lookup();
            (void)kick;
        } catch (...) {
        }

        // 立刻返回成功，查询在线程中进行
        m_status = 200;
        m_msg    = "OK";
        m_res_data["result"] = "wakeup_started";
        send_to_js();
        finish_job();
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

// Start machine discovery
void SSWCP_MachineFind_Instance::sw_StartMachineFind()
{
    try {

        std::vector<string> protocols;

        float last_time = -1;
        if (m_param_data.count("last_time") && m_param_data["last_time"].is_number()) {
            last_time = m_param_data["last_time"].get<float>();
        }
        last_time = -1;

        // 目前只支持通过mdns协议搜索snapmaker,prusalink，之后可以再扩充
        protocols.push_back("mdns");

        for (size_t i = 0; i < protocols.size(); ++i) {
            if (protocols[i] == "mdns") {
                std::vector<std::string> mdns_service_names;

                mdns_service_names.push_back("snapmaker");
                //mdns_service_names.push_back("prusalink");
                //mdns_service_names.push_back("rdlink");
                //mdns_service_names.push_back("raop");

                m_engines.clear();
                for (size_t i = 0; i < mdns_service_names.size(); ++i) {
                    m_engines.push_back(nullptr);
                }

                //// 预热扫描：发送一轮短超时查询以激活休眠后的mDNS响应
                //for (const auto& svc : mdns_service_names) {
                //    Bonjour(svc)
                //        .set_retries(1)
                //        .set_timeout(2)
                //        .on_reply([](BonjourReply&&){})
                //        .on_complete([]{})
                //        .lookup();
                //}
                //std::this_thread::sleep_for(std::chrono::milliseconds(300));

                Bonjour::TxtKeys txt_keys   = {"sn", "version", "machine_type", "link_mode", "userid", "device_name", "ip", "region"};
                std::string      unique_key = "sn";

                for (size_t i = 0; i < m_engines.size(); ++i) {
                    auto self = std::dynamic_pointer_cast<SSWCP_MachineFind_Instance>(shared_from_this());
                    if(!self){
                        continue;
                    }
                    std::weak_ptr<SSWCP_MachineFind_Instance> weak_self(self);
                    m_engines[i] = Bonjour(mdns_service_names[i])
                                     .set_txt_keys(std::move(txt_keys))
                                     .set_retries(3)
                                     .set_timeout(last_time >= 0.0 ? last_time/1000 : 20)
                                     .on_reply([weak_self, unique_key](BonjourReply&& reply) {
                                         // Check if application is still alive before processing
                                         if (!GUI_App::m_app_alive.load()) {
                                             return;
                                         }
                                         
                                         auto self = weak_self.lock();
                                         if(!self || self->is_stop()){
                                            return;
                                         }
                                         
                                         // Double check application is still alive after locking
                                         if (!GUI_App::m_app_alive.load()) {
                                             return;
                                         }
                                         
                                         json machine_data;

                                         std::string hostname = reply.hostname;
                                         size_t      pos      = hostname.find(".local");
                                         if (pos != std::string::npos) {
                                             hostname = hostname.substr(0, pos);
                                         }

                                         machine_data["name"] = hostname != "" ? hostname : reply.service_name;
                                         // machine_data["hostname"] = reply.hostname;

                                         if (!reply.ip.is_v4()) {
                                             return;
                                         }

                                         machine_data["ip"]       = reply.ip.to_string();
                                         machine_data["port"]     = reply.port;
                                         if (reply.txt_data.count("protocol")) {
                                             machine_data["protocol"] = reply.txt_data["protocol"];
                                         }
                                         if (reply.txt_data.count("version")) {
                                             machine_data["pro_version"] = reply.txt_data["version"];
                                         }
                                         if (reply.txt_data.count(unique_key)) {
                                             machine_data["unique_key"]   = unique_key;
                                             machine_data["unique_value"] = reply.txt_data[unique_key];
                                             machine_data[unique_key]     = machine_data["unique_value"];
                                         }
                                         if (reply.txt_data.count("link_mode")) {
                                             machine_data["link_mode"] = reply.txt_data["link_mode"];
                                         }
                                         if (reply.txt_data.count("userid")) {
                                             machine_data["userid"] = reply.txt_data["userid"];
                                         }
                                         if (reply.txt_data.count("device_name")) {
                                             machine_data["device_name"] = reply.txt_data["device_name"];
                                         }

                                         // 模拟一下
                                        /* machine_data["cover"] = LOCALHOST_URL + std::to_string(PAGE_HTTP_PORT) +
                                                                 "/profiles/Snapmaker/Snapmaker A350 Dual BKit_cover.png";*/

                                         if (reply.txt_data.count("machine_type")) {
                                             std::string machine_type      = reply.txt_data["machine_type"];
                                             machine_data["machine_type"] = machine_type;

                                             auto machine_ip_type = MachineIPType::getInstance();
                                             if (machine_ip_type) {
                                                 machine_ip_type->add_instance(reply.ip.to_string(), machine_type);
                                             }

                                             size_t      vendor_pos    = machine_type.find_first_of(" ");
                                             if (vendor_pos != std::string::npos) {
                                                 std::string vendor = machine_type.substr(0, vendor_pos);
                                                 // Check application is still alive before accessing wxGetApp()
                                                 if (GUI_App::m_app_alive.load()) {
                                                     try {
                                                         std::string machine_cover = LOCALHOST_URL + std::to_string(wxGetApp().m_page_http_server.get_port()) + "/profiles/" +
                                                                                     vendor + "/" + machine_type + "_cover.png";
                                                         machine_data["cover"] = machine_cover;
                                                     } catch (...) {
                                                         // Application is shutting down, skip setting cover
                                                     }
                                                 }
                                             }
                                         } else {
                                             // test
                                             /*auto machine_ip_type = MachineIPType::getInstance();
                                             if (machine_ip_type) {
                                                 machine_ip_type->add_instance(reply.ip.to_string(), "unknown");
                                             }*/
                                         }

                                         if (reply.txt_data.count("ip") && machine_data["ip"] == "") {
                                             BOOST_LOG_TRIVIAL(warning) << "[Machine Find] Can't find the endpoint's ip, use the txtData: "
                                                                        << reply.txt_data["ip"];
                                             machine_data["ip"] = reply.txt_data["ip"];
                                         }

                                         machine_data["txt_ip"] = reply.txt_data["ip"];

                                         if (reply.txt_data.count("region")) {
                                             machine_data["region"] = reply.txt_data["region"];
                                         }

                                         // Final check before adding to list
                                         if (!GUI_App::m_app_alive.load() || !self || self->is_stop()) {
                                             return;
                                         }

                                         json machine_object;
                                         if (machine_data.count("unique_value")) {
                                             self->add_machine_to_list(machine_object);
                                             machine_object[reply.txt_data[unique_key]] = machine_data;
                                         } else {
                                             machine_object[reply.ip.to_string()] = machine_data;
                                         }
                                         self->add_machine_to_list(machine_object);
                                         
                                     })
                                     .on_complete([weak_self]() {
                                         // Check if application is still alive before scheduling callback
                                         if (!GUI_App::m_app_alive.load()) {
                                             return;
                                         }
                                         
                                         try {
                                             wxGetApp().CallAfter([weak_self]() {
                                                 // Check again inside the callback
                                                 if (!GUI_App::m_app_alive.load()) {
                                                     return;
                                                 }
                                                 
                                                 auto self = weak_self.lock();
                                                 if (self && !self->is_stop()) {
                                                     self->onOneEngineEnd();
                                                 }
                                             });
                                         } catch (...) {
                                             // Application is shutting down, ignore the callback
                                         }
                                     })
                                     .lookup();
                }

            } else {
                // 支持其他机器发现协议
            }
        }

    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineFind_Instance::sw_StopMachineFind()
{
    try {
        SSWCP::stop_machine_find();
        send_to_js();
        finish_job();
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}



void SSWCP_MachineFind_Instance::add_machine_to_list(const json& machine_info)
{
    try {
        BOOST_LOG_TRIVIAL(info) << "check the machine list on json: " << machine_info.dump();
        for (const auto& [key, value] : machine_info.items()) {
            std::string sn        = value["sn"].get<std::string>();
            bool        need_send = false;
            m_machine_list_mtx.lock();
            if (!m_machine_list.count(sn)) {
                m_machine_list[sn] = machine_info;
                m_machine_list_mtx.unlock();

                m_res_data[key] = value;
                need_send       = true;
            } else {
                m_machine_list_mtx.unlock();
            }

            m_res_data[key]["connected"] = false;

            DeviceInfo info;
            if (wxGetApp().app_config->get_device_info(sn, info)) {
                if (info.connected) {
                    m_res_data[key]["connected"] = true;
                }
                std::string ip = value["ip"].get<std::string>();
                if (0) {
                    info.ip = ip;
                    wxGetApp().app_config->save_device_info(info);

                    

                    wxGetApp().CallAfter([]() {
                        auto devices = wxGetApp().app_config->get_devices();
                        json param;
                        param["command"]       = "local_devices_arrived";
                        param["sequece_id"]    = "10001";
                        param["data"]          = devices;
                        std::string logout_cmd = param.dump();
                        wxString    strJS      = wxString::Format("window.postMessage(%s)", logout_cmd);
                        GUI::wxGetApp().run_script(strJS);

                        // wcp订阅
                        json data = devices;
                        wxGetApp().device_card_notify(data);
                    });
                    
                }
            }

            if (need_send) {
                send_to_js();
            }
        }
    }
    catch (std::exception& e) {

    }
}

void SSWCP_MachineFind_Instance::onOneEngineEnd()
{
    ++m_engine_end_count;
    if (m_engine_end_count == m_engines.size()) {
        this->finish_job();
    }
}


// SSWCP_MachineOption_Instance
void SSWCP_MachineOption_Instance::process()
{
    if (m_event_id != "") {
        json header;
        send_to_js();

        m_header.clear();
        m_header["event_id"] = m_event_id;
    }
    if (m_cmd == "sw_SendGCodes") {
        sw_SendGCodes();
    } else if (m_cmd == "sw_FileGetStatus") {
        sw_FileGetStatus();
    } else if (m_cmd == "sw_SystemGetDeviceInfo") {
        sw_SystemGetDeviceInfo();
    } else if (m_cmd == "sw_GetMachineState") {
        sw_GetMachineState();
    } else if (m_cmd == "sw_SubscribeMachineState") {
        sw_SubscribeMachineState();
    } else if (m_cmd == "sw_GetMachineObjects") {
        sw_GetMachineObjects();
    } else if (m_cmd == "sw_SetSubscribeFilter") {
        sw_SetMachineSubscribeFilter();
    } else if (m_cmd == "sw_StopMachineStateSubscription") {
        sw_UnSubscribeMachineState();
    } else if (m_cmd == "sw_GetPrinterInfo") {
        sw_GetPrintInfo();
    } else if (m_cmd == "sw_GetMachineSystemInfo") {
        sw_GetSystemInfo();
    } else if (m_cmd == "sw_MachinePrintStart") {
        sw_MachinePrintStart();
    } else if (m_cmd == "sw_MachinePrintPause") {
        sw_MachinePrintPause();
    } else if (m_cmd == "sw_MachinePrintResume") {
        sw_MachinePrintResume();
    } else if (m_cmd == "sw_MachinePrintCancel") {
        sw_MachinePrintCancel();
    } else if (m_cmd == "sw_MachineFilesRoots") {
        sw_MachineFilesRoots();
    } else if (m_cmd == "sw_MachineFilesMetadata") {
        sw_MachineFilesMetadata();
    } else if (m_cmd == "sw_MachineFilesThumbnails") {
        sw_MachineFilesThumbnails();
    } else if (m_cmd == "sw_MachineFilesGetDirectory") {
        sw_MachineFilesGetDirectory();
    } else if (m_cmd == "sw_CameraStartMonitor") {
        sw_CameraStartMonitor();
    } else if (m_cmd == "sw_CameraStopMonitor") {
        sw_CameraStopMonitor();
    } else if (m_cmd == "sw_DeleteMachineFile") {
        sw_DeleteMachineFile();
    } else if (m_cmd == "sw_SetFilamentMappingComplete") {
        sw_SetFilamentMappingComplete();
    } else if (m_cmd == "sw_GetFileFilamentMapping") {
        sw_GetFileFilamentMapping();
    } else if (m_cmd == "sw_FinishFilamentMapping") {
        sw_FinishFilamentMapping();
    } else if(m_cmd == "sw_DownloadMachineFile"){
        sw_DownloadMachineFile();
    } else if (m_cmd == "sw_UploadFiletoMachine") {
        sw_UploadFiletoMachine();
    } else if (m_cmd == "sw_GetPrintLegal") {
        sw_GetPrintLegal();
    } else if (m_cmd == "sw_GetPrintZip") {
        sw_GetPrintZip();
    } else if (m_cmd == "sw_FinishPreprint") {
        sw_FinishPreprint();
    } else if (m_cmd == "sw_PullCloudFile") {
        sw_PullCloudFile();
    } else if (m_cmd == "sw_CancelPullCloudFile") {
        sw_CancelPullCloudFile();
    } else if (m_cmd == "sw_StartCloudPrint") {
        sw_StartCloudPrint();
    } else if (m_cmd == "sw_SetDeviceName") {
        sw_SetDeviceName();
    } else if (m_cmd == "sw_ControlLed") {
        sw_ControlLed();
    } else if (m_cmd == "sw_ControlPrintSpeed") {
        sw_ControlPrintSpeed();
    } else if (m_cmd == "sw_BedMesh_AbortProbeMesh") {
        sw_BedMesh_AbortProbeMesh();
    } else if (m_cmd == "sw_ControlPurifier") {
        sw_ControlPurifier();
    } else if (m_cmd == "sw_ControlMainFan") {
        sw_ControlMainFan();
    } else if (m_cmd == "sw_ControlGenericFan") {
        sw_ControlGenericFan();
    } else if (m_cmd == "sw_ControlBedTemp") {
        sw_ControlBedTemp();
    } else if (m_cmd == "sw_ControlExtruderTemp") {
        sw_ControlExtruderTemp();
    } else if (m_cmd == "sw_FilesThumbnailsBase64") {
        sw_FilesThumbnailsBase64();
    } else if (m_cmd == "sw_exception_query") {
        sw_exception_query();
    } else if (m_cmd == "sw_GetFileListPage") {
        sw_GetFileListPage();
    } else if (m_cmd == "sw_UpdateMachineFilamentInfo") {
        sw_UpdateMachineFilamentInfo();
    } else if (m_cmd == "sw_UploadCameraTimelapse") {
        sw_UploadCameraTimelapse();
    } else if (m_cmd == "sw_DeleteCameraTimelapse") {
        sw_DeleteCameraTimelapse();
    } else if (m_cmd == "sw_GetTimelapseInstance") {
        sw_GetTimelapseInstance();
    } else if (m_cmd == "sw_ServerClientManagerSetUserinfo") {
        sw_ServerClientManagerSetUserinfo();
    } else if (m_cmd == "sw_DefectDetactionConfig"){
        sw_DefectDetactionConfig();
    }
    
    else {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_UnSubscribeMachineState() {
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            m_status = 1;
            m_msg    = "failure";
            send_to_js();
            finish_job();
        }

        auto weak_self  = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        std::string key       = m_event_id + std::to_string(int64_t(m_webview));
        host->async_unsubscribe_machine_info(key, [weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });

        SSWCP::stop_subscribe_machine();


    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_SubscribeMachineState() {
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            m_status = 1;
            m_msg    = "failure";
            send_to_js();
            finish_job();
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        std::string key       = m_event_id + std::to_string(int64_t(m_webview));
        host->async_subscribe_machine_info(key, [weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_status_msg_arrived(self, response);
            }
        });

    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_GetPrintInfo() {
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail();
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_get_printer_info([weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_GetMachineState() {
    try {
        if (m_param_data.count("objects")) {
            std::shared_ptr<PrintHost> host = nullptr;
            wxGetApp().get_connect_host(host);
            std::vector<std::pair<std::string, std::vector<std::string>>> targets;

            json items = m_param_data["objects"];
            for (auto& [key, value] : items.items()) {
                if (value.is_null()) {
                    targets.push_back({key, {}});
                } else {
                    std::vector<std::string> items;
                    if (value.is_array()) {
                        for (size_t i = 0; i < value.size(); ++i) {
                            items.push_back(value[i].get<std::string>());
                        }
                    } else {
                        items.push_back(value.get<std::string>());
                    }
                    targets.push_back({key, items});
                }
            }

            if (!host) {
                handle_general_fail();
                return;
            }

            auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
            host->async_get_machine_info(targets, [weak_self](const json& response) {
                auto self = weak_self.lock();
                if (self) {
                    SSWCP_Instance::on_mqtt_msg_arrived(self, response);
                }
            });
        } else {
            handle_general_fail();
        }

    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_SystemGetDeviceInfo() {
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);
        if (!host) {
            handle_general_fail();
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_get_device_info([weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });

    } catch (const std::exception&) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_FileGetStatus()
{
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail();
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_server_files_get_status   ([weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_SendGCodes() {
    try {
        if (m_param_data.count("script")) {
            std::shared_ptr<PrintHost> host = nullptr;
            wxGetApp().get_connect_host(host);
            std::vector<std::string>   str_codes;
        

            if (m_param_data["script"].is_array()) {
                json codes = m_param_data["script"];
                for (size_t i = 0; i < codes.size(); ++i) {
                    str_codes.push_back(codes[i].get<std::string>());
                }
            } else if (m_param_data["script"].is_string()) {
                str_codes.push_back(m_param_data["script"].get<std::string>());
            }

            if (!host) {
                handle_general_fail();
                return;
            }

            auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
            host->async_send_gcodes(str_codes, [weak_self](const json& response) {
                auto self = weak_self.lock();
                if (self) {
                    SSWCP_Instance::on_mqtt_msg_arrived(self, response);
                }
            });
        }
        
    } catch (const std::exception&) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_MachinePrintStart() {
    try {
        if (m_param_data.count("filename")) {
            std::shared_ptr<PrintHost> host = nullptr;
            wxGetApp().get_connect_host(host);

            std::string filename = m_param_data["filename"].get<std::string>();

            if (!host) {
                handle_general_fail();
                return;
            }

            auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
            host->async_start_print_job(filename, [weak_self](const json& response) {
                auto self = weak_self.lock();
                if (self) {
                    SSWCP_Instance::on_mqtt_msg_arrived(self, response); 
                }
            });
        }
    }
    catch(std::exception& e){
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_MachinePrintPause()
{
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail();
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_pause_print_job([weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response); 
            }
        });
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_MachinePrintResume()
{
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail();
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_resume_print_job([weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response); 
            }
        });
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_MachinePrintCancel()
{
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail();
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_cancel_print_job([weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response); 
            }
        });
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_GetSystemInfo()
{
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail();
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_get_system_info([weak_self](const json& response) { 
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response); 
            }
        });
    }
    catch(std::exception& e){
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_SetMachineSubscribeFilter()
{
    try {
        if (m_param_data.count("objects")) {
            std::shared_ptr<PrintHost> host = nullptr;
            wxGetApp().get_connect_host(host);
            std::vector<std::pair<std::string, std::vector<std::string>>> targets;

            json items = m_param_data["objects"];
            for (auto& [key, value] : items.items()) {
                if (value.is_null()) {
                    targets.push_back({key, {}});
                } else {
                    std::vector<std::string> items;
                    if (value.is_array()) {
                        for (size_t i = 0; i < value.size(); ++i) {
                            items.push_back(value[i].get<std::string>());
                        }
                    } else {
                        items.push_back(value.get<std::string>());
                    }
                    targets.push_back({key, items});
                }
            }

            if (!host) {
                // 错误处理
                handle_general_fail();
            } else {
                auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
                host->async_set_machine_subscribe_filter(targets, [weak_self](const json& response) {
                    auto self = weak_self.lock();
                    if (self) {
                        SSWCP_Instance::on_mqtt_msg_arrived(self, response);
                    }
                });
            }
        } else {
            handle_general_fail();
        }

    } catch (std::exception& e) {
        handle_general_fail();
    }
}
void SSWCP_MachineOption_Instance::sw_GetMachineObjects()
{
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);
            
        if (!host) {
            handle_general_fail(-1, "Can't find the active machine");
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_get_machine_objects([weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });

    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_PullCloudFile()
{
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail(-1, "Can't find the active machine");
            return;
        }

        json items = m_param_data;

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_pull_cloud_file(items, [weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_UpdateMachineFilamentInfo()
{
    try {
        if (!m_param_data.count("objects") || !m_param_data["objects"].is_array()) {
            handle_general_fail(-1, "param [objects] required or wrong type!");
            return;
        }

        update_filament_info(m_param_data["objects"], true);

    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_CancelPullCloudFile()
{
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail(-1, "Can't find the active machine");
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_cancel_pull_cloud_file([weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_StartCloudPrint()
{
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail(-1, "Can't find the active machine");
            return;
        }

        json items = m_param_data;

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_start_cloud_print(items, [weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_MachineFilesRoots()
{
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);
            
        if (!host) {
            handle_general_fail();
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_machine_files_roots([weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_MachineFilesMetadata() {
    try {
        if (m_param_data.count("filename")) {
            std::shared_ptr<PrintHost> host = nullptr;
            wxGetApp().get_connect_host(host);

            if (!host) {
                handle_general_fail();
                return;
            }

            std::string filename = m_param_data["filename"].get<std::string>();

            auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
            host->async_machine_files_metadata(filename, [weak_self](const json& response) {
                auto self = weak_self.lock();
                if (self) {
                    SSWCP_Instance::on_mqtt_msg_arrived(self, response); 
                }
            });
        } else {
            handle_general_fail();
        }
        
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_MachineFilesThumbnails()
{
    try {
        if (m_param_data.count("filename")) {
            std::shared_ptr<PrintHost> host = nullptr;
            wxGetApp().get_connect_host(host);

            if (!host) {
                handle_general_fail();
                return;
            }

            std::string filename = m_param_data["filename"].get<std::string>();

            auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
            host->async_machine_files_thumbnails(filename,
                                               [weak_self](const json& response) {
                auto self = weak_self.lock();
                if (self) {
                    SSWCP_Instance::on_mqtt_msg_arrived(self, response); 
                }
            });
        } else {
            handle_general_fail();
        }

    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_MachineFilesGetDirectory()
{
    try {
        if (m_param_data.count("path") && m_param_data.count("extended")) {
            std::shared_ptr<PrintHost> host = nullptr;
            wxGetApp().get_connect_host(host);

            if (!host) {
                handle_general_fail();
                return;
            }

            std::string path = m_param_data["path"].get<std::string>();
            bool        extend = m_param_data["extended"].get<bool>();

            auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
            host->async_machine_files_directory(path, extend, [weak_self](const json& response) {
                auto self = weak_self.lock();
                if (self) {
                    SSWCP_Instance::on_mqtt_msg_arrived(self, response); 
                }
            });
        } else {
            handle_general_fail();
        }

    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_ServerClientManagerSetUserinfo()
{
    try {
        if (!m_param_data.count("auther")) {
            handle_general_fail(-1, "param [auther] is required");
            return;
        }

        json auther = m_param_data["auther"];

        if(!auther.count("id") || !auther.count("nickname")){
            handle_general_fail(-1, "param [id] and [nickname] is required");
            return;
        }

        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail(-1, "can't find the active machine");
            return;
        }
        
        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_server_client_manager_set_userinfo(m_param_data,
                                            [weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response); 
            }
        });

    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_FinishPreprint()
{
    try {
        if (m_param_data.count("status")) {
            std::string status = m_param_data["status"].get<std::string>();

            auto p_dialog = dynamic_cast<WebPreprintDialog*>(wxGetApp().get_web_preprint_dialog());
            if (p_dialog) {
                if (status != "success") {
                    p_dialog->set_swtich_to_device(false);
                }
            }

            send_to_js();
            finish_job();
            return;
        }

        handle_general_fail();
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_GetPrintZip()
{
    try {
        auto oriname = SSWCP::get_active_filename();
        auto targetname = SSWCP::get_display_filename();


        std::weak_ptr<SSWCP_Instance> weak_self           = shared_from_this();
        if (m_work_thread.joinable())
            m_work_thread.join();

        m_work_thread       = std::thread([oriname, targetname, weak_self]() {
            auto self = weak_self.lock();
            if (self) {
                std::string zipname = generate_zip_path(oriname, targetname);
                json        res     = get_or_create_zip_json(oriname, targetname, zipname);
                wxGetApp().CallAfter([weak_self, res]() {
                    auto self = weak_self.lock();
                    if (self) {
                        self->m_res_data["name"]    = res["zip_name"];
                        self->m_res_data["content"] = res["zip_data"];

                        self->send_to_js();
                        self->finish_job();
                    }
                });
            }
        });

    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_GetPrintLegal()
{
    try {
        if (m_param_data.count("connected_model")) {
            std::string connected_model = m_param_data["connected_model"].get<std::string>();
            const auto& edit_preset     = wxGetApp().preset_bundle->printers.get_edited_preset();

            std::string local_name = "";
            if (edit_preset.is_system) {
                local_name = edit_preset.name;
            } else {
                const auto& base_preset = wxGetApp().preset_bundle->printers.get_preset_base(edit_preset);
                local_name              = base_preset->name;
            }
            local_name.erase(std::remove(local_name.begin(), local_name.end(), '('), local_name.end());
            local_name.erase(std::remove(local_name.begin(), local_name.end(), ')'), local_name.end());
            
            m_res_data["preset_model"] = local_name;
            m_res_data["legal"] = (local_name == connected_model);

            send_to_js();
            finish_job();
            return;
        }

        handle_general_fail();
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_UploadFiletoMachine() {
    try {
        if (!m_param_data.count("url")) {
            handle_general_fail();
            return;
        }

        std::string upload_url = std::string(wxString(m_param_data["url"].get<std::string>()).ToUTF8());

        wxString wildcard = "All files (*.*)|*.*";

        wxGetApp().CallAfter([wildcard, this, upload_url]() {
            // 创建选择文件对话框
            wxFileDialog picFileDialog(nullptr,
                                        L("select file"),                     // 标题
                                        "",                                 // 默认路径
                                        "",                                  // 默认文件名
                                        wildcard,                           // 文件类型过滤器
                                        wxFD_OPEN | wxFD_OVERWRITE_PROMPT); // 样式

            if (picFileDialog.ShowModal() == wxID_CANCEL) {
                // 用户取消上传
                handle_general_fail();
                return;
            }

            // 获取选择的保存路径
            wxString filepath     = picFileDialog.GetPath();
            wxString filename = picFileDialog.GetFilename();
            std::string tmp_url  = upload_url;

            auto final_url = Http::encode_url_path(tmp_url);

            Http http_object = Http::post(final_url);
            http_object
                .form_add("print", "false")
                .form_add_file("file", std::string(filepath.ToUTF8()), std::string(filename.ToUTF8()))
                .on_error([=](std::string body, std::string error, unsigned status) {
                    handle_general_fail();
                    wxGetApp().CallAfter([filename]() {
                        MessageDialog msg_window(nullptr, " " + filename + _L(" upload failed") + "\n", _L("UpLoad Failed"),
                                                 wxICON_QUESTION | wxOK);
                        msg_window.ShowModal();
                    });
                })
                .on_complete([=](std::string body, unsigned) {
                    try {
                        wxGetApp().CallAfter([=]() {
                            json response;
                            response["filename"] = std::string(filename.ToUTF8());
                            m_res_data           = response;
                            send_to_js();
                            

                            MessageDialog msg_window(nullptr, " " + filename + _L(" has already been uploaded") + "\n",
                                                     _L("UpLoad Successfully"), wxICON_QUESTION | wxOK);
                            msg_window.ShowModal();
                            finish_job();
                        });
                    } catch (std::exception& e) {
                        handle_general_fail();
                    }
                })
                .on_progress([&](Http::Progress progress, bool& cancel) {

                })
                .perform();
        });
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_DownloadMachineFile() {
    try {
        if (!m_param_data.count("url")) {
            handle_general_fail();
            return;
        }

        wxString download_url = wxString::FromUTF8(m_param_data["url"].get<std::string>());

        // 从 URL 获取默认文件名（如果没有提供）
        wxString filename   = "";
        if (!m_param_data.count("filename")) {
            size_t last_slash = download_url.find_last_of("/");
            if (last_slash != std::string::npos) {
                filename = download_url.substr(last_slash + 1);
            }
        } else {
            filename = wxString::FromUTF8(m_param_data["filename"].get<std::string>());
        }
        

        // 获取文件扩展名
        wxString extension;
        size_t      dot_pos = filename.find_last_of(".");
        if (dot_pos != std::string::npos) {
            extension = filename.substr(dot_pos + 1);
        }

        // 构建文件类型过滤器
        wxString wildcard;
        if (!extension.empty()) {
            // 例如: "PNG files (*.png)|*.png|All files (*.*)|*.*"
            wildcard = wxString::Format("%s files (*.%s)|*.%s|All files (*.*)|*.*", extension, extension, extension);
        } else {
            wildcard = "All files (*.*)|*.*";
        }

        wxGetApp().CallAfter([filename, extension, wildcard, this, download_url]() {
            // 创建保存文件对话框
            wxFileDialog saveFileDialog(nullptr,
                                        L("Save file"),                     // 标题
                                        "",                                 // 默认路径
                                        filename,                           // 默认文件名
                                        wildcard,                           // 文件类型过滤器
                                        wxFD_SAVE | wxFD_OVERWRITE_PROMPT); // 样式

            if (saveFileDialog.ShowModal() == wxID_CANCEL) {
                // 用户取消下载
                handle_general_fail();
                return;
            }

            // 获取选择的保存路径
            wxString path = saveFileDialog.GetPath();

            auto final_url = Http::encode_url_path(download_url.ToStdString(wxConvUTF8));
            

            Http http_object = Http::get(final_url);
            http_object
                .on_error([=](std::string body, std::string error, unsigned status) {
                    handle_general_fail();
                    wxGetApp().CallAfter([filename]() {
                        MessageDialog msg_window(nullptr, " " + filename + _L(" download failed") + "\n", _L("DownLoad Failed"),
                                                 wxICON_QUESTION | wxOK);
                        msg_window.ShowModal();
                    });
                })
                .on_complete([=](std::string body, unsigned) {
                    try {
                        boost::nowide::ofstream file(path.ToStdString(wxConvUTF8), std::ios::binary);
                        if (!file.is_open()) {
                            BOOST_LOG_TRIVIAL(error) << "Failed to open file for writing: " << path;
                            return;
                        }

                        file.write(body.c_str(), body.size());
                        file.close();

                        wxGetApp().CallAfter([=]() {
                            MessageDialog msg_window(nullptr, " " + filename + _L(" has already been downloaded") + "\n",
                                                     _L("DownLoad Successfully"), wxICON_QUESTION | wxOK);
                            msg_window.ShowModal();
                        });
                    } catch (std::exception& e) {
                        handle_general_fail();
                    }
                })
                .on_progress([&](Http::Progress progress, bool& cancel) {

                })
                .perform();
        });
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_FinishFilamentMapping()
{
    try {
        if (wxGetApp().get_web_preprint_dialog()) {
            WebPreprintDialog* dialog = dynamic_cast<WebPreprintDialog*>(wxGetApp().get_web_preprint_dialog());
            if (dialog) {
                if(dialog->is_finish()){
                    dialog->EndModal(wxID_OK);
                }else{
                    dialog->EndModal(wxID_CANCEL);
                }
            }
        }
    } catch (std::exception& e) {
        handle_general_fail();
    }
}
void SSWCP_MachineOption_Instance::sw_GetFileFilamentMapping()
{
    try {
        std::string filename = m_param_data.count("filename") ? m_param_data["filename"].get<std::string>() : "";

        if (filename == "") {
            filename = SSWCP::get_active_filename();
        }

        if (filename == "") {
            handle_general_fail();
            return;
        }

        json response = json::object();

        // 检查文件是否存在且可读
        if (!boost::filesystem::exists(filename) || !boost::filesystem::is_regular_file(filename)) {
            handle_general_fail();
            return;
        }

        auto& config = wxGetApp().plater()->get_partplate_list().get_curr_plate()->fff_print()->config();
        auto& result = *(wxGetApp().plater()->get_partplate_list().get_curr_plate()->get_slice_result());
        /*GCodeProcessor processor;
        processor.process_file(filename.data());
        auto& result = processor.result();
        auto& config = processor.current_dynamic_config();*/

        auto time = wxGetApp()
            .mainframe->plater()
            ->get_partplate_list()
            .get_curr_plate()
            ->get_slice_result()
            ->print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)]
            .time;
        response["estimated_time"] = time;

        auto color_to_int = [](const std::string& oriclr) -> long long {

            long long res = 0;
            if ((oriclr.size() != 7 && oriclr.size() != 9) || oriclr[0] != '#') {
                return -1;
            }

            if (oriclr.size() == 7) {
                for (int i = 1; i <= 6; ++i) {
                    if (oriclr[7 - i] - '0' >= 0 && oriclr[7 - i] - '0' <= 9) {
                        res += std::pow(16, i - 1) * (oriclr[7 - i] - '0');
                    } else {
                        res += std::pow(16, i - 1) * (oriclr[7 - i] - 'A' + 10);
                    }
                }
            } else {
                for (int i = 1; i <= 8; ++i) {
                    if (oriclr[7 - i] - '0' >= 0 && oriclr[7 - i] - '0' <= 9) {
                        res += std::pow(16, i - 1) * (oriclr[7 - i] - '0');
                    } else {
                        res += std::pow(16, i - 1) * (oriclr[7 - i] - 'A' + 10);
                    }
                }
            }

            

            return res;
        };

        // filament colour
        if (config.has("filament_colour")) {
            auto filament_color        = config.option<ConfigOptionStrings>("filament_colour")->values;

            std::vector<long long> number_res(filament_color.size(), 0);
            std::vector<std::string> str_res(filament_color.size());
            for (int i = 0; i < filament_color.size(); ++i) {
                number_res[i] = color_to_int(filament_color[i]);
                str_res[i]    = filament_color[i];
            }

            response["filament_color"] = number_res;
            response["filament_color_rgba"] = str_res;
        }
        

        // filament type 
        if (config.has("filament_type")) {
            auto filament_type        = config.option<ConfigOptionStrings>("filament_type")->values;
            response["filament_type"] = filament_type;
        }
        

        // filament used
        if (config.has("filament_density")) {
            auto filament_density = config.option<ConfigOptionFloats>("filament_density")->values;

            std::vector<double> filament_used_g(filament_density.size(), 0);
            double              total_weight = 0;
            for (const auto& pr : result.print_statistics.total_volumes_per_extruder) {
                filament_used_g[pr.first] = filament_density[pr.first] * pr.second * 0.001;
                total_weight += filament_used_g[pr.first];
            }

            response["filament_weight"] = filament_used_g;
            response["filament_weight_total"] = total_weight;
        }
        
        // filament extruder
        auto& filament_extruder_map = wxGetApp().app_config->get_filament_extruder_map_ref();
        if (!filament_extruder_map.empty()) {
            json object;
            for (const auto& item : filament_extruder_map) {
                object[std::to_string(item.first)] = std::to_string(item.second); 
            }
            response["filament_extruder_map"] = object;
        }

        // printer model
        auto current_preset = wxGetApp().preset_bundle->printers.get_edited_preset();
        std::string c_preset = "";
        if (current_preset.is_system) {
            c_preset = current_preset.name;
        } else {
            auto base_preset = wxGetApp().preset_bundle->printers.get_preset_base(current_preset);
            c_preset         = base_preset->name;
        }
        response["machine_model"] = c_preset;

        // file cover
        json thumbnails = json::array();
        int         thumbnail_count     = 0;
        if (config.has("thumbnails")) {
            std::string thumbnails_describe = config.option<ConfigOptionString>("thumbnails")->value;
            std::vector<std::pair<double, double>> thumbnails_size;

            std::vector<std::string> temp;
            do{
                size_t pos = thumbnails_describe.find(", ");
                std::string tmp = "";
                if (pos != std::string::npos) {
                    tmp     = thumbnails_describe.substr(0, pos);
                    thumbnails_describe = thumbnails_describe.substr(pos + 2);
                } else {
                    tmp = thumbnails_describe;
                    thumbnails_describe = "";
                }
                size_t end_tail_pos = tmp.find("/");
                if (end_tail_pos == std::string::npos) {
                    break;
                }

                tmp                    = tmp.substr(0, end_tail_pos);
                std::string str_width  = tmp.substr(0, tmp.find("x"));
                std::string str_height = tmp.substr(tmp.find("x") + 1);
                thumbnails_size.push_back({atof(str_width.c_str()), atof(str_height.c_str())});
                

            } while (thumbnails_describe != "");

            thumbnail_count = thumbnails_size.size();

            auto thumbnail_list = load_thumbnails(filename, thumbnail_count);
            for (int i = 0; i < thumbnail_list.size(); ++i) {
                json thumbnail        = json::object();
                auto thumbnail_string = "data:image/png;base64," + thumbnail_list[i];
                thumbnail["url"]      = thumbnail_string;
                thumbnail["width"]    = thumbnails_size[i].first;
                thumbnail["height"]   = thumbnails_size[i].second;

                thumbnails.push_back(thumbnail);
            }
        }
        
        response["thumbnails"] = thumbnails;

        
        
        // file name
        response["filename"] = SSWCP::get_display_filename();
        response["filepath"] = SSWCP::get_active_filename();

        

        m_res_data = response;
        send_to_js();
        finish_job();

    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_SetFilamentMappingComplete()
{
    try {
        if (!m_param_data.count("status")) {
            handle_general_fail();
        }

        std::string status = m_param_data["status"].get<std::string>();
        if (status == "success" || status == "canceled") {
            int flag = -1;
            if (status == "success") {
                flag = wxID_OK;
            }
            //// 耗材绑定成功
            //if (status == "success") {
            //    MessageDialog msg_window(nullptr, " " + _L("setting successfully, continue to print?") + "\n", _L("Print Job Setting"),
            //                             wxICON_QUESTION | wxOK | wxCANCEL);
            //    flag = msg_window.ShowModal();
            //} else {
            //    MessageDialog msg_window(nullptr, " " + _L("cancel the setting, continue to print?") + "\n", _L("Print Job Setting"),
            //                             wxICON_QUESTION | wxOK | wxCANCEL);
            //    flag = msg_window.ShowModal();
            //}
            
            WebPreprintDialog* dialog = dynamic_cast<WebPreprintDialog*>(wxGetApp().get_web_preprint_dialog());
            if (dialog) {
                if(flag == wxID_OK){
                    dialog->set_finish(true);
                }else{
                    dialog->set_finish(false);
                }
            }
            
        } else {
            MessageDialog msg_window(nullptr, " " + _L("setting failed") + "\n", _L("Print Job Setting"),
                                     wxICON_QUESTION | wxOK);
            msg_window.ShowModal();
        }

        send_to_js();
        finish_job();
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_CameraStartMonitor() {
    try {
        if (m_param_data.count("domain")) {
            std::shared_ptr<PrintHost> host = nullptr;
            wxGetApp().get_connect_host(host);

            if (!host) {
                handle_general_fail();
                return;
            }

            std::string domain = m_param_data["domain"].get<std::string>();

            int interval = m_param_data.count("interval") ? m_param_data["interval"].get<int>() : 2;

            bool expect_pw = m_param_data.count("expect_pw") ? m_param_data["expect_pw"].get<bool>() : false;

            auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
            host->async_camera_start(domain, interval, expect_pw, [weak_self](const json& response) {
                auto self = weak_self.lock();
                if (self) {
                    SSWCP_Instance::on_mqtt_msg_arrived(self, response); 
                }
            });
        } else {
            handle_general_fail();
        }

    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_DeleteMachineFile() {
    try {
        if(!m_param_data.count("path")){
            handle_general_fail(-1, "param [path] required!");
            return;
        }

        std::string path = m_param_data["path"].get<std::string>();

        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail(-1, "Connection lost!");
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_delete_machine_file(path, [weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });

    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_CameraStopMonitor() {
    try {
        if (m_param_data.count("domain")) {
            std::shared_ptr<PrintHost> host = nullptr;
            wxGetApp().get_connect_host(host);

            if (!host) {
                handle_general_fail();
                return;
            }

            std::string domain = m_param_data["domain"].get<std::string>();

            auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
            host->async_canmera_stop(domain, [weak_self](const json& response) {
                auto self = weak_self.lock();
                if (self) {
                    SSWCP_Instance::on_mqtt_msg_arrived(self, response); 
                }
            });
        } else {
            handle_general_fail();
        }

    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_SetDeviceName()
{
    try {
        if (m_param_data.count("name")) {
            std::string name = m_param_data["name"].get<std::string>();

            std::shared_ptr<PrintHost> host = nullptr;
            wxGetApp().get_connect_host(host);

            if (!host) {
                handle_general_fail(-1, "Connection lost!");
                return;
            }

            auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
            host->async_set_device_name(name, [weak_self](const json& response) {
                auto self = weak_self.lock();
                if (self) {
                    SSWCP_Instance::on_mqtt_msg_arrived(self, response);
                }
            });
        } else {
            handle_general_fail(-1, "param [name] required!");
        }
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_ControlLed()
{
    try {
        if (!m_param_data.count("name")) {
            handle_general_fail(-1, "param [name] required!");
            return;
        }

        if (!m_param_data.count("white")) {
            handle_general_fail(-1, "param [white] required!");
            return;
        }

        std::string name    = m_param_data["name"].get<std::string>();
        int white = m_param_data["white"].get<int>();

        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail(-1, "Connection lost!");
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_control_led(name, white, [weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_ControlPrintSpeed()
{
    try {
        if (!m_param_data.count("percentage")) {
            handle_general_fail(-1, "param [percentage] required!");
            return;
        }

        int percentage = m_param_data["percentage"].get<int>();

        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail(-1, "Connection lost!");
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_control_print_speed(percentage, [weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_BedMesh_AbortProbeMesh()
{
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail(-1, "Connection lost!");
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_bedmesh_abort_probe_mesh([weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_ControlPurifier()
{
    try {

        int fan_speed = m_param_data.count("fan_speed") ? m_param_data["fan_speed"].get<int>() : -1;
        int delay_time = m_param_data.count("delay_time") ? m_param_data["delay_time"].get<int>() : -1;
        int work_time  = m_param_data.count("work_time") ? m_param_data["work_time"].get<int>() : -1;
        

        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail(-1, "Connection lost!");
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_controlPurifier(fan_speed, delay_time, work_time, [weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_ControlMainFan()
{
    try {
        if (!m_param_data.count("speed")) {
            handle_general_fail(-1, "param [speed] required!");
            return;
        }

        int speed = m_param_data["speed"].get<int>();

        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail(-1, "Connection lost!");
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_control_main_fan(speed, [weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_ControlGenericFan()
{
    try {
        if (!m_param_data.count("name")) {
            handle_general_fail(-1, "param [fan_id] required!");
            return;
        }

        if (!m_param_data.count("speed")) {
            handle_general_fail(-1, "param [speed] required!");
            return;
        }

        std::string name = m_param_data["name"].get<std::string>();
        int speed  = m_param_data["speed"].get<int>();

        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail(-1, "Connection lost!");
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_control_generic_fan(name, speed, [weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });
    } catch (std::exception& e) {
        handle_general_fail();
    }
}


void SSWCP_MachineOption_Instance::sw_ControlBedTemp()
{
    try {
        if (!m_param_data.count("temp")) {
            handle_general_fail(-1, "param [temp] required!");
            return;
        }

        int temp = m_param_data["temp"].get<int>();

        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail(-1, "Connection lost!");
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_control_bed_temp(temp, [weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_ControlExtruderTemp()
{
    try {
        if (!m_param_data.count("temp")) {
            handle_general_fail(-1, "param [temp] required!");
            return;
        }

        int index = m_param_data.count("index") ? m_param_data["index"].get<int>() : -1;
        int map   = m_param_data.count("map") ? m_param_data["map"].get<int>() : -1;

        int temp = m_param_data["temp"].get<int>();

        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail(-1, "Connection lost!");
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_control_extruder_temp(temp, index, map, [weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_FilesThumbnailsBase64()
{
    try {
        if (!m_param_data.count("path")) {
            handle_general_fail(-1, "param [path] required");
            return;
        }

        std::string path = m_param_data["path"].get<std::string>();

        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail(-1, "Connection lost!");
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_files_thumbnails_base64(path, [weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });

    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_UploadCameraTimelapse()
{
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail(-1, "Connection lost!");
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_upload_camera_timelapse(m_param_data, [weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_GetTimelapseInstance()
{
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail(-1, "Connection lost!");
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_get_timelapse_instance(m_param_data, [weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_DeleteCameraTimelapse()
{
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail(-1, "Connection lost!");
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_delete_camera_timelapse(m_param_data, [weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_DefectDetactionConfig()
{ 
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail(-1, "Connection lost!");
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_defect_detaction_config(m_param_data, [weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_GetFileListPage()
{
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail(-1, "Connection lost!");
            return;
        }

        if (!m_param_data.count("root") || !m_param_data["root"].is_string()) {
            handle_general_fail(-1, "param [root] required or wrong type!");
            return;
        }

        if (!m_param_data.count("files_per_page") || !m_param_data["files_per_page"].is_number()) {
            handle_general_fail(-1, "param [files_per_page] required or wrong type");
            return;
        }

        if (!m_param_data.count("page_number") || !m_param_data["page_number"].is_number()) {
            handle_general_fail(-1, "param [page_number] required or wrong type");
            return;
        }

        std::string root = m_param_data["root"].get<std::string>();
        int files_per_page = m_param_data["files_per_page"].get<int>();
        int         page_number    = m_param_data["page_number"].get<int>();

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_get_file_page_list(root, files_per_page, page_number, [weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });

    } catch (std::exception& e) {
        handle_general_fail();
    }
}


void SSWCP_MachineOption_Instance::sw_exception_query()
{
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail(-1, "Connection lost!");
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_exception_query([weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });

    } catch (std::exception& e) {
        handle_general_fail();
    }
}


// SSWCP_MachineConnect_Instance
void SSWCP_MachineConnect_Instance::process() {
    if (m_event_id != "") {
        json header;
        send_to_js();

        m_header.clear();
        m_header["event_id"] = m_event_id;
    }
    if (m_cmd == "sw_Test_connect") {
        sw_test_connect();
    } else if (m_cmd == "sw_Connect") {
        sw_connect();
    } else if (m_cmd == "sw_Disconnect") {
        sw_disconnect();
    } else if (m_cmd == "sw_GetConnectedMachine") {
        sw_get_connect_machine();
    } else if (m_cmd == "sw_ConnectOtherMachine"){
        sw_connect_other_device();
    } else if (m_cmd == "sw_GetPincode") {
        sw_get_pin_code();
    }
    else {
        handle_general_fail();
    }
}

void SSWCP_MachineConnect_Instance::sw_get_pin_code()
{
    try {
        if (m_param_data.count("ip") && m_param_data.count("userid") && m_param_data.count("nickname")) {
            std::string ip = m_param_data["ip"].get<std::string>();
            std::string userid    = m_param_data["userid"].get<std::string>();
            std::string nickname  = m_param_data["nickname"].get<std::string>();
            int  port      = m_param_data.count("port") ? m_param_data["port"].get<int>() : 1884;

            auto        weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
            wxGetApp().CallAfter([=]() {
                MqttClient* mqtt_client = new MqttClient("mqtt://" + ip + ":" + std::to_string(port), "Snapmaker Orca");
                std::string connect_msg = "";
                if (mqtt_client->Connect(connect_msg)) {
                    std::string sub_msg = "success";
                    if (mqtt_client->Subscribe("cloud/config/response", 1, sub_msg)) {
                        mqtt_client->SetMessageCallback([weak_self, mqtt_client](const std::string& topic, const std::string& message) {
                            auto self = weak_self.lock();
                            if (self) {
                                if (topic == "cloud/config/response") {
                                    json response = json::parse(message);
                                    if (response.count("result")) {
                                        self->m_res_data = response["result"];
                                        self->send_to_js();
                                        self->finish_job();

                                        std::string dc_msg = "success";
                                        bool flag = mqtt_client->Disconnect(dc_msg);
                                        wxGetApp().CallAfter([mqtt_client]() { delete mqtt_client; });
                                        return;
                                    }

                                    self->handle_general_fail();
                                }


                            }
                        });

                        // 构建请求消息
                        json req_body;
                        req_body["jsonrpc"] = "2.0",
                        req_body["method"]  = "server.client_manager.request_pin_code";
                        req_body["params"] = json::object();
                        req_body["params"]["userid"] = userid;
                        req_body["params"]["nickname"] = nickname;
                        Moonraker_Mqtt::SequenceGenerator generator;
                        req_body["id"]                 = generator.generate_seq_id();

                        // 发送请求
                        std::string pub_msg = "success";
                        if (mqtt_client->Publish("cloud/config/request", req_body.dump(), 1, pub_msg)) {
                            return;
                        }
                    }
                    return;
                }
                auto self = weak_self.lock();
                if (self) {
                    self->handle_general_fail();
                }
            });
        } else {
            handle_general_fail();
        }
    } catch (std::exception& e) {
        handle_general_fail();
    }
}


void SSWCP_MachineConnect_Instance::sw_connect_other_device() {
    try {
        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        wxGetApp().CallAfter([weak_self](){
            
            auto config = &wxGetApp().preset_bundle->printers.get_edited_preset().config;
            config->set("print_host", "");
            config->set("printhost_apikey", "");

            auto dialog = SMPhysicalPrinterDialog(wxGetApp().mainframe->plater()->GetParent()); //  todo
            int res = dialog.ShowModal();

            dialog.EndModal(1);

            if (dialog.m_connected) {
                auto device_dialog = wxGetApp().get_web_device_dialog();
                if (device_dialog) {
                    device_dialog->EndModal(1);
                }
            } else {
                auto self = weak_self.lock();
                if (self) {
                    self->handle_general_fail();
                }
            }
            
            
        });
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}



void SSWCP_MachineConnect_Instance::sw_test_connect() {
    try {
        if (m_param_data.count("ip")) {
            std::string protocol = "moonraker";
            
            if (m_param_data.count("protcol")) {
                protocol = m_param_data["protocol"].get<std::string>();
            }
                
            std::string ip       = m_param_data["ip"].get<std::string>();
            int         port     = -1;

            if (m_param_data.count("port") && m_param_data["port"].is_number_integer()) {
                port = m_param_data["port"].get<int>();
            }

            auto p_config = &(wxGetApp().preset_bundle->printers.get_edited_preset().config);


            PrintHostType type = PrintHostType::htMoonRaker;
            // todo : 增加输入与type的映射
            
            p_config->option<ConfigOptionEnum<PrintHostType>>("host_type")->value = type;
            
            p_config->set("print_host", ip + (port == -1 ? "" : std::to_string(port)));

            std::shared_ptr<PrintHost> host(PrintHost::get_print_host(&wxGetApp().preset_bundle->printers.get_edited_preset().config));

            if (!host) {
                // 错误处理
                finish_job();
            } else {
                if (m_work_thread.joinable())
                    m_work_thread.join();
                m_work_thread = std::thread([this, host] {
                    wxString msg;
                    bool        res = host->test(msg);
                    if (res) {
                        send_to_js();
                    } else {
                        // 错误处理
                        m_status = 1;
                        m_msg    = msg.c_str();
                        send_to_js();
                    }

                    finish_job();
                });
            }
        } else {
            // 错误处理
            finish_job();
        }
    } catch (const std::exception&) {
        handle_general_fail();
    }
}

void SSWCP_MachineConnect_Instance::sw_connect() {
    
}

void SSWCP_MachineConnect_Instance::sw_get_connect_machine() {
    try {
        auto devices = wxGetApp().app_config->get_devices();
        for (const auto& device : devices) {
            if (device.connected) {
                m_res_data = device;
                break;
            }
        }

        send_to_js();
        finish_job();
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineConnect_Instance::sw_disconnect() {
    bool need_reload = m_param_data.count("need_reload") ? m_param_data["need_reload"].get<bool>() : true;

    std::string dev_id = m_param_data.count("dev_id") ? m_param_data["dev_id"] : "";

    auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());

    if (m_work_thread.joinable())
        m_work_thread.join();

    m_work_thread = std::thread([weak_self, need_reload, dev_id](){
        auto                       self = weak_self.lock();

        if (dev_id != "") {
            DeviceInfo info;
            if (wxGetApp().app_config->get_device_info(dev_id, info)) {
                if (info.connected == false) {
                    if (self) {
                        self->handle_general_fail(-3, dev_id + " is not connected before disconnect!");
                        return;
                    }
                }
            } else {
                if (self) {
                    self->handle_general_fail(-1, dev_id + " is not exist!");
                    return;
                }
            }
        }
        

        

        bool res = wxGetApp().sm_disconnect_current_machine(need_reload);
        m_first_connected = true;
        if (!res) {
            if (self) {
                self->m_status = 1;
                self->m_msg    = "disconnected failed";
            }
        }

       

        wxGetApp().CallAfter([]() {

            wxGetApp().app_config->clear_filament_extruder_map();
            wxGetApp().preset_bundle->machine_filaments.clear();
            wxGetApp().load_current_presets();
        });

        if (self) {
            self->send_to_js();
            self->finish_job();
        }
    });
}

// SSWCP_SliceProject_Instance
// Process machine find commands
void SSWCP_SliceProject_Instance::process()
{
    if (m_event_id != "") {
        json header;
        send_to_js();

        m_header.clear();
        m_header["event_id"] = m_event_id;
    }

    if (m_cmd == "sw_NewProject") {
        sw_NewProject();
    } else if (m_cmd == "sw_OpenProject") {
        sw_OpenProject();
    } else if (m_cmd == "sw_GetRecentProjects") {
        sw_GetRecentProjects();
    } else if (m_cmd == "sw_OpenRecentFile") {
        sw_OpenRecentFile();
    } else if (m_cmd == "sw_DeleteRecentFiles") {
        sw_DeleteRecentFiles();
    } else if (m_cmd == "sw_SubscribeRecentFiles") {
        sw_SubscribeRecentFiles();
    } else {
        handle_general_fail();
    }
}

void SSWCP_SliceProject_Instance::sw_NewProject()
{
    try { 
        if (!m_param_data.count("preset_name") || m_param_data["preset_name"].get<std::string>() == "")
            wxGetApp().request_open_project("<new>");
        else {
            std::string preset_name = m_param_data["preset_name"].get<std::string>();
            wxGetApp().CallAfter([this, preset_name]() {
                try {
                    if (wxGetApp().get_tab(Preset::TYPE_PRINTER)->select_preset(preset_name)) {
                        wxGetApp().plater()->new_project();
                    } else {
                        MessageDialog msg_window(nullptr, "The machine model has not been installed!", _L("Create Failed"),
                                                 wxICON_QUESTION | wxOK);
                        msg_window.ShowModal();
                    }
                    
                } catch (std::exception& e) {
                    // 异常处理
                }
            });
        }
        send_to_js();
        finish_job();
    }
    catch (std::exception& e) {
        handle_general_fail();
    }  
}

void SSWCP_SliceProject_Instance::sw_OpenProject()
{
    try {
        wxGetApp().request_open_project({});
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_SliceProject_Instance::sw_GetRecentProjects()
{
    try {
        
        json data;
        wxGetApp().mainframe->get_recent_projects(data, INT_MAX);

        m_res_data = data;

        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_SliceProject_Instance::sw_OpenRecentFile()
{
    try {
        if (m_param_data.count("path")) {
            std::string path = m_param_data["path"].get<std::string>();
            if (path != "") {
                wxGetApp().request_open_project(path);
            } else {
                handle_general_fail();
                return;
            }
        } else {
            handle_general_fail();
            return;
        }
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_SliceProject_Instance::sw_DeleteRecentFiles()
{
    try {
        if (m_param_data.count("paths") && m_param_data["paths"].is_array()) {
            auto paths = m_param_data["paths"];
            send_to_js();
            if (paths.size() == 0) {
                wxGetApp().sm_request_remove_project("");
            } else {
                for (size_t i = 0; i < paths.size(); ++i) {
                    wxGetApp().sm_request_remove_project(paths[i].get<std::string>());
                }
            }
            
        } else {
            handle_general_fail();
            return;
        }
        
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_SliceProject_Instance::sw_SubscribeRecentFiles()
{
    try {
        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        wxGetApp().m_recent_file_subscribers[m_webview] = weak_self;
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

// SSWCP_UserLogin_Instance
void SSWCP_UserLogin_Instance::process()
{
    if (m_event_id != "") {
        json header;
        send_to_js();

        m_header.clear();
        m_header["event_id"] = m_event_id;
    }

    if (m_cmd == "sw_UserLogin") {
        sw_UserLogin();
    } else if (m_cmd == "sw_UserLogout") {
        sw_UserLogout();
    } else if (m_cmd == "sw_GetUserLoginState") {
        sw_GetUserLoginState();
    } else if (m_cmd == "sw_SubscribeUserLoginState") {
        sw_SubscribeUserLoginState();
    } else {
        handle_general_fail();
    }
}
void SSWCP_UserLogin_Instance::sw_UserLogin()
{
    try {
        send_to_js();

        finish_job();

        bool show = m_param_data.count("show") ? m_param_data["show"].get<bool>() : true;

        wxGetApp().CallAfter([show]() {
            wxGetApp().sm_request_login(show);
        });
        
        
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_UserLogin_Instance::sw_UserLogout()
{
    try {
        send_to_js();
        wxGetApp().sm_request_user_logout();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_UserLogin_Instance::sw_GetUserLoginState()
{
    try {
        json data;
        auto pInfo = wxGetApp().sm_get_userinfo();
        if (pInfo) {
            bool islogin = pInfo->is_user_login();
            if (islogin) {
                data["status"] = "online";
                data["nickname"] = pInfo->get_user_name();
                data["icon"]     = pInfo->get_user_icon_url();
                data["token"]    = pInfo->get_user_token();
                data["userid"]   = pInfo->get_user_id();
                data["account"]  = pInfo->get_user_account();
            } else {
                data["status"] = "offline";
            }

            m_res_data = data;
            send_to_js();
            finish_job();
        } else {
            handle_general_fail();
        }
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_UserLogin_Instance::sw_SubscribeUserLoginState()
{
    try {
        std::weak_ptr<SSWCP_Instance> weak_ptr = shared_from_this();
        wxGetApp().m_user_login_subscribers[m_webview]  = weak_ptr;
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

// SSWCP_MachineManage_Instance
void SSWCP_MachineManage_Instance::process()
{
    if (m_event_id != "") {
        json header;
        send_to_js();

        m_header.clear();
        m_header["event_id"] = m_event_id;
    }

    if (m_cmd == "sw_GetLocalDevices") {
        sw_GetLocalDevices();
    } else if (m_cmd == "sw_AddDevice") {
        sw_AddDevice();
    } else if (m_cmd == "sw_SubscribeLocalDevices") {
        sw_SubscribeLocalDevices();
    } else if (m_cmd == "sw_RenameDevice") {
        sw_RenameDevice();  
    } else if (m_cmd == "sw_SwitchModel") {
        sw_SwitchModel();
    } else if (m_cmd == "sw_DeleteDevices") {
        sw_DeleteDevices();
    } else {
        handle_general_fail();
    }
}

void SSWCP_MachineManage_Instance::sw_GetLocalDevices()
{
    try {
        auto devices = wxGetApp().app_config->get_devices();
        m_res_data = devices;

        send_to_js();
        finish_job();
    }
    catch (std::exception& e)
    {
        handle_general_fail();
    }
}

void SSWCP_MachineManage_Instance::sw_SubscribeLocalDevices()
{
    try {
        auto self = shared_from_this();
        wxGetApp().m_device_card_subscribers[m_webview] = self;
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

// SSWCP_PageStateChange_Instance
void SSWCP_PageStateChange_Instance::process()
{
    if (m_event_id != "") {
        json header;
        send_to_js();

        m_header.clear();
        m_header["event_id"] = m_event_id;
    }

    if (m_cmd == "sw_SubscribePageStateChange") {
        sw_SubscribePageStateChange();
    } else if (m_cmd == "sw_UnsubscribePageStateChange") {
        sw_UnsubscribePageStateChange();
    } else {
        handle_general_fail();
    }
}

void SSWCP_PageStateChange_Instance::sw_SubscribePageStateChange()
{
    try {
        auto self = shared_from_this();
        wxGetApp().m_page_state_subscribers[m_webview] = self;
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_PageStateChange_Instance::sw_UnsubscribePageStateChange()
{
    try {
        auto& page_state_map = wxGetApp().m_page_state_subscribers;
        std::string event_id = m_param_data.count("event_id") ? m_param_data["event_id"].get<std::string>() : "";

        for (auto iter = page_state_map.begin(); iter != page_state_map.end();) {
            if (iter->first == m_webview) {
                auto ptr = iter->second.lock();
                if (ptr) {
                    if (event_id == "" || (event_id != "" && event_id == ptr->m_event_id)) {
                        iter = page_state_map.erase(iter);
                    } else {
                        iter++;
                    }
                } else {
                    iter = page_state_map.erase(iter);
                }
            } else {
                iter++;
            }
        }

        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineManage_Instance::sw_AddDevice()
{
    try {
        wxGetApp().CallAfter([] {
            if (wxGetApp().web_device_dialog)
                delete wxGetApp().web_device_dialog;

            wxGetApp().web_device_dialog = new WebDeviceDialog;
            wxGetApp().web_device_dialog->run();
        });
        send_to_js();
        
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineManage_Instance::sw_RenameDevice()
{
    try {
        if (m_param_data.count("dev_id") && m_param_data.count("dev_name")) {
            std::string dev_id = m_param_data["dev_id"].get<std::string>();
            std::string dev_name = m_param_data["dev_name"].get<std::string>();

            DeviceInfo info;
            wxGetApp().app_config->get_device_info(dev_id, info);
            info.dev_name = dev_name;
            wxGetApp().app_config->save_device_info(info);

            wxGetApp().CallAfter([] {

                // wcp订阅
                json data = wxGetApp().app_config->get_devices();
                wxGetApp().device_card_notify(data);
            });

            send_to_js();
            finish_job();
        } else {
            handle_general_fail();
        }
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineManage_Instance::sw_DeleteDevices()
{
    try {
        if (m_param_data.count("dev_ids") && m_param_data["dev_ids"].is_array()) {
            auto ids = m_param_data["dev_ids"];
            if (ids.size() == 0) {
                auto devices = wxGetApp().app_config->get_devices();
                for (size_t i = 0; i < devices.size(); ++i) {
                    if (devices[i].connected) {
                        std::shared_ptr<PrintHost> current_host;
                        wxGetApp().get_connect_host(current_host);
                        if (current_host && current_host->get_sn() == devices[i].sn) {
                            wxString msg = "";
                            json     param;
                            current_host->disconnect(msg, param);
                        }
                    }
                }
                wxGetApp().app_config->clear_device_info();
            } else {
                for (size_t i = 0; i < ids.size(); ++i) {
                    std::string dev_id = ids[i].get<std::string>();

                    DeviceInfo info;
                    if (wxGetApp().app_config->get_device_info(dev_id, info)) {
                        if (info.connected) {
                            std::shared_ptr<PrintHost> current_host;
                            wxGetApp().get_connect_host(current_host);
                            wxString msg = "";
                            json     param;
                            current_host->disconnect(msg, param);
                        }
                    }
                    wxGetApp().app_config->remove_device_info(dev_id);

                }
            }

            wxGetApp().CallAfter([] {
                // wcp订阅
                json data = wxGetApp().app_config->get_devices();
                wxGetApp().device_card_notify(data);
            });

            send_to_js();
            finish_job();
        } else {
            handle_general_fail();
        }
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineManage_Instance::sw_SwitchModel()
{
    try {
        if (m_param_data.count("dev_id")) {
            std::string dev_id = m_param_data["dev_id"].get<std::string>();
            send_to_js();
            wxGetApp().CallAfter([dev_id]() {
                WebPresetDialog dialog(&wxGetApp());
                dialog.m_device_id = dev_id;
                dialog.run();
            });
            finish_job();
            
        } else {
            handle_general_fail();
        }
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

// SSWCP_MqttAgent_Instance

std::unordered_map<wxWebView*, std::pair<std::string, std::shared_ptr<MqttClient>>> SSWCP_MqttAgent_Instance::m_mqtt_engine_map;
std::mutex                                          SSWCP_MqttAgent_Instance::m_engine_map_mtx;
std::map<std::pair<std::string, wxWebView*>, std::string>   SSWCP_MqttAgent_Instance::m_subscribe_map;
std::map<std::pair<std::string, wxWebView*>, std::weak_ptr<SSWCP_Instance>> SSWCP_MqttAgent_Instance::m_subscribe_instance_map;
WebPresetDialog*                                                                    SSWCP_MqttAgent_Instance::m_dialog = nullptr;

void SSWCP_MqttAgent_Instance::process()
{
    if (m_cmd == "sw_create_mqtt_client") {
        sw_create_mqtt_client();
    } else if (m_cmd == "sw_mqtt_connect") {
        sw_mqtt_connect();
    } else if (m_cmd == "sw_mqtt_disconnect") {
        sw_mqtt_disconnect();
    } else if (m_cmd == "sw_mqtt_subscribe") {
        sw_mqtt_subscribe();
    } else if (m_cmd == "sw_mqtt_unsubscribe") {
        sw_mqtt_unsubscribe();
    } else if (m_cmd == "sw_mqtt_publish") {
        sw_mqtt_publish();
    } else if (m_cmd == "sw_mqtt_set_engine") {
        sw_mqtt_set_engine();
    } else {
        handle_general_fail();
    }
}

// 校验mqtt引擎id
bool SSWCP_MqttAgent_Instance::validate_id(const std::string& id)
{
    bool flag = true;

    m_engine_map_mtx.lock();
    if (!m_mqtt_engine_map.count(m_webview)) {
        flag = false;
    } else {
        flag = m_mqtt_engine_map[m_webview].first == id;
    }
    
    m_engine_map_mtx.unlock();

    return flag;
}

// webview析构回调
void SSWCP_MqttAgent_Instance::set_Instance_illegal()
{
    SSWCP_Instance::set_Instance_illegal();

    clean_current_engine();
}

// 清空当前mqtt实例
void SSWCP_MqttAgent_Instance::clean_current_engine()
{
    //清除该实例的所有订阅
    for (auto iter = m_subscribe_map.begin(); iter != m_subscribe_map.end();) {
        if (iter->first.second == m_webview) {
            iter = m_subscribe_map.erase(iter);
        } else {
            iter++;
        }
    }

    for (auto iter = m_subscribe_instance_map.begin(); iter != m_subscribe_instance_map.end();) {
        if (iter->first.second == m_webview) {
            iter = m_subscribe_instance_map.erase(iter);
        } else {
            iter++;
        }
    }

    m_engine_map_mtx.lock();
    m_mqtt_engine_map.erase(m_webview);
    m_engine_map_mtx.unlock();
}

// mqtt静态消息回调
void SSWCP_MqttAgent_Instance::mqtt_msg_cb(const std::string& topic, const std::string& payload, void* client)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Mqtt_Agent] 收到MQTT消息，主题: " << topic << ", 载荷长度: " << payload.length();
    wcp_loger.add_log("收到MQTTS消息，主题: " + topic + ", 载荷长度: " + std::to_string(payload.length()), false, "", "Mqtt_Agent",
                      "info");
    try {
        wxGetApp().CallAfter([topic, payload, client]() {
            for (const auto& item : SSWCP_MqttAgent_Instance::m_subscribe_map) {

                std::string id_topic = item.second;
                std::string target_topic = topic;
                if (id_topic.find("+") != std::string::npos) {
                    id_topic = id_topic.substr(id_topic.find_first_of("/"));
                    target_topic = topic.substr(topic.find_first_of("/"));

                }

                if (id_topic == target_topic) {
                    if (SSWCP_MqttAgent_Instance::m_subscribe_instance_map.count(item.first)) {
                        auto& instance                = SSWCP_MqttAgent_Instance::m_subscribe_instance_map[item.first];
                        if (auto self = instance.lock()) {
                            auto mqtt_self            = dynamic_pointer_cast<SSWCP_MqttAgent_Instance>(self);

                            if (mqtt_self && (void*)(mqtt_self->get_current_engine().get()) == client) {
                                self->m_res_data["topic"] = topic;
                                self->m_res_data["data"]  = payload;
                                self->send_to_js();
                            }
                        }
                        
                    } else {
                        return;
                    }
                }
            }
        });

    } catch (std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 处理MQTT消息异常: " << e.what();
        wcp_loger.add_log("处理MQTTS消息异常: " + std::string(e.what()), false, "", "Moonraker_Mqtt", "error");
    }
}

// 创建mqtt实例
void SSWCP_MqttAgent_Instance::sw_create_mqtt_client()
{
    try {
        // 解析参数
        std::string server_address = "";
        std::string clientId       = "";
        std::string ca             = "";
        std::string cert           = "";
        std::string key            = "";
        std::string username       = "";
        std::string password       = "";
        bool        clean_session  = false;

        if (m_param_data.count("server_address") || !m_param_data["server_address"].is_string()) {
            server_address = m_param_data["server_address"].get<std::string>();
            if (server_address == "") {
                handle_general_fail(-1, "the value of param [server_address] is illegal");
                return;
            }
        }
        else {
            handle_general_fail(-1, "param [server_address] is required or wrong type");
            return;
        }

        if (m_param_data.count("clientId") || !m_param_data["clientId"].is_string()) {
            clientId = m_param_data["clientId"].get<std::string>();
            if (clientId == "") {
                handle_general_fail(-1, "the value of param [clientId] is illegal");
                return;
            }
        } else {
            handle_general_fail(-1, "param [clientId] is required or wroing type");
            return;
        }

        ca = m_param_data.count("ca") ? m_param_data["ca"].get<std::string>() : "";
        cert = m_param_data.count("cert") ? m_param_data["cert"].get<std::string>() : "";
        key  = m_param_data.count("key") ? m_param_data["key"].get<std::string>() : "";

        clean_session = m_param_data.count("clean_session") ? m_param_data["clean_session"].get<bool>() : false;

        username = m_param_data.count("username") ? m_param_data["username"].get<std::string>() : "";
        password = m_param_data.count("password") ? m_param_data["password"].get<std::string>() : "";


        // 确认mqtt连接类型，并创建实例
        std::shared_ptr<MqttClient> client = nullptr;
        std::string type = "mqtt";
        if (ca != "" && cert != "" && key != "") {
            type = "mqtts";
            client.reset(new MqttClient(server_address, clientId, ca, cert, key, username, password, clean_session));
        }else{
            client.reset(new MqttClient(server_address, clientId, username, password, clean_session));
        }

        if (client == nullptr) {
            // 创建失败
            handle_general_fail(-1, "create instance failed");
            return;
        }

        // 清空当前m_clinet的订阅列表
        auto ptr = get_current_engine();
        if (!ptr) {
            ptr.reset();
        }
        clean_current_engine();

        // 替换新引擎
        bool flag = set_current_engine({std::to_string(int64_t(client.get())), client});
        if (!flag) {
            handle_general_fail(-1, "create failed");
            return;
        }
        
        // 绑定静态回调 
        client->SetMessageCallback(SSWCP_MqttAgent_Instance::mqtt_msg_cb);

        m_res_data["type"] = type;
        m_res_data["id"]   = std::to_string(int64_t(get_current_engine().get()));

        send_to_js();
        finish_job();

    } catch (std::exception& e) {
        handle_general_fail();
    }
}

// mqtt引擎建立连接
void SSWCP_MqttAgent_Instance::sw_mqtt_connect()
{
    try {
        if (!m_param_data.count("id") || !m_param_data["id"].is_string()) {
            handle_general_fail(-1, "param [id] is required or wrong type");

            Slic3r::sentryReportLog(Slic3r::SENTRY_LOG_ERROR, std::string("device_connect param [id] is required or wrong type"), DEVICE_CONNECT_ERR);
            return;
        }

        std::string id = m_param_data["id"].get<std::string>();

        if (!validate_id(id)) {
            handle_general_fail(-1, "id is illegal");
            Slic3r::sentryReportLog(Slic3r::SENTRY_LOG_ERROR, std::string("device_connect id is illegal"),DEVICE_CONNECT_ERR);
            return;
        }

        std::weak_ptr<SSWCP_Instance> weak_ptr = shared_from_this();
        auto                          engine   = get_current_engine();

        if (m_work_thread.joinable())
            m_work_thread.join();

        m_work_thread = std::thread([weak_ptr, engine]() {
            if (!weak_ptr.lock()) {
                return;
            }
            auto self = std::dynamic_pointer_cast<SSWCP_MqttAgent_Instance>(weak_ptr.lock());

            engine->SetConnectionFailureCallback([engine]() {
                std::string msg = "";
                engine->Disconnect(msg);
            });

            std::string msg;
            bool flag = engine->Connect(msg);

            wxGetApp().CallAfter([weak_ptr, msg, flag]() {
                auto self = weak_ptr.lock();
                if (self) {
                    if (flag) {
                        self->m_msg = msg;
                        self->send_to_js();
                        self->finish_job();
                    } else {
                        self->handle_general_fail(-1, msg);
                    }
                }
            });
        });
        

    } catch (std::exception& e) {
        handle_general_fail();
    }
}

// mqtt引擎断开连接
void SSWCP_MqttAgent_Instance::sw_mqtt_disconnect()
{
    try {
        if (!m_param_data.count("id") || !m_param_data["id"].is_string()) {
            handle_general_fail(-1, "param [id] is required or wrong type");
            return;
        }

        std::string id = m_param_data["id"].get<std::string>();

        if (!validate_id(id)) {
            handle_general_fail(-1, "id is illegal");
            return;
        }

        std::weak_ptr<SSWCP_Instance> weak_ptr = shared_from_this();
        auto                          engine   = get_current_engine();

        if (m_work_thread.joinable())
            m_work_thread.join();

        m_work_thread                          = std::thread([weak_ptr, engine]() {
            if (!weak_ptr.lock()) {
                return;
            }
            auto self = std::dynamic_pointer_cast<SSWCP_MqttAgent_Instance>(weak_ptr.lock());

            std::string msg  = "success";
            bool        flag = engine->Disconnect(msg);

            wxGetApp().CallAfter([weak_ptr, msg, flag]() {
                auto self = weak_ptr.lock();
                if (self) {
                    if (flag) {
                        self->m_msg = msg;
                        self->send_to_js();
                        self->finish_job();
                    } else {
                        self->handle_general_fail(-1, msg);
                    }
                }
            });
        });
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

// 订阅topic
void SSWCP_MqttAgent_Instance::sw_mqtt_subscribe()
{
    try {
        if (!m_param_data.count("id") || !m_param_data["id"].is_string()) {
            handle_general_fail(-1, "param [id] is required or wrong type");
            Slic3r::sentryReportLog(Slic3r::SENTRY_LOG_ERROR, std::string("device_subscribe param [id] is required or wrong type"), DEVICE_SUBSCRIBE_ERR);
            return;
        }

        std::string id = m_param_data["id"].get<std::string>();

        if (!validate_id(id)) {
            handle_general_fail(-1, "id is illegal");
            Slic3r::sentryReportLog(Slic3r::SENTRY_LOG_ERROR, std::string("device_subscribe id is illegal with:")+id,DEVICE_SUBSCRIBE_ERR);
            return;
        }

        if (m_event_id == "") {
            handle_general_fail(-1, "event_id is required or wrong type");
            Slic3r::sentryReportLog(Slic3r::SENTRY_LOG_ERROR, std::string("device_subscribe event_id is required or wrong type with:") + id,DEVICE_SUBSCRIBE_ERR);
            return;
        }

        std::string event_id = m_event_id;

        if (!m_param_data.count("topic") || !m_param_data["topic"].is_string()) {
            handle_general_fail(-1, "param [topic] is required or wrong type");
            Slic3r::sentryReportLog(Slic3r::SENTRY_LOG_ERROR, std::string("device_subscribe param [topic] is required or wrong type"),DEVICE_SUBSCRIBE_ERR);
            return;
        }
        std::string topic = m_param_data["topic"].get<std::string>();


        m_subscribe_map[{event_id, m_webview}] = topic;
        m_subscribe_instance_map[{event_id, m_webview}] = shared_from_this();

        if (!m_param_data.count("qos") || !m_param_data["qos"].is_number()) {
            handle_general_fail(-1, "param [qos] is required or wrong type");
            Slic3r::sentryReportLog(Slic3r::SENTRY_LOG_ERROR, std::string("device_subscribe param [qos] is required or wrong type"), DEVICE_SUBSCRIBE_ERR);
            return;
        }
        int qos = m_param_data["qos"].get<int>();

        std::weak_ptr<SSWCP_Instance> weak_ptr = shared_from_this();
        auto                          engine   = get_current_engine();

        if (m_work_thread.joinable())
            m_work_thread.join();

        m_work_thread                          = std::thread([weak_ptr, engine, topic, qos]() {
            if (!weak_ptr.lock()) {
                return;
            }
            auto self = std::dynamic_pointer_cast<SSWCP_MqttAgent_Instance>(weak_ptr.lock());

            std::string msg  = "success";
            bool        flag = engine->Subscribe(topic, qos, msg);

            wxGetApp().CallAfter([weak_ptr, msg, flag]() {
                auto self = weak_ptr.lock();
                if (self) {
                    if (flag) {     
                        // 回复后， 设置event_id, 长期保留对象
                        if (self->m_event_id != "") {
                            self->m_msg = msg;
                            self->send_to_js();
                            
                            json header;
                            self->m_header.clear();
                            self->m_header["event_id"] = self->m_event_id;
                        } else {
                            self->handle_general_fail(-1, "event_id is null");
                        }
                        
                    } else {
                        self->handle_general_fail(-1, msg);
                    }
                }
            });
        });
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

// 取消订阅
void SSWCP_MqttAgent_Instance::sw_mqtt_unsubscribe() {
    try {
        if (!m_param_data.count("id") || !m_param_data["id"].is_string()) {
            handle_general_fail(-1, "param [id] is required or wrong type");
            return;
        }

        std::string id = m_param_data["id"].get<std::string>();

        if (!validate_id(id)) {
            handle_general_fail(-1, "id is illegal");
            return;
        }

        if (!m_param_data.count("topic") || !m_param_data["topic"].is_string()) {
            handle_general_fail(-1, "param [topic] is required or wrong type");
            return;
        }
        std::string topic = m_param_data["topic"].get<std::string>();

        // 维护订阅topic表和eventid实例表
        for (auto iter = m_subscribe_map.begin();
            iter != m_subscribe_map.end(); ) {
            if (iter->second == topic) {
                if (m_subscribe_instance_map.count(iter->first)) {
                    m_subscribe_instance_map.erase(iter->first);
                }
                iter = m_subscribe_map.erase(iter);
            } else {
                ++iter;
            }
        }

        std::weak_ptr<SSWCP_Instance> weak_ptr = shared_from_this();
        auto                          engine   = get_current_engine();

        if (m_work_thread.joinable())
            m_work_thread.join();

        m_work_thread                          = std::thread([weak_ptr, engine, topic]() {
            if (!weak_ptr.lock()) {
                return;
            }
            auto self = std::dynamic_pointer_cast<SSWCP_MqttAgent_Instance>(weak_ptr.lock());

            std::string msg  = "success";
            bool        flag = engine->Unsubscribe(topic, msg);

            wxGetApp().CallAfter([weak_ptr, msg, flag]() {
                auto self = weak_ptr.lock();
                if (self) {
                    if (flag) {
                        self->m_msg = msg;
                        self->send_to_js();
                        self->finish_job();
                    } else {
                        self->handle_general_fail(-1, msg);
                    }
                }
            });
        });
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MqttAgent_Instance::sw_mqtt_set_engine()
{
    try {
        if (!m_param_data.count("engine_id") || !m_param_data["engine_id"].is_string()) {
            handle_general_fail(-1, "param [engine_id] is required or wrong type");
            Slic3r::sentryReportLog(Slic3r::SENTRY_LOG_ERROR, std::string("device_set_engine param [engine_id] is required or wrong type"),DEVICE_SET_ENGINE_ERR);
            return;
        }

        std::string engine_id = m_param_data["engine_id"].get<std::string>();

        if (!validate_id(engine_id)) {
            handle_general_fail(-1, "id is illegal");
            Slic3r::sentryReportLog(Slic3r::SENTRY_LOG_ERROR, std::string("device_set_engine id is illegal with:") + engine_id, DEVICE_SET_ENGINE_ERR);
            return;
        }

        if (!m_param_data.count("ip") || !m_param_data["ip"].is_string()) {
            handle_general_fail(-1, "param [ip] is required or wrong type");
            Slic3r::sentryReportLog(Slic3r::SENTRY_LOG_ERROR, std::string("device_set_engine param [ip] is required or wrong type"),DEVICE_SET_ENGINE_ERR);
            return;
        }
        std::string ip = m_param_data["ip"].get<std::string>();

        if (!m_param_data.count("port") || !m_param_data["port"].is_number()) {
            handle_general_fail(-1, "param [port] is required or wrong type");
            Slic3r::sentryReportLog(Slic3r::SENTRY_LOG_ERROR, std::string("device_set_engine param [port] is required or wrong type"), DEVICE_SET_ENGINE_ERR);
            return;
        }

        bool reload_device_view = m_param_data.count("need_reload") ? m_param_data["need_reload"].get<bool>() : true; 

        int port = m_param_data["port"].get<int>();

        auto config = wxGetApp().preset_bundle->printers.get_edited_preset().config;

        PrintHostType type = PrintHostType::htMoonRaker_mqtt;
        // todo : 增加输入与type的映射

        config.option<ConfigOptionEnum<PrintHostType>>("host_type")->value = type;

        config.set("print_host", ip + (port == -1 ? "" : ":" + std::to_string(port)));

        std::shared_ptr<PrintHost> tmp_host(PrintHost::get_print_host(&config));
        wxGetApp().set_connect_host(tmp_host);
        wxGetApp().set_host_config(config);

        std::shared_ptr<Moonraker_Mqtt> host = dynamic_pointer_cast<Moonraker_Mqtt>(tmp_host);
        if (host) {
            auto engine = get_current_engine();
            
            if (engine == nullptr) {
                handle_general_fail(-1, "invalid engine");
                Slic3r::sentryReportLog(Slic3r::SENTRY_LOG_ERROR, std::string("device_set_engine invalid engine"),DEVICE_SET_ENGINE_ERR);
                return;
            }

            BOOST_LOG_TRIVIAL(info) << "[SSWCP_MqttAgent_Instance] 检查引擎连接状态...";

            if (!engine->CheckConnected()) {
                BOOST_LOG_TRIVIAL(error) << "[SSWCP_MqttAgent_Instance] 引擎连接状态异常";
                handle_general_fail(-1, "engine connection lost");
                Slic3r::sentryReportLog(Slic3r::SENTRY_LOG_ERROR, std::string("device_set_engine engine connection lost"), DEVICE_SET_ENGINE_ERR);
                return;
            }
            BOOST_LOG_TRIVIAL(info) << "[SSWCP_MqttAgent_Instance] 引擎连接状态正常";

            std::string msg    = "success";

            BOOST_LOG_TRIVIAL(info) << "[SSWCP_MqttAgent_Instance] 设置引擎...";

            bool flag = host->set_engine(engine, msg);

            BOOST_LOG_TRIVIAL(info) << "[SSWCP_MqttAgent_Instance] 引擎设置完成, flag: " << flag;

            if (flag)
            {
                if (m_param_data.count("ip")) {
                    std::string ip = m_param_data["ip"].get<std::string>();

                    int port = -1;
                    if (m_param_data.count("port") && m_param_data["port"].is_number_integer()) {
                        port = m_param_data["port"].get<int>();
                    }

                    // test
                    if (port == -1 || port == 1883) {
                        port = 1884;
                    }

                    json connect_params;
                    if (m_param_data.count("sn") && m_param_data["sn"].is_string()) {
                        connect_params["sn"] = m_param_data["sn"].get<std::string>();

                        host->m_sn_mtx.lock();
                        host->m_sn = m_param_data["sn"].get<std::string>();
                        host->m_sn_mtx.unlock();
                    } else {
                        handle_general_fail(-1, "param [sn] is required or wrong type");
                        return;
                    }

                    // 序列化参数
                    if (m_param_data.count("code")){
                        connect_params["code"] = m_param_data["code"];
                    }
                        

                    if (m_param_data.count("ca")) {
                        connect_params["ca"] = m_param_data["ca"];
                        host->m_ca           = m_param_data["ca"].get<std::string>();
                    }
                        

                    if (m_param_data.count("cert")) {
                        connect_params["cert"] = m_param_data["cert"];
                        host->m_cert           = m_param_data["cert"].get<std::string>();
                    }
                        

                    if (m_param_data.count("key")) {
                        connect_params["key"] = m_param_data["key"];
                        host->m_key           = m_param_data["key"].get<std::string>();
                    }

                    if (m_param_data.count("user")) {
                        connect_params["user"] = m_param_data["user"];
                        host->m_user_name           = m_param_data["user"].get<std::string>();
                    }

                    if (m_param_data.count("password")) {
                        connect_params["password"] = m_param_data["password"];
                        host->m_password           = m_param_data["password"].get<std::string>();
                    }

                    if (m_param_data.count("port")) {
                        connect_params["port"] = m_param_data["port"];
                        host->m_port           = m_param_data["port"].get<int>();
                    }

                    if (m_param_data.count("clientId")) {
                        connect_params["clientId"] = m_param_data["clientId"];
                        host->m_client_id           = m_param_data["clientId"].get<std::string>();
                    }
                    


                    std::string link_mode       = m_param_data.count("link_mode") ? m_param_data["link_mode"] : "lan";
                    connect_params["link_mode"] = link_mode;

                    std::string id     = m_param_data.count("id") ? m_param_data["id"].get<std::string>() : "";
                    std::string userid = m_param_data.count("userid") ? m_param_data["userid"].get<std::string>() : "";

                    if (!host) {
                        handle_general_fail(-1, "host created failed");
                        Slic3r::sentryReportLog(Slic3r::SENTRY_LOG_ERROR, std::string("device_set_engine host created failed"), DEVICE_SET_ENGINE_ERR);
                        return;
                    } else {
                        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
                        // 设置断联回调

                        if (m_work_thread.joinable())
                            m_work_thread.join();

                        m_work_thread = std::thread([weak_self, host, connect_params, link_mode, id, userid, reload_device_view] {
                            auto     self = weak_self.lock();
                            wxString msg  = "";
                            json     params;
                            host->set_connection_lost([]() {
                                wxGetApp().CallAfter([]() {
                                    SSWCP_Instance::m_first_connected = true;
                                    wxGetApp().app_config->clear_filament_extruder_map();
                                    wxGetApp().preset_bundle->machine_filaments.clear();
                                    wxGetApp().load_current_presets();
                                });
                                wxGetApp().CallAfter([]() {
                                    wxGetApp().app_config->set("use_new_connect", "false");
                                    auto p_config = &(wxGetApp().preset_bundle->printers.get_edited_preset().config);
                                    p_config->set("print_host", "");

                                    std::shared_ptr<PrintHost> ptr = nullptr;
                                    wxGetApp().get_connect_host(ptr);
                                    if (ptr) {
                                        wxString disconn_msg = "";
                                        json     disconn_param;
                                        ptr->disconnect(disconn_msg, disconn_param);
                                    }

                                    wxGetApp().set_connect_host(nullptr);

                                    auto devices = wxGetApp().app_config->get_devices();
                                    for (size_t i = 0; i < devices.size(); ++i) {
                                        if (devices[i].connected) {
                                            devices[i].connected = false;
                                            wxGetApp().app_config->save_device_info(devices[i]);
                                            break;
                                        }
                                    }

                                    // 更新卡片
                                    json param;
                                    param["command"]       = "local_devices_arrived";
                                    param["sequece_id"]    = "10001";
                                    param["data"]          = devices;
                                    std::string logout_cmd = param.dump();
                                    wxString    strJS      = wxString::Format("window.postMessage(%s)", logout_cmd);
                                    GUI::wxGetApp().run_script(strJS);

                                    // wcp订阅
                                    json data = devices;
                                    wxGetApp().device_card_notify(data);

                                    MessageDialog msg_window(nullptr, " " + _L("Connection has been disconnected and recovery attempt failed. Please reconnect.") + "\n", _L("Machine Disconnected"),
                                                             wxICON_QUESTION | wxOK);
                                    msg_window.ShowModal();

                                    wxGetApp().set_connect_host(nullptr);

                                    wxGetApp().mainframe->plater()->sidebar().update_all_preset_comboboxes();
                                });
                            });
                            bool res = true;

                            std::string ip_port = host->get_host();
                            if (res) {
                                int         pos = ip_port.find(':');
                                std::string ip  = ip_port;
                                if (pos != std::string::npos) {
                                    ip = ip_port.substr(0, pos);
                                }

                                // 更新其他设备连接状态为断开
                                auto devices = wxGetApp().app_config->get_devices();
                                for (size_t i = 0; i < devices.size(); ++i) {
                                    if (devices[i].connected) {
                                        devices[i].connected = false;
                                        wxGetApp().app_config->save_device_info(devices[i]);
                                        break;
                                    }
                                }

                                // 查询机器的机型和喷嘴信息
                                std::string              machine_type = "";
                                std::vector<std::string> nozzle_diameters;
                                std::string              device_name = "";

                                std::shared_ptr<PrintHost> host = nullptr;
                                wxGetApp().get_connect_host(host);

                                // 设置sn

                                if (SSWCP::query_machine_info(host, machine_type, nozzle_diameters, device_name) && machine_type != "") {
                                    wxGetApp().CallAfter([ip, host, link_mode, machine_type, connect_params, nozzle_diameters, device_name,
                                                          id, userid, reload_device_view]() {
                                        // 查询成功
                                        DeviceInfo info;
                                        info.ip        = ip;
                                        info.dev_id    = host->get_sn() != "" ? host->get_sn() : ip;
                                        info.dev_name  = ip;
                                        info.connected = true;
                                        info.link_mode = link_mode;
                                        info.id        = id;
                                        info.userid    = userid;
                                        ;

                                        info.model_name = machine_type;
                                        info.protocol   = int(PrintHostType::htMoonRaker_mqtt);
                                        if (connect_params.count("sn") && connect_params["sn"].is_string()) {
                                            std::string sn = host->get_sn();
                                            info.sn        = connect_params["sn"].get<std::string>();
                                            if (sn != "" && sn != info.sn) {
                                                info.sn = sn;
                                            }
                                            info.dev_name = info.sn != "" ? info.sn : info.dev_name;
                                            info.dev_id   = info.sn != "" ? info.sn : info.ip;
                                        }

                                        if (device_name != "") {
                                            info.dev_name = device_name;
                                        }

                                        size_t vendor_pos = machine_type.find_first_of(" ");
                                        if (vendor_pos != std::string::npos) {
                                            std::string vendor        = machine_type.substr(0, vendor_pos);
                                            std::string machine_cover = LOCALHOST_URL +
                                                                        std::to_string(wxGetApp().m_page_http_server.get_port()) +
                                                                        "/profiles/" + vendor + "/" + machine_type + "_cover.png";
                                            info.img = machine_cover;
                                        }

                                        auto auth_info = host->get_auth_info();
                                        try {
                                            info.ca       = /* auth_info["ca"]*/ "";
                                            info.cert     = /* auth_info["cert"]*/ "";
                                            info.key      = /* auth_info["key"]*/ "";
                                            info.user     = auth_info["user"];
                                            info.password = auth_info["password"];
                                            info.port     = auth_info["port"];
                                            info.clientId = auth_info["clientId"];
                                        } catch (std::exception& e) {}

                                        DeviceInfo query_info;
                                        bool       exist = wxGetApp().app_config->get_device_info(info.dev_id, query_info);
                                        if (nozzle_diameters.empty()) {
                                            if (exist) {
                                                query_info.connected = true;
                                                wxGetApp().app_config->save_device_info(query_info);
                                            } else {
                                                wxGetApp().app_config->save_device_info(info);
                                                MessageDialog msg_window(nullptr,
                                                                         ip + " " + _L("The target machine model has been detected as") +
                                                                             "" + machine_type + "\n" +
                                                                             _L("Please bind the nozzle information") + "\n",
                                                                         _L("Nozzle Bind"), wxICON_QUESTION | wxOK);
                                                msg_window.ShowModal();

                                                m_dialog->m_bind_nozzle = true;
                                                m_dialog->m_device_id  = ip;
                                                m_dialog->run();
                                            }

                                        } else {
                                            info.nozzle_sizes = nozzle_diameters;
                                            info.preset_name  = machine_type + " (" + nozzle_diameters[0] + " nozzle)";
                                            wxGetApp().app_config->save_device_info(info);

                                            m_dialog->m_device_id = ip;

                                            // 检查是否该预设已经选入系统
                                            {
                                                std::lock_guard<std::mutex> lock(m_ProfileJson_mutex);
                                                int  nModel = m_ProfileJson["model"].size();
                                                bool isFind = false;
                                                for (int m = 0; m < nModel; m++) {
                                                    if (m_ProfileJson["model"][m]["model"].get<std::string>() == info.model_name) {
                                                        // 绑定的预设已被选入系统
                                                        isFind                      = true;
                                                        std::string nozzle_selected = m_ProfileJson["model"][m]["nozzle_selected"]
                                                                                              .get<std::string>();
                                                        std::string se_nozz_selected = nozzle_diameters[0];
                                                        if (nozzle_selected.find(se_nozz_selected) == std::string::npos) {
                                                            nozzle_selected += ";" + se_nozz_selected;
                                                            m_ProfileJson["model"][m]["nozzle_selected"] = nozzle_selected;
                                                        }

                                                        break;
                                                    }
                                                }

                                                if (!isFind) {
                                                    json new_item;
                                                    new_item["vendor"]          = "Snapmaker";
                                                    new_item["model"]           = info.model_name;
                                                    new_item["nozzle_selected"] = nozzle_diameters[0];
                                                    m_ProfileJson["model"].push_back(new_item);
                                                }
                                            }

                                            wxGetApp().mainframe->plater()->sidebar().update_all_preset_comboboxes(false);

                                            m_dialog->SaveProfile();
                                            bool flag = false;
                                            m_dialog->apply_config(wxGetApp().app_config, wxGetApp().preset_bundle, wxGetApp().preset_updater,
                                                                flag);
                                            wxGetApp().update_mode();
                                        }
                                    });
                                } else {
                                    wxGetApp().CallAfter([connect_params, ip, host, link_mode, id, userid]() {
                                        // 是否为连接过的设备
                                        DeviceInfo  query_info;
                                        std::string dev_id = connect_params.count("sn") ? connect_params["sn"].get<std::string>() : ip;
                                        if (wxGetApp().app_config->get_device_info(dev_id, query_info)) {
                                            query_info.connected = true;
                                            wxGetApp().app_config->save_device_info(query_info);
                                        } else {
                                            auto machine_ip_type = MachineIPType::getInstance();
                                            if (machine_ip_type) {
                                                std::string machine_type = "";
                                                if (machine_ip_type->get_machine_type(ip, machine_type)) {
                                                    // 已经发现过的机型信息
                                                    // test
                                                    
                                                if (machine_type == "lava" || machine_type == "Snapmaker test") {
                                                    machine_type = "Snapmaker U1";
                                                }

                                                    DeviceInfo info;
                                                    host->get_auth_info();
                                                    auto auth_info = host->get_auth_info();
                                                    try {
                                                        info.ca       = auth_info["ca"];
                                                        info.cert     = auth_info["cert"];
                                                        info.key      = auth_info["key"];
                                                        info.user     = auth_info["user"];
                                                        info.password = auth_info["password"];
                                                        info.port     = auth_info["port"];
                                                        info.clientId = auth_info["clientId"];
                                                    } catch (std::exception& e) {}
                                                    info.ip         = ip;
                                                    info.dev_id     = dev_id;
                                                    info.dev_name   = ip;
                                                    info.connected  = true;
                                                    info.model_name = machine_type;
                                                    info.protocol   = int(PrintHostType::htMoonRaker_mqtt);
                                                    info.link_mode  = link_mode;
                                                    info.id         = id;
                                                    info.userid     = userid;
                                                    if (connect_params.count("sn") && connect_params["sn"].is_string()) {
                                                        info.sn       = connect_params["sn"].get<std::string>();
                                                        info.dev_name = info.sn != "" ? info.sn : info.dev_name;
                                                        info.dev_id   = info.sn != "" ? info.sn : info.dev_name;
                                                    }

                                                    size_t vendor_pos = machine_type.find_first_of(" ");
                                                    if (vendor_pos != std::string::npos) {
                                                        std::string vendor        = machine_type.substr(0, vendor_pos);
                                                        std::string machine_cover = LOCALHOST_URL +
                                                                                    std::to_string(
                                                                                        wxGetApp().m_page_http_server.get_port()) +
                                                                                    "/profiles/" + vendor + "/" + machine_type +
                                                                                    "_cover.png";
                                                        info.img = machine_cover;
                                                    }

                                                    wxGetApp().app_config->save_device_info(info);
                                                    // todo 绑定喷嘴

                                                    MessageDialog msg_window(nullptr,
                                                                             ip + " " + _L("The target machine model has been detected as") +
                                                                                 " " + machine_type + "\n" +
                                                                                 _L("Please bind the nozzle information") + "\n",
                                                                             _L("Nozzle Bind"), wxICON_QUESTION | wxOK);
                                                    msg_window.ShowModal();

                                                    m_dialog->m_bind_nozzle = true;
                                                    m_dialog->m_device_id   = ip;
                                                    m_dialog->run();

                                                    if (info.nozzle_sizes.empty())
                                                        info.nozzle_sizes.push_back("0.4");

                                                    info.preset_name = machine_type + " (" + info.nozzle_sizes[0] + " nozzle)";

                                                    wxGetApp().app_config->save_device_info(info);
                                                } else {
                                                    DeviceInfo info;
                                                    auto       auth_info = host->get_auth_info();
                                                    try {
                                                        info.ca       = auth_info["ca"];
                                                        info.cert     = auth_info["cert"];
                                                        info.key      = auth_info["key"];
                                                        info.user     = auth_info["user"];
                                                        info.password = auth_info["password"];
                                                        info.port     = auth_info["port"];
                                                        info.clientId = auth_info["clientId"];
                                                    } catch (std::exception& e) {}
                                                    info.ip        = ip;
                                                    info.dev_id    = dev_id;
                                                    info.dev_name  = ip;
                                                    info.connected = true;
                                                    info.link_mode = link_mode;
                                                    info.id        = id;
                                                    info.userid    = id;
                                                    info.protocol  = int(PrintHostType::htMoonRaker_mqtt);
                                                    if (connect_params.count("sn") && connect_params["sn"].is_string()) {
                                                        info.sn       = connect_params["sn"].get<std::string>();
                                                        info.dev_name = info.sn != "" ? info.sn : info.dev_name;
                                                        info.dev_id   = info.sn != "" ? info.sn : info.dev_name;
                                                    }
                                                    wxGetApp().app_config->save_device_info(info);
                                                    MessageDialog msg_window(
                                                        nullptr,
                                                        ip + " " +
                                                            _L("The target machine model has not been detected. Please bind manually."),
                                                        _L("Machine Bind"), wxICON_QUESTION | wxOK);
                                                    msg_window.ShowModal();

                                                    m_dialog->m_device_id = info.dev_id;
                                                    m_dialog->run();
                                                }
                                            }
                                        }
                                    });
                                }

                                wxGetApp().CallAfter([weak_self, reload_device_view]() {
                                    // 更新首页设备卡片
                                    auto devices = wxGetApp().app_config->get_devices();

                                    json param;
                                    param["command"]       = "local_devices_arrived";
                                    param["sequece_id"]    = "10001";
                                    param["data"]          = devices;
                                    std::string logout_cmd = param.dump();
                                    wxString    strJS      = wxString::Format("window.postMessage(%s)", logout_cmd);
                                    GUI::wxGetApp().run_script(strJS);

                                    // wcp订阅
                                    json data = devices;
                                    wxGetApp().device_card_notify(data);

                                    /*MessageDialog msg_window(nullptr, ip + " " + _L("connected sucessfully !") + "\n", _L("Machine
                                    Connected"), wxICON_QUESTION | wxOK); msg_window.ShowModal();*/

                                    auto dialog = wxGetApp().get_web_device_dialog();
                                    if (dialog) {
                                        dialog->EndModal(1);
                                    }

                                    wxGetApp().app_config->set("use_new_connect", "true");
                                    wxGetApp().mainframe->plater()->sidebar().update_all_preset_comboboxes(reload_device_view);
                                    wxGetApp().mainframe->m_print_enable = true;
                                    wxGetApp().mainframe->update_slice_print_status(MainFrame::eEventPlateUpdate);
                                    // wxGetApp().mainframe->load_printer_url("http://" + ip);  //到时全部加载本地交互页面

                                    if (!wxGetApp().mainframe->m_printer_view->isSnapmakerPage()) {
                                        wxString url      = wxString::FromUTF8(LOCALHOST_URL + std::to_string(PAGE_HTTP_PORT) +
                                                                               "/web/flutter_web/index.html?path=2");
                                        auto     real_url = wxGetApp().get_international_url(url);
                                        wxGetApp().mainframe->load_printer_url(real_url); // 到时全部加载本地交互页面
                                    } else {
                                        if (reload_device_view) {
                                            wxString url      = wxString::FromUTF8(LOCALHOST_URL + std::to_string(PAGE_HTTP_PORT) +
                                                                                   "/web/flutter_web/index.html?path=2");
                                            auto     real_url = wxGetApp().get_international_url(url);

                                            wxGetApp().mainframe->load_printer_url(real_url); // 到时全部加载本地交互页面
                                        }
                                        
                                    }

                                    auto self = weak_self.lock();
                                    if (!self) {
                                        return;
                                    }

                                    // 清除耗材喷嘴映射信息
                                    wxGetApp().app_config->clear_filament_extruder_map();

                                    // 尝试获取新的耗材喷嘴映射信息
                                    if (self->m_wcp_cache.count("deviceFilamentInfo")) {
                                        std::string value_str = m_wcp_cache["deviceFilamentInfo"].get<std::string>();
                                        json value                 = json::parse(value_str);
                                        json value_item            = value["value"];
                                        auto machines     = wxGetApp().app_config->get_devices();
                                        bool find                  = false;
                                        for (auto& [key, value] : value_item.items()) {
                                            if (find) {
                                                break;
                                            }

                                            for (const auto& machine : machines) {
                                                if (machine.sn == key && machine.connected) {
                                                    find = true;
                                                    json target = json::array();
                                                    json object = json::object();
                                                    object["key"] = key;
                                                    object["value"]    = value.dump();
                                                    target.push_back(object);
                                                    self->update_filament_info(target, false);
                                                    break;
                                                }
                                            }

                                        }
                                    }

                                    // 整理订阅列表，取消权限，但是保留真正的底层订阅
                                    // 维护订阅topic表和eventid实例表
                                    auto mqtt_self = dynamic_pointer_cast<SSWCP_MqttAgent_Instance>(self);
                                    mqtt_self->clean_current_engine();
                                    self->send_to_js();
                                    self->finish_job();
                                });

                            } 
                        });
                    }

                } else {
                    handle_general_fail(-1, "param [ip] required");
                }
            } else {
                handle_general_fail();
            }
        } else {
            handle_general_fail();
        }

    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MqttAgent_Instance::sw_mqtt_publish()
{
    try {
        if (!m_param_data.count("id") || !m_param_data["id"].is_string()) {
            handle_general_fail(-1, "param [id] is required or wrong type");
            Slic3r::sentryReportLog(Slic3r::SENTRY_LOG_ERROR, std::string("device_publish host created failed"), DEVICE_PBLISH_ERR);
            return;
        }

        std::string id = m_param_data["id"].get<std::string>();

        if (!validate_id(id)) {
            handle_general_fail(-1, "id is illegal");
            Slic3r::sentryReportLog(Slic3r::SENTRY_LOG_ERROR, std::string("device_publish id is illegal"), DEVICE_PBLISH_ERR);
            return;
        }

        if (!m_param_data.count("topic") || !m_param_data["topic"].is_string()) {
            handle_general_fail(-1, "param [topic] is required or wrong type");
            Slic3r::sentryReportLog(Slic3r::SENTRY_LOG_ERROR, std::string("device_publish param [topic] is required or wrong type"), DEVICE_PBLISH_ERR);
            return;
        }
        std::string topic = m_param_data["topic"].get<std::string>();

        if (!m_param_data.count("qos") || !m_param_data["qos"].is_number()) {
            handle_general_fail(-1, "param [qos] is required or wrong type");
            Slic3r::sentryReportLog(Slic3r::SENTRY_LOG_ERROR, std::string("device_publish param [qos] is required or wrong type"), DEVICE_PBLISH_ERR);
            return;
        }
        int qos = m_param_data["qos"].get<int>();

        if (!m_param_data.count("payload") || !m_param_data["payload"].is_string()) {
            handle_general_fail(-1, "param [payload] required");
            Slic3r::sentryReportLog(Slic3r::SENTRY_LOG_ERROR, std::string("device_publish param [payload] required"), DEVICE_PBLISH_ERR);
            return;
        }
        std::string payload = m_param_data["payload"].get<std::string>();
        

        std::weak_ptr<SSWCP_Instance> weak_ptr = shared_from_this();
        auto                          engine   = get_current_engine();

        if (m_work_thread.joinable())
            m_work_thread.join();

        m_work_thread                          = std::thread([weak_ptr, engine, topic, payload, qos]() {
            if (!weak_ptr.lock()) {
                return;
            }
            auto self = std::dynamic_pointer_cast<SSWCP_MqttAgent_Instance>(weak_ptr.lock());

            std::string msg  = "success";
            bool        flag = engine->Publish(topic, payload, qos, msg);

            wxGetApp().CallAfter([weak_ptr, msg, flag]() {
                auto self = weak_ptr.lock();
                if (self) {
                    if (flag) {
                        self->m_msg = msg;
                        self->send_to_js();
                        self->finish_job();
                    } else {
                        self->handle_general_fail(-1, msg);
                    }
                }
            });
        });
    } catch (std::exception& e) {
        handle_general_fail();
    }
}



// SSWCP
TimeoutMap<SSWCP_Instance*, std::shared_ptr<SSWCP_Instance>> SSWCP::m_instance_list;
constexpr std::chrono::milliseconds SSWCP::DEFAULT_INSTANCE_TIMEOUT;

std::string SSWCP::m_active_gcode_filename = "";
std::string SSWCP::m_display_gcode_filename = "";
long long   SSWCP::m_active_file_size       = 0;
std::mutex  SSWCP::m_file_size_mutex;

std::unordered_map<std::string, int> SSWCP::m_tab_map = {
    {"Home", MainFrame::TabPosition::tpHome},
    {"3DEditor", MainFrame::TabPosition::tp3DEditor},
    {"Preview", MainFrame::TabPosition::tpPreview},
    {"Monitor", MainFrame::TabPosition::tpMonitor},
    {"MultiDevice", MainFrame::TabPosition::tpMultiDevice},
    {"Project", MainFrame::TabPosition::tpProject},
    {"Calibration", MainFrame::TabPosition::tpCalibration},
    {"Auxiliary", MainFrame::TabPosition::tpAuxiliary},
    {"DebugTool", MainFrame::TabPosition::toDebugTool}
};

std::unordered_set<std::string> SSWCP::m_machine_find_cmd_list = {
    "sw_GetMachineFindSupportInfo",
    "sw_StartMachineFind",
    "sw_StopMachineFind",
    "sw_WakeupFind",
};

std::unordered_set<std::string> SSWCP::m_machine_option_cmd_list = {
    "system.get_device_info",
    "sw_SendGCodes",
    "sw_FileGetStatus",
    "sw_SystemGetDeviceInfo",
    "sw_GetMachineState",
    "sw_SubscribeMachineState",
    "sw_GetMachineObjects",
    "sw_SetSubscribeFilter",
    "sw_StopMachineStateSubscription",
    "sw_GetPrinterInfo",
    "sw_MachinePrintStart",
    "sw_MachinePrintPause",
    "sw_MachinePrintResume",
    "sw_MachinePrintCancel",
    "sw_GetMachineSystemInfo",
    "sw_MachineFilesRoots",
    "sw_MachineFilesMetadata",
    "sw_MachineFilesThumbnails",
    "sw_MachineFilesGetDirectory",
    "sw_CameraStartMonitor",
    "sw_CameraStopMonitor",
    "sw_DeleteMachineFile",
    "sw_GetFileFilamentMapping",
    "sw_SetFilamentMappingComplete",
    "sw_FinishFilamentMapping",
    "sw_DownloadMachineFile",
    "sw_UploadFiletoMachine",
    "sw_GetPrintLegal",
    "sw_GetPrintZip",
    "sw_FinishPreprint",
    "sw_PullCloudFile",
    "sw_CancelPullCloudFile",
    "sw_StartCloudPrint",
    "sw_SetDeviceName",
    "sw_ControlLed",
    "sw_ControlPrintSpeed",
    "sw_BedMesh_AbortProbeMesh",
    "sw_ControlPurifier",
    "sw_ControlMainFan",
    "sw_ControlGenericFan",
    "sw_ControlBedTemp",
    "sw_ControlExtruderTemp",
    "sw_FilesThumbnailsBase64",
    "sw_exception_query",
    "sw_GetFileListPage",
    "sw_UpdateMachineFilamentInfo",
    "sw_UploadCameraTimelapse",
    "sw_DeleteCameraTimelapse",
    "sw_GetTimelapseInstance",
    "sw_ServerClientManagerSetUserinfo",
    "sw_DefectDetactionConfig"
};

std::unordered_set<std::string> SSWCP::m_machine_connect_cmd_list = {
    "sw_Test_connect",
    "sw_Connect",
    "sw_Disconnect",
    "sw_GetConnectedMachine",
    "sw_ConnectOtherMachine",
    "sw_GetPincode"
};

std::unordered_set<std::string> SSWCP::m_project_cmd_list = {
    "sw_NewProject", "sw_OpenProject", "sw_GetRecentProjects", "sw_OpenRecentFile", "sw_DeleteRecentFiles", "sw_SubscribeRecentFiles",
};

std::unordered_set<std::string> SSWCP::m_login_cmd_list = {
    "sw_UserLogin", "sw_UserLogout", "sw_GetUserLoginState", "sw_SubscribeUserLoginState"
};

std::unordered_set<std::string> SSWCP::m_machine_manage_cmd_list = {
    "sw_GetLocalDevices", "sw_AddDevice", "sw_SubscribeLocalDevices", "sw_RenameDevice", "sw_SwitchModel", "sw_DeleteDevices"
};

std::unordered_set<std::string> SSWCP::m_page_state_cmd_list = {
    "sw_SubscribePageStateChange", "sw_UnsubscribePageStateChange"
};

std::unordered_set<std::string> SSWCP::m_mqtt_agent_cmd_list = {
    "sw_create_mqtt_client", "sw_mqtt_connect", "sw_mqtt_disconnect", "sw_mqtt_subscribe", "sw_mqtt_unpublish", "sw_mqtt_publish", "sw_mqtt_set_engine"
};

std::shared_ptr<SSWCP_Instance> SSWCP::create_sswcp_instance(std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview)
{
    std::shared_ptr<SSWCP_Instance> instance;
    
    if (m_machine_find_cmd_list.find(cmd) != m_machine_find_cmd_list.end()) {
        instance = std::make_shared<SSWCP_MachineFind_Instance>(cmd, header, data, event_id, webview);
    } else if (m_machine_connect_cmd_list.find(cmd) != m_machine_connect_cmd_list.end()) {
        instance = std::make_shared<SSWCP_MachineConnect_Instance>(cmd, header, data, event_id, webview);
    } else if (m_machine_option_cmd_list.find(cmd) != m_machine_option_cmd_list.end()) {
        instance = std::make_shared<SSWCP_MachineOption_Instance>(cmd, header, data, event_id, webview);
    } else if (m_project_cmd_list.find(cmd) != m_project_cmd_list.end()) {
        instance = std::make_shared<SSWCP_SliceProject_Instance>(cmd, header, data, event_id, webview);
    } else if (m_login_cmd_list.find(cmd) != m_login_cmd_list.end()) {
        instance = std::make_shared<SSWCP_UserLogin_Instance>(cmd, header, data, event_id, webview);
    } else if (m_machine_manage_cmd_list.find(cmd) != m_machine_manage_cmd_list.end()) {
        instance = std::make_shared<SSWCP_MachineManage_Instance>(cmd, header, data, event_id, webview);
    } else if (m_page_state_cmd_list.find(cmd) != m_page_state_cmd_list.end()) {
        instance = std::make_shared<SSWCP_PageStateChange_Instance>(cmd, header, data, event_id, webview);
    } else if(m_mqtt_agent_cmd_list.find(cmd) != m_mqtt_agent_cmd_list.end()) {
        instance = std::make_shared<SSWCP_MqttAgent_Instance>(cmd, header, data, event_id, webview);
    }
    else {
        instance = std::make_shared<SSWCP_Instance>(cmd, header, data, event_id, webview);
    }
    
    return instance;
}

// Handle incoming web messages
void SSWCP::handle_web_message(std::string message, wxWebView* webview) {
    try {

        if (!webview) {
            return;
        }
        WCP_Logger::getInstance().add_log(message, false, "", "WCP", "info");

        json j_message = json::parse(message);

        if (j_message.empty() || !j_message.count("header") || !j_message.count("payload") || !j_message["payload"].count("cmd")) {
            return;
        }

        json header = j_message["header"];
        json payload = j_message["payload"];

        std::string cmd = "";
        std::string event_id = "";
        json params;

        if (payload.count("cmd")) {
            cmd = payload["cmd"].get<std::string>();
        }
        if (payload.count("params")) {
            params = payload["params"];
        }

        if (payload.count("event_id") && !payload["event_id"].is_null()) {
            event_id = payload["event_id"].get<std::string>();
        }

        std::shared_ptr<SSWCP_Instance> instance = create_sswcp_instance(cmd, header, params, event_id, webview);
        if (instance) {
            if (event_id != "") {
                m_instance_list.add_infinite(instance.get(), instance);
            } else {
                m_instance_list.add(instance.get(), instance, DEFAULT_INSTANCE_TIMEOUT);
            }
            instance->process();
        }
        //if (!m_func_map.count(cmd)) {
        //    // todo:返回不支持处理
        //}

        //m_func_map[cmd](sequenceId, data, callback_name, webview);

    }
    catch (std::exception& e) {
    }
}

// Delete instance from list
void SSWCP::delete_target(SSWCP_Instance* target) {
    wxGetApp().CallAfter([target]() {
        m_instance_list.remove(target);
    });
}

// Stop all machine subscriptions
void SSWCP::stop_subscribe_machine()
{
    wxGetApp().CallAfter([]() {
        std::vector<SSWCP_Instance*> instances_to_stop;
        
        auto snapshot = m_instance_list.get_snapshot();

        // Get all subscription instances to stop
        for (const auto& instance : snapshot) {  
            if (instance.second->getType() == SSWCP_MachineFind_Instance::MACHINE_OPTION && instance.second->m_cmd == "sw_SubscribeMachineState") {
                instances_to_stop.push_back(instance.first);
            }
        }
        
        // Stop each instance
        for (auto* instance : instances_to_stop) {
            auto instance_ptr = m_instance_list.get(instance);
            if (instance_ptr) {
                (*instance_ptr)->finish_job();
            }
        }
    });
}

// Stop all machine discovery instances
void SSWCP::stop_machine_find() {
    wxGetApp().CallAfter([]() {
        std::vector<SSWCP_Instance*> instances_to_stop;
        
        auto snapshot = m_instance_list.get_snapshot();

        // Get all discovery instances to stop
        for (const auto& instance : snapshot) {  
            if (instance.second->getType() == SSWCP_MachineFind_Instance::MACHINE_FIND) {
                instances_to_stop.push_back(instance.first);
            }
        }
        
        // Set stop flag for each instance
        for (auto* instance : instances_to_stop) {
            auto instance_ptr = m_instance_list.get(instance);
            if (instance_ptr) {
                (*instance_ptr)->set_stop(true);
            }
        }
    });
}

// Handle webview deletion
void SSWCP::on_webview_delete(wxWebView* view)
{
    // Mark all instances associated with this webview as invalid
    std::vector<SSWCP_Instance*> instances_to_invalidate;
    
    // Get all instances using this webview
    for (const auto& instance : m_instance_list) {
        if (instance.second->value->get_web_view() == view) {
            instances_to_invalidate.push_back(instance.first);
            instance.second->value->set_web_view(nullptr);
        }
    }
    
    // Mark each instance as invalid
    for (auto* instance : instances_to_invalidate) {
        auto instance_ptr = m_instance_list.get(instance);
        if (instance_ptr) {
            (*instance_ptr)->set_Instance_illegal();
        }
    }

    auto& device_map = wxGetApp().m_device_card_subscribers;
    for (auto iter = device_map.begin(); iter != device_map.end();) {
        if (iter->first == view) {
            iter = device_map.erase(iter);
        } else {
            iter++;
        }
    }

    auto& login_map = wxGetApp().m_user_login_subscribers;
    for (auto iter = login_map.begin(); iter != login_map.end();) {
        if (iter->first == view) {
            iter = login_map.erase(iter);
        } else {
            iter++;
        }
    }

    auto& privacy_map = wxGetApp().m_user_update_privacy_subscribers;
    for (auto iter = privacy_map.begin(); iter != privacy_map.end();) {
        if (iter->first == view) {
            iter = privacy_map.erase(iter);
        } else {
            iter++;
        }
    }

    auto& recent_file_map = wxGetApp().m_recent_file_subscribers;
    for (auto iter = recent_file_map.begin(); iter != recent_file_map.end();) {
        if (iter->first == view) {
            iter = recent_file_map.erase(iter);
        } else {
            iter++;
        }
    }

    auto& cache_map = wxGetApp().m_cache_subscribers;
    for (auto iter = cache_map.begin(); iter != cache_map.end();) {
        if (iter->first.first == view) {
            iter = cache_map.erase(iter);
        } else {
            iter++;
        }
    }

    auto& page_state_map = wxGetApp().m_page_state_subscribers;
    for (auto iter = page_state_map.begin(); iter != page_state_map.end();) {
        if (iter->first == view) {
            iter = page_state_map.erase(iter);
        } else {
            iter++;
        }
    }
}

std::string SSWCP::get_display_filename()
{
    return m_display_gcode_filename;
}

// get the active filename
std::string SSWCP::get_active_filename()
{
    return m_active_gcode_filename;
}

// set the display name
void SSWCP::update_display_filename(const std::string& filename)
{ m_display_gcode_filename = filename; }


// set the active filename
void SSWCP::update_active_filename(const std::string& filename)
{
    m_active_gcode_filename = filename;
}

// query the info of the machine
bool SSWCP::query_machine_info(std::shared_ptr<PrintHost>& host, std::string& out_model, std::vector<std::string>& out_nozzle_diameters, std::string& device_name, int timeout_second)
{
    if (!host) return false;

    // 创建同步等待的条件变量和互斥锁
    std::condition_variable cv;
    std::shared_ptr<std::mutex> mutex(new std::mutex);
    std::weak_ptr<std::mutex>   cb_mutex = mutex;
    bool received = false;
    bool timeout = false;
    json system_info;

    // 发送查询请求
    host->async_get_system_info(
        [&, cb_mutex](const json& response) {
            if (cb_mutex.expired()) {
                return;
            }
            std::lock_guard<std::mutex> lock(*mutex);
            if (!response.is_null() && !response.count("error")) {
                system_info = response;
            }
            received = true;
            cv.notify_one();
        }
    );

    // 等待响应
    {
        std::unique_lock<std::mutex> lock(*mutex);
        auto predicate = [&received]() { return received; };
        timeout = !cv.wait_for(lock, std::chrono::seconds(timeout_second), predicate);
    }

    if (!timeout && !system_info.is_null()) {
        // 成功获取到信息
        if (system_info.count("data")) {
            system_info = system_info["data"];
        }
        if (system_info.contains("system_info")) {
            auto& system_data = system_info["system_info"];
            
            if(system_data.contains("product_info")){
                auto& product_info = system_data["product_info"];

                // 获取机型
                if(product_info.contains("machine_type")){
                    out_model = product_info["machine_type"].get<std::string>();
                }

                // 获取喷嘴信息
                if(product_info.contains("nozzle_diameter")){
                    try {
                        if (product_info["nozzle_diameter"].is_array()) {
                            for (const auto& nozzle : product_info["nozzle_diameter"]) {
                                // todo 不一定是string
                                if (nozzle.is_number()) {
                                    double temp = nozzle.get<double>();
                                    if (fabs(temp - 0.2) < 1e-6) {
                                        out_nozzle_diameters.push_back("0.2");
                                    } else if (fabs(temp - 0.4) < 1e-6) {
                                        out_nozzle_diameters.push_back("0.4");
                                    } else if (fabs(temp - 0.6) < 1e-6) {
                                        out_nozzle_diameters.push_back("0.6");
                                    } else if (fabs(temp - 0.8) < 1e-6) {
                                        out_nozzle_diameters.push_back("0.8");
                                    }
                                    
                                } else {
                                    std::string temp = nozzle.get<std::string>();
                                    if (temp == "0.2" || temp == "0.4" || temp == "0.6" || temp == "0.8") {
                                        out_nozzle_diameters.push_back(temp);
                                    }
                                }
                                
                            }
                        } else {
                            // 如果是单个值
                            if (product_info["nozzle_diameter"].is_number()) {
                                double temp = product_info["nozzle_diameter"].get<double>();
                                if (fabs(temp - 0.2) < 1e-6) {
                                    out_nozzle_diameters.push_back("0.2");
                                } else if (fabs(temp - 0.4) < 1e-6) {
                                    out_nozzle_diameters.push_back("0.2");
                                } else if (fabs(temp - 0.6) < 1e-6) {
                                    out_nozzle_diameters.push_back("0.2");
                                } else if (fabs(temp - 0.8) < 1e-6) {
                                    out_nozzle_diameters.push_back("0.2");
                                }

                            } else {
                                std::string temp = product_info["nozzle_diameter"].get<std::string>();
                                if (temp == "0.2" || temp == "0.4" || temp == "0.6" || temp == "0.8") {
                                    out_nozzle_diameters.push_back(temp);
                                }
                            }
                        }
                    }
                    catch (std::exception& e) {
                        return false;
                    }
                }

                if (product_info.contains("device_name")) {
                    device_name = product_info["device_name"].get<std::string>();
                }

            }

            return true;
        }
    }
    
    return false;
}

MachineIPType* MachineIPType::getInstance()
{
    static MachineIPType mipt_instance;
    return &mipt_instance;
}


}}; // namespace Slic3r::GUI


