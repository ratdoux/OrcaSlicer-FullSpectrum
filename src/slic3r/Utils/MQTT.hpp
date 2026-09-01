#ifndef MQTT_H
#define MQTT_H

#include <functional>
#include <string>
#include <map>
#include <mqtt/async_client.h>
#include <boost/log/trivial.hpp>
#include <memory>
#include <atomic>
#include <boost/filesystem.hpp>

// Number of retries for connection and subscription attempts
#define CONNECT_RETRY_TIME 3
#define SUBSCRIBE_RETRY_TIME 3

// Action listener class to handle MQTT operation callbacks
class action_listener : public virtual mqtt::iaction_listener
{
private:
    std::string name_;  // Name identifier for the listener

public:
    // Constructor with a name for identification
    action_listener(const std::string& name) : name_(name) {}

    // Called when an MQTT operation fails
    void on_failure(const mqtt::token& tok) override
    {
        BOOST_LOG_TRIVIAL(error) << name_ << " operation failed";
        if (tok.get_message_id() != 0) {
            BOOST_LOG_TRIVIAL(error) << "Token: [" << tok.get_message_id() << "]";
        }
    }

    // Called when an MQTT operation succeeds
    void on_success(const mqtt::token& tok) override
    {
        BOOST_LOG_TRIVIAL(debug) << name_ << " operation successful";
        if (tok.get_message_id() != 0) {
            BOOST_LOG_TRIVIAL(debug) << "Token: [" << tok.get_message_id() << "]";
        }
        auto top = tok.get_topics();
        if (top && !top->empty()) {
            BOOST_LOG_TRIVIAL(debug) << "Token topic: '" << (*top)[0] << "'";
        }
    }
};

// Main MQTT client class implementing both callback and action listener interfaces
class MqttClient : public mqtt::callback, 
                  public virtual mqtt::iaction_listener,
                  public std::enable_shared_from_this<MqttClient>
{
public:
    // normal MQTT connect 
    MqttClient(const std::string& server_address,
               const std::string& client_id,
               const std::string& username = "",
               const std::string& password = "",
               bool               clean_session = false);
               
    // SSL/TLS - and use CA connect
    MqttClient(const std::string& server_address, 
               const std::string& client_id,
               const std::string& ca_content,
               const std::string& cert_content = "",
               const std::string& key_content = "",
               const std::string& username = "",
               const std::string& password = "",
               bool clean_session = false);

    // Destructor
    ~MqttClient();

    // Connect to the MQTT broker
    bool Connect(std::string& msg);

    // Disconnect from the MQTT broker
    bool Disconnect(std::string& msg);

    // Subscribe to a specific topic with given QoS
    bool Subscribe(const std::string& topic, int qos, std::string& msg);

    // Unsubscribe from a specific topic
    bool Unsubscribe(const std::string& topic, std::string& msg);

    // Publish a message to a specific topic with given QoS
    bool Publish(const std::string& topic, const std::string& payload, int qos, std::string& msg);

    // Set callback for handling incoming messages
    void SetMessageCallback(std::function<void(const std::string& topic, const std::string& payload)> callback);
    void SetMessageCallback(std::function<void(const std::string& topic, const std::string& payload, void* this_)> callback);

    //  add set connect callback
    void SetConnectionFailureCallback(std::function<void()> callback) {
        connection_failure_callback_ = callback;
    }

    // Callback interface implementations
    void connection_lost(const std::string& cause) override;
    void message_arrived(mqtt::const_message_ptr msg) override;
    void delivery_complete(mqtt::delivery_token_ptr token) override;
    void connected(const std::string& cause) override;

    // Action listener interface implementations
    void on_failure(const mqtt::token& tok) override;
    void on_success(const mqtt::token& tok) override;

    // Check if client is currently connected
    bool CheckConnected();

    std::string get_client_id() {return client_id_;}
private:
    std::string server_address_;     // MQTT broker address
    std::string client_id_;          // Unique client identifier
    std::unique_ptr<mqtt::async_client> client_;      // Async MQTT client instance
    std::function<void(const std::string& topic, const std::string& payload)> message_callback_;  // Message handler
    std::function<void(const std::string& topic, const std::string& payload, void* this_)> message_callback1_;  // Message handler

    mqtt::connect_options connOpts_; // Connection options
    std::atomic<bool> connected_;    // Connection status flag
    std::map<std::string, int> topics_to_resubscribe_;  // Topics to resubscribe after reconnection
    action_listener subListener_;    // Subscription listener
    int connect_retry_time_;         // Connection retry counter
    int subscribe_retry_time_;       // Subscription retry counter
    std::function<void()> connection_failure_callback_; 

    std::atomic<bool> is_reconnecting; 
    std::atomic<int> pending_reconnect_checks;  
    std::atomic<bool> ever_connected_;  

    // tmp path
    boost::filesystem::path temp_ca_path_;
    boost::filesystem::path temp_cert_path_;
    boost::filesystem::path temp_key_path_;
    
    // clean tmp files
    void cleanup_temp_files();

    //add new fucntion to resubscribe
    void resubscribe_topics();
    void add_topic_to_resubscribe(const std::string& topic, int qos);
    void remove_topic_from_resubscribe(const std::string& topic);
};

#endif // MQTT_H
