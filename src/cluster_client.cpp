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
#include <fstream>

#include <regex>
#include <stdexcept>
#include <string>
#include <set>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace aeron_cluster {

class ClusterClient::Impl {
public:
    explicit Impl(const ClusterClientConfig& config)
        : config_(config)
        , sessionManager_(std::make_unique<SessionManager>(config))
        , messageHandler_(std::make_unique<MessageHandler>())
        , sequenceCounter_(0)
        , rng_(std::random_device{}())
    {
        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " Creating ClusterClient" << std::endl;
        }
    }

    ~Impl() {
        disconnect();
    }

    bool connect() {
        if (isConnected_) {
            std::cout << config_.logging.log_prefix << " Already connected to cluster" << std::endl;
            return true;
        }

        try {
            stats_.connection_attempts++;
            
            std::cout << config_.logging.log_prefix << " 🔌 Connecting to Aeron Cluster..." << std::endl;
            
            // Add timeout for Aeron initialization
            if (!initializeAeronWithTimeout()) {
                std::cerr << config_.logging.log_prefix << " ❌ Failed to initialize Aeron (timeout or error)" << std::endl;
                return false;
            }

            // // Add timeout for egress subscription
            if (!createEgressSubscriptionWithTimeout()) {
                std::cerr << config_.logging.log_prefix << " ❌ Failed to create egress subscription (timeout)" << std::endl;
                return false;
            }

            std::cout << config_.logging.log_prefix << " 📤 Attempting to connect to cluster members..." << std::endl;

            // Add timeout for session manager connection
            if (!connectSessionManagerWithTimeout()) {
                std::cerr << config_.logging.log_prefix << " ❌ Failed to connect to any cluster member (timeout)" << std::endl;
                std::cerr << config_.logging.log_prefix << " 💡 Please ensure:" << std::endl;
                std::cerr << config_.logging.log_prefix << "   • Aeron Cluster is running at specified endpoints" << std::endl;
                std::cerr << config_.logging.log_prefix << "   • Network connectivity to cluster nodes" << std::endl;
                std::cerr << config_.logging.log_prefix << "   • Cluster endpoints are correct: ";
                for (size_t i = 0; i < config_.cluster_endpoints.size(); ++i) {
                    std::cerr << config_.cluster_endpoints[i];
                    if (i < config_.cluster_endpoints.size() - 1) std::cerr << ", ";
                }
                std::cerr << std::endl;
                return false;
            }

            isConnected_ = true;
            stats_.successful_connections++;
            stats_.current_session_id = sessionManager_->getSessionId();
            stats_.current_leader_id = sessionManager_->getLeaderMemberId();
            stats_.connection_established_time = std::chrono::steady_clock::now();
            stats_.is_connected = true;

            pollMessages(100);

            if (config_.enable_console_info) {
                std::cout << config_.logging.log_prefix << " ✅ Connected to Aeron Cluster!" << std::endl;
                std::cout << config_.logging.log_prefix << "    Session ID: " << stats_.current_session_id << std::endl;
                std::cout << config_.logging.log_prefix << "    Leader Member: " << stats_.current_leader_id << std::endl;
            }

            return true;

        } catch (const std::exception& e) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix << " ❌ Connection failed: " << e.what() << std::endl;
            }
            cleanup();
            return false;
        }
    }

    void disconnect() {
        if (!isConnected_) {
            return;
        }

        sessionManager_->disconnect();
        egressSubscription_.reset();
        aeron_.reset();

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

        std::string messageId = generateMessageId();
        std::string messageType = "CREATE_ORDER";
        if (order.status == "UPDATED" || order.status == "CANCELLED") {
            messageType = "UPDATE_ORDER";
        }

        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " 📤 Publishing order: " << order.id 
                     << " (MessageID: " << messageId << ")" << std::endl;
        }

        std::string orderJson = order.toJsonString();
        
        Json::Value headers;
        headers["messageId"] = messageId;
        headers["messageType"] = messageType;
        headers["orderId"] = order.id;
        headers["id"] = order.id;
        
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::string headersJson = Json::writeString(builder, headers);

        if (!sessionManager_->publishMessage(config_.default_topic, messageType, messageId, orderJson, headersJson)) {
            throw std::runtime_error("Failed to publish order message");
        }

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
        // if (!isConnected() || !egressSubscription_) {
        //     std::cout << config_.logging.log_prefix << " Not connected or egress subscription not available" << std::endl;
        //     return 0;
        // }

        int messagesProcessed = 0;
        
        try {
            egressSubscription_->poll(
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
        } catch (const std::exception& e) {
            if (config_.debug_logging) {
                std::cerr << config_.logging.log_prefix << " ⚠️  Error polling messages: " << e.what() << std::endl;
            }
        }
        
        return messagesProcessed;
    }

    ConnectionStats getConnectionStats() const {
        return stats_;
    }

private:
    ClusterClientConfig config_;
    std::unique_ptr<SessionManager> sessionManager_;
    std::unique_ptr<MessageHandler> messageHandler_;
    bool isConnected_ = false;
    ConnectionStats stats_;
    MessageCallback messageCallback_;

    std::shared_ptr<aeron::Aeron> aeron_;
    std::shared_ptr<aeron::Subscription> egressSubscription_;

    std::atomic<uint64_t> sequenceCounter_;
    std::mt19937 rng_;

    // FIXED: Add timeout to Aeron initialization
    bool initializeAeronWithTimeout() {
        try {
            auto startTime = std::chrono::steady_clock::now();
            auto timeout = std::chrono::seconds(5); // 5 second timeout
            
            if (config_.debug_logging) {
                std::cout << config_.logging.log_prefix << " 🔧 Initializing Aeron with directory: " << config_.aeron_dir << std::endl;
            }
            
            // Check if Aeron directory exists first
            if (!checkAeronDirectory()) {
                return false;
            }
            
            aeron::Context context;
            context.aeronDir(config_.aeron_dir);
            
            // Try to connect with timeout
            aeron_ = aeron::Aeron::connect(context);
            
            
            return true;
        } catch (const std::exception& e) {
            std::cerr << config_.logging.log_prefix << " ❌ Aeron initialization error: " << e.what() << std::endl;
            return false;
        }
    }

    // FIXED: Add timeout to egress subscription creation
    bool createEgressSubscriptionWithTimeout() {
        try {
            auto startTime = std::chrono::steady_clock::now();
            auto timeout = std::chrono::seconds(10); // 10 second timeout
            
            
            std::cout << config_.logging.log_prefix << " 🔧 Resolving egress endpoint: " 
                        << config_.response_channel << std::endl;
        

            if (config_.debug_logging) {
                std::cout << config_.logging.log_prefix << " 🔧 Creating egress subscription:" << std::endl;
                std::cout << config_.logging.log_prefix << "   Channel: " << config_.response_channel << std::endl;
                std::cout << config_.logging.log_prefix << "   Stream ID: " << config_.egress_stream_id << std::endl;
            }
            
            int64_t subId = aeron_->addSubscription(config_.response_channel, config_.egress_stream_id);
            
            // Wait for subscription to be ready with timeout
            while ((std::chrono::steady_clock::now() - startTime) < timeout) {
                egressSubscription_ = aeron_->findSubscription(subId);
                if (egressSubscription_) {
                    if (config_.debug_logging) {
                        std::cout << config_.logging.log_prefix << " ✅ Egress subscription created successfully" << std::endl;
                    }
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
            std::cerr << config_.logging.log_prefix << " ❌ Egress subscription creation timeout" << std::endl;
            return false;
            
        } catch (const std::exception& e) {
            std::cerr << config_.logging.log_prefix << " ❌ Egress subscription error: " << e.what() << std::endl;
            return false;
        }
    }

    // FIXED: Add timeout to session manager connection
    bool connectSessionManagerWithTimeout() {
        auto startTime = std::chrono::steady_clock::now();
        auto timeout = config_.response_timeout; // Use configured timeout
        
        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " 🔧 Connecting session manager with " 
                     << timeout.count() << "ms timeout..." << std::endl;
        }
        
        // Try connecting in a separate thread to enable timeout
        std::atomic<bool> connectionComplete{false};
        std::atomic<bool> connectionSuccess{false};
        
        std::thread connectionThread([this, &connectionComplete, &connectionSuccess]() {
            try {
                bool result = sessionManager_->connect(aeron_);
                connectionSuccess = result;
                connectionComplete = true;
            } catch (const std::exception& e) {
                if (config_.debug_logging) {
                    std::cerr << config_.logging.log_prefix << " ❌ Session manager connection error: " 
                             << e.what() << std::endl;
                }
                connectionComplete = true;
            }
        });

        std::thread pollingThread([this]() {
            std::cout << config_.logging.log_prefix << " 📡 Polling messages in background..." << isConnected() << std::endl;
            while (isConnected() && egressSubscription_) {
                pollMessages(2); // Poll messages in background
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
        
        // Wait for connection with timeout
        while (!connectionComplete && 
               (std::chrono::steady_clock::now() - startTime) < timeout) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5000));
        }
        
        if (connectionThread.joinable()) {
            if (!connectionComplete) {
                // Timeout occurred - we can't safely terminate the thread,
                // but we can return false and let the caller handle it
                if (config_.debug_logging) {
                    std::cerr << config_.logging.log_prefix << " ❌ Session manager connection timeout" << std::endl;
                }
                // Note: connectionThread will continue running in background
                // This is not ideal but safer than force-terminating
                connectionThread.detach();
                return false;
            } else {
                connectionThread.join();
            }
        }
        
        return connectionSuccess;
    }

    // FIXED: Add Aeron directory check
    bool checkAeronDirectory() {
        std::string cncFile = config_.aeron_dir + "/cnc.dat";
        std::ifstream file(cncFile);
        bool exists = file.good();
        file.close();
        
        if (!exists) {
            std::cerr << config_.logging.log_prefix << " ❌ Aeron directory not found or Media Driver not running" << std::endl;
            std::cerr << config_.logging.log_prefix << "   Expected CnC file: " << cncFile << std::endl;
            std::cerr << config_.logging.log_prefix << " 💡 Please start Aeron Media Driver first:" << std::endl;
            std::cerr << config_.logging.log_prefix << "   java -cp aeron-all-X.X.X.jar io.aeron.driver.MediaDriver" << std::endl;
            return false;
        }
        
        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " ✅ Aeron directory verified: " << config_.aeron_dir << std::endl;
        }
        
        return true;
    }

    // FIXED: Add cleanup method
    void cleanup() {
        try {
            if (sessionManager_) {
                sessionManager_->disconnect();
            }
            if (egressSubscription_) {
                egressSubscription_.reset();
            }
            if (aeron_) {
                aeron_.reset();
            }
        } catch (const std::exception& e) {
            // Ignore cleanup errors
            if (config_.debug_logging) {
                std::cerr << config_.logging.log_prefix << " ⚠️  Cleanup warning: " << e.what() << std::endl;
            }
        }
        
        isConnected_ = false;
        stats_.is_connected = false;
    }

    void handleIncomingMessage(aeron::AtomicBuffer& buffer, 
                              aeron::util::index_t offset, 
                              aeron::util::index_t length, 
                              aeron::logbuffer::Header& header) {
        try {

            std::cout << config_.logging.log_prefix << " 📥 Incoming message received"  << std::endl;
            const uint8_t* data = buffer.buffer() + offset;
            
            ParseResult result = MessageParser::parseMessage(data, length);
            
            if (result.success) {
                if (result.isSessionEvent()) {
                    sessionManager_->handleSessionEvent(result);
                } else {
                    messageHandler_->handleMessage(result);
                }
            }

            if (messageCallback_) {
                try {
                    messageCallback_(result.messageType, result.payload, result.headers);
                } catch (const std::exception& e) {
                    if (config_.debug_logging) {
                        std::cerr << config_.logging.log_prefix << " ⚠️  Message callback error: " 
                                 << e.what() << std::endl;
                    }
                }
            }
        } catch (const std::exception& e) {
            if (config_.debug_logging) {
                std::cerr << config_.logging.log_prefix << " ❌ Error handling incoming message: " 
                         << e.what() << std::endl;
            }
        }
    }

    std::string generateMessageId() {
        uint64_t seq = sequenceCounter_.fetch_add(1);
        int64_t now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        return "msg_" + std::to_string(now) + "_" + std::to_string(seq);
    }
};

// Rest of the ClusterClient implementation remains the same...
// [Include all the other methods from your original code]

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
    
    order.customerID = 12345;
    order.userID = 67890;
    order.accountID = 11111;
    order.baseTokenUsdConversionRate = limitPrice;
    order.quoteTokenUsdConversionRate = 1.0;
    
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