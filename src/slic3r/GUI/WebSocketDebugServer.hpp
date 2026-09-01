// WebSocket Debug Server for Flutter Web debugging
#ifndef WEBSOCKET_DEBUG_SERVER_HPP
#define WEBSOCKET_DEBUG_SERVER_HPP

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <thread>
#include <memory>
#include <functional>
#include <mutex>
#include <queue>
#include <atomic>
#include <string>
#include <vector>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace Slic3r { namespace GUI {

class WebSocketDebugServer {
public:
    using MessageCallback = std::function<void(const std::string&)>;

    explicit WebSocketDebugServer(unsigned short port = 8766);
    ~WebSocketDebugServer();

    // Start the WebSocket server
    bool start();

    // Stop the WebSocket server
    void stop();

    // Send message to Flutter Web client
    void send_message(const std::string& message);

    // Set callback for receiving messages
    void set_message_callback(MessageCallback callback);

    // Check if a client is connected
    bool has_client() const { return m_has_client.load(); }

    // Check if server is running
    bool is_running() const { return m_running.load(); }

private:
    void accept_loop();
    void session_loop(tcp::socket socket);
    void send_worker();

    unsigned short m_port;
    std::atomic<bool> m_running;
    std::atomic<bool> m_has_client;

    std::unique_ptr<net::io_context> m_io_context;
    std::unique_ptr<tcp::acceptor> m_acceptor;
    std::thread m_accept_thread;
    std::thread m_send_thread;
    std::vector<std::thread> m_session_threads;

    std::shared_ptr<websocket::stream<tcp::socket>> m_ws_stream;
    std::vector<std::shared_ptr<websocket::stream<tcp::socket>>> m_old_streams;
    MessageCallback m_message_callback;

    std::mutex m_send_mutex;
    std::condition_variable m_send_cv;
    std::queue<std::string> m_send_queue;
    std::mutex m_client_mutex;
};

}} // namespace Slic3r::GUI

#endif // WEBSOCKET_DEBUG_SERVER_HPP
