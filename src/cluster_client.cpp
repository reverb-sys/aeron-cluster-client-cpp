#include "aeron_cluster/cluster_client.hpp"
#include "aeron_cluster/sbe_messages.hpp"
#include "session_manager.hpp"
#include "message_handlers.hpp"

#include <Aeron.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <random>
#include <json/json.h>

namespace aeron_cluster {

/**
 * @brief Private implementation class (PIMPL pattern)
 * 
 * Keeps internal implementation details hidden from the public interface
 * and allows for better ABI stability.
 */
class ClusterClient::Impl {
public:
    explicit Impl(const ClusterClientConfig& config)
        : config_(config)
        , sessionManager_(std::make_unique<SessionManager>(config))
        , messageHandler_(std::make_unique<MessageHandler>(*this))
        , sequenceCounter_(0)
        , rng_(std::random_device{}())
    {
        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " Creating ClusterClient with config:" << std::endl;
            printConfig();
        }
    }

    ~Impl() {
        disconnect();
    }

    bool connect() {
        if (isConnected_) {
            if (config_.debug_logging) {
                std::cout << config_.logging.log_prefix << " Already connected" << std::endl;
            }
            return true;
        }

        try {
            stats_.connection_attempts++;
            
            if (config_.debug_logging) {
                std::cout << config_.logging.log_prefix << " Starting connection process..." << std::endl;
            }

            // Initialize Aeron
            if (!initializeAeron()) {
                return false;
            }

            // Create egress subscription
            if (!createEgressSubscription()) {
                return false;
            }

            // Connect to cluster using session manager
            if (!sessionManager_->connect(aeron_)) {
                return false;
            }

            // Mark as connected
            isConnected_ = true;
            stats_.successful_connections++;
            stats_.current_session_id = sessionManager_->getSessionId();
            stats_.current_leader_id = sessionManager_->getLeaderMemberId();
            stats_.connection_established_time = std::chrono::steady_clock::now();
            stats_.is_connected = true;

            if (config_.enable_console_info) {
                std::cout << config_.logging.log_prefix << " ✅ Successfully connected to Aeron Cluster!" << std::endl;
                std::cout << config_.logging.log_prefix << "    Session ID: " << stats_.current_session_id << std::endl;
                std::cout << config_.logging.log_prefix << "    Leader Member: " << stats_.current_leader_id << std::endl;
            }

            return true;

        } catch (const std::exception& e) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix << " ❌ Connection failed: " << e.what() << std::endl;
            }
            return false;
        }
    }

    void disconnect() {
        if (!isConnected_) {
            return;
        }

        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " Disconnecting from cluster..." << std::endl;
        }

        // Update stats
        if (stats_.connection_established_time.time_since_epoch().count() > 0) {
            auto disconnectTime = std::chrono::steady_clock::now();
            auto sessionDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                disconnectTime - stats_.connection_established_time);
            stats_.total_uptime += sessionDuration;
        }

        // Clean shutdown
        sessionManager_->disconnect();
        egressSubscription_.reset();
        ingressPublication_.reset();
        aeron_.reset();

        // Reset state
        isConnected_ = false;
        stats_.current_session_id = -1;
        stats_.current_leader_id = -1;
        stats_.is_connected = false;

        if (config_.enable_console_info) {
            std::cout << config_.logging.log_prefix << " Disconnected from cluster" << std::endl;
        }
    }

    bool isConnected() const {
        return isConnected_ && sessionManager_ && sessionManager_->isConnected();
    }

    int64_t getSessionId() const {
        return sessionManager_ ? sessionManager_->getSessionId() : -1;
    }

    int32_t getLeaderMemberId() const {
        return sessionManager_ ? sessionManager_->getLeaderMemberId() : -1;
    }

    std::string publishOrder(const Order& order) {
        if (!isConnected()) {
            throw std::runtime_error("Not connected to cluster");
        }

        if (!order.validate()) {
            throw std::runtime_error("Invalid order data");
        }

        // Generate message ID
        std::string messageId = generateMessageId();
        
        // Determine message type based on order status
        std::string messageType = "CREATE_ORDER";
        if (order.status == "UPDATED" || order.status == "CANCELLED") {
            messageType = "UPDATE_ORDER";
        }

        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " 📤 Publishing order: " << order.id 
                     << " (MessageID: " << messageId << ")" << std::endl;
        }

        // Convert order to JSON
        std::string orderJson = order.toJsonString();
        
        // Create headers
        Json::Value headers;
        headers["messageId"] = messageId;
        headers["messageType"] = messageType;
        headers["orderId"] = order.id;
        headers["id"] = order.id;
        
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::string headersJson = Json::writeString(builder, headers);

        // Publish using session manager
        if (!sessionManager_->publishMessage(config_.default_topic, messageType, messageId, orderJson, headersJson)) {
            throw std::runtime_error("Failed to publish order message");
        }

        // Update stats
        stats_.messages_sent++;

        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " ✅ Order published successfully" << std::endl;
        }

        return messageId;
    }

    std::string publishMessage(const std::string& messageType,
                              const std::string& payload,
                              const std::string& headers) {
        if (!isConnected()) {
            throw std::runtime_error("Not connected to cluster");
        }

        std::string messageId = generateMessageId();

        if (!sessionManager_->publishMessage(config_.default_topic, messageType, messageId, payload, headers)) {
            throw std::runtime_error("Failed to publish message");
        }

        stats_.messages_sent++;
        return messageId;
    }

    void setMessageCallback(MessageCallback callback) {
        messageCallback_ = std::move(callback);
    }

    int pollMessages(int maxMessages) {
        if (!isConnected() || !egressSubscription_) {
            return 0;
        }

        int messagesProcessed = 0;
        
        int fragmentsRead = egressSubscription_->poll(
            [this, &messagesProcessed](aeron::AtomicBuffer& buffer, 
                                      aeron::util::index_t offset, 
                                      aeron::util::index_t length, 
                                      aeron::logbuffer::Header& header) {
                handleIncomingMessage(buffer, offset, length, header);
                messagesProcessed++;
            }, 
            maxMessages
        );

        stats_.messages_received += messagesProcessed;
        return messagesProcessed;
    }

    ConnectionStats getConnectionStats() const {
        ConnectionStats currentStats = stats_;
        
        // Update uptime if currently connected
        if (isConnected_ && stats_.connection_established_time.time_since_epoch().count() > 0) {
            auto now = std::chrono::steady_clock::now();
            auto currentSessionDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - stats_.connection_established_time);
            currentStats.total_uptime += currentSessionDuration;
        }
        
        return currentStats;
    }

    const ClusterClientConfig& getConfig() const {
        return config_;
    }

private:
    // Configuration and state
    ClusterClientConfig config_;
    std::unique_ptr<SessionManager> sessionManager_;
    std::unique_ptr<MessageHandler> messageHandler_;
    bool isConnected_ = false;
    ConnectionStats stats_;
    MessageCallback messageCallback_;

    // Aeron components
    std::shared_ptr<aeron::Aeron> aeron_;
    std::shared_ptr<aeron::ExclusivePublication> ingressPublication_;
    std::shared_ptr<aeron::Subscription> egressSubscription_;

    // Utilities
    std::atomic<uint64_t> sequenceCounter_;
    std::mt19937 rng_;

    bool initializeAeron() {
        try {
            if (config_.debug_logging) {
                std::cout << config_.logging.log_prefix << " Initializing Aeron..." << std::endl;
                std::cout << config_.logging.log_prefix << "   Aeron dir: " << config_.aeron_dir << std::endl;
            }

            aeron::Context context;
            context.aeronDir(config_.aeron_dir);
            
            aeron_ = aeron::Aeron::connect(context);
            
            if (config_.enable_console_info) {
                std::cout << config_.logging.log_prefix << " ✅ Aeron connected" << std::endl;
            }
            
            return true;
            
        } catch (const std::exception& e) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix << " ❌ Failed to initialize Aeron: " << e.what() << std::endl;
            }
            return false;
        }
    }

    bool createEgressSubscription() {
        try {
            if (config_.debug_logging) {
                std::cout << config_.logging.log_prefix << " Creating egress subscription..." << std::endl;
                std::cout << config_.logging.log_prefix << "   Channel: " << config_.response_channel << std::endl;
                std::cout << config_.logging.log_prefix << "   Stream ID: " << config_.egress_stream_id << std::endl;
            }

            int64_t subId = aeron_->addSubscription(config_.response_channel, config_.egress_stream_id);
            
            // Wait for subscription to be ready
            for (int i = 0; i < 50; ++i) {
                egressSubscription_ = aeron_->findSubscription(subId);
                if (egressSubscription_) {
                    if (config_.enable_console_info) {
                        std::cout << config_.logging.log_prefix << " ✅ Egress subscription ready" << std::endl;
                    }
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix << " ❌ Failed to create egress subscription" << std::endl;
            }
            return false;
            
        } catch (const std::exception& e) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix << " ❌ Error creating egress subscription: " << e.what() << std::endl;
            }
            return false;
        }
    }

    void handleIncomingMessage(aeron::AtomicBuffer& buffer, 
                              aeron::util::index_t offset, 
                              aeron::util::index_t length, 
                              aeron::logbuffer::Header& header) {
        const uint8_t* data = buffer.buffer() + offset;
        
        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " 📨 Received message: length=" << length 
                     << ", sessionId=" << header.sessionId() << std::endl;
        }

        // Parse the message
        ParseResult result = MessageParser::parseMessage(data, length);
        
        if (!result.success) {
            if (config_.enable_console_warnings) {
                std::cout << config_.logging.log_prefix << " ⚠️  Failed to parse message: " << result.errorMessage << std::endl;
            }
            return;
        }

        // Handle different message types
        if (result.isSessionEvent()) {
            handleSessionEvent(result);
        } else if (result.isAcknowledgment() || result.isTopicMessage()) {
            handleBusinessMessage(result);
        }

        // Invoke user callback if set
        if (messageCallback_) {
            try {
                messageCallback_(result.messageType, result.payload, result.headers);
            } catch (const std::exception& e) {
                if (config_.enable_console_errors) {
                    std::cerr << config_.logging.log_prefix << " ❌ Error in message callback: " << e.what() << std::endl;
                }
            }
        }
    }

    void handleSessionEvent(const ParseResult& result) {
        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " 🎯 Handling SessionEvent: " << result.getDescription() << std::endl;
        }

        // Delegate to session manager
        sessionManager_->handleSessionEvent(result);
        
        // Update our stats
        if (result.eventCode == SBEConstants::SESSION_EVENT_REDIRECT) {
            stats_.leader_redirects++;
            stats_.current_leader_id = result.leaderMemberId;
        }
    }

    void handleBusinessMessage(const ParseResult& result) {
        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " 📄 Handling business message: " << result.messageType << std::endl;
        }

        // Update acknowledgment stats
        if (result.isAcknowledgment()) {
            // Try to extract success/failure from the message
            if (result.payload.find("\"success\":true") != std::string::npos ||
                result.payload.find("\"status\":\"success\"") != std::string::npos) {
                stats_.messages_acknowledged++;
            } else {
                stats_.messages_failed++;
            }
        }

        if (config_.debug_logging && !result.messageId.empty()) {
            std::cout << config_.logging.log_prefix << "   Message ID: " << result.messageId << std::endl;
        }
    }

    std::string generateMessageId() {
        uint64_t seq = sequenceCounter_.fetch_add(1);
        int64_t now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        return "msg_" + std::to_string(now) + "_" + std::to_string(seq);
    }

    void printConfig() const {
        std::cout << config_.logging.log_prefix << "   Cluster endpoints: ";
        for (size_t i = 0; i < config_.cluster_endpoints.size(); ++i) {
            std::cout << config_.cluster_endpoints[i];
            if (i < config_.cluster_endpoints.size() - 1) std::cout << ", ";
        }
        std::cout << std::endl;
        std::cout << config_.logging.log_prefix << "   Response channel: " << config_.response_channel << std::endl;
        std::cout << config_.logging.log_prefix << "   Aeron dir: " << config_.aeron_dir << std::endl;
        std::cout << config_.logging.log_prefix << "   Ingress stream: " << config_.ingress_stream_id << std::endl;
        std::cout << config_.logging.log_prefix << "   Egress stream: " << config_.egress_stream_id << std::endl;
    }
};

// ClusterClient public interface implementation

ClusterClient::ClusterClient(const ClusterClientConfig& config)
    : pImpl(std::make_unique<Impl>(config)) {
}

ClusterClient::~ClusterClient() = default;

bool ClusterClient::connect() {
    return pImpl->connect();
}

void ClusterClient::disconnect() {
    pImpl->disconnect();
}

bool ClusterClient::isConnected() const {
    return pImpl->isConnected();
}

int64_t ClusterClient::getSessionId() const {
    return pImpl->getSessionId();
}

int32_t ClusterClient::getLeaderMemberId() const {
    return pImpl->getLeaderMemberId();
}

std::string ClusterClient::publishOrder(const Order& order) {
    return pImpl->publishOrder(order);
}

std::string ClusterClient::publishMessage(const std::string& messageType,
                                         const std::string& payload,
                                         const std::string& headers) {
    return pImpl->publishMessage(messageType, payload, headers);
}

void ClusterClient::setMessageCallback(MessageCallback callback) {
    pImpl->setMessageCallback(std::move(callback));
}

int ClusterClient::pollMessages(int maxMessages) {
    return pImpl->pollMessages(maxMessages);
}

ConnectionStats ClusterClient::getConnectionStats() const {
    return pImpl->getConnectionStats();
}

Order ClusterClient::createSampleLimitOrder(const std::string& baseToken,
                                           const std::string& quoteToken,
                                           const std::string& side,
                                           double quantity,
                                           double limitPrice) {
    Order order;
    order.id = OrderUtils::generateOrderId();
    order.baseToken = baseToken;
    order.quoteToken = quoteToken;
    order.side = side;
    order.quantity = quantity;
    order.quantityToken = baseToken;
    order.limitPrice = limitPrice;
    order.limitPriceToken = quoteToken;
    order.orderType = "LIMIT";
    order.tif = "GTC";
    order.status = "CREATED";
    order.requestSource = "API";
    order.requestChannel = "cpp_client";
    
    // Set reasonable defaults
    order.customerID = 12345;
    order.userID = 67890;
    order.accountID = 11111;
    order.baseTokenUsdConversionRate = limitPrice; // Approximate
    order.quoteTokenUsdConversionRate = 1.0; // Assume quote is USD-pegged
    
    order.initializeTimestamps();
    order.generateClientOrderUUID();
    
    return order;
}

// ClusterClientConfigBuilder implementation

ClusterClientConfigBuilder::ClusterClientConfigBuilder() = default;

ClusterClientConfigBuilder& ClusterClientConfigBuilder::withClusterEndpoints(const std::vector<std::string>& endpoints) {
    config_.cluster_endpoints = endpoints;
    return *this;
}

ClusterClientConfigBuilder& ClusterClientConfigBuilder::withResponseChannel(const std::string& channel) {
    config_.response_channel = channel;
    return *this;
}

ClusterClientConfigBuilder& ClusterClientConfigBuilder::withAeronDir(const std::string& aeronDir) {
    config_.aeron_dir = aeronDir;
    return *this;
}

ClusterClientConfigBuilder& ClusterClientConfigBuilder::withResponseTimeout(std::chrono::milliseconds timeout) {
    config_.response_timeout = timeout;
    return *this;
}

ClusterClientConfigBuilder& ClusterClientConfigBuilder::withMaxRetries(int maxRetries) {
    config_.max_retries = maxRetries;
    return *this;
}

ClusterClientConfigBuilder& ClusterClientConfigBuilder::withRetryDelay(std::chrono::milliseconds delay) {
    config_.retry_delay = delay;
    return *this;
}

ClusterClientConfig ClusterClientConfigBuilder::build() const {
    return config_;
}

} // namespace aeron_cluster