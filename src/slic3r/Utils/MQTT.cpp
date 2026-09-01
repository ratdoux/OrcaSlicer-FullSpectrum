#include "MQTT.hpp"
#include <thread>
#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <future>
#include <fstream>
#include <chrono>

namespace {

// Paho throws when disconnect() is called while already disconnected / handle gone.
bool is_soft_disconnect_error(const mqtt::exception& e)
{
    const int rc = e.get_return_code();
    return rc == MQTTASYNC_DISCONNECTED || rc == MQTTASYNC_FAILURE;
}

} // namespace

// Constructor: Initialize MQTT client with server address and client ID
// @param server_address: Address of the MQTT broker
// @param client_id: Unique identifier for this client
// @param clean_session: Whether to start with a clean session
MqttClient::MqttClient(const std::string& server_address, const std::string& client_id, const std::string& username, const std::string& password,  bool clean_session)
    : server_address_(server_address)
    , client_id_(client_id)
    , client_(nullptr)
    , connOpts_()
    , subListener_("Subscription")
    , connected_(false)            
    , is_reconnecting(false)
    , connection_failure_callback_(nullptr)
    , pending_reconnect_checks{0}  
    , ever_connected_(false)  
{
    BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] initializing MQTT connection, server_address: " << server_address << ", client_id: " << client_id;

    try {
        client_ = std::make_unique<mqtt::async_client>(server_address_, client_id_);

        // Configure connection options    
        connOpts_.set_clean_session(false);
        connOpts_.set_keep_alive_interval(30);
        connOpts_.set_connect_timeout(10);
        // auto-reconnect enabled only after first successful connection
        connOpts_.set_automatic_reconnect(std::chrono::seconds(0), std::chrono::seconds(0));
        client_->set_callback(*this);

        // set authentication info
        if (!username.empty()) {
            connOpts_.set_user_name(username);
            if (!password.empty()) {
                connOpts_.set_password(password);
            }
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] MQTT client construction failed: " << e.what();
        client_.reset();
    }
}

// SSL/TLS
MqttClient::MqttClient(const std::string& server_address, 
                      const std::string& client_id,
                      const std::string& ca_content,        
                      const std::string& cert_content,
                      const std::string& key_content,
                      const std::string& username,
                      const std::string& password,
                      bool clean_session)
    : MqttClient(server_address, client_id, username, password, clean_session)
{
    BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] initializing MQTT SSL connection, server_address: " << server_address << ", client_id: " << client_id
                            << ", ca_content: " << ca_content << ", cert_content: " << cert_content << ", username: " << username
                            << ", password: " << password;

    if (!client_) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] skip SSL initialization because MQTT client was not created";
        return;
    }
    
    try {
        boost::filesystem::path temp_dir = boost::filesystem::temp_directory_path();

        boost::filesystem::path ca_path = temp_dir / ("ca_" + client_id + std::to_string(int64_t(this)) + ".pem");
        if (!ca_content.empty()) {
            boost::filesystem::ofstream ca_file(ca_path);
            ca_file << ca_content;
            ca_file.close();
            if (!ca_file) {
                BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] failed to save CA certificate: " << ca_path;
                throw std::runtime_error("Failed to write CA certificate temporary file");
            }
            BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] CA certificate saved to: " << ca_path;
        }
        
        boost::filesystem::path cert_path = temp_dir / ("cert_" + client_id + std::to_string(int64_t(this)) + ".pem");
        if (!cert_content.empty()) {
            boost::filesystem::ofstream cert_file(cert_path);
            cert_file << cert_content;
            cert_file.close();
            if (!cert_file) {
                BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] failed to save client certificate: " << cert_path;
                throw std::runtime_error("Failed to write client certificate temporary file");
            }
            BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] client certificate saved to: " << cert_path;
        }
        
        boost::filesystem::path key_path = temp_dir / ("key_" + client_id + std::to_string(int64_t(this)) + ".pem");
        if (!key_content.empty()) {
            boost::filesystem::ofstream key_file(key_path);
            key_file << key_content;
            key_file.close();
            if (!key_file) {
                BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] failed to save private key: " << key_path;
                throw std::runtime_error("Failed to write private key temporary file");
            }
            BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] private key saved to: " << key_path;
        }
        
        mqtt::ssl_options ssl_opts;                
        ssl_opts.set_verify(false);
        ssl_opts.set_enable_server_cert_auth(true);
                
        if (!ca_content.empty()) {
            ssl_opts.set_trust_store(ca_path.string());
        }
        if (!cert_content.empty() && !key_content.empty()) {
            ssl_opts.set_key_store(cert_path.string());
            ssl_opts.set_private_key(key_path.string());
        }

        ssl_opts.set_ssl_version(MQTT_SSL_VERSION_TLS_1_2);                
        connOpts_.set_ssl(ssl_opts);             
        temp_ca_path_ = ca_path;
        temp_cert_path_ = cert_path;
        temp_key_path_ = key_path;
    } catch (const std::exception& e) {
        cleanup_temp_files();
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] MQTT SSL initialization failed: " << e.what();
        client_.reset();
    }
}

// Establish connection to the MQTT broker
// @return: true if connection successful, false otherwise
bool MqttClient::Connect(std::string& msg)
{
    if (connected_.load(std::memory_order_acquire)) {
        BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] " << client_id_ 
                                   << " Already connected to MQTT server " << server_address_;
        msg = "success";
        return true;
    }

    if (!client_) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Cannot connect: MQTT client pointer is null";
        msg = "MQTT client is null";
        connected_.store(false, std::memory_order_release);
        return false;
    }

    try {
         {
            auto ssl_opts = connOpts_.get_ssl_options();
            BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] SSL config info:"
                << "\n - server address: " << server_address_
                << "\n - client id: " << client_id_
                << "\n - CA length: " << (ssl_opts.get_trust_store().empty() ? 0 : ssl_opts.get_trust_store().length())
                << "\n - client cert length: " << (ssl_opts.get_key_store().empty() ? 0 : ssl_opts.get_key_store().length())
                << "\n - private key length: " << (ssl_opts.get_private_key().empty() ? 0 : ssl_opts.get_private_key().length());
        }

        const char* context = "connection";
        mqtt::token_ptr conntok = client_->connect(connOpts_, (void*)(context), *this);
        
        if (!conntok->wait_for(std::chrono::seconds(20))) {
            auto rc = conntok->get_return_code();
            auto reason = conntok->get_reason_code();
            
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Connection timeout. Return code: " << rc 
                                    << ", Reason code: " << static_cast<int>(reason)
                                    << ", Server: " << server_address_;

            if (rc == MQTTASYNC_FAILURE) {
                BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Connection failed - MQTTASYNC_FAILURE";
                msg = "MQTTASYNC_FAILURE";
            } else if (rc == MQTTASYNC_DISCONNECTED) {
                BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Connection failed - MQTTASYNC_DISCONNECTED";
                msg = "MQTTASYNC_DISCONNECTED";
            }
            
            connected_.store(false, std::memory_order_release);
            return false;
        }

        if (!client_->is_connected()) {
            auto rc = conntok->get_return_code();
            auto reason = conntok->get_reason_code();
            
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Connection failed. Return code: " << rc 
                                    << ", Reason code: " << static_cast<int>(reason)
                                    << ", Server: " << server_address_;

            msg = "connetion failed, Return code: " + std::to_string(rc) + " Reason code: " + std::to_string(reason);
            
            connected_.store(false, std::memory_order_release);
            return false;
        }

        connected_.store(true, std::memory_order_release);
        BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] Successfully connected to MQTT server";
        msg = "success";
        return true;
    } catch (const mqtt::exception& exc) {
        connected_.store(false, std::memory_order_release);
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] MQTT exception during connect: " << exc.what()
                                << ", Return code: " << exc.get_return_code()
                                << ", Message: " << exc.get_message();
        msg = std::string(exc.what()) + ";" + exc.get_reason_code_str() + ";" + exc.get_message();
        return false;
    } catch (const std::exception& e) {
        connected_.store(false, std::memory_order_release);
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] General exception during connect: " << e.what();
        msg = std::string(e.what());
        return false;
    }
}

// Disconnect from the MQTT broker
// @return: true if disconnection successful, false otherwise
bool MqttClient::Disconnect(std::string& msg)
{
    connected_.store(false, std::memory_order_release);

    if (!client_) {
        BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] Disconnect completed (no client)";
        msg = "success";
        return true;
    }

    // Always call disconnect(): even if already down, C lib clears shouldBeConnected
    // (stops auto-reconnect) then returns MQTTASYNC_DISCONNECTED which C++ throws.
    try {
        auto disctok = client_->disconnect();
        if (!disctok->wait_for(std::chrono::seconds(5))) {
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] MQTT disconnect timeout";
        }
    } catch (const mqtt::exception& e) {
        if (is_soft_disconnect_error(e)) {
            BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] MQTT disconnect soft error (already disconnected), rc="
                                      << e.get_return_code();
            msg = "success";
            return true;
        }
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] MQTT disconnect failed: " << e.what()
                                 << ", rc=" << e.get_return_code();
        msg = e.what();
        return false;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] General exception during disconnect: " << e.what();
        msg = e.what();
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] Disconnect completed";
    msg = "success";
    return true;
}

// Subscribe to a specific MQTT topic
// @param topic: The topic to subscribe to
// @param qos: Quality of Service level (0, 1, or 2)
// @return: true if subscription successful, false otherwise
bool MqttClient::Subscribe(const std::string& topic, int qos, std::string& msg)
{
    if (!CheckConnected()) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Cannot subscribe: client not connected";

        msg = "client not connected";
        return false;
    }

    try {
        BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] Subscribing to MQTT topic '" << topic << "' with QoS " << qos;
        mqtt::token_ptr subtok = client_->subscribe(topic, qos, nullptr, subListener_);
        if (!subtok->wait_for(std::chrono::seconds(5))) {
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Subscribe timeout for topic: " << topic;
            msg = "subscribe timeout";
            return false;
        }
        add_topic_to_resubscribe(topic, qos);
        msg = "success";
        return true;
    } catch (const mqtt::exception& exc) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Error subscribing to topic '" << topic << "': " << exc.what();
        msg = "Error: " + std::string(exc.what());
        return false;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] General exception subscribing to topic '" << topic << "': " << e.what();
        msg = "Error: " + std::string(e.what());
        return false;
    }
}

// Unsubscribe from a specific MQTT topic
// @param topic: The topic to unsubscribe from
// @return: true if unsubscription successful, false otherwise
bool MqttClient::Unsubscribe(const std::string& topic, std::string& msg)
{
    if (!CheckConnected()) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Cannot unsubscribe: client not connected";
        msg = "client not connect"; 
        return false;
    }

    try {
        BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] Unsubscribing from MQTT topic '" << topic << "'";
        mqtt::token_ptr unsubtok = client_->unsubscribe(topic);
        if (!unsubtok->wait_for(std::chrono::seconds(5))) {
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Unsubscribe timeout for topic: " << topic;
            msg = "Unsubscribe timeout for topic";
            return false;
        }
        remove_topic_from_resubscribe(topic);
        msg = "success";
        return true;
    } catch (const mqtt::exception& exc) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Error unsubscribing from topic '" << topic << "': " << exc.what();
        msg = "Error unsubscribing from topic: " + std::string(exc.what());
        return false;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] General exception unsubscribing from topic '" << topic << "': " << e.what();
        msg = "Error unsubscribing from topic: " + std::string(e.what());
        return false;
    }
}

// Publish a message to a specific MQTT topic
// @param topic: The topic to publish to
// @param payload: The message content
// @param qos: Quality of Service level (0, 1, or 2)
// @return: true if publish successful, false otherwise
bool MqttClient::Publish(const std::string& topic, const std::string& payload, int qos, std::string& msg)
{
    if (!CheckConnected()) {
        msg = "client not connect";
        return false;
    }

    try {
        mqtt::message_ptr pubmsg = mqtt::make_message(topic, payload);
        pubmsg->set_qos(qos);

        BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] Publishing message to topic '" << topic << "' with QoS " << qos;
        mqtt::token_ptr pubtok = client_->publish(pubmsg);
        /*if (!pubtok->wait_for(std::chrono::seconds(5))) {
            BOOST_LOG_TRIVIAL(error) << "Publish timeout for topic: " << topic;
            return false;
        }*/
        msg = "success";
        return true;
    } catch (const mqtt::exception& exc) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Error publishing to topic '" << topic << "': " << exc.what();
        msg = "error: " + std::string(exc.what());
        return false;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] General exception publishing to topic '" << topic << "': " << e.what();
        msg = "error: " + std::string(e.what());
        return false;
    }
}

// Set callback function for handling incoming messages
// @param callback: Function to be called when a message arrives
void MqttClient::SetMessageCallback(std::function<void(const std::string& topic, const std::string& payload)> callback)
{
    message_callback1_ = nullptr;
    message_callback_ = callback;
}

void MqttClient::SetMessageCallback(std::function<void(const std::string& topic, const std::string& payload, void* this_)> callback)
{
    message_callback_  = nullptr;
    message_callback1_ = callback;
}

// Check if the client is currently connected
// @return: true if connected, false otherwise
bool MqttClient::CheckConnected()
{
    try {
        if (!connected_.load(std::memory_order_acquire)) {
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] MQTT client is not connected to server";
            return false;
        }

        if (!client_) {
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] MQTT client pointer is null";
            connected_.store(false, std::memory_order_release);
            return false;
        }

        auto check_future = std::async(std::launch::async, [this]() {
            return client_->is_connected();
        });

        if (check_future.wait_for(std::chrono::seconds(3)) == std::future_status::timeout) {
            BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] Connection status check timeout";
            connected_.store(false, std::memory_order_release);
            return false;
        }

        if (!check_future.get()) {
            connected_.store(false, std::memory_order_release);
            return false;
        }

        return true;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Exception checking connection: " << e.what();
        connected_.store(false, std::memory_order_release);
        return false;
    }
}

// Callback when connection is lost
// Implements automatic reconnection with retry logic
// @param cause: Reason for connection loss
void MqttClient::connection_lost(const std::string& cause)
{
    BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] MQTT connection lost, address: " << this->server_address_;
    if (!cause.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] Cause: " << cause;
    }

    connected_.store(false, std::memory_order_release);
        
    if (!ever_connected_.load(std::memory_order_acquire)) {
        BOOST_LOG_TRIVIAL(error) << "The first connection failed. Since no successful connection has been made before, automatic reconnection remains disabled";
        if (connection_failure_callback_) {
            connection_failure_callback_();
        }
        return;
    }
    
    if (!is_reconnecting.load(std::memory_order_acquire)) {
        is_reconnecting.store(true, std::memory_order_release);
        pending_reconnect_checks.fetch_add(1, std::memory_order_release);
        
        
        {
            std::weak_ptr<MqttClient> weak_self = shared_from_this();
        
            std::thread([weak_self]() {
                
                std::this_thread::sleep_for(std::chrono::seconds(20));

                if (auto self = weak_self.lock()) {

                    if (self->client_ && self->is_reconnecting.load(std::memory_order_acquire)) {
                        int remaining = self->pending_reconnect_checks.fetch_sub(1, std::memory_order_acq_rel);
                        
                        if (remaining == 0){
                            self->is_reconnecting.store(false, std::memory_order_release);
                        } else if (remaining <= 1) {
                            if (!self->connected_.load(std::memory_order_acquire)) {
                                BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] MQTT connection not restored after 20 seconds";
                                std::string dc_msg = "";
                                self->Disconnect(dc_msg);
                                if (self->connection_failure_callback_) {
                                    self->connection_failure_callback_();
                                }
                            }
                            
                            self->is_reconnecting.store(false, std::memory_order_release);
                        }
                    } else {                        
                        BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] MQTT Client has been destructed";
                    }
                }
            }).detach();            
        }       
    } else {        
        pending_reconnect_checks.fetch_add(1, std::memory_order_release);
    }
}

// Callback when a message arrives
// @param msg: Pointer to the received message
void MqttClient::message_arrived(mqtt::const_message_ptr msg)
{
    if (message_callback_) {
        message_callback_(msg->get_topic(), msg->to_string());
    }

    if (message_callback1_) {
        message_callback1_(msg->get_topic(), msg->to_string(), this);
    }
}

// Callback when message delivery is complete
// @param token: Delivery token containing message details
void MqttClient::delivery_complete(mqtt::delivery_token_ptr token)
{
    BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] Message delivery complete for token: " << (token ? token->get_message_id() : -1);
}

// Callback for operation failure
// @param tok: Token containing operation details
void MqttClient::on_failure(const mqtt::token& tok)
{ 
    if (tok.get_user_context() && 
        std::string(static_cast<const char*>(tok.get_user_context())) == "connection") {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] MQTT connection attempt failed";
        if (tok.get_reason_code() != 0) {
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Reason code: " << tok.get_reason_code();
        }
 
        BOOST_LOG_TRIVIAL(error) << "The first connection failed. Since no successful connection has been made before, automatic reconnection remains disabled";
        
        connected_.store(false, std::memory_order_release);
                
        if (connection_failure_callback_) {
            connection_failure_callback_();
        }
    } else {        
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Operation failed for token: " << tok.get_message_id();
        if (tok.get_reason_code() != 0) {
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Reason code: " << tok.get_reason_code();
        }
    }
}

// Callback for operation success
// @param tok: Token containing operation details
void MqttClient::on_success(const mqtt::token& tok)
{
    BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] Operation successful for token: " << tok.get_message_id();
    auto top = tok.get_topics();
    if (top && !top->empty()) {
        BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] Token topic: '" << (*top)[0] << "'";
    }
}

void MqttClient::connected(const std::string& cause)
{

    connected_.store(true, std::memory_order_release);
    is_reconnecting.store(false, std::memory_order_release);
    
    ever_connected_.store(true, std::memory_order_release);
        
    connOpts_.set_automatic_reconnect(std::chrono::seconds(2), std::chrono::seconds(30));
    BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] auto-reconnect enabled after successful connection";
}

void MqttClient::resubscribe_topics() {
    if (topics_to_resubscribe_.empty() || !client_) {
        return;
    }

    for (const auto& topic_pair : topics_to_resubscribe_) {
        try {
            auto tok = client_->subscribe(topic_pair.first, topic_pair.second, nullptr,  subListener_);
            if (!tok->wait_for(std::chrono::seconds(5))) {
                BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] Subscribe timeout for topic: " << topic_pair.first;
                continue;
            }
            if (!tok->is_complete() || tok->get_return_code() != 0) {
                BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] Failed to resubscribe to topic: " << topic_pair.first;
            }
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Error resubscribing to topic '" << topic_pair.first << "': " << e.what();
        }
    }
}

void MqttClient::add_topic_to_resubscribe(const std::string& topic, int qos) {
    topics_to_resubscribe_[topic] = qos;
    BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] Added topic to resubscribe list: " << topic;
}

void MqttClient::remove_topic_from_resubscribe(const std::string& topic) {
    auto it = topics_to_resubscribe_.find(topic);
    if (it != topics_to_resubscribe_.end()) {
        topics_to_resubscribe_.erase(it);
        BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] Removed topic from resubscribe list: " << topic;
    }
}

MqttClient::~MqttClient()
{
    {        
        connected_.store(false, std::memory_order_release);
        is_reconnecting.store(false, std::memory_order_release);
                
        int timeout_count = 0;
        const int max_timeout = 20; // max 5s (50 * 100ms)
        while (pending_reconnect_checks.load(std::memory_order_acquire) > 0 && timeout_count < max_timeout) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            timeout_count++;
        }
        
        if (timeout_count >= max_timeout) {
            BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] timeout waiting for reconnect checks, forcing destruction";
        }
                
        if (client_) {
            // Reuse Disconnect: soft-catches already-disconnected and clears shouldBeConnected.
            std::string dc_msg;
            Disconnect(dc_msg);
        }
     
        topics_to_resubscribe_.clear();             
        message_callback_ = nullptr;
        message_callback1_           = nullptr;
        connection_failure_callback_ = nullptr;

        if (client_)
            client_.reset();             

        cleanup_temp_files();        
        BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] MQTT client resources freed";
    }
}

void MqttClient::cleanup_temp_files()
{
    {
        if (!temp_ca_path_.empty() && boost::filesystem::exists(temp_ca_path_)) {
            boost::filesystem::remove(temp_ca_path_);
        }
        if (!temp_cert_path_.empty() && boost::filesystem::exists(temp_cert_path_)) {
            boost::filesystem::remove(temp_cert_path_);
        }
        if (!temp_key_path_.empty() && boost::filesystem::exists(temp_key_path_)) {
            boost::filesystem::remove(temp_key_path_);
        }
    }
}
