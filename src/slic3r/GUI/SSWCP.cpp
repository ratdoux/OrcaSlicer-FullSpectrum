// Implementation of web communication protocol for Slicer Studio
#include "SSWCP.hpp"
#include "FilamentColorUtils.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "DownloadManager.hpp"
#include "Timelapse/TimelapseDownloadPopup.hpp"
#include "nlohmann/json.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/SSWCPProtocol.hpp"
#include "sentry_wrapper/SentryWrapper.hpp"
#include <algorithm>
#include <iterator>
#include <exception>
#include <cstdlib>
#include <iomanip>
#include <regex>
#include <thread>
#include <string_view>
#include <vector>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/asio/ip/host_name.hpp>
#include <wx/stdpaths.h>
#include <wx/msgdlg.h>
#include "slic3r/Utils/Http.hpp"

#include <slic3r/GUI/Widgets/WebView.hpp>
#include "NetworkTestDialog.hpp"

#include "MoonRaker.hpp"

#include "slic3r/GUI/WebPresetDialog.hpp"
#include "slic3r/GUI/HttpServer.hpp"
#include <mutex>

#include "slic3r/GUI/SMPhysicalPrinterDialog.hpp"
#include "slic3r/GUI/WebUrlDialog.hpp"
#include "slic3r/GUI/WebSMUserLoginDialog.hpp"
#include "slic3r/GUI/FilamentGroupDialog.hpp"
#include "slic3r/GUI/FlowTypeHelper.hpp"

#include "miniz/miniz.h"
#include "slic3r/Utils/MQTT.hpp"
#include "slic3r/Utils/Http.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/filesystem/operations.hpp>
#include <openssl/sha.h>

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

    
    socket = new tcp::socket(io_ctx);
    
    resolver       = new tcp::resolver(io_ctx);
    auto endpoints = resolver->resolve("127.0.0.1", "50000");
    

    boost::system::error_code ec;
    asio::connect(*socket, endpoints,ec);
    if (ec) {                 
        BOOST_LOG_TRIVIAL(error) << "socket connect failed: " << ec.message() << std::endl;
        return false;
    } else {        
        BOOST_LOG_TRIVIAL(debug) << "Connected successfully!" << std::endl;
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
        
        wxDateTime now = wxDateTime::Now();
        //sample 2026-6-30-18:10:58        
        wxString dateTimePart = now.Format(_T("%Y-%m-%d %H:%M:%S"));
        
        int      milliseconds = now.GetMillisecond();
        wxString msPart       = wxString::Format(_T("%03d"), milliseconds);
        
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
            boost::system::error_code ec;
            std::size_t bytes_transferred = asio::write(*socket, asio::buffer(log.ToUTF8().data(), log.length() + 1),ec);
            if (ec) {                
                //std::cout << "Write failed: " << ec.message() << std::endl;  todo if need to log              
            } else {                
                //std::cout << "Write success! Sent " << bytes_transferred << " bytes." << std::endl;
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

    if (socket != nullptr && socket->is_open()) {
        boost::system::error_code ec;
        socket->close(ec);
        if (ec) {
            BOOST_LOG_TRIVIAL(error) << "error closing socket: " << ec.message();
        }
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

        if (line.find("; THUMBNAIL_BLOCK_START") != std::string::npos) {
            std::string thumb_content = "";
            int         width         = 0;
            int         height        = 0;
            int         data_size     = 0;
            
            std::getline(ifs, line);
            std::getline(ifs, line);

            // get images info
            std::getline(ifs, line);
            if (line.find("; thumbnail begin") != std::string::npos) {
                // parse width, height and data size
                // format:"; thumbnail begin 48x48 1144"
                sscanf(line.c_str(), "; thumbnail begin %dx%d %d", &width, &height, &data_size);

                // get base64 data
                std::string base64_data;
                while (std::getline(ifs, line)) {
                    if (line.find("; thumbnail end") != std::string::npos) {
                        break;
                    }
                    //remove first line string "; " 
                    if (line.substr(0, 2) == "; ") {
                        base64_data += line.substr(2);
                    }
                }
                thumb_content = base64_data;
                res.emplace_back(thumb_content);

                ++thumbnail_id;
            }

            // read to end of block marker
            while (std::getline(ifs, line)) {
                if (line.find("; THUMBNAIL_BLOCK_END") != std::string::npos) {
                    break;
                }
            }
        }
    }

    ifs.clear();
    ifs.seekg(0);

    return std::move(res);
}

// Util
std::vector<char> create_zip_with_miniz(const std::string& name1, // "c:/xxx/1.gcode"
                                        const std::string& name2  // ZIP filename"target.gcode"
)
{
    // 1.read origin file content
    std::ifstream file(name1, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open source file: " + name1);
    }

    std::vector<char> file_content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    SSWCP::m_file_size_mutex.lock();
    SSWCP::m_active_file_size = file_content.size();
    SSWCP::m_file_size_mutex.unlock();

    // 2. on memory init zip writer
    mz_zip_archive zip_archive;
    memset(&zip_archive, 0, sizeof(zip_archive));

    // init zip and write it to heap memory
    if (!mz_zip_writer_init_heap(&zip_archive, 0, 0)) {
        throw std::runtime_error("Failed to initialize ZIP writer");
    }

    // 3. add file content to zip
    if (!mz_zip_writer_add_mem(&zip_archive,
                               name2.c_str(),         // ZIP filename
                               file_content.data(),   // file content point
                               file_content.size(),   // file content size
                               MZ_DEFAULT_COMPRESSION // zip level
                               )) {
        mz_zip_writer_end(&zip_archive);
        throw std::runtime_error("Failed to add file to ZIP");
    }

    // 4. finish save data to zip and get memory data
    void*  zip_data = nullptr;
    size_t zip_size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&zip_archive, &zip_data, &zip_size)) {
        mz_zip_writer_end(&zip_archive);
        throw std::runtime_error("Failed to finalize ZIP archive");
    }

    // copy zip data to vector
    std::vector<char> zip_stream(static_cast<char*>(zip_data), static_cast<char*>(zip_data) + zip_size);

    // 5.clear resource
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
    // get name1 path
    fs::path path1 = oriname;

    // such as "c:/xxx/xxx/xxx"
    fs::path parent_dir = path1.parent_path();

    // "target.gcode" -> "target.gcode.zip"
    fs::path new_filename = fs::path(targetname);
    new_filename += ".zip"; 

    // get the complete path
    fs::path zip_path = parent_dir / new_filename;
    return zip_path.string();
}

// check if reading file content
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

// Build a download URL served by the local HTTP server, bypassing the 512MB postMessage limit.
// The file path is URL-safe base64-encoded to avoid issues with special characters (# \ : etc.).
std::string make_wcp_download_url(const std::string& file_path)
{
    auto& server = wxGetApp().m_page_http_server;
    std::string b64 = base64_encode(file_path.data(), file_path.size());
    for (auto& c : b64) {
        if (c == '+') {
            c = '-';
        } else if (c == '/') {
            c = '_';
        }
    }
    return std::string(LOCALHOST_URL) + std::to_string(server.get_port()) + WCP_DOWNLOAD_PREFIX + b64;
}

// Compute SHA-256 digest → standard Base64, matching Flutter's base64Encode(sha256.convert(bytes).bytes)
static std::string calc_sha256_base64(const std::string& file_path)
{
    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs.is_open()) {
        return "";
    }

    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    static constexpr size_t kChunkSize = 64 * 1024;
    std::string             buf(kChunkSize, 0);
    while (ifs.read(buf.data(), buf.size()) || ifs.gcount() > 0) {
        SHA256_Update(&ctx, buf.data(), ifs.gcount());
    }

    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256_Final(digest, &ctx);

    return base64_encode((const char*) digest, SHA256_DIGEST_LENGTH);
}

json get_or_create_zip_json(const std::string& name1,   // origin file "1.gcode"
                            const std::string& name2,   // target file name "target.gcode"
                            const std::string& zip_path // output filename "output.zip"
)
{
    std::vector<char> zip_stream;

    // 1. check the same ZIP exist
    if (read_existing_zip(zip_path, zip_stream)) {
        std::cout << "Reusing existing ZIP file: " << zip_path << std::endl;
    } else {
        // 2. not exist, creating target file
        std::cout << "Creating new ZIP file: " << zip_path << std::endl;
        zip_stream = create_zip_with_miniz(name1, name2);

        // write data to zip
        std::ofstream out_file(zip_path, std::ios::binary);
        out_file.write(zip_stream.data(), zip_stream.size());
        if (!out_file.good()) {
            throw std::runtime_error("Failed to write ZIP to: " + zip_path);
        }
    }

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
    } else if (m_cmd == "sw_GetSoftwareInfo") {
        sw_GetSoftwareInfo();
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

void SSWCP_Instance::sw_GetSoftwareInfo()
{
    m_res_data["version"] = std::string(Snapmaker_VERSION);
    auto& server = wxGetApp().m_page_http_server;
    m_res_data["http_host"] = std::string(LOCALHOST_URL) + std::to_string(server.get_port());

    send_to_js();
    finish_job();
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
    BOOST_LOG_TRIVIAL(error) << "sw_Exit app";
    wxGetApp().Exit();
}

void SSWCP_Instance::sw_GetActiveFile()
{
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

    json metadata_json = json::object();
    if (wxGetApp().model().model_info) {
        auto& items = wxGetApp().model().model_info->metadata_items;
        auto lookup = [&](const std::string& key) {
            auto it = items.find(key);
            if (it != items.end())
                metadata_json[key] = it->second;
        };
        // Currently only these three metadata fields are returned for business needs
        lookup("DesignModelId");
        lookup("DesignProfileId");
        lookup("DesignRegion");
    }
    m_res_data["metadata"] = metadata_json;

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
                std::string url_zip_path = std::string(wxString(zipname).ToUTF8());
                std::replace(url_zip_path.begin(), url_zip_path.end(), '\\', '/');
                self->m_res_data["url"] = LOCALHOST_URL + std::to_string(wxGetApp().get_page_http_port()) + "/localfile/" + Http::url_encode(url_zip_path);
                SSWCP::m_file_size_mutex.unlock();

                // checksum: SHA-256 digest as standard Base64, for Flutter-side integrity verification
                self->m_res_data["checksum"] = calc_sha256_base64(file_path);

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
        std::string url_path = file_path;
        std::replace(url_path.begin(), url_path.end(), '\\', '/');
        m_res_data["file_path"] = file_path;
        m_res_data["origin_size"] = boost::filesystem::file_size(file_path);

        // checksum: SHA-256 digest as standard Base64, for Flutter-side integrity verification
        m_res_data["checksum"] = calc_sha256_base64(file_path);
        m_res_data["url"]      = LOCALHOST_URL + std::to_string(wxGetApp().get_page_http_port()) + "/localfile/" + Http::url_encode(url_path);

        send_to_js();
        finish_job();
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
        std::string file_path = SSWCP::get_active_filename();
        std::string file_name = SSWCP::get_display_filename();
        if (file_path == "" || file_name == "") {
            handle_general_fail();
            return;
        }

        bool isZip = false;
        if (m_param_data.count("is_zip")) {
            isZip = m_param_data["is_zip"].get<bool>();
        }

        if (isZip) {
            std::weak_ptr<SSWCP_Instance> weak_self = shared_from_this();
            if (m_work_thread.joinable()) {
                m_work_thread.join();
            }

            m_work_thread = std::thread([file_path, file_name, weak_self]() {
                auto self = weak_self.lock();
                if (!self) {
                    return;
                }

                std::string zipname = generate_zip_path(file_path, file_name);
                get_or_create_zip_json(file_path, file_name, zipname);

                size_t name_index = file_name.find_last_of(".");
                size_t path_index = file_path.find_last_of(".");
                if (name_index == std::string::npos || path_index == std::string::npos) {
                    wxGetApp().CallAfter([weak_self]() {
                        auto self = weak_self.lock();
                        if (self) {
                            self->handle_general_fail();
                        }
                    });
                    return;
                }

                std::string zip_file_name = file_name.substr(0, name_index) + ".zip";
                long long   file_size     = boost::filesystem::file_size(file_path);
                std::string sha256_base64 = calc_sha256_base64(file_path);

                wxGetApp().CallAfter([weak_self, zipname, zip_file_name, file_size, sha256_base64]() {
                    auto self = weak_self.lock();
                    if (!self) {
                        return;
                    }

                    self->m_res_data["file_name"]   = zip_file_name;
                    self->m_res_data["file_url"]    = make_wcp_download_url(zipname);
                    self->m_res_data["origin_size"] = file_size;

                    // checksum: SHA-256 digest as standard Base64, for Flutter-side integrity verification
                    self->m_res_data["checksum"]    = sha256_base64;

                    self->send_to_js();
                    self->finish_job();
                });
            });
        } else {
            std::string download_url = make_wcp_download_url(file_path);
            std::weak_ptr<SSWCP_Instance> weak_self = shared_from_this();
            if (m_work_thread.joinable()) {
                m_work_thread.join();
            }

            m_work_thread = std::thread([file_path, file_name, download_url, weak_self]() {
                auto self = weak_self.lock();
                if (!self) {
                    return;
                }

                long long file_size = boost::filesystem::file_size(file_path);

                std::string sha256_base64 = calc_sha256_base64(file_path);

                wxGetApp().CallAfter([weak_self, download_url, file_name, file_size, sha256_base64]() {
                    auto self = weak_self.lock();
                    if (!self) {
                        return;
                    }

                    self->m_res_data["file_name"]   = file_name;
                    self->m_res_data["file_url"]    = download_url;
                    self->m_res_data["origin_size"] = file_size;
                    // checksum: SHA-256 digest as standard Base64, for Flutter-side integrity verification
                    self->m_res_data["checksum"]    = sha256_base64;

                    self->send_to_js();
                    self->finish_job();
                });
            });
        }
    }
    catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_Instance::handle_general_fail(int code, const wxString& msg)
{
    {
        m_status = code;
        m_msg    = msg.ToUTF8();
        send_to_js();
        finish_job();
    }

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
    if (is_Instance_illegal()) 
        return;
        

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
    wxGetApp().CallAfter([weak_self, str_res, json_str]()
    {
        {
            auto self = weak_self.lock();
            if (!self)
                return;

            // Original communication path: send via WebView postMessage
            if (self->m_webview && self->m_webview->GetRefData()) {
                WebView::RunScript(self->m_webview, str_res);
            }

            // Flutter debug: copy message to Flutter debug interface via WebSocket
            // This is independent of the original communication path above
            SSWCP::send_message_to_flutter(json_str);
        } 
    });

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
                // remove the old sub
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
    wxGetApp().m_foreground_change_subscribers.clear();

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

    auto& foreground_map = wxGetApp().m_foreground_change_subscribers;
    for (auto iter = foreground_map.begin(); iter != foreground_map.end();) {
        if (iter->first == m_webview) {
            iter = foreground_map.erase(iter);
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
        else
        {
            BOOST_LOG_TRIVIAL(warning) << "unknown command: " << cmd;
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

            if (!j_value.count("nozzle_diameters") ||!j_value.count("filament_vendor") || !j_value["filament_vendor"].is_array() ||
                !j_value.count("filament_type") ||
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

            // Validate optional nozzle_volume_types
            std::vector<std::string> nozzle_volume_types;
            const auto flow_status = SSWCPProtocol::parse_optional_flow_types(
                j_value, "nozzle_volume_types", j_value["filament_official"].size(), nozzle_volume_types);
            if (flow_status == SSWCPProtocol::OptionalFlowTypesStatus::Invalid) {
                // Degrade: treat malformed flow data as absent rather than aborting the
                // entire filament update, which would discard otherwise-valid entries.
                nozzle_volume_types.clear();
            }


            // save filament and update the gui filament
            auto& filaments = wxGetApp().preset_bundle->machine_filaments;
            auto& machine_nozzles = wxGetApp().preset_bundle->m_connect_machine_info_list;
            static auto tmp_filaments = filaments;

            if (m_first_connected) {
                tmp_filaments = filaments;
                m_first_connected = false;
            }

            machine_nozzles.clear();
            filaments.clear();

            size_t count = 0;
            for (size_t i = 0; i < j_value["filament_official"].size(); ++i) {
                bool is_official = j_value["filament_official"][i].get<bool>();
                ConnectMachineInfo machineData;
                if (/*is_official*/ true) {
                    std::string vendor   = j_value["filament_vendor"][i].get<std::string>();
                    std::string type     = j_value["filament_type"][i].get<std::string>();
                    std::string sub_type = j_value["filament_sub_type"][i].get<std::string>();

                    machineData.filament_type = type;

                    std::string name = "";

                    if (sub_type == "Support") {
                        name = vendor + " Support" + " For " + type;
                    } else {
                        name = vendor + " " + type + ((sub_type != "NONE" && sub_type != "") ? " " + sub_type : "");
                    }

                    int extruder = j_value["extruder_map_table"][i].get<int>();
                    machineData.index = static_cast<int>(i);
                    machineData.filament_info = name;

                    json::const_iterator multiColorIt = j_value.find("filament_color_multi");
                    if (multiColorIt != j_value.end() && multiColorIt->is_array() && multiColorIt->size() > i &&
                        (*multiColorIt)[i].is_object())
                    {
                        const json& multiColor = (*multiColorIt)[i];
                        json::const_iterator colorsIt = multiColor.find("colors");
                        if (colorsIt != multiColor.end() && colorsIt->is_array())
                        {
                            for (const json& colorJson : *colorsIt)
                            {
                                if (!colorJson.is_string())
                                    continue;

                                const std::string colorText = colorJson.get<std::string>();
                                const std::string normalized = FilamentColorUtils::NormalizeHexColor(colorText);
                                if (!normalized.empty())
                                    machineData.multiColors.emplace_back(normalized);
                            }
                        }

                        json::const_iterator modeIt = multiColor.find("mode");
                        if (machineData.multiColors.size() > 1 && modeIt != multiColor.end() && modeIt->is_number_integer())
                            machineData.colorMode = FilamentColorModeFromConfig(modeIt->get<int>());
                    }

                    if (j_value.count("filament_color_rgba") && j_value["filament_color_rgba"].is_array() &&
                        j_value["filament_color_rgba"].size() != 0) {
                        std::string str_color = "#" + j_value["filament_color_rgba"][i].get<std::string>();
                        const std::string normalizedColor = FilamentColorUtils::NormalizeHexColor(str_color, "#FFFFFF");
                        str_color = normalizedColor.empty() ? str_color : normalizedColor;
                        filaments.insert({int(i), {name, str_color}});
                        machineData.color_info = str_color;
                    } else {
                        if (j_value["filament_color"][i].is_number()) {
                            int                color = j_value["filament_color"][i].get<int>();
                            std::ostringstream oss;
                            oss << "#" << std::uppercase << std::setfill('0') << std::setw(6) << std::hex
                                << (color & 0x00FFFFFF); // lower 24

                            std::string str_color = oss.str();
                            filaments.insert({int(i), {name, str_color}});
                            machineData.color_info    = str_color;
                        } else {
                            std::string str_color = "#" + j_value["filament_color"][i].get<std::string>();
                            const std::string normalizedColor = FilamentColorUtils::NormalizeHexColor(str_color, "#FFFFFF");
                            str_color = normalizedColor.empty() ? str_color : normalizedColor;
                            filaments.insert({int(i), {name, str_color}});
                            machineData.color_info    = str_color;
                        }
                    }
                    if (machineData.multiColors.empty() && !machineData.color_info.empty())
                        machineData.multiColors.emplace_back(machineData.color_info);
                    if (machineData.multiColors.size() <= 1)
                        machineData.colorMode = FilamentColorMode::Segment;
                    if (j_value["nozzle_diameters"].is_array() && !j_value["nozzle_diameters"].empty())
                        machineData.nozzle_info = j_value["nozzle_diameters"][i].get<std::string>();
                    if (flow_status == SSWCPProtocol::OptionalFlowTypesStatus::Valid && i < nozzle_volume_types.size())
                        machineData.nozzle_volume_type = nozzle_volume_types[i];
                    machine_nozzles.push_back(machineData);
                }
            }

            bool need_load_preset = false;

            if (filaments.size() == 0 && tmp_filaments.size() != 0)
                need_load_preset = true;

            for (auto iter = filaments.begin(); iter != filaments.end(); iter++) {
                if (tmp_filaments.count(iter->first)) {
                    auto pair     = iter->second;
                    auto tmp_pair = tmp_filaments[iter->first];
                    if (pair.first == tmp_pair.first && pair.second == tmp_pair.second) {
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
    // 2.0.0 only mdns - snapmaker
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
        // 1) Wide Area Service Browsing: _services._dns-sd._udp.local will work for ~8s
        //    this query causes the AP/switch to build 224.0.0.251:5353 multicast forwarding, prompting the device to issue a notification
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
        }

        // the target service queries once every 2 seconds
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

        // query in thread
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

        // only support mdns search snapmaker,prusalink,todo add the other supports
        protocols.push_back("mdns");

        for (size_t i = 0; i < protocols.size(); ++i) {
            if (protocols[i] == "mdns") {
                std::vector<std::string> mdns_service_names;

                mdns_service_names.push_back("snapmaker");

                m_engines.clear();
                for (size_t i = 0; i < mdns_service_names.size(); ++i) {
                    m_engines.push_back(nullptr);
                }

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
                                                     {
                                                         std::string machine_cover = LOCALHOST_URL + std::to_string(wxGetApp().m_page_http_server.get_port()) + "/profiles/" +
                                                                                     vendor + "/" + machine_type + "_cover.png";
                                                         machine_data["cover"] = machine_cover;
                                                     } 
                                                 }
                                             }
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

                                         {
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
                                         }
                                     })
                                     .lookup();
                }

            } else {
                // todo the other machine found 
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

                        // wcp sub
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
    } else if (m_cmd == "sw_StartLocalPrint") {
        sw_StartLocalPrint();
    } else if (m_cmd == "sw_MachineHeartbeat") {
        sw_MachineHeartbeat();
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
    } else if (m_cmd == "sw_UploadAsyncTimelapseInstance") {
        sw_UploadAsyncTimelapseInstance();
    } else if (m_cmd == "sw_DeleteCameraTimelapse") {
        sw_DeleteCameraTimelapse();
    } else if (m_cmd == "sw_GetCameraTimelapseInstance") {
        sw_GetCameraTimelapseInstance();
    } else if (m_cmd == "sw_ServerClientManagerSetUserinfo") {
        sw_ServerClientManagerSetUserinfo();
    } else if (m_cmd == "sw_DefectDetactionConfig"){
        sw_DefectDetactionConfig();
    } else if (m_cmd == "sw_PrinterDefectDetection") {
        sw_PrinterDefectDetection();
    }
    else if (m_cmd == GET_DEVICEDATA_STORAGESPACE) {
        sw_GetDeviceDataStorageSpace();
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

void SSWCP_MachineOption_Instance::sw_StartLocalPrint()
{
    if (!m_param_data.count("type") || !m_param_data.count("path")) {
        BOOST_LOG_TRIVIAL(error) << "[WCP] sw_StartLocalPrint: param [type] or [path] required!";
        handle_general_fail(-1, "param [type] or [path] required!");
        return;
    }

    std::shared_ptr<PrintHost> host = nullptr;
    wxGetApp().get_connect_host(host);

    if (!host) {
        BOOST_LOG_TRIVIAL(error) << "[WCP] sw_StartLocalPrint: Can't find the active machine";
        handle_general_fail(-1, "Can't find the active machine");
        return;
    }

    json items = m_param_data;

    auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
    host->async_start_local_print(items, [weak_self](const json& response) {
        auto self = weak_self.lock();
        if (self) {
            SSWCP_Instance::on_mqtt_msg_arrived(self, response);
        }
    });
}

void SSWCP_MachineOption_Instance::sw_MachineHeartbeat()
{
    std::shared_ptr<PrintHost> host = nullptr;
    wxGetApp().get_connect_host(host);

    if (!host) {
        BOOST_LOG_TRIVIAL(error) << "[WCP] sw_MachineHeartbeat: Can't find the active machine";
        handle_general_fail(-1, "Can't find the active machine");
        return;
    }

    auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
    host->async_machine_heartbeat(m_param_data, [weak_self](const json& response) {
        auto self = weak_self.lock();
        if (self) {
            SSWCP_Instance::on_mqtt_msg_arrived(self, response);
        }
    });
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
            
            wxFileDialog picFileDialog(nullptr,
                                        L("select file"),                    // title
                                        "",                                  // default path
                                        "",                                  // default filename
                                        wildcard,                            // filter 
                                        wxFD_OPEN | wxFD_OVERWRITE_PROMPT);  // style

            if (picFileDialog.ShowModal() == wxID_CANCEL) {                
                handle_general_fail();
                return;
            }
            
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

        //get the default name from url
        wxString filename   = "";
        if (!m_param_data.count("filename")) {
            size_t last_slash = download_url.find_last_of("/");
            if (last_slash != std::string::npos) {
                filename = download_url.substr(last_slash + 1);
            }
        } else {
            filename = wxString::FromUTF8(m_param_data["filename"].get<std::string>());
        }

        // get the file extension
        wxString extension;
        size_t      dot_pos = filename.find_last_of(".");
        if (dot_pos != std::string::npos) {
            extension = filename.substr(dot_pos + 1);
        }

        // create filter
        wxString wildcard;
        if (!extension.empty()) {
            // "PNG files (*.png)|*.png|All files (*.*)|*.*"
            wildcard = wxString::Format("%s files (*.%s)|*.%s|All files (*.*)|*.*", extension, extension, extension);
        } else {
            wildcard = "All files (*.*)|*.*";
        }

        wxGetApp().CallAfter([filename, extension, wildcard, this, download_url]() {
            
            wxFileDialog saveFileDialog(nullptr,
                                        L("Save file"),                     // title
                                        "",                                 // default path
                                        filename,                           // default filename
                                        wildcard,                           // filter
                                        wxFD_SAVE | wxFD_OVERWRITE_PROMPT); // style

            if (saveFileDialog.ShowModal() == wxID_CANCEL) {
                // use cancel download
                handle_general_fail();
                return;
            }
            
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
                // BBS: Use SafeEndModal to prevent duplicate EndModal calls
                if(dialog->is_finish()){
                    dialog->SafeEndModal(wxID_OK);
                }else{
                    dialog->SafeEndModal(wxID_CANCEL);
                }

                // Dispatch the post-close action selected by params.event.
                // Handlers run via CallAfter so the webview modal finishes
                // tearing down first. Add new cases alongside new enum values.
                int event_int = 0;
                if (m_param_data.count("event") && !m_param_data["event"].is_null()) {
                    event_int = m_param_data["event"].get<int>();
                }
                const auto event = static_cast<SSWCPProtocol::FinishFilamentMappingEvent>(event_int);
                switch (event) {
                case SSWCPProtocol::FinishFilamentMappingEvent::CustomFlowRegroup:
                    on_finish_filament_mapping_custom_flow_regroup();
                    break;
                case SSWCPProtocol::FinishFilamentMappingEvent::None:
                    break;
                }
            }
        }
    } catch (std::exception& e) {
        handle_general_fail();
    }
}
void SSWCP_MachineOption_Instance::on_finish_filament_mapping_custom_flow_regroup()
{
    // event 1: open the "custom filament flow regrouping" dialog once the
    // preprint webview has finished closing. Only meaningful for printers that
    // declare a high-flow nozzle. Scheduled via CallAfter to avoid running a
    // modal dialog nested inside the webview's own modal event loop.
    if (!GUI::FlowType::printer_supports_high_flow())
        return;
    wxGetApp().CallAfter([]() {
        wxWindow *parent = static_cast<wxWindow *>(wxGetApp().mainframe);
        GUI::FilamentGroupDialog dlg(parent);
        // Confirming the custom flow regrouping proceeds straight to slicing (the
        // same action as the slice button); cancelling changes nothing.
        if (dlg.ShowModal() == wxID_OK && wxGetApp().mainframe)
            wxGetApp().mainframe->start_slice();
    });
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

        if (!boost::filesystem::exists(filename) || !boost::filesystem::is_regular_file(filename)) {
            handle_general_fail();
            return;
        }

        auto* print = wxGetApp().plater()->get_partplate_list().get_curr_plate()->fff_print();
        auto& config = print->config();
        auto full_config = print->full_print_config();
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
                return 0;
            }

            auto colorSize = oriclr.size();//7 or 9
            for (auto i = 1; i < colorSize; i++)
            {
                if (oriclr[colorSize - i] - '0' >= 0 && oriclr[colorSize - i] - '0' <= 9) {
                    res += std::pow(16, i - 1) * (oriclr[colorSize - i] - '0');
                } else {
                    res += std::pow(16, i - 1) * (oriclr[colorSize - i] - 'A' + 10);
                }
            }
            return res;
        };

        // filament colour
        if (config.has("filament_colour")) {
            std::vector<std::string> filament_color = config.option<ConfigOptionStrings>("filament_colour")->values;
            const ConfigOptionStrings* filament_multi_colors = nullptr;
            if (config.has("filament_multi_colors"))
                filament_multi_colors = config.option<ConfigOptionStrings>("filament_multi_colors");

            const ConfigOptionInts* filament_colour_modes = nullptr;
            if (config.has("filament_colour_mode"))
                filament_colour_modes = config.option<ConfigOptionInts>("filament_colour_mode");

            std::vector<long long> number_res(filament_color.size(), 0);
            std::vector<std::string> str_res(filament_color.size());
            json multi_color_res = json::array();
            for (size_t i = 0; i < filament_color.size(); ++i) {
                number_res[i] = color_to_int(filament_color[i]);
                str_res[i] = filament_color[i];

                const bool has_multi_colors = filament_multi_colors != nullptr && filament_multi_colors->values.size() > i;
                const bool has_mode = filament_colour_modes != nullptr && filament_colour_modes->values.size() > i;
                const std::string multi_colors = has_multi_colors ? filament_multi_colors->values[i] : std::string();
                FilamentColorMode colorMode = FilamentColorMode::Segment;
                if (has_mode)
                    colorMode = FilamentColorModeFromConfig(filament_colour_modes->values[i]);
                multi_color_res.push_back(
                    FilamentColorUtils::BuildPreprintColorMultiItem(multi_colors, colorMode, filament_color[i]));
            }

            response["filament_color"] = number_res;
            response["filament_color_rgba"] = str_res;
            response["filament_color_multi"] = multi_color_res;
        }
        // filament type
        if (full_config.has("filament_type")) {
            std::vector<std::string> filament_types;
            size_t                   filament_count = full_config.option<ConfigOptionStrings>("filament_type")->values.size();
            if (full_config.has("filament_colour")) {
                filament_count = std::max(filament_count, full_config.option<ConfigOptionStrings>("filament_colour")->values.size());
            }

            filament_types.reserve(filament_count);
            for (size_t i = 0; i < filament_count; ++i) {
                std::string displayed_filament_type;
                std::string filament_type = full_config.get_filament_type(displayed_filament_type, int(i));
                boost::trim(filament_type);
                filament_types.emplace_back(std::move(filament_type));
            }
            response["filament_type"] = filament_types;
        }

        // filament volume type (flow type per filament)
        {
            size_t fvt_count = 0;
            if (const auto* ftype_opt = full_config.option<ConfigOptionStrings>("filament_type"))
                fvt_count = std::max(fvt_count, ftype_opt->values.size());
            if (const auto* fcolour_opt = full_config.option<ConfigOptionStrings>("filament_colour"))
                fvt_count = std::max(fvt_count, fcolour_opt->values.size());
            if (fvt_count == 0) fvt_count = 1;

            const auto* volume_types = full_config.option<ConfigOptionEnumsGeneric>("filament_volume_type");
            const std::vector<int>* fvt_values = volume_types ? &volume_types->values : nullptr;
            response["filament_volume_type"] = SSWCPProtocol::build_filament_volume_types(fvt_values, fvt_count);
        }

        // nozzle flow type (per-nozzle Standard/High Flow selection from the
        // project config "nozzle_volume_types", mirrored in AppConfig "nozzle_volume_types")
        response["nozzle_volume_types"] = GUI::FlowType::nozzle_volume_types();

        if (full_config.has("nozzle_diameter")) {
            const auto *opt_nozzle_diameters = full_config.option<ConfigOptionFloats>("nozzle_diameter");
            if (opt_nozzle_diameters != nullptr) {
                std::vector<std::string> nozzle_diameters;
                nozzle_diameters.reserve(opt_nozzle_diameters->values.size());
                for (double diameter : opt_nozzle_diameters->values) {
                    std::ostringstream stream;
                    stream << std::fixed << std::setprecision(1) << diameter;
                    nozzle_diameters.emplace_back(stream.str());
                }
                response["nozzle_diameters"] = nozzle_diameters;
            }
        }

        // filament used
        if (config.has("filament_density")) {
            auto filament_density = config.option<ConfigOptionFloats>("filament_density")->values;

            std::vector<double> filament_used_g(filament_density.size(), 0);
            double              total_weight = 0;
            for (const auto& pr : result.print_statistics.total_volumes_per_extruder) {
                if (pr.first >= filament_density.size()) {
                    continue;
                }
                filament_used_g[pr.first] = filament_density[pr.first] * pr.second * 0.001;
                total_weight += filament_used_g[pr.first];
            }

            response["filament_weight"] = filament_used_g;
            response["filament_weight_total"] = total_weight;
        }

        // filament used mm (length)
        if (config.has("filament_diameter")) {
            auto filament_diameter_opt = config.option<ConfigOptionFloats>("filament_diameter");
            if (!filament_diameter_opt) {
                handle_general_fail();
                return;
            }
            auto filament_diameter = filament_diameter_opt->values;

            std::vector<double> filament_used_mm(filament_diameter.size(), 0);
            for (const auto& pr : result.print_statistics.total_volumes_per_extruder) {
                if (pr.first >= filament_diameter.size()) {
                    continue;
                }
                auto diameter = static_cast<double>(filament_diameter[pr.first]);
                if (diameter > 0) {
                    filament_used_mm[pr.first] = pr.second / (M_PI * (diameter * 0.5) * (diameter * 0.5));
                }
            }
            response["filament_used_mm"] = filament_used_mm;
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

        //nozzle info
        PartPlate*  cur_plate        = wxGetApp().plater()->get_partplate_list().get_curr_plate();
        if (cur_plate)
        {
            auto*  nozzle_opt = cur_plate->fff_print()->config().option<ConfigOptionFloats>("nozzle_diameter");
            std::vector<std::string> nozzle_list;
            if (nozzle_opt) {
                for (float d : nozzle_opt->values) {
                    nozzle_list.push_back(std::abs(d - 0.2f) < 1e-5f ? "0.2" :
                                          std::abs(d - 0.4f) < 1e-5f ? "0.4" :
                                          std::abs(d - 0.6f) < 1e-5f ? "0.6" :
                                          std::abs(d - 0.8f) < 1e-5f ? "0.8" :
                                                                       std::to_string(d));
                }

                response["nozzle_info"] = nozzle_list;
            }
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
    std::shared_ptr<PrintHost> host = nullptr;
    wxGetApp().get_connect_host(host);

    if (!host) {
        BOOST_LOG_TRIVIAL(error) << "[WCP] sw_ControlPurifier: no active machine connected";
        handle_general_fail(-1, "Connection lost!");
        return;
    }

    auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
    host->async_controlPurifier(m_param_data, [weak_self](const json& response) {
        auto self = weak_self.lock();
        if (self) {
            SSWCP_Instance::on_mqtt_msg_arrived(self, response);
        }
    });
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
void SSWCP_MachineOption_Instance::sw_UploadAsyncTimelapseInstance()
{
    std::shared_ptr<PrintHost> host = nullptr;
    wxGetApp().get_connect_host(host);

    if (!host) {
        handle_general_fail(-1, "Connection lost!");
        return;
    }

    auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
    host->async_upload_timelapse_instance(m_param_data, [weak_self](const json& response) {
        auto self = weak_self.lock();
        if (self) {
            SSWCP_Instance::on_mqtt_msg_arrived(self, response);
        }
    });
}
void SSWCP_MachineOption_Instance::CmdForwarding()
{
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail(-1, "Connection lost!");
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->test_async_wcp_mqtt_moonraker(m_param_data, [weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

void SSWCP_MachineOption_Instance::sw_GetCameraTimelapseInstance()
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
void SSWCP_MachineOption_Instance::sw_GetDeviceDataStorageSpace()
{

    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail(-1, "Connection lost!");
            return;
        }

        auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
        host->async_get_userdata_space(m_param_data, [weak_self](const json& response) {
            auto self = weak_self.lock();
            if (self) {
                SSWCP_Instance::on_mqtt_msg_arrived(self, response);
            }
        });
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

// Forward decl: defined after g_active_ctxs. Used here to abort an in-progress
// PC download when the file is deleted from the device.
namespace { void cancel_active_timelapse_by_date_index(const std::string& date_index); }

void SSWCP_MachineOption_Instance::sw_DeleteCameraTimelapse()
{
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            handle_general_fail(-1, "Connection lost!");
            return;
        }

        // If this file is currently downloading to PC, abort that download too
        // (deleting a file mid-download must terminate the task, not leave it).
        std::string date_index = m_param_data.value("date_index", "");
        if (!date_index.empty()) {
            cancel_active_timelapse_by_date_index(date_index);
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

void SSWCP_MachineOption_Instance::sw_PrinterDefectDetection()
{
    std::shared_ptr<PrintHost> host = nullptr;
    wxGetApp().get_connect_host(host);

    if (!host) {
        BOOST_LOG_TRIVIAL(error) << "[WCP] sw_PrinterDefectDetection: no active machine connected";
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
    if (m_cmd == "sw_SubscribeForegroundChange") {
        if (m_event_id != "") {
            m_header["event_id"] = m_event_id;
            sw_SubscribeForegroundChange();
        }
        return;
    }
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
                        
                        json req_body;
                        req_body["jsonrpc"] = "2.0",
                        req_body["method"]  = "server.client_manager.request_pin_code";
                        req_body["params"] = json::object();
                        req_body["params"]["userid"] = userid;
                        req_body["params"]["nickname"] = nickname;
                        Moonraker_Mqtt::SequenceGenerator generator;
                        req_body["id"]                 = generator.generate_seq_id();
                        
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

void SSWCP_MachineConnect_Instance::sw_SubscribeForegroundChange()
{
    auto self = shared_from_this();
    wxGetApp().m_foreground_change_subscribers[m_webview] = self;
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
            // todo : add cin-type
            //            
            p_config->option<ConfigOptionEnum<PrintHostType>>("host_type")->value = type;            
            p_config->set("print_host", ip + (port == -1 ? "" : std::to_string(port)));
            std::shared_ptr<PrintHost> host(PrintHost::get_print_host(&wxGetApp().preset_bundle->printers.get_edited_preset().config));

            if (!host) {                
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
                        m_status = 1;
                        m_msg    = msg.c_str();
                        send_to_js();
                    }
                    finish_job();
                });
            }
        } else {
            
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
    } else if (m_cmd == "sw_AskUserLogin") {
        sw_AskUserLogin();
    } else if (m_cmd == "sw_UserLogout") {
        sw_UserLogout();
    } else if (m_cmd == "sw_GetUserLoginState") {
        sw_GetUserLoginState();
    } else if (m_cmd == "sw_SubscribeUserLoginState") {
        sw_SubscribeUserLoginState();
    }
    else if (m_cmd == UPDATE_PRIVACY_STATUS) {
        sw_SubUserUpdatePrivacy();
    } else if (m_cmd == GET_PRIVACY_STATUS) {
        sw_GetUserUpdatePrivacy();
    } else if (m_cmd == DOWNLOAD_FILE_AND_OPEN) {
        sw_DownloadFileAndOpen();
    } else if (m_cmd == DOWN_LOAD_FILE) {
        sw_DownLoadFile();
    } else if (m_cmd == SUBSCRIBE_DOWNLOAD_STATE) {
        sw_SubscribeDownloadState();
    } else if (m_cmd == UNSUBSCRIBE_DOWNLOAD_STATE) {
        sw_UnsubscribeDownloadState();
    } else if (m_cmd == CANCEL_DOWNLOAD) {
        sw_CancelDownload();
    } else if (m_cmd == FILE_VIEW) {
        sw_FileView();
    } else if (m_cmd == OPEN_TIMELAPSE_FOLDER) {
        sw_OpenTimelapseFolder();
    } else if (m_cmd == GET_FILES_FROM_DIR) {
        sw_GetFilesFromDir();
    } else if (m_cmd == NOTIFY_UPLOAD_TIMELASPE) {
        sw_NotifyUploadTimelaspe();
    }
    else {
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

void SSWCP_UserLogin_Instance::sw_AskUserLogin()
{
    auto weak_self = std::weak_ptr<SSWCP_Instance>(shared_from_this());
    wxGetApp().CallAfter([weak_self]() {
        auto self = weak_self.lock();
        if (!self)
            return;

        if (s_ask_dialog_showing) {
            auto already_queued = [&self](const std::weak_ptr<SSWCP_Instance>& waiter) {
                auto locked = waiter.lock();
                return locked && locked.get() == self.get();
            };
            if (!std::any_of(s_ask_waiters.begin(), s_ask_waiters.end(), already_queued))
                s_ask_waiters.push_back(self);
            return;
        }

        struct AskLoginDialogStateGuard
        {
            bool active = true;

            ~AskLoginDialogStateGuard()
            {
                if (active) {
                    s_ask_dialog_showing = false;
                    s_ask_waiters.clear();
                }
            }
        };

        bool                     login = false;
        AskLoginDialogStateGuard state_guard;
        s_ask_dialog_showing = true;
        s_ask_waiters.push_back(self);

        SMAskUserLoginDialog dlg(wxGetApp().mainframe);
        dlg.SetKeepAliveCallback([]() {
            for (auto& weak : s_ask_waiters)
                if (auto alive = weak.lock())
                    SSWCP::renew_instance_timeout(alive.get());
        });
        login = (dlg.ShowModal() == wxID_OK);

        s_ask_dialog_showing = false;
        json data;
        data["result"] = login ? "login" : "cancel";
        for (auto& weak : s_ask_waiters) {
            auto waiter = weak.lock();
            if (!waiter)
                continue;
            waiter->m_res_data = data;
            waiter->send_to_js();
            waiter->finish_job();
        }
        s_ask_waiters.clear();
        state_guard.active = false;

        if (login)
            wxGetApp().sm_request_login(true);
    });
}

bool SSWCP_UserLogin_Instance::s_ask_dialog_showing = false;
std::vector<std::weak_ptr<SSWCP_Instance>> SSWCP_UserLogin_Instance::s_ask_waiters;

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
void SSWCP_UserLogin_Instance::sw_GetUserUpdatePrivacy()
{
    json data;
    auto isAgree     = wxGetApp().app_config->get("app", PRIVACY_POLICY_FLAGS);
    bool isUserAgree               = false;

    if (isAgree == "true")
        isUserAgree = true;

    data[PRIVACY_POLICY_FLAGS] = isUserAgree;

    m_res_data = data;
    send_to_js();
    finish_job();

}

void SSWCP_UserLogin_Instance::sw_DownloadFileAndOpen()
{
    try {
        std::string fileName = m_param_data.count("file_name") ? m_param_data["file_name"].get<std::string>() : "";
        std::string fileUrl  = m_param_data.count("file_url") ? m_param_data["file_url"].get<std::string>() : "";

        if (fileUrl.empty() || fileName.empty()) {
            handle_general_fail(-1, wxString::FromUTF8("file_url and file_name are required"));
            return;
        }

        // Use Download Manager
        DownloadManager* download_mgr = wxGetApp().download_manager();
        if (!download_mgr) {
            handle_general_fail(-1, wxString::FromUTF8("Download Manager not available"));
            return;
        }

        // WebView script message runs inside the webview event handler; calling ShowModal() synchronously
        // (via GenericDownloadDialog in downloadOpenProject) causes re-entrancy / crashes on Windows.
        // Defer to the next event-loop iteration — same pattern as sw_UserLogin().
        std::shared_ptr<SSWCP_UserLogin_Instance> self =
            std::static_pointer_cast<SSWCP_UserLogin_Instance>(shared_from_this());
        wxGetApp().CallAfter([self, fileUrl, fileName]() {
            if (!wxGetApp().mainframe) {
                self->handle_general_fail(-1, wxString::FromUTF8("Main window not available"));
                return;
            }
            try {
                wxGetApp().mainframe->downloadOpenProject(fileUrl, fileName, "");
                self->m_status = 0;
                self->m_msg    = "success";
                self->send_to_js();
                self->finish_job();
            } catch (const std::exception& e) {
                self->handle_general_fail(-1, wxString::FromUTF8(e.what()));
            }
        });

    } catch (const std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

void SSWCP_UserLogin_Instance::sw_DownloadFileEx() {
    try {
        std::string fileName = m_param_data.count("file_name") ? m_param_data["file_name"].get<std::string>() : "";
        std::string fileUrl  = m_param_data.count("file_url") ? m_param_data["file_url"].get<std::string>() : "";

        if (fileUrl.empty() || fileName.empty()) {
            handle_general_fail(-1, "file_url and file_name are required");
            return;
        }

        // Use Download Manager
        DownloadManager* download_mgr = wxGetApp().download_manager();
        if (!download_mgr) {
            handle_general_fail(-1, "Download Manager not available");
            return;
        }
        size_t task_id = download_mgr->start_wcp_download(fileUrl,
                                                          fileName,
                                                          shared_from_this(),
                                                          true);

        json response;
        response["task_id"] = task_id;
        response["file_name"] = fileName;
        response["file_url"] = fileUrl;
        m_res_data = response;
        m_status = 0;
        m_msg = "success";
        send_to_js();

    } catch (std::exception& e) {
        handle_general_fail(-1, e.what());
    }
}

namespace {

// WAN upload orchestration helpers for sw_DownLoadFile. Kept in an anonymous
// namespace so sw_DownLoadFile stays focused on param parsing + queue setup;
// these own the device-upload trigger, notification routing and subscription
// lifecycle. LAN downloads never touch them.
struct TimelapseFileEntry {
    std::string mode;
    std::string file_name;
    std::string sn;
    std::string date_index;
    std::string url;        // lan: required from caller; wan: filled after upload callback
    size_t      file_size = 0;
};

struct TimelapseQueueContext {
    std::vector<TimelapseFileEntry>                      files;
    std::string                                          save_path;
    TimelapseDownloadPopup*                              popup            = nullptr;
    int                                                  total_count      = 0;
    int                                                  row_offset       = 0;  // first row index in the shared popup
    std::string                                          sn;               // device SN (files[0].sn)
    int                                                  current_index    = 0;
    int                                                  completed_count  = 0;
    int                                                  failed_count     = 0;
    size_t                                               current_task_id  = 0;
    bool                                                 cancelled        = false;
    std::set<int>                                        skipped_indices;
    std::set<int>                                        pushed_indices;  // idx already pushed to subscribers
    std::function<void()>                                start_next;
    std::weak_ptr<SSWCP_UserLogin_Instance>              wcp_self;
    std::vector<json>                                    completed_files;
    std::function<void(int idx, const std::string& url)> kickoff_download;
    bool                                                 is_wan             = false;
    bool                                                 waiting_for_upload = false;
    std::string                                          wan_sub_hash;
    std::unordered_map<std::string, int>                 pending_wan_date_to_idx;
};

// WAN: sw_DownLoadFile registers each file by date_index; sw_NotifyUploadTimelaspe
// (flutter forwards device upload url) looks it up to kickoff the download.
struct WanPendingEntry { std::shared_ptr<TimelapseQueueContext> ctx; int idx; };

static std::unordered_map<std::string, WanPendingEntry> g_wan_pending;
static std::mutex g_wan_pending_mtx;
static std::unordered_map<std::string, std::string> g_pending_upload_notifications;

// Shared download popup (one at a time) + active batch list + date_index dedup.
// Multiple sw_DownLoadFile calls append to the same popup; a file whose
// date_index is already downloading is skipped to prevent duplicate downloads.
static TimelapseDownloadPopup*                           g_timelapse_popup = nullptr;
static std::vector<std::shared_ptr<TimelapseQueueContext>> g_active_ctxs;
static std::set<std::string>                             g_active_date_indices;
static std::mutex                                        g_active_ctx_mtx;

TimelapseDownloadPopup* get_or_create_timelapse_popup() {
    if (!g_timelapse_popup || g_timelapse_popup->IsBeingDeleted()) {
        g_timelapse_popup = new TimelapseDownloadPopup(wxGetApp().mainframe);
        // Null the shared pointer on destroy so it can't dangle; otherwise the
        // next call here reads freed memory via IsBeingDeleted(). wxEVT_DESTROY
        // is the one choke point every teardown path hits.
        TimelapseDownloadPopup* self = g_timelapse_popup;
        self->Bind(wxEVT_DESTROY, [self](wxWindowDestroyEvent& e) {
            // wxWindowDestroyEvent is a wxCommandEvent, so child (row/divider)
            // destroys propagate here too — act only on the popup's own.
            if (e.GetEventObject() != self) { e.Skip(); return; }
            {
                std::lock_guard<std::mutex> lk(g_active_ctx_mtx);
                for (auto& c : g_active_ctxs) {
                    if (c->popup == self) { c->popup = nullptr; }
                }
            }
            // A newer popup may already own the global (deferred delete).
            if (g_timelapse_popup == self) { g_timelapse_popup = nullptr; }
            e.Skip();
        });
    }
    return g_timelapse_popup;
}
// Remove a date_index from the active set (called when a file reaches a
// terminal state, so the same file can be downloaded again later).
static void release_date_index(const std::string& date_index) {
    if (date_index.empty()) { return; }
    std::lock_guard<std::mutex> lk(g_active_ctx_mtx);
    g_active_date_indices.erase(date_index);
}
static std::mutex g_push_state_mtx;
void push_file_state(std::shared_ptr<TimelapseQueueContext> ctx, int idx,
                     const std::string& state, const std::string& save_path = "")
{
    std::lock_guard<std::mutex> lk(g_push_state_mtx);
    if (!ctx || ctx->pushed_indices.count(idx)) {
        return;
    }
    ctx->pushed_indices.insert(idx);
    const auto& f = ctx->files[idx];
    SSWCP_UserLogin_Instance::push_timelapse_state(
        f.sn, f.file_name, f.url, f.date_index, save_path, state);
}

// Abort the in-progress download of a file (matched by date_index) — used when
// the file is deleted from the device while still downloading to PC.
void cancel_active_timelapse_by_date_index(const std::string& date_index)
{
    if (date_index.empty()) { return; }
    std::vector<std::shared_ptr<TimelapseQueueContext>> ctxs;
    {
        std::lock_guard<std::mutex> lk(g_active_ctx_mtx);
        ctxs = g_active_ctxs;
    }
    for (auto& ctx : ctxs) {
        for (int i = 0; i < ctx->total_count; ++i) {
            if (ctx->files[i].date_index != date_index) {
                continue;
            }
            ctx->skipped_indices.insert(i);
            push_file_state(ctx, i, "cancelled");
            // WAN: drop from g_wan_pending so a late upload notification can't
            // re-kickoff this file.
            if (ctx->is_wan) {
                std::lock_guard<std::mutex> lk(g_wan_pending_mtx);
                g_wan_pending.erase(date_index);
            }
            // Stop the active HTTP download if this is the one in flight.
            if (i == ctx->current_index && ctx->current_task_id != 0) {
                DownloadManager::getInstance().cancel_download(ctx->current_task_id);
                ctx->current_task_id = 0;
            }
            if (ctx->popup && !ctx->popup->IsBeingDeleted()) {
                ctx->popup->mark_task_cancelled(ctx->row_offset + i);
            }
            release_date_index(date_index);
            return;
        }
    }
}

// Cancel ALL active timelapse downloads, marking each unfinished file as
// failed ("Download Failed"). Used when the user switches device — every
// in-flight download becomes stale and must stop.
void cancel_all_active_timelapse()
{
    std::vector<std::shared_ptr<TimelapseQueueContext>> to_cancel;
    {
        std::lock_guard<std::mutex> lk(g_active_ctx_mtx);
        to_cancel = g_active_ctxs;
    }
    for (auto& ctx : to_cancel) {
        ctx->cancelled = true;
        if (ctx->current_task_id != 0) {
            DownloadManager::getInstance().cancel_download(ctx->current_task_id);
            ctx->current_task_id = 0;
        }
        if (ctx->is_wan) {
            std::lock_guard<std::mutex> lk(g_wan_pending_mtx);
            for (int i = 0; i < ctx->total_count; ++i) {
                g_wan_pending.erase(ctx->files[i].date_index);
            }
        }
        for (int i = 0; i < ctx->total_count; ++i) {
            if (ctx->pushed_indices.count(i)) {
                continue;   // already reached a terminal state
            }
            ctx->skipped_indices.insert(i);
            push_file_state(ctx, i, "failed");
            release_date_index(ctx->files[i].date_index);
            if (ctx->popup && !ctx->popup->IsBeingDeleted()) {
                ctx->popup->mark_task_error(ctx->row_offset + i,
                    std::string(_L("Device disconnected.").ToUTF8().data()));
            }
        }
    }
}

// Tear down the WAN upload-notification subscription. Safe to call when not in
// WAN mode or when no subscription is active.
void unsubscribe_wan_upload(std::shared_ptr<TimelapseQueueContext> ctx)
{
    if (!ctx->is_wan || ctx->wan_sub_hash.empty()) {
        return;
    }
    std::shared_ptr<PrintHost> host = nullptr;
    wxGetApp().get_connect_host(host);
    if (host) {
        host->async_unsubscribe_machine_info(ctx->wan_sub_hash, [](const json&) {});
    }
    ctx->wan_sub_hash.clear();
    ctx->pending_wan_date_to_idx.clear();
}

// Route one batch of notify_camera_upload_timelapse items (main thread).
void handle_wan_upload_notification(std::shared_ptr<TimelapseQueueContext> ctx,
                                    const std::vector<json>& items)
{
    if (ctx->cancelled) {
        return;
    }
    for (const auto& item : items) {
        std::string date_index = item.value("date_index", "");
        std::string state      = item.value("state", "");
        if (date_index.empty()) {
            continue;
        }

        auto it = ctx->pending_wan_date_to_idx.find(date_index);
        if (it == ctx->pending_wan_date_to_idx.end()) {
            continue;
        }
        int idx = it->second;

        if (state == "uploading") {
            continue;
        }

        // Terminal state — remove from pending
        ctx->pending_wan_date_to_idx.erase(it);

        if (state == "success") {
            std::string url = item.value("url", "");
            if (url.empty()) {
                BOOST_LOG_TRIVIAL(error)
                    << "DownloadFile[WAN]: success state but no url for idx="
                    << idx << " date_index=" << date_index;
                if (ctx->popup && !ctx->popup->IsBeingDeleted()) {
                    ctx->popup->mark_task_error(idx, "Empty url in upload notification");
                }
                ctx->failed_count++;
                ctx->waiting_for_upload = false;
                ctx->current_index = idx + 1;
                if (ctx->start_next) {
                    ctx->start_next();
                }
                continue;
            }

            ctx->files[idx].url = url;

            if (ctx->cancelled || ctx->skipped_indices.count(idx)) {
                if (ctx->popup && !ctx->popup->IsBeingDeleted()) {
                    ctx->popup->mark_task_cancelled(idx);
                }
                ctx->waiting_for_upload = false;
                ctx->current_index = idx + 1;
                if (ctx->start_next) {
                    ctx->start_next();
                }
                continue;
            }
            if (idx != ctx->current_index) {
                BOOST_LOG_TRIVIAL(warning)
                    << "DownloadFile[WAN]: notification idx=" << idx
                    << " != current_index=" << ctx->current_index
                    << " (out-of-order notification, skipping)";
                continue;
            }
            ctx->waiting_for_upload = false;
            ctx->kickoff_download(idx, url);
        } else if (state == "failed") {
            std::string message = item.value("message", "upload failed");
            BOOST_LOG_TRIVIAL(warning)
                << "DownloadFile[WAN]: upload failed idx=" << idx
                << " date_index=" << date_index << " message=" << message;
            if (ctx->popup && !ctx->popup->IsBeingDeleted()) {
                ctx->popup->mark_task_error(idx, message);
            }
            ctx->failed_count++;
            ctx->waiting_for_upload = false;
            ctx->current_index = idx + 1;
            if (ctx->start_next) {
                ctx->start_next();
            }
        }
    }
}

// Subscribe to notify_camera_upload_timelapse for the whole batch. No-op for LAN.
// Returns false (after reporting) if WAN but the device connection is lost, so the
// caller can abort the orchestration; true otherwise.
bool subscribe_wan_upload_notifications(std::shared_ptr<TimelapseQueueContext> ctx)
{
    if (!ctx->is_wan) {
        return true;
    }
    std::shared_ptr<PrintHost> host = nullptr;
    wxGetApp().get_connect_host(host);
    if (!host) {
        wxGetApp().CallAfter([ctx]() {
            if (ctx->popup && !ctx->popup->IsBeingDeleted()) {
                ctx->popup->Close();
            }
            if (auto wcp = ctx->wcp_self.lock()) {
                wcp->m_res_data = json::object();
                wcp->m_status   = -1;
                wcp->m_msg      = "Connection lost";
                wcp->send_to_js();
                wcp->finish_job();
            }
        });
        return false;
    }

    ctx->wan_sub_hash = "wan_dl_" +
        std::to_string(reinterpret_cast<uintptr_t>(ctx.get()));

    host->async_subscribe_machine_info(ctx->wan_sub_hash,
        [ctx](const json& notification) {
            // MQTT thread: only filter by method + shape, then forward every
            // candidate item to the main thread. All map mutations and popup
            // calls happen on main thread to avoid races.
            std::string method = notification.value("method", "");
            if (method != "notify_camera_upload_timelapse") {
                return;
            }
            if (!notification.count("data") || !notification["data"].is_array()) {
                return;
            }
            std::vector<json> items_copy;
            for (const auto& item : notification["data"]) {
                if (item.is_object()) {
                    items_copy.push_back(item);
                }
            }
            if (items_copy.empty()) {
                return;
            }
            wxGetApp().CallAfter([ctx, items = std::move(items_copy)]() {
                handle_wan_upload_notification(ctx, items);
            });
        });
    return true;
}

// Trigger the device-side async upload for one file; the subscription registered
// by subscribe_wan_upload_notifications drives kickoff_download on success.
void trigger_async_upload_and_wait(std::shared_ptr<TimelapseQueueContext> ctx, int idx)
{
    if (ctx->waiting_for_upload) {
        return;
    }
    std::shared_ptr<PrintHost> host = nullptr;
    wxGetApp().get_connect_host(host);
    if (!host) {
        if (ctx->popup && !ctx->popup->IsBeingDeleted()) {
            ctx->popup->mark_task_error(idx, "Connection lost");
        }
        ctx->failed_count++;
        ctx->current_index++;
        if (ctx->start_next) {
            ctx->start_next();
        }
        return;
    }

    const auto& info = ctx->files[idx];
    ctx->waiting_for_upload = true;
    ctx->pending_wan_date_to_idx[info.date_index] = idx;

    json upload_params;
    upload_params["date_index"] = info.date_index;
    upload_params["type"]       = "video";

    host->async_upload_timelapse_instance(upload_params, [](const json&) {});
}

} // anonymous namespace

void SSWCP_UserLogin_Instance::sw_NotifyUploadTimelaspe()
{
    // Flutter forwards each device notify_camera_upload_timelapse entry here.
    // Accept either an array of entries or a single entry object:
    //   { date_index, type, state, url, percent? }  (or [{...}, ...])
    json entries;
    if (m_param_data.is_array()) {
        entries = m_param_data;
    } else if (m_param_data.is_object()) {
        entries = json::array({ m_param_data });
    } else {
        handle_general_fail(-1, "params must be an array or object of notify entries");
        return;
    }
    for (const auto& item : entries) {
        if (!item.is_object()) {
            continue;
        }
        const std::string date_index = item.value("date_index", "");
        const std::string type       = item.value("type", "video");
        const std::string state      = item.value("state", "");
        const std::string url        = item.value("url", "");

        // On success with a url, drive the WAN download that sw_DownLoadFile
        // registered (looked up by date_index). Run on main thread — ctx and
        // popup are touched there.
        if (state == "success" && !url.empty() && !date_index.empty()) {
            WanPendingEntry entry;
            {
                std::lock_guard<std::mutex> lk(g_wan_pending_mtx);
                auto it = g_wan_pending.find(date_index);
                if (it == g_wan_pending.end()) {
                    g_pending_upload_notifications[date_index] = url;
                    continue;
                }
                entry = it->second;
            }
            auto ctx = entry.ctx;
            int idx = entry.idx;
            wxGetApp().CallAfter([ctx, idx, url]() {
                if (!ctx || ctx->cancelled) {
                    return;
                }
                if (ctx->skipped_indices.count(idx)) {
                    // Cancelled between the upload notification arriving and this
                    // deferred kickoff — don't start (or restart) the download.
                    return;
                }
                // WAN: kickoff directly — kickoff_download's callbacks use idx
                // (not current_index), so no race with other files.
                ctx->kickoff_download(idx, url);
            });
        }
    }

    m_res_data = json::object();
    m_status   = 200;
    m_msg      = "OK";
    send_to_js();
    finish_job();
}

void SSWCP_UserLogin_Instance::sw_DownLoadFile()
{
    // Phase A: Parse keyed params and validate
    if (!m_param_data.is_object()) {
        handle_general_fail(-1, "params must be an object with download type keys");
        return;
    }

    bool has_timelapse = m_param_data.count("timelapse")
        && m_param_data["timelapse"].is_array()
        && !m_param_data["timelapse"].empty();

    if (!has_timelapse) {
        handle_general_fail(-1, "No supported download type found in params. Currently supports: timelapse");
        return;
    }

    // Top-level mode applies to every entry in timelapse[].
    //   "lan" → caller provides url directly, download without decryption.
    //   "wan" → caller omits url; we trigger device upload via sn + date_index,
    //           then download + decrypt (PBKDF2 + AES, need_decrypt=true).
    std::string mode = m_param_data.value("mode", "");
    if (mode != "lan" && mode != "wan") {
        handle_general_fail(-1, "params.mode must be 'lan' or 'wan'");
        return;
    }
    bool is_lan = (mode == "lan");

    std::vector<TimelapseFileEntry> files;

    const json& arr = m_param_data["timelapse"];
    for (size_t i = 0; i < arr.size(); ++i) {
        const json& item = arr[i];
        if (!item.is_object()) {
            handle_general_fail(-1, "Each timelapse entry must be an object");
            return;
        }

        TimelapseFileEntry entry;
        entry.mode         = mode;
        entry.file_name    = item.value("file_name", "");
        entry.sn           = item.value("sn", "");
        entry.date_index   = item.value("date_index", "");
        entry.url          = item.value("url", "");
        entry.file_size    = item.value("file_size", size_t(0));

        BOOST_LOG_TRIVIAL(warning) << "DownloadFile: mode=" << entry.mode
            << " sn=" << entry.sn << " file_name=" << entry.file_name
            << " date_index=" << entry.date_index << " url=" << entry.url
            << " file_size=" << entry.file_size;

        if (entry.file_name.empty()) {
            handle_general_fail(-1, "Each timelapse entry must have file_name");
            return;
        }

        if (is_lan) {
            if (entry.url.empty()) {
                handle_general_fail(-1, "url is required for each entry in lan mode");
                return;
            }
        } else {
            if (entry.sn.empty() || entry.date_index.empty()) {
                handle_general_fail(-1,
                    "sn and date_index are required for each entry in wan mode");
                return;
            }
        }

        files.push_back(std::move(entry));
    }

    // Compute save_path and check disk space BEFORE responding to flutter.
    const std::string& sn = files[0].sn;
#ifdef _WIN32
    boost::filesystem::path save_path_fs(boost::nowide::widen(wxGetApp().app_config->get("download_path")));
#else
    boost::filesystem::path save_path_fs(wxGetApp().app_config->get("download_path"));
#endif
    save_path_fs /= sn;
    boost::system::error_code mk_ec;
    boost::filesystem::create_directories(save_path_fs, mk_ec);
#ifdef _WIN32
    std::string save_path = boost::nowide::narrow(save_path_fs.wstring());
#else
    std::string save_path = save_path_fs.string();
#endif

    if (mk_ec) {
        // Download path invalid (e.g. configured USB drive was removed).
        // Bail out before starting any download — otherwise file writes crash.
        json result;
        result["error"] = "invalid_download_path";
        result["path"]  = save_path;
        m_res_data = result;
        m_status   = -1;
        m_msg      = "invalid_download_path";
        send_to_js();
        finish_job();
    }

    {
        size_t total_file_size = 0;
        for (const auto& f : files) {
            total_file_size += f.file_size;
        }
        size_t required = static_cast<size_t>(total_file_size * 1.2);
        if (required < 100ULL * 1024 * 1024) {
            required = 100ULL * 1024 * 1024;
        }
        boost::system::error_code space_ec;
        boost::filesystem::space_info si = boost::filesystem::space(save_path, space_ec);
        if (space_ec) {
            // Can't query disk space (drive gone / path not on a real filesystem).
            json result;
            result["error"] = "invalid_download_path";
            result["path"]  = save_path;
            m_res_data = result;
            m_status   = -1;
            m_msg      = "invalid_download_path";
            send_to_js();
            finish_job();
            return;
        }
        if (si.available < required) {
            json result;
            result["error"]      = "insufficient_disk_space";
            result["required"]   = required;
            result["available"]  = si.available;
            m_res_data = result;
            m_status   = -1;
            m_msg      = "Insufficient storage space. Please free up space.";
            send_to_js();
            finish_job();
            return;
        }
    }

    // Phase B: Respond immediately, defer finish_job until downloads complete
    m_res_data = json::object();
    m_status = 200;
    m_msg = "success";
    send_to_js();

    auto wcp_self = std::weak_ptr<SSWCP_UserLogin_Instance>(
        std::static_pointer_cast<SSWCP_UserLogin_Instance>(shared_from_this()));

    // Phase C: Defer UI and download orchestration to main thread
    wxGetApp().CallAfter([files = std::move(files), save_path, wcp_self]() {
        // Dedup: skip files whose date_index is already downloading in any
        // active batch, so re-triggering download doesn't duplicate rows.
        std::vector<TimelapseFileEntry> unique_files;
        {
            std::lock_guard<std::mutex> lk(g_active_ctx_mtx);
            for (const auto& f : files) {
                if (!f.date_index.empty() && g_active_date_indices.count(f.date_index)) {
                    continue;
                }
                unique_files.push_back(f);
            }
        }
        if (unique_files.empty()) { return; }

        int total_count = static_cast<int>(unique_files.size());

        // Reuse the shared download popup (created on first batch).
        auto* popup = get_or_create_timelapse_popup();
        if (!popup->IsShown()) { 
            popup->Show(); 
        }

        std::vector<TimelapseDownloadPopup::TaskInfo> task_infos;
        for (const auto& f : unique_files)
        {
            TimelapseDownloadPopup::TaskInfo info;
            info.file_name    = f.file_name;
            info.file_url     = f.url;
            info.sn           = f.sn;
            info.date_index   = f.date_index;
            task_infos.push_back(info);
        }
        int row_offset = popup->add_tasks(task_infos);

        // Queue state (shared_ptr for safe capture across callbacks)
        auto ctx = std::make_shared<TimelapseQueueContext>();
        ctx->files           = unique_files;
        ctx->save_path       = save_path;
        ctx->popup           = popup;
        ctx->total_count     = total_count;
        ctx->row_offset      = row_offset;
        ctx->current_index   = 0;
        ctx->completed_count = 0;
        ctx->failed_count    = 0;
        ctx->current_task_id = 0;
        ctx->cancelled       = false;
        ctx->wcp_self        = wcp_self;
        ctx->is_wan          = !unique_files.empty() && unique_files[0].mode == "wan";
        ctx->sn              = unique_files.empty() ? "" : unique_files[0].sn;

        // Register this batch's date_indices + ctx as active.
        {
            std::lock_guard<std::mutex> lk(g_active_ctx_mtx);
            for (int i = 0; i < total_count; ++i) {
                if (!unique_files[i].date_index.empty()) {
                    g_active_date_indices.insert(unique_files[i].date_index);
                }
            }
            g_active_ctxs.push_back(ctx);
        }

        // WAN: register each file by date_index so sw_NotifyUploadTimelaspe can
        // find it when flutter forwards the device upload url.
        if (ctx->is_wan) {
            // Register each date_index and replay any upload notification that arrived before this registration
            std::vector<std::pair<int, std::string>> replay;
            {
                std::lock_guard<std::mutex> lk(g_wan_pending_mtx);
                for (int i = 0; i < total_count; ++i) {
                    g_wan_pending[unique_files[i].date_index] = { ctx, i };
                    auto nit = g_pending_upload_notifications.find(unique_files[i].date_index);
                    if (nit != g_pending_upload_notifications.end()) {
                        replay.emplace_back(i, nit->second);
                        g_pending_upload_notifications.erase(nit);
                    }
                }
            }
            for (auto& r : replay) {
                int idx = r.first;
                std::string url = r.second;
                wxGetApp().CallAfter([ctx, idx, url]() {
                    if (!ctx || ctx->cancelled) return;
                    if (ctx->skipped_indices.count(idx)) return;
                    ctx->kickoff_download(idx, url);
                });
            }
        }

        // Set per-task cancel callbacks for independent cancel.
        // i is the per-batch index (ctx->files[i]); global_row = row_offset + i
        // addresses the row in the shared popup. Cancel runs immediately (no
        // CallAfter) so Http::cancel interrupts before the download completes —
        // otherwise the file finishes during the deferred window and shows as
        // "open folder" instead of staying downloadable.
        for (int i = 0; i < total_count; i++) {
            int global_row = ctx->row_offset + i;
            popup->set_task_cancel_callback(global_row, [ctx, i, global_row]() {
                if (!ctx->popup || ctx->popup->IsBeingDeleted()) { return; }
                bool is_active = (i == ctx->current_index);
                ctx->skipped_indices.insert(i);
                push_file_state(ctx, i, "cancelled");
                release_date_index(ctx->files[i].date_index);
                // WAN: drop from g_wan_pending so a late sw_NotifyUploadTimelaspe
                // can't re-kickoff this cancelled file.
                if (ctx->is_wan) {
                    std::lock_guard<std::mutex> lk(g_wan_pending_mtx);
                    g_wan_pending.erase(ctx->files[i].date_index);
                }
                if (is_active && ctx->current_task_id != 0) {
                    size_t task_id = ctx->current_task_id;
                    DownloadManager::getInstance().cancel_download(task_id);
                    ctx->current_task_id = 0;
                    if (ctx->popup && !ctx->popup->IsBeingDeleted()) {
                        ctx->popup->mark_task_cancelled(global_row);
                    }
                    ctx->current_index++;
                    if (ctx->start_next) { ctx->start_next(); }
                } else if (is_active && ctx->waiting_for_upload) {
                    const auto& info = ctx->files[i];
                    ctx->pending_wan_date_to_idx.erase(info.date_index);
                    if (ctx->popup && !ctx->popup->IsBeingDeleted()) {
                        ctx->popup->mark_task_cancelled(global_row);
                    }
                    ctx->waiting_for_upload = false;
                    ctx->current_index++;
                    if (ctx->start_next) { ctx->start_next(); }
                } else {
                    if (ctx->popup && !ctx->popup->IsBeingDeleted()) {
                        ctx->popup->mark_task_cancelled(global_row);
                    }
                }
            });
        }

        // Shared download kicker — resolves a url for a given file index and
        // launches the actual HTTP download. Used by LAN direct-download path
        // inside start_next, and by the WAN notification callback after the
        // device signals upload complete.
        ctx->kickoff_download = [ctx](int idx, const std::string& url) {
            const auto& info = ctx->files[idx];
            int global_row = ctx->row_offset + idx;   // row index in shared popup

            DownloadCallbacks callbacks;

            callbacks.on_progress = [ctx, global_row](size_t, int percent,
                                           size_t downloaded, size_t total) {
                if (!ctx->popup || ctx->popup->IsBeingDeleted())
                {
                    return;
                }
                ctx->popup->set_task_progress(global_row, percent,
                                               downloaded, total);
            };

            callbacks.on_complete = [ctx, idx, global_row](size_t, const std::string& file_path) {
                if (ctx->skipped_indices.count(idx)) {
                    ctx->current_task_id = 0;
                    if (ctx->is_wan) {
                        std::lock_guard<std::mutex> lk(g_wan_pending_mtx);
                        g_wan_pending.erase(ctx->files[idx].date_index);
                    }
                    return;
                }
                const auto& cur = ctx->files[idx];
                json entry;
                entry["sn"]         = cur.sn;
                entry["file_name"]  = cur.file_name;
                entry["location"]   = file_path;
                entry["file_type"]  = "mp4";
                entry["date_index"] = cur.date_index;
                ctx->completed_files.push_back(entry);

                if (ctx->popup && !ctx->popup->IsBeingDeleted())
                {
                    ctx->popup->mark_task_complete(global_row, file_path);
                }

                push_file_state(ctx, idx, "success", file_path);
                release_date_index(cur.date_index);

                ctx->completed_count++;
                ctx->current_task_id = 0;
                if (ctx->is_wan) {
                    // WAN: each file is driven by sw_NotifyUploadTimelaspe, no start_next.
                    // Clean up the pending entry and check if all done.
                    {
                        std::lock_guard<std::mutex> lk(g_wan_pending_mtx);
                        g_wan_pending.erase(cur.date_index);
                    }
                    if (ctx->completed_count + ctx->failed_count >= ctx->total_count) {
                        ctx->start_next = nullptr;  // break self-ref cycle
                        wxGetApp().CallAfter([ctx, file_path]() {
                            if (ctx->popup && !ctx->popup->IsBeingDeleted()) {
                                std::string latest_file = file_path;
                                ctx->popup->mark_all_complete(
                                    ctx->completed_count, ctx->failed_count,
                                    ctx->save_path, latest_file);
                            }
                            if (auto wcp = ctx->wcp_self.lock()) {
                                wcp->finish_job();
                            }
                        });
                    }
                } else {
                    // LAN: serial — advance and start_next.
                    ctx->current_index = idx + 1;
                    if (ctx->start_next) { ctx->start_next(); }
                }
            };

            callbacks.on_error = [ctx, idx, global_row](size_t, const std::string& error) {
                if (ctx->skipped_indices.count(idx)) {
                    // ctx->cancelled is set by device-switch (cancel_all) and popup
                    // close — in those cases the row was already marked (failed /
                    // closing), so don't override with "cancelled" here. Only the
                    // per-task cancel button path (ctx->cancelled stays false)
                    // wants mark_task_cancelled.
                    if (!ctx->cancelled && ctx->popup && !ctx->popup->IsBeingDeleted())
                    {
                        ctx->popup->mark_task_cancelled(global_row);
                    }
                    push_file_state(ctx, idx, "cancelled");
                    release_date_index(ctx->files[idx].date_index);
                    ctx->failed_count++;
                    ctx->current_task_id = 0;
                    if (ctx->is_wan) {
                        {
                            std::lock_guard<std::mutex> lk(g_wan_pending_mtx);
                            g_wan_pending.erase(ctx->files[idx].date_index);
                        }
                    } else {
                        ctx->current_index = idx + 1;
                        if (ctx->start_next) { ctx->start_next(); }
                    }
                    return;
                }

                BOOST_LOG_TRIVIAL(error)
                    << "DownloadFile: Download failed for '"
                    << ctx->files[idx].file_name
                    << "'. Error: " << error;

                if (ctx->popup && !ctx->popup->IsBeingDeleted())
                {
                    // Always show a single friendly message — never expose the
                    // raw Http error string to the UI.
                    ctx->popup->mark_task_error(global_row,
                        std::string(_L("Failed to download the file. Please check your network and try again.").ToUTF8().data()));
                }

                push_file_state(ctx, idx, "failed");
                release_date_index(ctx->files[idx].date_index);
                ctx->failed_count++;
                ctx->current_task_id = 0;
                if (ctx->is_wan) {
                    {
                        std::lock_guard<std::mutex> lk(g_wan_pending_mtx);
                        g_wan_pending.erase(ctx->files[idx].date_index);
                    }
                    if (ctx->completed_count + ctx->failed_count >= ctx->total_count) {
                        ctx->start_next = nullptr;
                        wxGetApp().CallAfter([ctx]() {
                            if (ctx->popup && !ctx->popup->IsBeingDeleted()) {
                                ctx->popup->mark_all_complete(
                                    ctx->completed_count, ctx->failed_count,
                                    ctx->save_path, "");
                            }
                            if (auto wcp = ctx->wcp_self.lock()) {
                                wcp->finish_job();
                            }
                        });
                    }
                } else {
                    ctx->current_index = idx + 1;
                    if (ctx->start_next) { ctx->start_next(); }
                }
            };

            callbacks.on_should_write = [ctx, idx]() {
                return !ctx->skipped_indices.count(idx);
            };

            ctx->current_task_id = DownloadManager::getInstance().start_internal_download(
                url, info.file_name,
                std::move(callbacks), info.mode == "wan", info.sn);
        };

        // WAN: no MQTT subscription — flutter forwards device upload url via
        // sw_NotifyUploadTimelaspe, which looks up g_wan_pending to kickoff.

        // Define the download-starter (recursive, one file at a time)
        ctx->start_next = [ctx]() {
            if (ctx->cancelled)
            {
                ctx->start_next = nullptr;
                return;
            }

            // Skip cancelled/skipped tasks
            while (ctx->current_index < ctx->total_count &&
                   ctx->skipped_indices.count(ctx->current_index)) {
                ctx->current_index++;
            }

            // All files processed
            if (ctx->current_index >= ctx->total_count) {
                // WAN: unsubscribe from upload notifications before final cleanup
                unsubscribe_wan_upload(ctx);
                wxGetApp().CallAfter([ctx]() {
                    if (ctx->popup && !ctx->popup->IsBeingDeleted()) {
                        std::string latest_file;
                        if (!ctx->completed_files.empty()) {
                            latest_file = ctx->completed_files.back().value("location", "");
                        }
                        ctx->popup->mark_all_complete(
                            ctx->completed_count, ctx->failed_count,
                            ctx->save_path, latest_file);
                    }
                    // Per-file states were already pushed as each file
                    // completed — no batch response (it would time out).
                    if (auto wcp = ctx->wcp_self.lock()) {
                        wcp->finish_job();
                    }
                });
                ctx->start_next = nullptr;
                return;
            }

            const auto& info = ctx->files[ctx->current_index];
            int cur_idx = ctx->current_index;

            // Update task row: show waiting/uploading/downloading state
            wxGetApp().CallAfter([ctx, cur_idx]() {
                if (!ctx->popup || ctx->popup->IsBeingDeleted())
                {
                    return;
                }
                wxString status = ctx->is_wan
                    ? _L("Waiting for device to upload")
                    : _L("Downloading");
                ctx->popup->set_task_status(ctx->row_offset + cur_idx,
                    wxString::FromUTF8(status.ToUTF8().data()));
            });

            if (info.mode == "lan") {
                ctx->kickoff_download(cur_idx, info.url);
            } else {
                // WAN: flutter sends url via sw_NotifyUploadTimelaspe; just wait.
                ctx->waiting_for_upload = true;
            }
        };

        // Set close callback (X button): cancel ALL active batches sharing this
        // popup, mark every unfinished file cancelled, then destroy the popup.
        popup->set_close_callback([]() {
            std::vector<std::shared_ptr<TimelapseQueueContext>> ctxs;
            {
                std::lock_guard<std::mutex> lk(g_active_ctx_mtx);
                ctxs = g_active_ctxs;
                g_active_ctxs.clear();
                g_active_date_indices.clear();
            }
            for (auto& c : ctxs) {
                if (c->current_task_id != 0) {
                    DownloadManager::getInstance().cancel_download(c->current_task_id);
                    c->current_task_id = 0;
                }
                unsubscribe_wan_upload(c);
                c->cancelled = true;
                c->popup = nullptr;
            }

            if (g_timelapse_popup && !g_timelapse_popup->IsBeingDeleted()) {
                g_timelapse_popup->Destroy();
            }
            g_timelapse_popup = nullptr;
            // Push cancelled state on next idle (doesn't touch the popup).
            wxGetApp().CallAfter([ctxs]() {
                for (auto& c : ctxs) {
                    for (int i = 0; i < c->total_count; ++i) {
                        push_file_state(c, i, "cancelled");
                    }
                    if (auto wcp = c->wcp_self.lock()) {
                        wcp->finish_job();
                    }
                }
            });
        });

        // Start the first download
        ctx->start_next();
    });
}

// Push a single file's download state to every subscriber whose sn matches.
void SSWCP_UserLogin_Instance::push_timelapse_state(const std::string& sn,
                                                     const std::string& file_name,
                                                     const std::string& file_url,
                                                     const std::string& date_index,
                                                     const std::string& save_path,
                                                     const std::string& state)
{
    if (sn.empty() || m_subscribe_map.empty()) {
        return;
    }

    json item;
    item["file_name"]  = file_name;
    item["file_url"]   = file_url;
    item["sn"]         = sn;
    item["date_index"] = date_index;
    item["save_path"]  = save_path;

    for (const auto& kv : m_subscribe_map) {
        const auto& sub = kv.second;
        if (!sub || sub->sn != sn) {
            continue;
        }

        json response, payload, data;
        response["header"]["event_id"] = sub->event_id;
        payload["code"]     = 200;
        payload["message"]  = "download_complete";
        payload["state"]    = state;

        json timelapse_arr = json::array();
        timelapse_arr.push_back(item);
        data["timelapse"]   = timelapse_arr;
        payload["data"]     = data;
        response["payload"] = payload;

        std::string json_str = response.dump(4, ' ', true);
        std::string script = "window.postMessage(JSON.stringify(" + json_str + "), '*');";

        std::string event_id = sub->event_id;
        wxGetApp().CallAfter([event_id, script, json_str]() {
            auto it = m_subscribe_map.find(event_id);
            if (it != m_subscribe_map.end() && it->second && it->second->webview) {
                WebView::RunScript(it->second->webview, script);
            }
            SSWCP::send_message_to_flutter(json_str);
        });
    }
}

// sw_SubscribeDownloadState — passive subscription.
// Registers (event_id, sn, type) and returns immediately. The actual
// "download_complete" event is pushed when a sw_DownLoadFile call finishes
// for the same sn.
void SSWCP_UserLogin_Instance::sw_SubscribeDownloadState()
{
    if (m_event_id.empty()) {
        handle_general_fail(-1, "event_id is required for sw_SubscribeDownloadState");
        return;
    }

    if (!m_param_data.is_object()) {
        handle_general_fail(-1, "params must be an object");
        return;
    }

    std::string sn   = m_param_data.count("sn")   && m_param_data["sn"].is_string()
        ? m_param_data["sn"].get<std::string>()   : "";
    std::string type = m_param_data.count("type") && m_param_data["type"].is_string()
        ? m_param_data["type"].get<std::string>() : "";

    if (sn.empty() || type.empty()) {
        handle_general_fail(-1, "sn and type are required for sw_SubscribeDownloadState");
        return;
    }

    auto sub = std::make_shared<SubscribeInfo>();
    sub->event_id = m_event_id;
    sub->sn       = sn;
    sub->type     = type;
    sub->webview  = m_webview;

    m_subscribe_map[m_event_id] = sub;

    // When the webview is destroyed (e.g. user switches machines during download),
    // clean up all subscriptions that reference it so push_timelapse_state won't
    // post CallAfter lambdas with dangling webview pointers.
    if (m_webview) {
        m_webview->Bind(wxEVT_DESTROY, [webview = m_webview](wxWindowDestroyEvent& e) {
            if (e.GetEventObject() != webview) { e.Skip(); return; }
            for (auto it = m_subscribe_map.begin(); it != m_subscribe_map.end();) {
                auto& sub = it->second;
                if (sub && sub->webview == webview) {
                    it = m_subscribe_map.erase(it);
                } else {
                    ++it;
                }
            }
            e.Skip();
        });
    }

    m_res_data = json::object();
    m_status   = 200;
    m_msg      = "success";
    send_to_js();
    finish_job();
}


// Tear down a download-state subscription registered by sw_SubscribeDownloadState.
// Mirrors the sw_UnsubscribePageStateChange pattern: if event_id is given, remove
// that single subscription; otherwise remove every subscription from this webview
// (i.e. global unsubscribe for the page).
void SSWCP_UserLogin_Instance::sw_UnsubscribeDownloadState()
{
    std::string event_id = m_param_data.count("event_id") && m_param_data["event_id"].is_string()
        ? m_param_data["event_id"].get<std::string>() : "";

    if (event_id.empty()) {
        for (auto it = m_subscribe_map.begin(); it != m_subscribe_map.end();) {
            auto& sub = it->second;
            if (sub && sub->webview == m_webview) {
                it = m_subscribe_map.erase(it);
            } else {
                ++it;
            }
        }
    } else {
        auto it = m_subscribe_map.find(event_id);
        if (it != m_subscribe_map.end()) {
            m_subscribe_map.erase(it);
        }
    }

    m_res_data = json::object();
    m_status   = 200;
    m_msg      = "success";
    send_to_js();
    finish_job();
}


void SSWCP_UserLogin_Instance::sw_CancelDownload() {
    // Cancel a single download task by task_id.
    // (Subscription teardown is handled by sw_UnsubscribeDownloadState.)
    size_t task_id = m_param_data.count("task_id") ? m_param_data["task_id"].get<size_t>() : 0;

    if (task_id == 0) {
        handle_general_fail(-1, "task_id is required");
        return;
    }

    DownloadManager* download_mgr = wxGetApp().download_manager();
    if (!download_mgr) {
        handle_general_fail(-1, "WCP Download Manager not available");
        return;
    }

    bool success = download_mgr->cancel_download(task_id);

    if (success) {
        json response;
        response["task_id"] = task_id;
        response["canceled"] = true;
        m_res_data = response;
        m_status = 0;
        m_msg = "Download canceled";
    } else {
        handle_general_fail(-1, "Failed to cancel download or task not found");
        return;
    }

    send_to_js();
    finish_job();
}

void SSWCP_UserLogin_Instance::sw_FileView() {
    try {
        std::string file_path = m_param_data.count("file_path") ? m_param_data["file_path"].get<std::string>() : "";
        wxFileName  file(wxString::FromUTF8(file_path));

        if (!file.FileExists()) {
            handle_general_fail();
            //wxMessageBox(wxT("file not exsit"), wxT("tips"), wxOK | wxICON_WARNING);
            return;
        }

        std::weak_ptr<SSWCP_Instance> weak_self = shared_from_this();

        wxGetApp().CallAfter([file_path, weak_self]() {
            auto self = weak_self.lock();
            if (!self) {
                return;
            }

            //open file in folder
            desktop_open_any_folderEx(file_path);

            self->send_to_js();
            self->finish_job();

        });
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

// Timelapse-specific: open the containing folder. Unlike sw_FileView, if the
// file was deleted externally we still open the folder (just can't locate the
// file) instead of failing — matches the "open folder" button expectation.
void SSWCP_UserLogin_Instance::sw_OpenTimelapseFolder() {
    std::string file_path = m_param_data.count("file_path") ? m_param_data["file_path"].get<std::string>() : "";
    wxFileName  file(wxString::FromUTF8(file_path));

    std::weak_ptr<SSWCP_Instance> weak_self = shared_from_this();

    wxGetApp().CallAfter([file_path, file, weak_self]() {
        auto self = weak_self.lock();
        if (!self) {
            return;
        }

        if (file.FileExists()) {
            desktop_open_any_folderEx(file_path);
        } else {
            wxString dir = file.GetPath();
            if (!dir.empty()) {
                desktop_open_folder(std::string(dir.ToUTF8().data()));
            }
        }

        self->send_to_js();
        self->finish_job();

    });
}

void SSWCP_UserLogin_Instance::sw_GetFilesFromDir()
{
    std::string dir_path;
    if (m_param_data.contains("dir_path") && m_param_data["dir_path"].is_string()) {
        dir_path = m_param_data["dir_path"].get<std::string>();
    } else {
        dir_path = wxGetApp().app_config->get("download_path");
        if (m_param_data.contains("sn") && m_param_data["sn"].is_string()) {
            std::string sn = m_param_data["sn"].get<std::string>();
            if (!sn.empty()) {
                boost::filesystem::path p(dir_path);
                p = p / sn;
                dir_path = p.string();
            }
        }
    }
    if (dir_path.empty()) {
        handle_general_fail(-1, "dir_path is empty and no default download path configured");
        return;
    }

    json result;
    result["dir_path"] = dir_path;
    json files_arr = json::array();

    boost::system::error_code ec;
    if (!boost::filesystem::exists(dir_path, ec) || !boost::filesystem::is_directory(dir_path, ec)) {
        result["files"] = files_arr;
        m_res_data = result;
        m_status = 200;
        m_msg = "success";
        send_to_js();
        finish_job();
        return;
    }

    boost::filesystem::directory_iterator end_iter;
    boost::filesystem::directory_iterator it(dir_path, ec);
    while (it != end_iter) {
        if (ec) { ec.clear(); }
        boost::system::error_code file_ec;
        if (boost::filesystem::is_regular_file(it->path(), file_ec) && !file_ec) {
            const std::string file_name = it->path().filename().string();
            json file_info;
            file_info["file_name"] = file_name;
            file_info["file_path"] = it->path().string();
            boost::system::error_code sz_ec;
            file_info["file_size"] = boost::filesystem::file_size(it->path(), sz_ec);
            if (sz_ec) { file_info["file_size"] = 0; }
            files_arr.push_back(file_info);
        }
        it.increment(ec);
    }

    result["files"] = files_arr;

    m_res_data = result;
    m_status = 200;
    m_msg = "success";
    send_to_js();
    finish_job();
}

void SSWCP_UserLogin_Instance::sw_SubUserUpdatePrivacy()
{
    try {
        std::weak_ptr<SSWCP_Instance> weak_ptr         = shared_from_this();
        wxGetApp().m_user_update_privacy_subscribers[m_webview] = weak_ptr;
    } catch (std::exception& e) {
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
    } else if (m_cmd == "sw_UpdateDeviceInfo") {
        sw_UpdateDeviceInfo();
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
                // wcp sub
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

void SSWCP_MachineManage_Instance::sw_UpdateDeviceInfo()
{
    // Expect "devices": [ { dev_id, ... }, ... ] — batch merge update
    if (!m_param_data.count("devices") || !m_param_data["devices"].is_array()) {
        handle_general_fail(-1, "param [devices] is required (array)");
        return;
    }

    std::string new_preset_model;
    std::string new_preset_nozzle;
    bool        any_changed = false;

    wxGetApp().CallAfter([devices_json = m_param_data["devices"], new_preset_model, new_preset_nozzle, any_changed]() mutable {
        for (const auto& item : devices_json) {
            if (!item.is_object())
                continue;
            if (!item.count("dev_id") || !item["dev_id"].is_string())
                continue;

            std::string dev_id = item["dev_id"].get<std::string>();

            // Load existing or start fresh
            DeviceInfo info;
            bool       exist = wxGetApp().app_config->get_device_info(dev_id, info);
            if (!exist) {
                info.dev_id    = dev_id;
                info.connected = false;
                info.protocol  = 0;
                info.port      = 0;
            }

            // Snapshot before merge for diff
            DeviceInfo before = info;

            // —— Merge: only overwrite fields that are present ——

            if (item.count("ip") && item["ip"].is_string())
                info.ip = item["ip"].get<std::string>();

            if (item.count("dev_name") && item["dev_name"].is_string())
                info.dev_name = item["dev_name"].get<std::string>();

            if (item.count("model_name") && item["model_name"].is_string()) {
                info.model_name  = item["model_name"].get<std::string>();
                new_preset_model = info.model_name;

                // Machine cover image
                size_t vendor_pos = info.model_name.find_first_of(" ");
                if (vendor_pos != std::string::npos) {
                    std::string vendor = info.model_name.substr(0, vendor_pos);
                    info.img = LOCALHOST_URL + std::to_string(wxGetApp().m_page_http_server.get_port()) + "/profiles/" + vendor + "/" +
                               info.model_name + "_cover.png";
                }
            }

            if (item.count("connected") && item["connected"].is_boolean())
                info.connected = item["connected"].get<bool>();

            if (item.count("sn") && item["sn"].is_string())
                info.sn = item["sn"].get<std::string>();

            if (item.count("link_mode") && item["link_mode"].is_string())
                info.link_mode = item["link_mode"].get<std::string>();

            if (item.count("id") && item["id"].is_string())
                info.id = item["id"].get<std::string>();

            if (item.count("userid") && item["userid"].is_string())
                info.userid = item["userid"].get<std::string>();

            if (item.count("protocol") && item["protocol"].is_number())
                info.protocol = item["protocol"].get<int>();

            if (item.count("port") && item["port"].is_number())
                info.port = item["port"].get<int>();

            if (item.count("user") && item["user"].is_string())
                info.user = item["user"].get<std::string>();

            if (item.count("password") && item["password"].is_string())
                info.password = item["password"].get<std::string>();

            if (item.count("ca") && item["ca"].is_string())
                info.ca = item["ca"].get<std::string>();

            if (item.count("cert") && item["cert"].is_string())
                info.cert = item["cert"].get<std::string>();

            if (item.count("key") && item["key"].is_string())
                info.key = item["key"].get<std::string>();

            if (item.count("clientId") && item["clientId"].is_string())
                info.clientId = item["clientId"].get<std::string>();

            if (item.count("api_key") && item["api_key"].is_string())
                info.api_key = item["api_key"].get<std::string>();

            if (item.count("img") && item["img"].is_string())
                info.img = item["img"].get<std::string>();

            if (item.count("nozzle_sizes") && item["nozzle_sizes"].is_array()) {
                info.nozzle_sizes.clear();
                for (const auto& n : item["nozzle_sizes"]) {
                    if (n.is_string()) {
                        std::string v = n.get<std::string>();
                        if (v == "0.2" || v == "0.4" || v == "0.6" || v == "0.8")
                            info.nozzle_sizes.push_back(v);
                    } else if (n.is_number()) {
                        double d = n.get<double>();
                        if (fabs(d - 0.2) < 1e-6)
                            info.nozzle_sizes.push_back("0.2");
                        else if (fabs(d - 0.4) < 1e-6)
                            info.nozzle_sizes.push_back("0.4");
                        else if (fabs(d - 0.6) < 1e-6)
                            info.nozzle_sizes.push_back("0.6");
                        else if (fabs(d - 0.8) < 1e-6)
                            info.nozzle_sizes.push_back("0.8");
                    }
                }
                if (!info.nozzle_sizes.empty())
                    new_preset_nozzle = info.nozzle_sizes[0];
            }

            if (item.count("nozzle_volume_type") && item["nozzle_volume_type"].is_array()) {
                info.nozzle_volume_type.clear();
                for (const auto& volume_type : item["nozzle_volume_type"]) {
                    if (!volume_type.is_string())
                        continue;

                    const std::string type = volume_type.get<std::string>();
                    if (type == "standard" || type == "high_flow")
                        info.nozzle_volume_type.push_back(type);
                }
            }

            if (item.count("preset_name") && item["preset_name"].is_string())
                info.preset_name = item["preset_name"].get<std::string>();

            // Build preset name if it wasn't explicitly set but we have model + nozzle
            if (info.preset_name.empty() && !info.model_name.empty() && !info.nozzle_sizes.empty())
                info.preset_name = info.model_name + " (" + info.nozzle_sizes[0] + " nozzle)";

            // —— Diff: skip save if nothing changed ——
            if (exist && before.ip == info.ip && before.dev_name == info.dev_name && before.model_name == info.model_name &&
                before.preset_name == info.preset_name && before.connected == info.connected && before.img == info.img &&
                before.nozzle_sizes == info.nozzle_sizes && before.nozzle_volume_type == info.nozzle_volume_type && before.sn == info.sn &&
                before.protocol == info.protocol && before.api_key == info.api_key && before.user == info.user &&
                before.password == info.password && before.ca == info.ca && before.cert == info.cert && before.key == info.key &&
                before.clientId == info.clientId && before.port == info.port && before.link_mode == info.link_mode &&
                before.userid == info.userid && before.id == info.id) {
                continue;
            }

            wxGetApp().app_config->save_device_info(info);
            any_changed = true;
        }

        if (!any_changed)
            return;

        // Update ProfileJson for any newly-seen model/nozzle combination
        if (!new_preset_model.empty() && !new_preset_nozzle.empty()) {
            std::lock_guard<std::mutex> lock(m_ProfileJson_mutex);
            int                         nModel = m_ProfileJson["model"].size();
            bool                        isFind = false;
            for (int m = 0; m < nModel; m++) {
                if (m_ProfileJson["model"][m]["model"].get<std::string>() == new_preset_model) {
                    isFind                      = true;
                    std::string nozzle_selected = m_ProfileJson["model"][m]["nozzle_selected"].get<std::string>();
                    if (nozzle_selected.find(new_preset_nozzle) == std::string::npos) {
                        nozzle_selected += ";" + new_preset_nozzle;
                        m_ProfileJson["model"][m]["nozzle_selected"] = nozzle_selected;
                    }
                    break;
                }
            }

            if (!isFind) {
                json new_item;
                new_item["vendor"]          = "Snapmaker";
                new_item["model"]           = new_preset_model;
                new_item["nozzle_selected"] = new_preset_nozzle;
                new_item["nozzle_diameter"] = new_preset_nozzle;
                m_ProfileJson["model"].push_back(new_item);
            }
        }

        // Update UI
        auto devices = wxGetApp().app_config->get_devices();

        json param;
        param["command"]       = "local_devices_arrived";
        param["sequece_id"]    = "10001";
        param["data"]          = devices;
        std::string logout_cmd = param.dump();
        wxString    strJS      = wxString::Format("window.postMessage(%s)", logout_cmd);
        GUI::wxGetApp().run_script(strJS);

        json data = devices;
        wxGetApp().device_card_notify(data);

        wxGetApp().mainframe->plater()->sidebar().update_all_preset_comboboxes(false);

        if (SSWCP_MqttAgent_Instance::m_dialog) {
            SSWCP_MqttAgent_Instance::m_dialog->SaveProfile();
            bool flag = false;
            SSWCP_MqttAgent_Instance::m_dialog->apply_config(wxGetApp().app_config, wxGetApp().preset_bundle, wxGetApp().preset_updater,
                                                             flag);
            wxGetApp().update_mode();
        } else {
            BOOST_LOG_TRIVIAL(warning) << "sw_UpdateDeviceInfo: preset dialog not available, skip preset install";
        }
    });

    send_to_js();
    finish_job();
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

// check mqtt id
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

// webview illegal call back
void SSWCP_MqttAgent_Instance::set_Instance_illegal()
{
    SSWCP_Instance::set_Instance_illegal();

    clean_current_engine();
}

// detele mqtt instance
void SSWCP_MqttAgent_Instance::clean_current_engine()
{

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

// mqtt static msg callback
void SSWCP_MqttAgent_Instance::mqtt_msg_cb(const std::string& topic, const std::string& payload, void* client)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    {
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

    }
}

void SSWCP_MqttAgent_Instance::sw_create_mqtt_client()
{
    try {
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

        
        std::shared_ptr<MqttClient> client = nullptr;
        std::string type = "mqtt";
        if (ca != "" && cert != "" && key != "") {
            type = "mqtts";
            client.reset(new MqttClient(server_address, clientId, ca, cert, key, username, password, clean_session));
        }else{
            client.reset(new MqttClient(server_address, clientId, username, password, clean_session));
        }

        if (client == nullptr) {            
            handle_general_fail(-1, "create instance failed");
            return;
        }

        // clear list for current sub machine
        auto ptr = get_current_engine();
        if (!ptr) {
            ptr.reset();
        }
        clean_current_engine();

        //
        bool flag = set_current_engine({std::to_string(int64_t(client.get())), client});
        if (!flag) {
            handle_general_fail(-1, "create failed");
            return;
        }
        
        client->SetMessageCallback(SSWCP_MqttAgent_Instance::mqtt_msg_cb);

        m_res_data["type"] = type;
        m_res_data["id"]   = std::to_string(int64_t(get_current_engine().get()));

        send_to_js();
        finish_job();

    } catch (std::exception& e) {
        handle_general_fail();
    }
}

// mqtt connect
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
                        // response set event_id 
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
        // todo : add cin-type

        config.option<ConfigOptionEnum<PrintHostType>>("host_type")->value = type;

        config.set("print_host", ip + (port == -1 ? "" : ":" + std::to_string(port)));

        std::shared_ptr<PrintHost> tmp_host(PrintHost::get_print_host(&config));
        wxGetApp().set_connect_host(tmp_host);
        wxGetApp().set_host_config(config);

        // Device switch: every in-progress timelapse download is now stale —
        // stop them all and mark each unfinished file as "Download Failed".
        cancel_all_active_timelapse();

        std::shared_ptr<Moonraker_Mqtt> host = dynamic_pointer_cast<Moonraker_Mqtt>(tmp_host);
        if (host) {
            auto engine = get_current_engine();

            if (engine == nullptr) {
                handle_general_fail(-1, "invalid engine");
                Slic3r::sentryReportLog(Slic3r::SENTRY_LOG_ERROR, std::string("device_set_engine invalid engine"),DEVICE_SET_ENGINE_ERR);
                return;
            }            

            if (!engine->CheckConnected()) {
                BOOST_LOG_TRIVIAL(error) << "[SSWCP_MqttAgent_Instance] connect error";
                handle_general_fail(-1, "engine connection lost");
                Slic3r::sentryReportLog(Slic3r::SENTRY_LOG_ERROR, std::string("device_set_engine engine connection lost"), DEVICE_SET_ENGINE_ERR);
                return;
            }            

            std::string msg    = "success";          
            bool flag = host->set_engine(engine, msg);            

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

                                    // update card
                                    json param;
                                    param["command"]       = "local_devices_arrived";
                                    param["sequece_id"]    = "10001";
                                    param["data"]          = devices;
                                    std::string logout_cmd = param.dump();
                                    wxString    strJS      = wxString::Format("window.postMessage(%s)", logout_cmd);
                                    GUI::wxGetApp().run_script(strJS);

                                    // wcp sub
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
                            std::string ip;
                            {
                                int pos = ip_port.find(':');
                                ip      = (pos != std::string::npos) ? ip_port.substr(0, pos) : ip_port;
                            }

                            // Defer all AppConfig writes to the UI thread to avoid data races
                            // with sw_UpdateDeviceInfo and other MachineManage handlers that
                            // also read/write DeviceInfo on the UI thread via CallAfter.
                            wxGetApp().CallAfter([weak_self, reload_device_view, ip, host, connect_params, link_mode, id, userid]() {
                                // Mark other devices as disconnected
                                auto devices = wxGetApp().app_config->get_devices();
                                for (size_t i = 0; i < devices.size(); ++i) {
                                    if (devices[i].connected) {
                                        devices[i].connected = false;
                                        wxGetApp().app_config->save_device_info(devices[i]);
                                        break;
                                    }
                                }

                                // Save a minimal DeviceInfo from the connection params.
                                // Full device details (machine_type, nozzle_sizes, device_name)
                                // are pushed later by Flutter via sw_UpdateDeviceInfo.
                                std::string dev_id = connect_params.count("sn") ? connect_params["sn"].get<std::string>() : ip;
                                {
                                    DeviceInfo info;
                                    bool exist = wxGetApp().app_config->get_device_info(dev_id, info);
                                    if (!exist) {
                                        info.dev_id    = dev_id;
                                        info.connected = false;
                                        info.protocol  = 0;
                                        info.port      = 0;
                                    }
                                    info.ip        = ip;
                                    info.connected = true;
                                    info.link_mode = link_mode;
                                    info.protocol  = int(PrintHostType::htMoonRaker_mqtt);
                                    info.id        = id;
                                    info.userid    = userid;
                                    info.dev_name  = ip; // fallback device name
                                    if (connect_params.count("sn") && connect_params["sn"].is_string()) {
                                        info.sn       = connect_params["sn"].get<std::string>();
                                        info.dev_name = info.sn != "" ? info.sn : ip;
                                        info.dev_id   = info.sn != "" ? info.sn : info.dev_id;
                                    }
                                    // Carry over auth info from host
                                    auto auth_info = host->get_auth_info();
                                    info.ca       = "";
                                    info.cert     = "";
                                    info.key      = "";
                                    info.user     = auth_info["user"];
                                    info.password = auth_info["password"];
                                    info.port     = auth_info["port"];
                                    info.clientId = auth_info["clientId"];
                                    wxGetApp().app_config->save_device_info(info);
                                }

                                devices = wxGetApp().app_config->get_devices();

                                    json param;
                                    param["command"]       = "local_devices_arrived";
                                    param["sequece_id"]    = "10001";
                                    param["data"]          = devices;
                                    std::string logout_cmd = param.dump();
                                    wxString    strJS      = wxString::Format("window.postMessage(%s)", logout_cmd);
                                    GUI::wxGetApp().run_script(strJS);

                                    // wcp sub
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

                                    if (!wxGetApp().mainframe->m_printer_view->isSnapmakerPage()) {
                                        wxString url      = wxGetApp().build_flutter_web_url("2");
                                        auto     real_url = wxGetApp().get_international_url(url);
                                        wxGetApp().mainframe->load_printer_url(real_url);
                                    } else {
                                        if (reload_device_view) {
                                            wxString url      = wxGetApp().build_flutter_web_url("2");
                                            auto     real_url = wxGetApp().get_international_url(url);

                                            wxGetApp().mainframe->load_printer_url(real_url);
                                        }

                                    }

                                    auto self = weak_self.lock();
                                    if (!self) {
                                        return;
                                    }

                                    wxGetApp().app_config->clear_filament_extruder_map();

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

                                    auto mqtt_self = dynamic_pointer_cast<SSWCP_MqttAgent_Instance>(self);
                                    mqtt_self->clean_current_engine();
                                    self->send_to_js();
                                    self->finish_job();
                                });

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

std::unordered_map<std::string, std::shared_ptr<SSWCP_UserLogin_Instance::SubscribeInfo>>
    SSWCP_UserLogin_Instance::m_subscribe_map;
std::mutex  SSWCP::m_file_size_mutex;

// WebSocket Debug Server static members
std::unique_ptr<WebSocketDebugServer> SSWCP::m_debug_server = nullptr;
std::mutex SSWCP::m_debug_server_mutex;
bool SSWCP::m_debug_mode_enabled = false;

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
    "sw_StartLocalPrint",
    "sw_MachineHeartbeat",
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
    "sw_UploadAsyncTimelapseInstance",
    "sw_DeleteCameraTimelapse",
    "sw_GetCameraTimelapseInstance",
    "sw_ServerClientManagerSetUserinfo",
    "sw_DefectDetactionConfig",
    "sw_PrinterDefectDetection",
    GET_DEVICEDATA_STORAGESPACE
};

std::unordered_set<std::string> SSWCP::m_machine_connect_cmd_list = {
    "sw_Test_connect",
    "sw_Connect",
    "sw_Disconnect",
    "sw_GetConnectedMachine",
    "sw_ConnectOtherMachine",
    "sw_GetPincode",
    "sw_SubscribeForegroundChange"
};

std::unordered_set<std::string> SSWCP::m_project_cmd_list = {
    "sw_NewProject", "sw_OpenProject", "sw_GetRecentProjects", "sw_OpenRecentFile", "sw_DeleteRecentFiles", "sw_SubscribeRecentFiles",
};

std::unordered_set<std::string> SSWCP::m_login_cmd_list = {"sw_UserLogin", "sw_AskUserLogin", "sw_UserLogout", "sw_GetUserLoginState", "sw_SubscribeUserLoginState",
                                                           UPDATE_PRIVACY_STATUS,  GET_PRIVACY_STATUS,
                                                           FILE_VIEW, OPEN_TIMELAPSE_FOLDER, CANCEL_DOWNLOAD, DOWNLOAD_FILE_AND_OPEN, DOWN_LOAD_FILE, SUBSCRIBE_DOWNLOAD_STATE, UNSUBSCRIBE_DOWNLOAD_STATE, NOTIFY_UPLOAD_TIMELASPE, GET_FILES_FROM_DIR};

std::unordered_set<std::string> SSWCP::m_machine_manage_cmd_list = {
    "sw_GetLocalDevices", "sw_AddDevice", "sw_SubscribeLocalDevices", "sw_RenameDevice", "sw_SwitchModel", "sw_DeleteDevices",
    "sw_UpdateDeviceInfo"
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

    }
    catch (std::exception& e) {
    }
}

// Handle incoming web messages for Flutter debug (no webview required)
void SSWCP::handle_webmsg_for_debug(std::string message) {
    {
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
        std::shared_ptr<SSWCP_Instance> instance = create_sswcp_instance(cmd, header, params, event_id, nullptr);
        if (instance) {
            if (event_id != "") {
                m_instance_list.add_infinite(instance.get(), instance);
            } else {
                m_instance_list.add(instance.get(), instance, DEFAULT_INSTANCE_TIMEOUT);
            }
            instance->process();
        }
    }
}

// Delete instance from list
void SSWCP::delete_target(SSWCP_Instance* target) {
    wxGetApp().CallAfter([target]() {
        m_instance_list.remove(target);
    });
}

// Extend a one-shot instance's timeout by the default timeout
void SSWCP::renew_instance_timeout(SSWCP_Instance* instance) {
    m_instance_list.update_timeout(instance, DEFAULT_INSTANCE_TIMEOUT);
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

    auto& foreground_map = wxGetApp().m_foreground_change_subscribers;
    for (auto iter = foreground_map.begin(); iter != foreground_map.end();) {
        if (iter->first == view) {
            iter = foreground_map.erase(iter);
        } else {
            iter++;
        }
    }

    // Clean up timelapse download subscriptions referencing this webview.
    // Without this, push_timelapse_state posts CallAfter lambdas with dangling
    // pointers after the user switches machines during a download.
    auto& timelapse_sub_map = SSWCP_UserLogin_Instance::m_subscribe_map;
    for (auto iter = timelapse_sub_map.begin(); iter != timelapse_sub_map.end();) {
        auto& sub = iter->second;
        if (sub && sub->webview == view) {
            iter = timelapse_sub_map.erase(iter);
        } else {
            ++iter;
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
bool SSWCP::query_machine_info(std::shared_ptr<PrintHost>& host, MachineInfo& out, int timeout_second)
{
    if (!host) return false;

    std::condition_variable cv;
    std::shared_ptr<std::mutex> mutex(new std::mutex);
    std::weak_ptr<std::mutex>   cb_mutex = mutex;
    bool received = false;
    bool timeout = false;
    json system_info;

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

    {
        std::unique_lock<std::mutex> lock(*mutex);
        auto predicate = [&received]() { return received; };
        timeout = !cv.wait_for(lock, std::chrono::seconds(timeout_second), predicate);
    }

    if (!timeout && !system_info.is_null())
        return SSWCPProtocol::parse_machine_info_response(system_info, out);
    return false;
}


SSWCPProtocol::ResolveResult SSWCP::resolve_machine_info(std::shared_ptr<PrintHost>& host, int timeout_second)
{
    SSWCPProtocol::ResolveResult result;
    if (!host) return result;

    std::condition_variable cv;
    std::shared_ptr<std::mutex> mutex(new std::mutex);
    std::weak_ptr<std::mutex>   cb_mutex = mutex;
    std::shared_ptr<bool>       done(new bool(false));
    int                         received_count = 0;
    const int                   expected_count = 2;
    json                        system_info;
    json                        objects_query;

    auto on_system_info = [&, cb_mutex, done](const json& response) {
        auto locked = cb_mutex.lock();
        if (!locked) return;
        std::lock_guard<std::mutex> lock(*locked);
        if (*done) return;
        if (!response.is_null() && !response.count("error"))
            system_info = response;
        if (++received_count >= expected_count) cv.notify_one();
    };
    auto on_objects_query = [&, cb_mutex, done](const json& response) {
        auto locked = cb_mutex.lock();
        if (!locked) return;
        std::lock_guard<std::mutex> lock(*locked);
        if (*done) return;
        if (!response.is_null() && !response.count("error"))
            objects_query = response;
        if (++received_count >= expected_count) cv.notify_one();
    };

    // Send both requests in parallel; seq_ids are distinct so they never collide.
    // Query up to 8 extruder objects. Moonraker ignores keys that don't exist on the printer,
    // so extra entries are harmless. The parser dynamically discovers whatever is returned.
    std::vector<std::pair<std::string, std::vector<std::string>>> extruder_targets;
    extruder_targets.emplace_back("extruder", std::vector<std::string>{});
    for (int i = 1; i <= 7; ++i)
        extruder_targets.emplace_back("extruder" + std::to_string(i), std::vector<std::string>{});
    host->async_get_system_info(on_system_info);
    host->async_get_machine_info(extruder_targets, on_objects_query);

    {
        std::unique_lock<std::mutex> lock(*mutex);
        cv.wait_for(lock, std::chrono::seconds(timeout_second),
                    [&received_count, expected_count]() { return received_count >= expected_count; });
        *done = true;
    }
    // --- Merge by field priority ---
    MachineInfo& mi = result.info;
    // model / device_name: system_info is authoritative, normalized for whitelist safety.
    MachineInfo sys_mi;
    bool        got_system = !system_info.is_null() && SSWCPProtocol::parse_machine_info_response(system_info, sys_mi);
    if (got_system) {
        mi.model       = SSWCPProtocol::normalize_machine_model(sys_mi.model);
        mi.device_name = sys_mi.device_name;
        // system_info nozzle data as fallback
        mi.nozzle_diameters    = sys_mi.nozzle_diameters;
        mi.nozzle_volume_types = sys_mi.nozzle_volume_types;
    }
    // nozzle: objects.query is preferred (real-time), overrides system_info.
    std::vector<std::string> obj_diameters, obj_flows;
    if (!objects_query.is_null() && SSWCPProtocol::parse_extruder_nozzle_info(objects_query, obj_diameters, obj_flows)) {
        mi.nozzle_diameters    = std::move(obj_diameters);
        // Only overwrite flows when objects.query actually reported them; otherwise keep
        // whatever system_info provided so we never silently clear the user's flow config.
        if (!obj_flows.empty())
            mi.nozzle_volume_types = std::move(obj_flows);
    }

    // If the machine reported nozzle diameters but no flow types (neither system_info
    // nor objects.query carried them), default to standard -- one per nozzle. This
    // matches the read-side semantics (FlowType::nozzle_volume_types() pads missing
    // entries with standard) and lets sync always write a complete, usable vector
    // instead of silently skipping the update.
    if (!mi.nozzle_diameters.empty() && mi.nozzle_volume_types.empty())
        mi.nozzle_volume_types.assign(mi.nozzle_diameters.size(), FLOW_MODE_STANDARD);

    // Determine status
    if (mi.model.empty())
        result.status = SSWCPProtocol::ResolveStatus::NoResponse;
    else if (mi.nozzle_diameters.empty())
        result.status = SSWCPProtocol::ResolveStatus::GotIdentity;
    else
        result.status = SSWCPProtocol::ResolveStatus::Complete;

    return result;
}


MachineIPType* MachineIPType::getInstance()
{
    static MachineIPType mipt_instance;
    return &mipt_instance;
}

// WebSocket Debug Server implementation
void SSWCP::enable_debug_mode(bool enable, unsigned short port)
{
    std::lock_guard<std::mutex> lock(m_debug_server_mutex);

    if (enable && !m_debug_server) {
        BOOST_LOG_TRIVIAL(info) << "Enabling WebSocket debug mode on port " << port;

        m_debug_server = std::make_unique<WebSocketDebugServer>(port);

        // Set message callback to handle messages from Flutter Web
        m_debug_server->set_message_callback([](const std::string& message) {
            BOOST_LOG_TRIVIAL(debug) << "Received message from Flutter Web via WebSocket";

            // Handle the message using existing logic (no webview in debug/Flutter path)
            wxGetApp().CallAfter([message]() {
                SSWCP::handle_webmsg_for_debug(message);
            });
        });

        if (m_debug_server->start()) {
            m_debug_mode_enabled = true;
            BOOST_LOG_TRIVIAL(debug) << " WebSocket debug mode enabled successfully";
            BOOST_LOG_TRIVIAL(debug) << " Flutter Web can connect to: ws://localhost:" << port;
        } else {
            BOOST_LOG_TRIVIAL(error) << "Failed to start WebSocket debug server";
            m_debug_server.reset();
            m_debug_mode_enabled = false;
        }
    } else if (!enable && m_debug_server) {
        BOOST_LOG_TRIVIAL(debug) << "Disabling WebSocket debug mode";
        m_debug_server->stop();
        m_debug_server.reset();
        m_debug_mode_enabled = false;
    }
}

void SSWCP::disable_debug_mode()
{
    enable_debug_mode(false);
}

bool SSWCP::is_debug_mode_enabled()
{
    std::lock_guard<std::mutex> lock(m_debug_server_mutex);
    return m_debug_mode_enabled;
}

void SSWCP::send_message_to_flutter(const std::string& message)
{
    std::lock_guard<std::mutex> lock(m_debug_server_mutex);

    if (m_debug_server && m_debug_mode_enabled) {
        m_debug_server->send_message(message);
    } else {
        BOOST_LOG_TRIVIAL(debug) << "Cannot send message: WebSocket debug mode not enabled";
    }
}

void SSWCP::send_message_auto(const std::string& message, wxWebView* webview)
{
    // Original production path: send via WebView postMessage (unchanged, not affected by debug logic)
    if (webview && webview->GetRefData()) {
        BOOST_LOG_TRIVIAL(debug) << "Sending message to Flutter via WebView postMessage";
        std::string js_code = "window.postMessage(JSON.stringify(" + message + "), '*');";
        WebView::RunScript(webview, js_code);
    }

    // Debug path: copy the message to Flutter debug interface via WebSocket (independent of original path)
    {
        std::lock_guard<std::mutex> lock(m_debug_server_mutex);
        if (m_debug_mode_enabled && m_debug_server && m_debug_server->has_client()) {
            BOOST_LOG_TRIVIAL(debug) << "[DEBUG] Sending message to Flutter via WebSocket";
            m_debug_server->send_message(message);
        }
    }

    // Warning if no channel is available
    if ((!webview || !webview->GetRefData()) &&
        (!m_debug_mode_enabled || !m_debug_server || !m_debug_server->has_client())) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot send message: no WebSocket client and no WebView available";
    }
}

}}; // namespace Slic3r::GUI


