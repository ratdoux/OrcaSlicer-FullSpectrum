#include "HttpServer.hpp"
#include <boost/log/trivial.hpp>
#include <condition_variable>
#include "GUI_App.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include  "sentry_wrapper/SentryWrapper.hpp"
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#endif

namespace Slic3r { namespace GUI {

// 检测Windows系统是否支持UTF-8模式
bool is_windows_utf8_mode()
{
#ifdef _WIN32
    static bool checked = false;
    static bool utf8_mode = false;
    
    if (!checked) {
        // 检查系统是否启用了UTF-8模式
        // 最可靠的方法：检查ANSI代码页是否为UTF-8
        if (GetACP() == CP_UTF8) {
            utf8_mode = true;
        } else {
            // 对于现代Windows系统（Windows 10 1903+），
            // 即使没有全局启用UTF-8模式，文件系统API通常也能处理UTF-8路径
            // 这里我们采用保守策略：只有在明确启用UTF-8模式时才返回true
            utf8_mode = false;
        }
        checked = true;
    }
    
    return utf8_mode;
#else
    return true; // 非Windows系统通常支持UTF-8
#endif
}

// 辅助函数：将UTF-8字符串转换为适合文件系统操作的编码
std::string utf8_to_filesystem_encoding(const std::string& utf8_str)
{
#ifdef _WIN32
    if (utf8_str.empty()) return utf8_str;
    
    // 策略1：如果系统明确支持UTF-8模式，直接返回UTF-8字符串
    if (is_windows_utf8_mode()) {
        return utf8_str;
    }
    
    // 策略2：传统模式需要转换为系统编码（GBK）
    // 虽然某些情况下UTF-8路径可能成功，但为了确保兼容性，还是进行编码转换
    // 将UTF-8转换为宽字符（UTF-16）
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return utf8_str; // 转换失败，返回原字符串
    
    std::wstring wstr(wlen - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, &wstr[0], wlen);
    
    // 将宽字符转换为系统编码（GBK）
    int len = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return utf8_str; // 转换失败，返回原字符串
    
    std::string result(len - 1, 0);
    WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, &result[0], len, nullptr, nullptr);
    
    return result;
#else
    // 在非Windows系统上，UTF-8通常就是系统编码
    return utf8_str;
#endif
}

std::string url_get_param(const std::string& url, const std::string& key)
{
    size_t start = url.find(key);
    if (start == std::string::npos)
        return "";
    size_t eq = url.find('=', start);
    if (eq == std::string::npos)
        return "";
    std::string key_str = url.substr(start, eq - start);
    if (key_str != key)
        return "";
    start += key.size() + 1;
    size_t end = url.find('&', start);
    if (end == std::string::npos)
        end = url.length(); // Last param
    std::string result = url.substr(start, end - start);
    return result;
}

void session::start() { read_first_line(); }

void session::stop()
{
    boost::system::error_code ignored_ec;
    socket.shutdown(boost::asio::socket_base::shutdown_both, ignored_ec);
    socket.close(ignored_ec);
}

void session::read_first_line()
{
    auto self(shared_from_this());

    async_read_until(socket, buff, '\r', [this, self](const boost::beast::error_code& e, std::size_t s) {
        if (!e) {
            std::string  line, ignore;
            std::istream stream{&buff};
            std::getline(stream, line, '\r');
            std::getline(stream, ignore, '\n');
            headers.on_read_request_line(line);
            read_next_line();
        } else if (e != boost::asio::error::operation_aborted) {
            server.stop(self);
        }
    });
}

void session::read_body()
{
    auto self(shared_from_this());

    int                                nbuffer = 1000;
    std::shared_ptr<std::vector<char>> bufptr  = std::make_shared<std::vector<char>>(nbuffer);
    async_read(socket, boost::asio::buffer(*bufptr, nbuffer),
               [this, self](const boost::beast::error_code& e, std::size_t s) { server.stop(self); });
}

void session::read_next_line()
{
    auto self(shared_from_this());

    if (headers.method == "OPTIONS") {
        // 构造OPTIONS响应（允许跨域）
        std::stringstream ssOut;
        ssOut << "HTTP/1.1 200 OK\r\n";
        ssOut << "Access-Control-Allow-Origin: *\r\n";                            // 允许所有源
        ssOut << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";          // 允许的方法
        ssOut << "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"; // 允许的请求头
        ssOut << "Content-Length: 0\r\n";                                         // 无响应体
        ssOut << "\r\n";                                                          // 头和主体之间的空行（必须）

        // 异步发送响应
        async_write(socket, boost::asio::buffer(ssOut.str()), [this, self](const boost::beast::error_code& e, std::size_t s) {
            std::cout << "OPTIONS预检请求已处理" << std::endl;
            server.stop(self); // 关闭连接
        });
        return; // 提前返回，避免后续逻辑
    }

    async_read_until(socket, buff, '\r', [this, self](const boost::beast::error_code& e, std::size_t s) {
        if (!e) {
            std::string  line, ignore;
            std::istream stream{&buff};
            std::getline(stream, line, '\r');
            std::getline(stream, ignore, '\n');
            headers.on_read_header(line);

            if (line.length() == 0) {
                if (headers.content_length() == 0) {
                    std::cout << "Request received: " << headers.method << " " << headers.get_url();
                    if (headers.method == "OPTIONS") {
                        // Ignore http OPTIONS
                        server.stop(self);
                        return;
                    }

                    const std::string url_str = Http::url_decode(headers.get_url());
                    const auto        resp    = server.server.m_request_handler(url_str);
                    std::stringstream ssOut;
                    resp->write_response(ssOut);
                    std::shared_ptr<std::string> str = std::make_shared<std::string>(ssOut.str());
                    async_write(socket, boost::asio::buffer(str->c_str(), str->length()),
                                [this, self, str](const boost::beast::error_code& e, std::size_t s) {
                                    std::cout << "done" << std::endl;
                                    server.stop(self);
                                });
                } else {
                    read_body();
                }
            } else {
                read_next_line();
            }
        } else if (e != boost::asio::error::operation_aborted) {
            server.stop(self);
        }
    });
}

void HttpServer::IOServer::do_accept()
{
    acceptor.async_accept([this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
        if (!acceptor.is_open()) {
            return;
        }

        if (!ec) {
            const auto ss = std::make_shared<session>(*this, std::move(socket));
            start(ss);
        }

        do_accept();
    });
}

void HttpServer::IOServer::start(std::shared_ptr<session> session)
{
    sessions.insert(session);
    session->start();
}

void HttpServer::IOServer::stop(std::shared_ptr<session> session)
{
    sessions.erase(session);
    session->stop();
}

void HttpServer::IOServer::stop_all()
{
    for (auto s : sessions) {
        s->stop();
    }
    sessions.clear();
}

HttpServer::IOServer::IOServer(HttpServer& server) : server(server), acceptor(io_service)
{
    boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::tcp::v4(), server.port);
    acceptor.open(endpoint.protocol());
    acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
    acceptor.bind(endpoint);
}

HttpServer::HttpServer(boost::asio::ip::port_type port) : port(port) {}

HttpServer::~HttpServer()
{
    BOOST_LOG_TRIVIAL(debug) << "HttpServer destructor called, cleaning up resources...";
    stop();
    BOOST_LOG_TRIVIAL(debug) << "HttpServer destructor completed";
}

bool HttpServer::is_port_available(boost::asio::ip::port_type port)
{
    try {
        boost::asio::io_service        io_service;
        boost::asio::ip::tcp::acceptor acceptor(io_service);
        boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::tcp::v4(), port);

        acceptor.open(endpoint.protocol());
        acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.close();
        return true;
    } catch (const boost::system::system_error&) {
        return false;
    }
}

boost::asio::ip::port_type HttpServer::find_available_port(boost::asio::ip::port_type start_port)
{
    // 尝试从起始端口开始查找可用端口
    for (boost::asio::ip::port_type p = start_port; p < start_port + 1000; ++p) {
        if (is_port_available(p)) {
            return p;
        }
    }
    throw std::runtime_error("No available ports found");
}

void HttpServer::start()
{
    BOOST_LOG_TRIVIAL(info) << "start_http_service...";

    try {
        // 如果指定端口不可用，查找下一个可用端口
        if (!is_port_available(port)) {
            auto new_port = find_available_port(port + 1);
            BOOST_LOG_TRIVIAL(info) << "Original port " << port << " is in use, switching to port " << new_port;
            port = new_port;
        }

        start_http_server    = true;
        m_http_server_thread = create_thread([this] {
            try {
                set_current_thread_name("http_server");
                server_ = std::make_unique<IOServer>(*this);
                server_->acceptor.listen();
                server_->do_accept();
                server_->io_service.run();
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "HTTP server error: " << e.what();
                Slic3r::sentryReportLog(Slic3r::SENTRY_LOG_FATAL,std::string("bury_point_HttpServer::start ") + e.what(), BP_LOCAL_SERVER);
                start_http_server = false;
            }
        });

        // 等待服务器启动
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (!start_http_server) {
            throw std::runtime_error("Failed to start HTTP server");
        }

        BOOST_LOG_TRIVIAL(info) << "HTTP server started successfully on port " << port;
        
        // 启动健康检查
        BOOST_LOG_TRIVIAL(debug) << "Starting health check for HTTP server...";
        start_health_check();
        
        // 重启检查已集成到健康检查中，无需单独线程
        
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Failed to start HTTP server: " << e.what();
        std::string error_msg = "bury_point_Failed to start HTTP server on port " + std::to_string(port) + ": " + e.what();
        Slic3r::sentryReportLog(Slic3r::SENTRY_LOG_FATAL, error_msg.c_str(), BP_LOCAL_SERVER);
        start_http_server = false;
        throw;
    }
}

void HttpServer::stop()
{
    start_http_server = false;
    
    // 停止健康检查
    stop_health_check();
    
    // 重启检查已集成到健康检查中，无需单独停止
    
    if (server_) {
        server_->acceptor.close();
        server_->stop_all();
        server_->io_service.stop();
    }
    if (m_http_server_thread.joinable())
        m_http_server_thread.join();
    server_.reset();
}

void HttpServer::restart()
{
    BOOST_LOG_TRIVIAL(info) << "Restarting HTTP server on port " << port << "...";
    
    BOOST_LOG_TRIVIAL(debug) << "Stopping current HTTP server...";
    // 只停止HTTP服务器，不停止健康检查和重启检查线程
    start_http_server = false;
    
    if (server_) {
        server_->acceptor.close();
        server_->stop_all();
        server_->io_service.stop();
    }
    if (m_http_server_thread.joinable())
        m_http_server_thread.join();
    server_.reset();
    
    BOOST_LOG_TRIVIAL(debug) << "Waiting for resources to be released...";
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // 等待资源释放
    
    BOOST_LOG_TRIVIAL(debug) << "Starting new HTTP server...";
    start();
    
    BOOST_LOG_TRIVIAL(info) << "HTTP server restart completed";
}

bool HttpServer::is_healthy()
{
    if (!start_http_server || !server_) {
        BOOST_LOG_TRIVIAL(debug) << "Health check failed: server not started or server object is null";
        return false;
    }
    
    try {
        // 检查acceptor是否正常打开
        if (!server_->acceptor.is_open()) {
            BOOST_LOG_TRIVIAL(debug) << "Health check failed: acceptor is not open";
            return false;
        }
        
        // 检查io_service是否正在运行
        if (server_->io_service.stopped()) {
            BOOST_LOG_TRIVIAL(debug) << "Health check failed: io_service is stopped";
            return false;
        }
        
        // 尝试创建一个测试连接来验证服务器是否真正响应
        boost::asio::io_service test_io_service;
        boost::asio::ip::tcp::socket test_socket(test_io_service);
        boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::address::from_string("127.0.0.1"), port);
        
        boost::system::error_code ec;
        test_socket.connect(endpoint, ec);
        
        if (!ec) {
            test_socket.close();
            BOOST_LOG_TRIVIAL(debug) << "Health check passed: test connection successful on port " << port;
            return true;
        }
        
        BOOST_LOG_TRIVIAL(debug) << "Health check failed: test connection failed with error: " << ec.message();
        return false;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(debug) << "Health check failed with exception: " << e.what();
        return false;
    }
}

void HttpServer::start_health_check()
{
    std::lock_guard<std::mutex> lock(m_health_check_mutex);
    
    if (m_health_check_enabled) {
        BOOST_LOG_TRIVIAL(info) << "Health check is already running";
        return; // 已经在运行
    }
    
    BOOST_LOG_TRIVIAL(info) << "Starting HTTP server health check with interval: " << m_health_check_interval << "ms";
    m_health_check_enabled = true;
    m_health_check_thread = create_thread([this] {
        set_current_thread_name("http_health_check");
        
        while (true) {
            // 检查是否应该继续运行
            bool should_continue;
            int current_interval;
            {
                std::lock_guard<std::mutex> lock(m_health_check_mutex);
                should_continue = m_health_check_enabled;
                current_interval = m_health_check_interval;
            }
            
            if (!should_continue) {
                break;
            }
            
            // 使用条件变量等待，可以响应间隔变化
            {
                std::unique_lock<std::mutex> lock(m_health_check_mutex);
                if (m_health_check_cv.wait_for(lock, std::chrono::milliseconds(current_interval), 
                    [this] { return !m_health_check_enabled; })) {
                    // 如果等待被中断（健康检查被禁用），退出循环
                    break;
                }
            }
            
            // 再次检查是否应该继续运行
            {
                std::lock_guard<std::mutex> lock(m_health_check_mutex);
                if (!m_health_check_enabled) {
                    break;
                }
            }
            
            // 检查服务器是否健康，或者服务器是否已经停止运行
            if ((start_http_server && !is_healthy()) || !start_http_server) {
                BOOST_LOG_TRIVIAL(warning) << "HTTP server health check failed or server stopped, performing restart...";
                try {
                    // 在健康检查线程中直接执行重启，避免通过标志传递
                    restart();
                    BOOST_LOG_TRIVIAL(info) << "HTTP server restart completed by health check thread";
                } catch (const std::exception& e) {
                    BOOST_LOG_TRIVIAL(error) << "Failed to restart HTTP server: " << e.what();
                    std::string error_msg = "bury_point_HTTP server restart failed after health check on port " + std::to_string(port) + ": " + e.what();
                    Slic3r::sentryReportLog(Slic3r::SENTRY_LOG_ERROR, error_msg.c_str(), BP_LOCAL_SERVER);
                }
            } else if (start_http_server) {
                BOOST_LOG_TRIVIAL(debug) << "HTTP server health check passed";
            }
        }
        BOOST_LOG_TRIVIAL(info) << "Health check thread stopped";
    });
}

void HttpServer::stop_health_check()
{
    bool was_running = false;
    {
        std::lock_guard<std::mutex> lock(m_health_check_mutex);
        
        if (!m_health_check_enabled) {
            BOOST_LOG_TRIVIAL(debug) << "Health check is not running";
            return;
        }
        
        BOOST_LOG_TRIVIAL(info) << "Stopping HTTP server health check...";
        m_health_check_enabled = false;
        was_running = true;
        
        // 通知条件变量，确保健康检查线程能够立即响应停止信号
        m_health_check_cv.notify_all();
    } // 锁在这里自动释放
    
    if (was_running && m_health_check_thread.joinable()) {
        m_health_check_thread.join();
        BOOST_LOG_TRIVIAL(info) << "Health check thread joined successfully";
    }
}

void HttpServer::start_restart_check()
{
    std::lock_guard<std::mutex> lock(m_health_check_mutex);
    
    if (m_restart_check_enabled) {
        BOOST_LOG_TRIVIAL(info) << "Restart check is already running";
        return;
    }
    
    BOOST_LOG_TRIVIAL(info) << "Starting HTTP server restart check...";
    m_restart_check_enabled = true;
    m_restart_check_thread = create_thread([this] {
        set_current_thread_name("http_restart_check");
        
        while (true) {
            // 检查是否应该继续运行
            bool should_continue;
            {
                std::lock_guard<std::mutex> lock(m_health_check_mutex);
                should_continue = m_restart_check_enabled;
            }
            
            if (!should_continue) {
                break;
            }
            
            // 检查是否有重启请求
            if (is_restart_requested()) {
                BOOST_LOG_TRIVIAL(warning) << "HTTP server restart requested by health check, clearing restart flag...";
                // 清除重启请求标志，避免重复处理
                {
                    std::lock_guard<std::mutex> lock(m_health_check_mutex);
                    m_restart_requested = false;
                }
                
                // 在重启检查线程中，我们只负责清理标志，不直接调用restart()
                // 实际的重启操作由主线程或其他机制来处理
                BOOST_LOG_TRIVIAL(info) << "Restart flag cleared, restart should be handled externally";
            }
            
            // 等待一段时间再检查
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
        BOOST_LOG_TRIVIAL(info) << "Restart check thread stopped";
    });
}

void HttpServer::stop_restart_check()
{
    bool was_running = false;
    {
        std::lock_guard<std::mutex> lock(m_health_check_mutex);
        
        if (!m_restart_check_enabled) {
            BOOST_LOG_TRIVIAL(debug) << "Restart check is not running";
            return;
        }
        
        BOOST_LOG_TRIVIAL(info) << "Stopping HTTP server restart check...";
        m_restart_check_enabled = false;
        was_running = true;
    }
    
    if (was_running && m_restart_check_thread.joinable()) {
        m_restart_check_thread.join();
        BOOST_LOG_TRIVIAL(info) << "Restart check thread joined successfully";
    }
}

void HttpServer::set_health_check_interval(int interval_ms)
{
    std::lock_guard<std::mutex> lock(m_health_check_mutex);
    
    if (interval_ms > 0) {
        BOOST_LOG_TRIVIAL(info) << "Changing health check interval from " << m_health_check_interval << "ms to " << interval_ms << "ms";
        m_health_check_interval = interval_ms;
        // 通知条件变量，使新的间隔值立即生效
        m_health_check_cv.notify_all();
    } else {
        BOOST_LOG_TRIVIAL(warning) << "Invalid health check interval: " << interval_ms << "ms, must be positive";
    }
}

int HttpServer::get_health_check_interval() const
{
    std::lock_guard<std::mutex> lock(m_health_check_mutex);
    return m_health_check_interval;
}

bool HttpServer::is_health_check_enabled() const
{
    std::lock_guard<std::mutex> lock(m_health_check_mutex);
    return m_health_check_enabled;
}

bool HttpServer::is_restart_requested() const
{
    std::lock_guard<std::mutex> lock(m_health_check_mutex);
    return m_restart_requested;
}

void HttpServer::simulate_crash()
{
    BOOST_LOG_TRIVIAL(warning) << "Simulating HTTP server crash for testing restart mechanism...";
    
    try {
        if (server_) {
            // 关闭acceptor来模拟崩溃
            server_->acceptor.close();
            BOOST_LOG_TRIVIAL(info) << "Acceptor closed to simulate crash";
            
            // 停止io_service来模拟崩溃
            server_->io_service.stop();
            BOOST_LOG_TRIVIAL(info) << "IO service stopped to simulate crash";
        }
        
        // 设置标志，让健康检查能够检测到崩溃
        start_http_server = false;
        BOOST_LOG_TRIVIAL(info) << "Server state set to crashed for health check detection";
        
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Error during crash simulation: " << e.what();
    }
}

void HttpServer::set_request_handler(const std::function<std::shared_ptr<Response>(const std::string&)>& request_handler)
{
    this->m_request_handler = request_handler;
}

std::shared_ptr<HttpServer::Response> HttpServer::bbl_auth_handle_request(const std::string& url)
{
    BOOST_LOG_TRIVIAL(info) << "thirdparty_login: get_response";

    if (boost::contains(url, "access_token")) {
        std::string   redirect_url           = url_get_param(url, "redirect_url");
        std::string   access_token           = url_get_param(url, "access_token");
        std::string   refresh_token          = url_get_param(url, "refresh_token");
        std::string   expires_in_str         = url_get_param(url, "expires_in");
        std::string   refresh_expires_in_str = url_get_param(url, "refresh_expires_in");
        NetworkAgent* agent                  = wxGetApp().getAgent();

        unsigned int http_code;
        std::string  http_body;
        int          result = agent->get_my_profile(access_token, &http_code, &http_body);
        if (result == 0) {
            std::string user_id;
            std::string user_name;
            std::string user_account;
            std::string user_avatar;
            try {
                json user_j = json::parse(http_body);
                if (user_j.contains("uidStr"))
                    user_id = user_j["uidStr"].get<std::string>();
                if (user_j.contains("name"))
                    user_name = user_j["name"].get<std::string>();
                if (user_j.contains("avatar"))
                    user_avatar = user_j["avatar"].get<std::string>();
                if (user_j.contains("account"))
                    user_account = user_j["account"].get<std::string>();
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "Failed to parse user profile JSON: " << e.what();
                std::string error_msg = "bury_point_User profile JSON parse error: " + std::string(e.what());
                Slic3r::sentryReportLog(Slic3r::SENTRY_LOG_ERROR, error_msg.c_str(), BP_LOCAL_SERVER);
            }
            json j;
            j["data"]["refresh_token"]      = refresh_token;
            j["data"]["token"]              = access_token;
            j["data"]["expires_in"]         = expires_in_str;
            j["data"]["refresh_expires_in"] = refresh_expires_in_str;
            j["data"]["user"]["uid"]        = user_id;
            j["data"]["user"]["name"]       = user_name;
            j["data"]["user"]["account"]    = user_account;
            j["data"]["user"]["avatar"]     = user_avatar;
            agent->change_user(j.dump());
            if (agent->is_user_login()) {
                //wxGetApp().request_user_login(1);
            }
            GUI::wxGetApp().CallAfter([] { wxGetApp().ShowUserLogin(false); });
            std::string location_str = (boost::format("%1%?result=success") % redirect_url).str();
            return std::make_shared<ResponseRedirect>(location_str);
        } else {
            std::string error_str    = "get_user_profile_error_" + std::to_string(result);
            std::string location_str = (boost::format("%1%?result=fail&error=%2%") % redirect_url % error_str).str();
            return std::make_shared<ResponseRedirect>(location_str);
        }
    } else {
        return std::make_shared<ResponseNotFound>();
    }
}

std::shared_ptr<HttpServer::Response> HttpServer::web_server_handle_request(const std::string& url)
{
    BOOST_LOG_TRIVIAL(info) << "Handling file request for URL: " << url;

    std::string file_path = map_url_to_file_path(url);

    BOOST_LOG_TRIVIAL(info) << "Handling file_path request for URL: " << file_path;
    return std::make_shared<ResponseFile>(file_path);
}

std::string HttpServer::map_url_to_file_path(const std::string& url)
{
    if (url.find("..") != std::string::npos) {
        return "";
    }

    wxString trimmed_url = wxString::FromUTF8(url);

    size_t question_mark = trimmed_url.find('?');
    if (question_mark != wxString::npos) {
        trimmed_url = trimmed_url.substr(0, question_mark);
    }

    if (trimmed_url == "/") {
        trimmed_url = "/flutter_web/index.html"; // 默认首页
    } else if (trimmed_url.substr(0, 11) == "/localfile/") {
        auto real_path = trimmed_url.substr(11);
        return real_path.ToStdString(wxConvUTF8);
    }
    auto data_web_path = boost::filesystem::path(data_dir()) / "web";
    if (!boost::filesystem::exists(data_web_path / "flutter_web")) {
        auto source_path = boost::filesystem::path(resources_dir()) / "web" / "flutter_web";
        auto target_path = data_web_path / "flutter_web";
        copy_directory_recursively(source_path, target_path);
    }

    if (trimmed_url.find("flutter_web") == std::string::npos) {
        wxString res = wxString::FromUTF8(resources_dir()) + trimmed_url;
        return res.ToStdString(wxConvUTF8);
    } else {
        wxString res = wxString::FromUTF8(data_dir()) + trimmed_url;
        return res.ToStdString(wxConvUTF8);
    }
}

void HttpServer::ResponseRedirect::write_response(std::stringstream& ssOut)
{
    const std::string sHTML          = "<html><body><p>redirect to url </p></body></html>";
    size_t            content_length = sHTML.size(); // 字节长度（与字符数相同，因无多字节字符）

    ssOut << "HTTP/1.1 302 Found\r\n";
    ssOut << "Location: " << location_str << "\r\n";
    ssOut << "Content-Type: text/html\r\n";
    ssOut << "Content-Length: " << content_length << "\r\n"; // 正确计算长度
    ssOut << "Access-Control-Allow-Origin: *\r\n";           // CORS头
    ssOut << "\r\n";                                         // 头和主体之间的空行（必须）
    ssOut << sHTML;                                          // 响应体（长度必须匹配）
}

void HttpServer::ResponseNotFound::write_response(std::stringstream& ssOut)
{
    const std::string sHTML          = "<html><body><h1>404 Not Found</h1><p>There's nothing here.</p></body></html>";
    size_t            content_length = sHTML.size(); // 字节长度

    ssOut << "HTTP/1.1 404 Not Found\r\n";
    ssOut << "Content-Type: text/html\r\n";
    ssOut << "Content-Length: " << content_length << "\r\n"; // 正确计算长度
    ssOut << "Access-Control-Allow-Origin: *\r\n";           // CORS头
    ssOut << "\r\n";                                         // 头和主体之间的空行（必须）
    ssOut << sHTML;                                          // 响应体（长度必须匹配）
}

void HttpServer::ResponseFile::write_response(std::stringstream& ssOut)
{
    // 将UTF-8路径转换为适合文件系统操作的编码，自动适配Windows的UTF-8模式
    std::string system_file_path = utf8_to_filesystem_encoding(file_path);
    
    std::ifstream file(system_file_path, std::ios::binary);
    if (!file) {
        ResponseNotFound notFoundResponse;
        notFoundResponse.write_response(ssOut);
        return;
    }

    // 读取文件内容并计算长度（关键：使用字节长度）
    std::ostringstream fileStream;
    fileStream << file.rdbuf();
    std::string fileContent    = fileStream.str();
    size_t      content_length = fileContent.size(); // 字节长度，非字符数

    // 确定Content-Type（保持原有逻辑）
    std::string content_type = "application/octet-stream";
    if (ends_with(file_path, ".html"))
        content_type = "text/html";
    else if (ends_with(file_path, ".css"))
        content_type = "text/css";
    else if (ends_with(file_path, ".js"))
        content_type = "text/javascript";
    else if (ends_with(file_path, ".png"))
        content_type = "image/png";
    else if (ends_with(file_path, ".jpg"))
        content_type = "image/jpeg";
    else if (ends_with(file_path, ".gif"))
        content_type = "image/gif";
    else if (ends_with(file_path, ".svg"))
        content_type = "image/svg+xml";
    else if (ends_with(file_path, ".ttf"))
        content_type = "application/x-font-ttf";
    else if (ends_with(file_path, ".json"))
        content_type = "application/json";
    else if (ends_with(file_path, ".webp"))
        content_type = "image/webp";
    else if (ends_with(file_path, ".woff"))
        content_type = "font/woff";
    else if (ends_with(file_path, ".woff2"))
        content_type = "font/woff2";

    // 构造响应头（严格使用\r\n，头结束后空行）
    ssOut << "HTTP/1.1 200 OK\r\n";
    ssOut << "Content-Type: " << content_type << "\r\n";
    ssOut << "Content-Length: " << content_length << "\r\n"; // 必须与实际内容长度一致
    ssOut << "Access-Control-Allow-Origin: *\r\n";           // CORS头
    ssOut << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    ssOut << "Access-Control-Allow-Headers: Content-Type, Authorization\r\n";
    ssOut << "\r\n";      // 头和主体之间的空行（必须）
    ssOut << fileContent; // 响应体（长度必须与Content-Length一致）
}

}} // namespace Slic3r::GUI