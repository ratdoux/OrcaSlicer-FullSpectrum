// WebSocket Debug Server implementation
#include "WebSocketDebugServer.hpp"
#include <boost/log/trivial.hpp>
#include <iostream>

namespace Slic3r { namespace GUI {

WebSocketDebugServer::WebSocketDebugServer(unsigned short port)
    : m_port(port)
    , m_running(false)
    , m_has_client(false)
{
    BOOST_LOG_TRIVIAL(info) << "WebSocketDebugServer created on port " << m_port;
}

WebSocketDebugServer::~WebSocketDebugServer()
{
    stop();
}

bool WebSocketDebugServer::start()
{
    if (m_running.load()) {
        BOOST_LOG_TRIVIAL(warning) << "WebSocket Debug Server already running";
        return true;
    }

    m_io_context = std::make_unique<net::io_context>();

    tcp::endpoint endpoint(tcp::v4(), m_port);
    m_acceptor = std::make_unique<tcp::acceptor>(*m_io_context, endpoint);

    m_running.store(true);

    // Start accept thread
    m_accept_thread = std::thread(&WebSocketDebugServer::accept_loop, this);

    // Start send worker thread
    m_send_thread = std::thread(&WebSocketDebugServer::send_worker, this);

    BOOST_LOG_TRIVIAL(info) << " WebSocket Debug Server started on ws://localhost:" << m_port;
    BOOST_LOG_TRIVIAL(info) << "   Waiting for Flutter Web client to connect...";
    return true;

}

void WebSocketDebugServer::stop()
{
    if (!m_running.load()) 
        return;

    BOOST_LOG_TRIVIAL(info) << "Stopping WebSocket Debug Server...";
    m_running.store(false);
    m_send_cv.notify_all(); 

    if (m_acceptor && m_acceptor->is_open()) 
    {
        boost::system::error_code ec;
        m_acceptor->close(ec);

        if (ec)
            BOOST_LOG_TRIVIAL(warning) << "Error closing acceptor: " << ec.message();        
    }

    // Close WebSocket connection
    if (m_ws_stream) 
    {
        boost::system::error_code ec;
        m_ws_stream->close(websocket::close_code::normal, ec);

        if (ec)         
            BOOST_LOG_TRIVIAL(warning) << "Error closing WebSocket: " << ec.message();        
    }

    // Stop io_context
    if (m_io_context)
        m_io_context->stop();
    
    // Join threads
    if (m_accept_thread.joinable())    
        m_accept_thread.join();
    
    if (m_send_thread.joinable()) 
        m_send_thread.join();

    for (auto& t : m_session_threads) 
        if (t.joinable()) t.join();
    
    m_session_threads.clear();
    m_has_client.store(false);

    BOOST_LOG_TRIVIAL(info) << "WebSocket Debug Server stopped";
}

void WebSocketDebugServer::accept_loop()
{
    while (m_running.load())
    {       
        tcp::socket socket(*m_io_context);

        // Accept connection (blocking)
        boost::system::error_code ec;
        m_acceptor->accept(socket, ec);

        if (ec)
        {

            if (m_running.load()) 
                BOOST_LOG_TRIVIAL(error) << "Accept error: " << ec.message(); 

            continue;
        }

        BOOST_LOG_TRIVIAL(info) << "Flutter Web client connected from "
                                << socket.remote_endpoint().address().to_string();

        // Spawn a thread per session so accept_loop stays unblocked
        m_session_threads.emplace_back(&WebSocketDebugServer::session_loop, this, std::move(socket));        
    }
}

void WebSocketDebugServer::session_loop(tcp::socket socket)
{
    // Create WebSocket stream
    auto ws = std::make_shared<websocket::stream<tcp::socket>>(std::move(socket));

    // Set WebSocket options
    ws->set_option(websocket::stream_base::decorator(
        [](websocket::response_type& res) {
            res.set(beast::http::field::server, "OrcaSlicer-Debug-Server");
        }
    ));

    // Accept WebSocket handshake
    ws->accept();

    // Swap in the new stream under the lock, then close old streams outside
    // the lock so send_worker is never blocked waiting for TCP teardown.
    std::vector<std::shared_ptr<websocket::stream<tcp::socket>>> to_close;
    {
        std::lock_guard<std::mutex> lock(m_client_mutex);
        to_close = std::move(m_old_streams);
        if (m_ws_stream) {
            to_close.push_back(m_ws_stream);
        }
        m_ws_stream = ws;
        m_has_client.store(true);
    }

    for (auto& old : to_close) {
        if (old && old->is_open()) {
            boost::system::error_code ec;
            old->close(websocket::close_code::going_away, ec);
        }
    }

    BOOST_LOG_TRIVIAL(info) << " WebSocket handshake completed, client ready";

    // Message receive loop
    while (m_running.load()) {
        beast::flat_buffer buffer;

        boost::system::error_code ec;
        ws->read(buffer, ec);

        if (ec == websocket::error::closed) {
            BOOST_LOG_TRIVIAL(info) << "Client closed connection";
            break;
        }

        if (ec) {
            BOOST_LOG_TRIVIAL(error) << "Read error: " << ec.message();
            break;
        }

        std::string message = beast::buffers_to_string(buffer.data());

        BOOST_LOG_TRIVIAL(debug) << "Received from Flutter: " << message.substr(0, 200)
                                    << (message.length() > 200 ? "..." : "");

        // Call message callback
        if (m_message_callback)
                m_message_callback(message);       
    }

    // Clean up
    {
        std::lock_guard<std::mutex> lock(m_client_mutex);
        if (m_ws_stream == ws) {
            m_ws_stream.reset();
            m_has_client.store(false);
        }
    }

    BOOST_LOG_TRIVIAL(info) << " Flutter Web client disconnected";
}

void WebSocketDebugServer::send_worker()
{
    while (m_running.load()) {
        std::string message;

        {
            std::unique_lock<std::mutex> lock(m_send_mutex);
            m_send_cv.wait(lock, [this] {
                return !m_send_queue.empty() || !m_running.load();
            });

            if (!m_running.load()) break;

            message = m_send_queue.front();
            m_send_queue.pop();
        }

        if (!message.empty()) {
            std::lock_guard<std::mutex> lock(m_client_mutex);
            if (m_ws_stream && m_has_client.load()) 
            {

                    boost::system::error_code ec;
                    m_ws_stream->write(net::buffer(message), ec);

                    if (ec) 
                        BOOST_LOG_TRIVIAL(error) << "Send error: " << ec.message();
                     else 
                        BOOST_LOG_TRIVIAL(debug) << "Sent to Flutter: " << message.substr(0, 200)
                                                 << (message.length() > 200 ? "..." : "");                    

            }
        }
    }
}

void WebSocketDebugServer::send_message(const std::string& message)
{
    if (!m_running.load()) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot send message: server not running";
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_send_mutex);
        m_send_queue.push(message);
    }
    m_send_cv.notify_one();
}

void WebSocketDebugServer::set_message_callback(MessageCallback callback)
{
    m_message_callback = callback;
}

}} // namespace Slic3r::GUI
