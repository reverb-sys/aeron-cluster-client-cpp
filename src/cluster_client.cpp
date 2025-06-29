#include "aeron_cluster/cluster_client.hpp"
#include "aeron_cluster/session_manager.hpp"
#include "aeron_cluster/message_handlers.hpp"
#include "aeron_cluster/sbe_messages.hpp"

#include <Aeron.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <json/json.h>

namespace aeron_cluster {

class ClusterClient::Impl {
public:
    explicit Impl(const ClusterClientConfig& config)
        : config_(config)
        , session_manager_(std::make_unique<SessionManager>(config))
        , connection_state_(ConnectionState::DISCONNECTED)
        , auto_reconnect_enabled_(false)
    {
        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " Creating ClusterClient" << std::endl;
        }
    }

    ~Impl() {
        stop_polling();
        disconnect();
    }

    std::future<bool> connect_async() {
        return std::async(std::launch::async, [this]() {
            return connect();
        });
    }

    bool connect() {
        if (connection_state_ == ConnectionState::CONNECTED) {
            return true;
        }

        set_connection_state(ConnectionState::CONNECTING);

        try {
            stats_.connection_attempts++;

            std::cout << config_.logging.log_prefix << " Connecting to Aeron Cluster..." << std::endl;
            // Initialize Aeron
            if (!initialize_aeron()) {
                set_connection_state(ConnectionState::FAILED);
                return false;
            }

            start_polling();

            std::cout << config_.logging.log_prefix << " Aeron initialized successfully" << std::endl;

            // Create egress subscription
            if (!create_egress_subscription()) {
                set_connection_state(ConnectionState::FAILED);
                return false;
            }

            std::cout << config_.logging.log_prefix << " Egress subscription created successfully" << std::endl;

            // Connect session manager
            SessionConnectionResult result = session_manager_->connect(aeron_);
            if (!result.success) {
                set_connection_state(ConnectionState::FAILED);
                return false;
            }

            std::cout << config_.logging.log_prefix << " Session manager connected successfully" << std::endl;

            // Update statistics
            stats_.successful_connections++;
            stats_.current_session_id = result.session_id;
            stats_.current_leader_id = result.leader_member_id;
            stats_.connection_established_time = std::chrono::steady_clock::now();
            stats_.is_connected = true;

            set_connection_state(ConnectionState::CONNECTED);

            if (config_.enable_console_info) {
                std::cout << config_.logging.log_prefix << " Connected to Aeron Cluster!" << std::endl;
                std::cout << config_.logging.log_prefix << "    Session ID: " << stats_.current_session_id << std::endl;
                std::cout << config_.logging.log_prefix << "    Leader Member: " << stats_.current_leader_id << std::endl;
            }

            return true;

        } catch (const std::exception& e) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix << " Connection failed: " << e.what() << std::endl;
            }
            set_connection_state(ConnectionState::FAILED);
            return false;
        }
    }

    bool connect_with_timeout(std::chrono::milliseconds timeout) {
        auto future = connect_async();
        return future.wait_for(timeout) == std::future_status::ready && future.get();
    }

    void disconnect() {
        if (connection_state_ == ConnectionState::DISCONNECTED) {
            return;
        }

        set_connection_state(ConnectionState::DISCONNECTED);

        if (session_manager_) {
            session_manager_->disconnect();
        }
        
        egress_subscription_.reset();
        aeron_.reset();

        stats_.current_session_id = -1;
        stats_.current_leader_id = -1;
        stats_.is_connected = false;

        if (config_.enable_console_info) {
            std::cout << config_.logging.log_prefix << " Disconnected from cluster" << std::endl;
        }
    }

    bool is_connected() const {
        return connection_state_ == ConnectionState::CONNECTED && 
               session_manager_ && session_manager_->is_connected();
    }

    ConnectionState get_connection_state() const {
        return connection_state_;
    }

    std::int64_t get_session_id() const {
        return session_manager_ ? session_manager_->get_session_id() : -1;
    }

    std::int32_t get_leader_member_id() const {
        return session_manager_ ? session_manager_->get_leader_member_id() : -1;
    }

    std::string publish_order(const Order& order) {
        if (!is_connected()) {
            throw NotConnectedException();
        }

        auto validation_errors = order.validate();
        if (!validation_errors.empty()) {
            throw InvalidMessageException("Order validation failed: " + validation_errors[0]);
        }

        std::string message_id = generate_message_id();
        std::string message_type = "CREATE_ORDER";
        if (order.status == "UPDATED" || order.status == "CANCELLED") {
            message_type = "UPDATE_ORDER";
        }

        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " Publishing order: " << order.id 
                     << " (MessageID: " << message_id << ")" << std::endl;
        }

        std::string order_json = order.to_json();
        
        Json::Value headers;
        headers["messageId"] = message_id;
        headers["messageType"] = message_type;
        headers["orderId"] = order.id;
        
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::string headers_json = Json::writeString(builder, headers);

        if (!session_manager_->publish_message(config_.default_topic, message_type, message_id, order_json, headers_json)) {
            throw ClusterClientException("Failed to publish order message");
        }

        stats_.messages_sent++;
        return message_id;
    }

    std::future<std::string> publish_order_async(const Order& order) {
        return std::async(std::launch::async, [this, order]() {
            return publish_order(order);
        });
    }

    std::string publish_message(const std::string& message_type,
                               const std::string& payload,
                               const std::string& headers) {
        if (!is_connected()) {
            throw NotConnectedException();
        }

        std::string message_id = generate_message_id();

        if (!session_manager_->publish_message(config_.default_topic, message_type, message_id, payload, headers)) {
            throw ClusterClientException("Failed to publish message");
        }

        stats_.messages_sent++;
        return message_id;
    }

    std::future<std::string> publish_message_async(const std::string& message_type,
                                                   const std::string& payload,
                                                   const std::string& headers) {
        return std::async(std::launch::async, [this, message_type, payload, headers]() {
            return publish_message(message_type, payload, headers);
        });
    }

    std::string send_subscription_request(const std::string& topic) {
        if (!is_connected()) {
            throw NotConnectedException();
        }

        std::string message_id = generate_message_id();
        std::string message_type = "SUBSCRIBE";

        Json::Value headers;
        headers["messageId"] = message_id;
        headers["topic"] = "_subscriptions";

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::string headers_json = Json::writeString(builder, headers);

         //bool publish_message(const std::string& topic, const std::string& message_type,
                        //  const std::string& message_id, const std::string& payload,
                        //  const std::string& headers) {

        /*
            sample message payload in golang

            Message: map[string]interface{}{
			"topic":          topic,
			"action":         "SUBSCRIBE",
			"resumeStrategy": "FROM_LAST",
			"lastMessageId":  "1000",
            }

            Headers: map[string]interface{}{
			    "clientId": "subscriberClient",
		    },
        */

        Json::Value payload;
        payload["topic"] = topic;
        payload["action"] = "SUBSCRIBE";
        payload["resumeStrategy"] = "FROM_LAST";
        payload["lastMessageId"] = "0"; // Placeholder, can be updated later
        std::string payload_json = Json::writeString(builder, payload);
        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " Sending subscription request for topic: " 
                      << topic << " (MessageID: " << message_id << ")" << std::endl;
        }


        if (!session_manager_->publish_message("_subscriptions", message_type, message_id, payload_json, headers_json)) {
            throw ClusterClientException("Failed to send subscription request");
        }

        stats_.messages_sent++;
        return message_id;
    }

    bool wait_for_acknowledgment(const std::string& message_id, std::chrono::milliseconds timeout) {
        // This would require tracking pending messages and their acknowledgments
        // For now, return true as a placeholder
        return true;
    }

    void set_message_callback(MessageCallback callback) {
        message_callback_ = std::move(callback);
    }

    void set_connection_state_callback(ConnectionStateCallback callback) {
        connection_state_callback_ = std::move(callback);
    }

    void set_error_callback(ErrorCallback callback) {
        error_callback_ = std::move(callback);
    }

    void start_polling() {
        if (polling_active_) {
            return;
        }

        polling_active_ = true;
        polling_thread_ = std::thread(&ClusterClient::Impl::polling_worker, this);
    }

    void stop_polling() {
        if (!polling_active_) {
            return;
        }

        polling_active_ = false;
        if (polling_thread_.joinable()) {
            polling_thread_.join();
        }
    }

    int poll_messages(int max_messages) {
        if (!egress_subscription_ || egress_subscription_->imageCount() <= 0) {
            return 0;
        }

        int messages_processed = 0;

        try {
            egress_subscription_->poll(
                [this, &messages_processed](aeron::AtomicBuffer& buffer, 
                                          aeron::util::index_t offset, 
                                          aeron::util::index_t length, 
                                          aeron::logbuffer::Header& /*header*/) {
                    handle_incoming_message(buffer, offset, length);
                    messages_processed++;
                }, 
                max_messages
            );

            stats_.messages_received += messages_processed;
        } catch (const std::exception& e) {
            if (config_.debug_logging) {
                std::cerr << config_.logging.log_prefix << " Error polling messages: " << e.what() << std::endl;
            }
        }
        
        return messages_processed;
    }

    ConnectionStats get_connection_stats() const {
        return stats_;
    }

    const ClusterClientConfig& get_config() const {
        return config_;
    }

    void set_auto_reconnect(bool enabled) {
        auto_reconnect_enabled_ = enabled;
    }

    bool is_auto_reconnect_enabled() const {
        return auto_reconnect_enabled_;
    }

    std::future<bool> reconnect_async() {
        return std::async(std::launch::async, [this]() {
            disconnect();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            return connect();
        });
    }

private:
    ClusterClientConfig config_;
    std::unique_ptr<SessionManager> session_manager_;
    std::shared_ptr<aeron::Aeron> aeron_;
    std::shared_ptr<aeron::Subscription> egress_subscription_;
    
    std::atomic<ConnectionState> connection_state_;
    ConnectionStats stats_;
    bool auto_reconnect_enabled_;
    
    // Callbacks
    MessageCallback message_callback_;
    ConnectionStateCallback connection_state_callback_;
    ErrorCallback error_callback_;
    
    // Polling
    std::atomic<bool> polling_active_{false};
    std::thread polling_thread_;
    
    bool initialize_aeron() {
        try {
            aeron::Context context;
            context.aeronDir(config_.aeron_dir);
            aeron_ = aeron::Aeron::connect(context);
            return true;
        } catch (const std::exception& e) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix << " Aeron initialization error: " << e.what() << std::endl;
            }
            return false;
        }
    }

    bool create_egress_subscription() {
        try {
            std::int64_t sub_id = aeron_->addSubscription(config_.response_channel, config_.egress_stream_id);
            
            auto timeout = std::chrono::seconds(10);
            auto start_time = std::chrono::steady_clock::now();
            
            while ((std::chrono::steady_clock::now() - start_time) < timeout) {
                egress_subscription_ = aeron_->findSubscription(sub_id);
                if (egress_subscription_) {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
            return false;
        } catch (const std::exception& e) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix << " Egress subscription error: " << e.what() << std::endl;
            }
            return false;
        }
    }

    void handle_incoming_message(aeron::AtomicBuffer& buffer, 
                                aeron::util::index_t offset, 
                                aeron::util::index_t length) {
        try {
            const std::uint8_t* data = buffer.buffer() + offset;
            ParseResult result = MessageParser::parse_message(data, length);
            
            if (result.success) {
                if (result.is_session_event()) {
                    session_manager_->handle_session_event(result);
                }
                
                if (message_callback_) {
                    message_callback_(result);
                }
            }
        } catch (const std::exception& e) {
            if (config_.debug_logging) {
                std::cerr << config_.logging.log_prefix 
                          << " Error handling incoming message: " << e.what() << std::endl;
            }
        }
    }

    void polling_worker() {
        while (polling_active_) {
            try {
                int processed = poll_messages(10);
                if (processed == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            } catch (const std::exception& e) {
                if (error_callback_) {
                    error_callback_("Polling error: " + std::string(e.what()));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    void set_connection_state(ConnectionState new_state) {
        ConnectionState old_state = connection_state_.exchange(new_state);
        if (old_state != new_state && connection_state_callback_) {
            connection_state_callback_(old_state, new_state);
        }
    }

    std::string generate_message_id() {
        return OrderUtils::generate_message_id("msg");
    }
};

// ClusterClient public interface implementation
ClusterClient::ClusterClient(const ClusterClientConfig& config)
    : pImpl_(std::make_unique<Impl>(config)) {
}

ClusterClient::~ClusterClient() = default;

std::future<bool> ClusterClient::connect_async() {
    return pImpl_->connect_async();
}

bool ClusterClient::connect() {
    return pImpl_->connect();
}

bool ClusterClient::connect_with_timeout(std::chrono::milliseconds timeout) {
    return pImpl_->connect_with_timeout(timeout);
}

void ClusterClient::disconnect() {
    pImpl_->disconnect();
}

bool ClusterClient::is_connected() const {
    return pImpl_->is_connected();
}

ConnectionState ClusterClient::get_connection_state() const {
    return pImpl_->get_connection_state();
}

std::int64_t ClusterClient::get_session_id() const {
    return pImpl_->get_session_id();
}

std::int32_t ClusterClient::get_leader_member_id() const {
    return pImpl_->get_leader_member_id();
}

std::string ClusterClient::publish_order(const Order& order) {
    return pImpl_->publish_order(order);
}

std::future<std::string> ClusterClient::publish_order_async(const Order& order) {
    return pImpl_->publish_order_async(order);
}

std::string ClusterClient::send_subscription_request(const std::string& topic) {
    return pImpl_->send_subscription_request(topic);
}

std::string ClusterClient::publish_message(const std::string& message_type,
                                          const std::string& payload,
                                          const std::string& headers) {
    return pImpl_->publish_message(message_type, payload, headers);
}

std::future<std::string> ClusterClient::publish_message_async(const std::string& message_type,
                                                              const std::string& payload,
                                                              const std::string& headers) {
    return pImpl_->publish_message_async(message_type, payload, headers);
}

bool ClusterClient::wait_for_acknowledgment(const std::string& message_id,
                                           std::chrono::milliseconds timeout) {
    return pImpl_->wait_for_acknowledgment(message_id, timeout);
}

void ClusterClient::set_message_callback(MessageCallback callback) {
    pImpl_->set_message_callback(std::move(callback));
}

void ClusterClient::set_connection_state_callback(ConnectionStateCallback callback) {
    pImpl_->set_connection_state_callback(std::move(callback));
}

void ClusterClient::set_error_callback(ErrorCallback callback) {
    pImpl_->set_error_callback(std::move(callback));
}

void ClusterClient::start_polling() {
    pImpl_->start_polling();
}

void ClusterClient::stop_polling() {
    pImpl_->stop_polling();
}

int ClusterClient::poll_messages(int max_messages) {
    return pImpl_->poll_messages(max_messages);
}

ConnectionStats ClusterClient::get_connection_stats() const {
    return pImpl_->get_connection_stats();
}

const ClusterClientConfig& ClusterClient::get_config() const {
    return pImpl_->get_config();
}

void ClusterClient::set_auto_reconnect(bool enabled) {
    pImpl_->set_auto_reconnect(enabled);
}

bool ClusterClient::is_auto_reconnect_enabled() const {
    return pImpl_->is_auto_reconnect_enabled();
}

std::future<bool> ClusterClient::reconnect_async() {
    return pImpl_->reconnect_async();
}

// Static factory methods
Order ClusterClient::create_sample_limit_order(const std::string& base_token,
                                              const std::string& quote_token,
                                              const std::string& side,
                                              double quantity,
                                              double limit_price) {
    Order order(base_token, quote_token, side, quantity, "LIMIT");
    order.id = OrderUtils::generate_order_id();
    order.limit_price = limit_price;
    order.time_in_force = "GTC";
    order.status = "CREATED";
    order.request_source = "API";
    order.request_channel = "cpp_client";
    
    order.customer_id = 12345;
    order.user_id = 67890;
    order.account_id = 11111;
    order.base_token_usd_rate = limit_price;
    order.quote_token_usd_rate = 1.0;
    
    return order;
}

Order ClusterClient::create_sample_market_order(const std::string& base_token,
                                               const std::string& quote_token,
                                               const std::string& side,
                                               double quantity) {
    Order order(base_token, quote_token, side, quantity, "MARKET");
    order.id = OrderUtils::generate_order_id();
    order.time_in_force = "IOC";
    order.status = "CREATED";
    order.request_source = "API";
    order.request_channel = "cpp_client";
    
    order.customer_id = 12345;
    order.user_id = 67890;
    order.account_id = 11111;
    
    return order;
}

// ClusterClientConfigBuilder implementation
ClusterClientConfigBuilder::ClusterClientConfigBuilder() = default;

ClusterClientConfigBuilder& ClusterClientConfigBuilder::with_cluster_endpoints(const std::vector<std::string>& endpoints) {
    config_.cluster_endpoints = endpoints;
    return *this;
}

ClusterClientConfigBuilder& ClusterClientConfigBuilder::with_response_channel(const std::string& channel) {
    config_.response_channel = channel;
    return *this;
}

ClusterClientConfigBuilder& ClusterClientConfigBuilder::with_aeron_dir(const std::string& aeron_dir) {
    config_.aeron_dir = aeron_dir;
    return *this;
}

ClusterClientConfigBuilder& ClusterClientConfigBuilder::with_response_timeout(std::chrono::milliseconds timeout) {
    config_.response_timeout = timeout;
    return *this;
}

ClusterClientConfigBuilder& ClusterClientConfigBuilder::with_max_retries(int max_retries) {
    config_.max_retries = max_retries;
    return *this;
}

ClusterClientConfigBuilder& ClusterClientConfigBuilder::with_retry_delay(std::chrono::milliseconds delay) {
    config_.retry_delay = delay;
    return *this;
}

ClusterClientConfigBuilder& ClusterClientConfigBuilder::with_keepalive_enabled(bool enabled) {
    config_.enable_keepalive = enabled;
    return *this;
}

ClusterClientConfigBuilder& ClusterClientConfigBuilder::with_keepalive_interval(std::chrono::milliseconds interval) {
    config_.keepalive_interval = interval;
    return *this;
}

ClusterClientConfigBuilder& ClusterClientConfigBuilder::with_debug_logging(bool enabled) {
    config_.debug_logging = enabled;
    return *this;
}

ClusterClientConfigBuilder& ClusterClientConfigBuilder::with_application_name(const std::string& name) {
    config_.application_name = name;
    return *this;
}

ClusterClientConfigBuilder& ClusterClientConfigBuilder::with_default_topic(const std::string& topic) {
    config_.default_topic = topic;
    return *this;
}

ClusterClientConfig ClusterClientConfigBuilder::build() const {
    // Validate the configuration before returning
    ClusterClientConfig config = config_;
    config.validate();
    return config;
}

} // namespace aeron_cluster