#pragma once

#include "config.hpp"
#include "order_types.hpp"
#include "sbe_messages.hpp"

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <future>
#include <chrono>
#include <exception>

namespace aeron_cluster {

/**
 * @brief Exception thrown by cluster client operations
 */
class ClusterClientException : public std::runtime_error {
public:
    explicit ClusterClientException(const std::string& message) 
        : std::runtime_error(message) {}
};

/**
 * @brief Exception thrown when client is not connected
 */
class NotConnectedException : public ClusterClientException {
public:
    NotConnectedException() 
        : ClusterClientException("Client is not connected to cluster") {}
};

/**
 * @brief Exception thrown when message validation fails
 */
class InvalidMessageException : public ClusterClientException {
public:
    explicit InvalidMessageException(const std::string& message)
        : ClusterClientException("Invalid message: " + message) {}
};

/**
 * @brief Connection state enumeration
 */
enum class ConnectionState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    RECONNECTING,
    FAILED
};

/**
 * @brief High-level client for connecting to and interacting with Aeron Cluster
 */
class ClusterClient {
public:
    /**
     * @brief Callback function type for handling received messages
     */
    using MessageCallback = std::function<void(const ParseResult& result)>;

    /**
     * @brief Callback function type for connection state changes
     */
    using ConnectionStateCallback = std::function<void(ConnectionState old_state, ConnectionState new_state)>;

    /**
     * @brief Callback function type for error notifications
     */
    using ErrorCallback = std::function<void(const std::string& error_message)>;

    /**
     * @brief Construct a new Cluster Client
     * @param config Configuration parameters for the client
     */
    explicit ClusterClient(const ClusterClientConfig& config);

    /**
     * @brief Destructor - automatically disconnects if connected
     */
    ~ClusterClient();

    // Non-copyable but movable
    ClusterClient(const ClusterClient&) = delete;
    ClusterClient& operator=(const ClusterClient&) = delete;
    ClusterClient(ClusterClient&&) = default;
    ClusterClient& operator=(ClusterClient&&) = default;

    /**
     * @brief Connect to the Aeron Cluster
     * @return Future that resolves to true if successfully connected
     */
    std::future<bool> connect_async();

    /**
     * @brief Connect to the Aeron Cluster (blocking)
     * @return true if successfully connected, false otherwise
     */
    bool connect();

    /**
     * @brief Connect to the Aeron Cluster with timeout
     * @param timeout Maximum time to wait for connection
     * @return true if successfully connected within timeout
     */
    bool connect_with_timeout(std::chrono::milliseconds timeout);

    /**
     * @brief Disconnect from the cluster
     */
    void disconnect();

    /**
     * @brief Check if client is currently connected to cluster
     */
    bool is_connected() const;

    /**
     * @brief Get current connection state
     */
    ConnectionState get_connection_state() const;

    /**
     * @brief Get the cluster session ID
     */
    std::int64_t get_session_id() const;

    /**
     * @brief Get the current leader member ID
     */
    std::int32_t get_leader_member_id() const;

    /**
     * @brief Publish a trading order to the cluster
     * @param order Order structure with all required fields
     * @return Message ID for tracking acknowledgments
     * @throws NotConnectedException if not connected
     * @throws InvalidMessageException if order validation fails
     */
    std::string publish_order(const Order& order);

    /**
     * @brief Publish a trading order asynchronously
     * @param order Order structure with all required fields
     * @return Future that resolves to the message ID
     */
    std::future<std::string> publish_order_async(const Order& order);

    /**
     * @brief Publish a custom message to the cluster
     * @param message_type Type identifier for the message
     * @param payload JSON payload string
     * @param headers Optional headers as JSON string
     * @return Message ID for tracking acknowledgments
     * @throws NotConnectedException if not connected
     */
    std::string publish_message(
        const std::string& message_type,
        const std::string& payload,
        const std::string& headers = "{}");

    /**
     * @brief Publish a custom message asynchronously
     */
    std::future<std::string> publish_message_async(
        const std::string& message_type,
        const std::string& payload,
        const std::string& headers = "{}");

    /**
     * @brief Wait for acknowledgment of a specific message
     * @param message_id Message ID to wait for
     * @param timeout Maximum time to wait
     * @return true if acknowledgment received within timeout
     */
    bool wait_for_acknowledgment(
        const std::string& message_id,
        std::chrono::milliseconds timeout = std::chrono::seconds(5));

    /**
     * @brief Set callback for handling received messages
     */
    void set_message_callback(MessageCallback callback);

    /**
     * @brief Set callback for connection state changes
     */
    void set_connection_state_callback(ConnectionStateCallback callback);

    /**
     * @brief Set callback for error notifications
     */
    void set_error_callback(ErrorCallback callback);

    /**
     * @brief Start automatic message polling in background thread
     */
    void start_polling();

    /**
     * @brief Stop automatic message polling
     */
    void stop_polling();

    /**
     * @brief Poll for incoming messages manually
     * @param max_messages Maximum number of messages to process
     * @return Number of messages processed
     */
    int poll_messages(int max_messages = 10);

    /**
     * @brief Get detailed connection statistics
     */
    ConnectionStats get_connection_stats() const;

    /**
     * @brief Get current configuration
     */
    const ClusterClientConfig& get_config() const;

    /**
     * @brief Update configuration (only certain fields can be updated while connected)
     * @param config New configuration
     * @throws std::invalid_argument if trying to change immutable fields while connected
     */
    void update_config(const ClusterClientConfig& config);

    /**
     * @brief Enable or disable automatic reconnection on connection loss
     */
    void set_auto_reconnect(bool enabled);

    /**
     * @brief Check if auto-reconnect is enabled
     */
    bool is_auto_reconnect_enabled() const;

    /**
     * @brief Force a reconnection attempt
     * @return Future that resolves when reconnection attempt completes
     */
    std::future<bool> reconnect_async();

    /**
     * @brief Create a sample limit order for testing
     */
    static Order create_sample_limit_order(
        const std::string& base_token = "ETH",
        const std::string& quote_token = "USDC",
        const std::string& side = "BUY",
        double quantity = 1.0,
        double limit_price = 3500.0);

    /**
     * @brief Create a sample market order for testing
     */
    static Order create_sample_market_order(
        const std::string& base_token = "ETH",
        const std::string& quote_token = "USDC",
        const std::string& side = "BUY",
        double quantity = 1.0);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl_;
};

/**
 * @brief Builder class for constructing cluster client configurations
 */
class ClusterClientConfigBuilder {
public:
    ClusterClientConfigBuilder();

    ClusterClientConfigBuilder& with_cluster_endpoints(const std::vector<std::string>& endpoints);
    ClusterClientConfigBuilder& with_response_channel(const std::string& channel);
    ClusterClientConfigBuilder& with_aeron_dir(const std::string& aeron_dir);
    ClusterClientConfigBuilder& with_response_timeout(std::chrono::milliseconds timeout);
    ClusterClientConfigBuilder& with_max_retries(int max_retries);
    ClusterClientConfigBuilder& with_retry_delay(std::chrono::milliseconds delay);
    ClusterClientConfigBuilder& with_keepalive_enabled(bool enabled);
    ClusterClientConfigBuilder& with_keepalive_interval(std::chrono::milliseconds interval);
    ClusterClientConfigBuilder& with_debug_logging(bool enabled);
    ClusterClientConfigBuilder& with_application_name(const std::string& name);
    ClusterClientConfigBuilder& with_default_topic(const std::string& topic);

    /**
     * @brief Build the configuration
     * @return Configured ClusterClientConfig
     * @throws std::invalid_argument if configuration is invalid
     */
    ClusterClientConfig build() const;

private:
    ClusterClientConfig config_;
};

/**
 * @brief RAII class for managing cluster client connections
 */
class ClusterClientConnection {
public:
    explicit ClusterClientConnection(ClusterClient& client) 
        : client_(client), connected_(false) {}

    ~ClusterClientConnection() {
        if (connected_) {
            client_.disconnect();
        }
    }

    bool connect() {
        connected_ = client_.connect();
        return connected_;
    }

    bool connect_with_timeout(std::chrono::milliseconds timeout) {
        connected_ = client_.connect_with_timeout(timeout);
        return connected_;
    }

    void disconnect() {
        if (connected_) {
            client_.disconnect();
            connected_ = false;
        }
    }

    bool is_connected() const {
        return connected_ && client_.is_connected();
    }

private:
    ClusterClient& client_;
    bool connected_;
};

} // namespace aeron_cluster