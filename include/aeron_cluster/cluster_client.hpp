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
#include "aeron_cluster/message_handler.hpp"
#include "aeron_cluster/subscription.hpp"
#include "aeron_cluster/protocol.hpp"
#include "aeron_cluster/signal_handler.hpp"

// Forward declarations
namespace aeron_cluster {
    struct CommitOffset;
    class CommitManager;
}

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
class ClusterClient : public std::enable_shared_from_this<ClusterClient> {
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
     * @brief Register this client for automatic signal handling
     * This enables graceful disconnection on SIGINT/SIGTERM signals
     */
    void enable_automatic_signal_handling();

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
     * @brief Publish a trading order to a specific topic
     * @param order Order structure with all required fields
     * @param topic Topic name to publish to
     * @return Message ID for tracking acknowledgments
     * @throws NotConnectedException if not connected
     * @throws InvalidMessageException if order validation fails
     */
    std::string publish_order_to_topic(const Order& order, const std::string& topic);

    /**
     * @brief Publish a trading order asynchronously
     * @param order Order structure with all required fields
     * @return Future that resolves to the message ID
     */
    std::future<std::string> publish_order_async(const Order& order);

    /**
     * @brief Publish a trading order asynchronously to a specific topic
     * @param order Order structure with all required fields
     * @param topic Topic name to publish to
     * @return Future that resolves to the message ID
     */
    std::future<std::string> publish_order_to_topic_async(const Order& order, const std::string& topic);

    /**
     * @brief Send a subscription request to a specific topic
     * @param topic Topic name to subscribe to
     * @param messageIdentifier Message identifier for tracking
     * @param resumeStrategy Resume strategy (LATEST, FROM_LAST, etc.)
     * @param instanceIdentifier Instance identifier for load balancing
     * @return Message ID for tracking acknowledgments
     * @throws NotConnectedException if not connected
     */
    std::string send_subscription_request(const std::string& topic, const std::string& messageIdentifier, 
                                        const std::string& resumeStrategy, const std::string& instanceIdentifier = "");

    
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
     * @brief Publish a custom message to a specific topic
     * @param message_type Type identifier for the message
     * @param payload JSON payload string
     * @param headers Optional headers as JSON string
     * @param topic Topic name to publish to
     * @return Message ID for tracking acknowledgments
     * @throws NotConnectedException if not connected
     */
    std::string publish_message_to_topic(
        const std::string& message_type,
        const std::string& payload,
        const std::string& headers, const std::string& topic);
    /**
     * @brief Publish a custom message asynchronously
     */
    std::future<std::string> publish_message_async(
        const std::string& message_type,
        const std::string& payload,
        const std::string& headers = "{}");

    /**
     * @brief Publish a custom message asynchronously to a specific topic
     * @param message_type Type identifier for the message
     * @param payload JSON payload string
     * @param headers Optional headers as JSON string
     * @param topic Topic name to publish to
     * @return Future that resolves to the message ID
     */
    std::future<std::string> publish_message_to_topic_async(
        const std::string& message_type,
        const std::string& payload,
        const std::string& headers, const std::string& topic);

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
        double limit_price = 3500.0,
        const std::string& identifier = "ROHIT_AERON01_TX");

    /**
     * @brief Create a sample market order for testing
     */
    static Order create_sample_market_order(
        const std::string& base_token = "ETH",
        const std::string& quote_token = "USDC",
        const std::string& side = "BUY",
        double quantity = 1.0);

    /**
     * @brief Create a complete message structure for order creation
     * @param base_token Base token symbol (e.g., "BTC")
     * @param quote_token Quote token symbol (e.g., "USD")
     * @param side Order side ("buy" or "sell")
     * @param quantity Order quantity
     * @param limit_price Limit price for the order
     * @return JSON string representing the complete message structure
     */
    static std::string create_order_message(
        const std::string& base_token = "BTC",
        const std::string& quote_token = "USD",
        const std::string& side = "sell",
        double quantity = 0.0001,
        double limit_price = 300000.0);

    // Publish to a topic with type and JSON payload
    // Returns message UUID for correlation
    std::string publish_topic(std::string_view topic,
        std::string_view message_type,
        std::string_view json_payload,
        std::string_view headers_json = "{}");

    // Send subscription (_subscriptions) for a topic
    bool subscribe_topic(std::string_view topic, std::string_view resume_strategy = "LATEST");

    // Send unsubscription (_subscriptions) for a topic
    bool unsubscribe_topic(std::string_view topic);

    // Send unsubscription request with message identifier
    std::string send_unsubscription_request(const std::string& topic, const std::string& messageIdentifier = "");

    // Commit a message for a specific topic and message identifier
    void commit_message(const std::string& topic, const std::string& message_identifier,
                      const std::string& message_id, std::uint64_t timestamp_nanos, 
                      std::uint64_t sequence_number);

    // Get last commit offset for a topic and message identifier
    std::shared_ptr<CommitOffset> get_last_commit(const std::string& topic, 
                                                 const std::string& message_identifier) const;

    // Send commit request for a topic
    bool send_commit_request(const std::string& topic);

    // Send commit offset for a topic
    bool send_commit_offset(const std::string& topic, const CommitOffset& offset);

    // Resume subscription from last commit for a topic and message identifier
    bool resume_from_last_commit(const std::string& topic, const std::string& message_identifier);

    // Load balancing and instance management
    void set_instance_identifier(const std::string& instance_id);
    std::string get_instance_identifier() const;
    
    // Subscribe to topic with load balancing support
    bool subscribe_topic_with_load_balancing(const std::string& topic, const std::string& message_identifier,
                                          const std::string& resume_strategy = "LATEST");
    
    // Unsubscribe from topic with instance cleanup
    bool unsubscribe_topic_with_cleanup(const std::string& topic, const std::string& message_identifier);

    // Callbacks
    void on_topic_message(TopicMessageCallback cb) { handler_.set_topic_message_callback(std::move(cb)); }
    void on_ack(AckCallback cb) { handler_.set_ack_callback(std::move(cb)); }

private:
    // ingress/egress plumbing
    bool offer_ingress(const std::uint8_t* data, std::size_t len); // wraps your underlying Aeron offer

    // correlation for RTT
    void remember_outgoing(const std::string& uuid, std::uint64_t ts_nanos);
    void on_ack_default(const AckInfo& ack);

private:
    ClusterClientConfig cfg_;
    MessageHandler handler_;

    // map: uuid -> send_timestamp_nanos
    std::unordered_map<std::string, std::uint64_t> inflight_;

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
    ClusterClientConfigBuilder& with_commit_logging(bool enabled);
    ClusterClientConfigBuilder& with_application_name(const std::string& name);
    ClusterClientConfigBuilder& with_default_topic(const std::string& topic);
    ClusterClientConfigBuilder& with_client_id(const std::string& client_id);

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

struct SubscriptionOptions {
    std::string resumeStrategy = "LATEST"; // or "FROM_LAST" etc.
    std::string lastMessageId;             // optional
    std::string clientId = "cpp_client";
};

bool subscribeToTopic(const std::string& topic, const SubscriptionOptions& opt = {});

} // namespace aeron_cluster