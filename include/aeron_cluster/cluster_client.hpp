#pragma once

#include <Aeron.h>
#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <functional>

#include "config.hpp"
#include "order_types.hpp"
#include "sbe_messages.hpp"

namespace aeron_cluster {

/**
 * @brief High-level client for connecting to and interacting with Aeron Cluster
 * 
 * This class provides a simplified interface for:
 * - Establishing cluster connections with proper SBE protocol handling
 * - Session management with automatic leader detection and failover
 * - Publishing orders and other business messages
 * - Receiving and parsing acknowledgments and responses
 * 
 * Example usage:
 * @code
 * ClusterClientConfig config;
 * config.cluster_endpoints = {"localhost:9002", "localhost:9102", "localhost:9202"};
 * 
 * ClusterClient client(config);
 * if (client.connect()) {
 *     Order order = createLimitOrder("ETH", "USDC", "BUY", 1.5, 3500.0);
 *     std::string messageId = client.publishOrder(order);
 *     std::cout << "Published order: " << messageId << std::endl;
 * }
 * @endcode
 */
class ClusterClient {
public:
    /**
     * @brief Callback function type for handling received messages
     * @param messageType Type of message received (e.g., "ORDER_ACK", "SESSION_EVENT")
     * @param payload Message payload as JSON string
     * @param headers Message headers as JSON string
     */
    using MessageCallback = std::function<void(const std::string& messageType, 
                                              const std::string& payload, 
                                              const std::string& headers)>;

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
     * 
     * This method will:
     * 1. Initialize Aeron context and connect to media driver
     * 2. Create egress subscription for receiving responses
     * 3. Attempt to connect to cluster leader
     * 4. Send SessionConnectRequest with proper SBE encoding
     * 5. Wait for SessionEvent confirmation
     * 
     * @return true if successfully connected, false otherwise
     * @throws std::runtime_error on configuration or network errors
     */
    bool connect();

    /**
     * @brief Disconnect from the cluster
     * 
     * Cleanly closes all Aeron publications and subscriptions.
     * Safe to call multiple times.
     */
    void disconnect();

    /**
     * @brief Check if client is currently connected to cluster
     * @return true if connected and session is active
     */
    bool isConnected() const;

    /**
     * @brief Get the cluster session ID
     * @return Session ID assigned by cluster, or -1 if not connected
     */
    int64_t getSessionId() const;

    /**
     * @brief Get the current leader member ID
     * @return Leader member ID, or -1 if unknown
     */
    int32_t getLeaderMemberId() const;

    /**
     * @brief Publish a trading order to the cluster
     * 
     * Encodes the order as JSON payload within an SBE TopicMessage
     * and publishes to the cluster's ingress stream.
     * 
     * @param order Order structure with all required fields
     * @return Message ID for tracking acknowledgments
     * @throws std::runtime_error if not connected or encoding fails
     */
    std::string publishOrder(const Order& order);

    /**
     * @brief Publish a custom message to the cluster
     * 
     * For advanced use cases where you need to send custom message types.
     * 
     * @param messageType Type identifier for the message
     * @param payload JSON payload string
     * @param headers Optional headers as JSON string
     * @return Message ID for tracking acknowledgments
     * @throws std::runtime_error if not connected or encoding fails
     */
    std::string publishMessage(const std::string& messageType,
                              const std::string& payload,
                              const std::string& headers = "{}");

    /**
     * @brief Set callback for handling received messages
     * 
     * The callback will be invoked for all messages received from the cluster,
     * including acknowledgments, session events, and business responses.
     * 
     * @param callback Function to call when messages are received
     */
    void setMessageCallback(MessageCallback callback);

    /**
     * @brief Poll for incoming messages
     * 
     * This should be called regularly to process incoming messages.
     * Will invoke the message callback for any received messages.
     * 
     * @param maxMessages Maximum number of messages to process
     * @return Number of messages processed
     */
    int pollMessages(int maxMessages = 10);

    /**
     * @brief Create a sample limit order for testing
     * 
     * Utility method that creates a properly formatted order with
     * reasonable default values for testing purposes.
     * 
     * @param baseToken Base token symbol (e.g., "ETH")
     * @param quoteToken Quote token symbol (e.g., "USDC")
     * @param side "BUY" or "SELL"
     * @param quantity Order quantity
     * @param limitPrice Limit price
     * @return Configured Order object
     */
    static Order createSampleLimitOrder(const std::string& baseToken = "ETH",
                                       const std::string& quoteToken = "USDC",
                                       const std::string& side = "BUY",
                                       double quantity = 1.0,
                                       double limitPrice = 3500.0);

    /**
     * @brief Get detailed connection statistics
     * @return ConnectionStats structure with performance metrics
     */
    ConnectionStats getConnectionStats() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

/**
 * @brief Utility class for building cluster client configurations
 * 
 * Provides a fluent interface for constructing ClusterClientConfig objects.
 * 
 * Example:
 * @code
 * auto config = ClusterClientConfigBuilder()
 *     .withClusterEndpoints({"localhost:9002", "localhost:9102"})
 *     .withAeronDir("/tmp/aeron")
 *     .withResponseTimeout(std::chrono::seconds(10))
 *     .build();
 * @endcode
 */
class ClusterClientConfigBuilder {
public:
    ClusterClientConfigBuilder();

    ClusterClientConfigBuilder& withClusterEndpoints(const std::vector<std::string>& endpoints);
    ClusterClientConfigBuilder& withResponseChannel(const std::string& channel);
    ClusterClientConfigBuilder& withAeronDir(const std::string& aeronDir);
    ClusterClientConfigBuilder& withResponseTimeout(std::chrono::milliseconds timeout);
    ClusterClientConfigBuilder& withMaxRetries(int maxRetries);
    ClusterClientConfigBuilder& withRetryDelay(std::chrono::milliseconds delay);

    ClusterClientConfig build() const;

private:
    ClusterClientConfig config_;
};

} // namespace aeron_cluster