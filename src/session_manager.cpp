#include "session_manager.hpp"
#include "aeron_cluster/sbe_messages.hpp"
#include <Aeron.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <random>

#include <regex>
#include <stdexcept>
#include <string>
#include <set>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// #include <./utils.hpp>

namespace aeron_cluster {

/**
 * @brief Private implementation for SessionManager (PIMPL pattern)
 */
class SessionManager::Impl {
public:
    explicit Impl(const ClusterClientConfig& config)
        : config_(config)
        , correlationId_(generateCorrelationId())
        , sessionId_(-1)
        , leaderMemberId_(0)
        , connected_(false)
        , rng_(std::random_device{}())
    {
        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " SessionManager created with correlation ID: " 
                     << correlationId_ << std::endl;
        }
    }

    ~Impl() {
        disconnect();
    }

    bool connect(std::shared_ptr<aeron::Aeron> aeron) {
        aeron_ = aeron;
        
        std::cout << config_.logging.log_prefix << " Starting session connection process..." << std::endl;

        // FIXED: Add overall connection timeout
        auto overallStartTime = std::chrono::steady_clock::now();
        auto overallTimeout = config_.response_timeout; // Use configured timeout
        
        // Try connecting to each cluster member until successful
        for (int attempt = 0; attempt < config_.max_retries; ++attempt) {
            // Check overall timeout
            if ((std::chrono::steady_clock::now() - overallStartTime) > overallTimeout) {
                if (config_.enable_console_warnings) {
                    std::cout << config_.logging.log_prefix << " ⏰ Overall connection timeout exceeded" << std::endl;
                }
                break;
            }
            
            for (int memberId = 0; memberId < static_cast<int>(config_.cluster_endpoints.size()); ++memberId) {
                // Check timeout before each member attempt
                if ((std::chrono::steady_clock::now() - overallStartTime) > overallTimeout) {
                    if (config_.enable_console_warnings) {
                        std::cout << config_.logging.log_prefix << " ⏰ Timeout reached during member iteration" << std::endl;
                    }
                    return false;
                }
                
                if (tryConnectToMemberWithTimeout(memberId)) {
                    // if (config_.debug_logging) {
                    std::cout << config_.logging.log_prefix << " ✅ Successfully connected to member " 
                                << memberId << " with session ID: " << sessionId_ << std::endl;
                    // }

                    return true;
                }
                
                // Short delay between member attempts
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
            if (attempt < config_.max_retries - 1) {
                if (config_.enable_console_warnings) {
                    std::cout << config_.logging.log_prefix << " ⚠️  Connection attempt " << (attempt + 1) 
                             << " failed, retrying in " << config_.retry_delay.count() << "ms..." << std::endl;
                }
                
                // Check if we have time for another retry
                auto remainingTime = overallTimeout - (std::chrono::steady_clock::now() - overallStartTime);
                if (remainingTime <= config_.retry_delay) {
                    if (config_.enable_console_warnings) {
                        std::cout << config_.logging.log_prefix << " ⏰ Not enough time for another retry" << std::endl;
                    }
                    break;
                }
                
                std::this_thread::sleep_for(config_.retry_delay);
            }
        }
        
        // FIXED: Provide better error message
        if (config_.enable_console_errors) {
            std::cerr << config_.logging.log_prefix << " ❌ Failed to connect to any cluster member after " 
                     << config_.max_retries << " attempts" << std::endl;
            std::cerr << config_.logging.log_prefix << " 💡 Possible issues:" << std::endl;
            std::cerr << config_.logging.log_prefix << "   • No Aeron Cluster running at specified endpoints" << std::endl;
            std::cerr << config_.logging.log_prefix << "   • Network connectivity issues" << std::endl;
            std::cerr << config_.logging.log_prefix << "   • Firewall blocking UDP traffic" << std::endl;
            std::cerr << config_.logging.log_prefix << "   • Incorrect cluster endpoint configuration" << std::endl;
        }
        
        return false;
    }

    void disconnect() {
        if (config_.debug_logging && connected_) {
            std::cout << config_.logging.log_prefix << " Disconnecting session..." << std::endl;
        }
        
        ingressPublication_.reset();
        connected_ = false;
        sessionId_ = -1;
    }

    bool isConnected() const {
        return connected_ && ingressPublication_ && ingressPublication_->isConnected();
    }

    int64_t getSessionId() const {
        return sessionId_;
    }

    int32_t getLeaderMemberId() const {
        return leaderMemberId_;
    }

    bool publishMessage(const std::string& topic,
                       const std::string& messageType,
                       const std::string& messageId,
                       const std::string& payload,
                       const std::string& headers) {
        if (!isConnected()) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix << " ❌ Cannot publish - not connected" << std::endl;
            }
            return false;
        }

        try {
            // Encode TopicMessage using SBE
            std::vector<uint8_t> encodedMessage = SBEEncoder::encodeTopicMessage(
                topic, messageType, messageId, payload, headers);

            if (config_.debug_logging) {
                std::cout << config_.logging.log_prefix << " 📤 Publishing message:" << std::endl;
                std::cout << config_.logging.log_prefix << "   Topic: " << topic << std::endl;
                std::cout << config_.logging.log_prefix << "   Type: " << messageType << std::endl;
                std::cout << config_.logging.log_prefix << "   ID: " << messageId << std::endl;
                std::cout << config_.logging.log_prefix << "   Size: " << encodedMessage.size() << " bytes" << std::endl;
                
                if (config_.enable_hex_dumps) {
                    SBEUtils::printHexDump(encodedMessage.data(), encodedMessage.size(), 
                                          config_.logging.log_prefix + "   ");
                }
            }

            // Send via Aeron
            aeron::AtomicBuffer buffer(encodedMessage.data(), encodedMessage.size());
            int64_t result = ingressPublication_->offer(buffer, 0, encodedMessage.size());

            if (result > 0) {
                if (config_.debug_logging) {
                    std::cout << config_.logging.log_prefix << " ✅ Message sent, position: " << result << std::endl;
                }
                return true;
            } else {
                if (config_.enable_console_errors) {
                    std::cerr << config_.logging.log_prefix << " ❌ Failed to send message: " << result << std::endl;
                }
                return false;
            }

        } catch (const std::exception& e) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix << " ❌ Error publishing message: " << e.what() << std::endl;
            }
            return false;
        }
    }

    void handleSessionEvent(const ParseResult& result) {
        if (!result.isSessionEvent()) {
            return;
        }

        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " 🎯 Handling SessionEvent:" << std::endl;
            std::cout << config_.logging.log_prefix << "   Correlation ID: " << result.correlationId << std::endl;
            std::cout << config_.logging.log_prefix << "   Event Code: " << result.eventCode 
                     << " (" << SBEUtils::getSessionEventCodeString(result.eventCode) << ")" << std::endl;
            std::cout << config_.logging.log_prefix << "   Session ID: " << result.sessionId << std::endl;
            std::cout << config_.logging.log_prefix << "   Leader Member: " << result.leaderMemberId << std::endl;
        }

        // Check if this event is for our correlation ID
        if (result.correlationId != correlationId_) {
            if (config_.debug_logging) {
                std::cout << config_.logging.log_prefix << " 👻 SessionEvent not for us (correlation " 
                         << result.correlationId << "), ignoring..." << std::endl;
            }
            return;
        }

        // Handle different event codes
        switch (result.eventCode) {
            case SBEConstants::SESSION_EVENT_OK:
                handleSessionOK(result);
                break;
                
            case SBEConstants::SESSION_EVENT_REDIRECT:
                handleSessionRedirect(result);
                break;
                
            case SBEConstants::SESSION_EVENT_ERROR:
            case SBEConstants::SESSION_EVENT_AUTHENTICATION_REJECTED:
                handleSessionError(result);
                break;
                
            case SBEConstants::SESSION_EVENT_CLOSED:
                handleSessionClosed(result);
                break;
                
            default:
                if (config_.enable_console_warnings) {
                    std::cout << config_.logging.log_prefix << " ⚠️  Unknown session event code: " 
                             << result.eventCode << std::endl;
                }
                break;
        }
    }

    // Resolve egress endpoint, replacing port 0 with an available port
    std::string resolveEgressEndpoint(const std::string& channel) {
        // Match hostnames (e.g., localhost), IPv4, and port numbers
        std::regex pattern(R"(endpoint=([a-zA-Z0-9\.\-]+):(\d+))");
        std::smatch match;

        if (std::regex_search(channel, match, pattern)) {
            std::string host = match[1].str();
            std::string portStr = match[2].str();
            int port = std::stoi(portStr);

            if (port == 0) {
                int newPort = getAvailablePortInRange();
                std::string newEndpoint = "endpoint=" + host + ":" + std::to_string(newPort);
                return std::regex_replace(channel, pattern, newEndpoint);
            } else {
                return channel;
            }
        } else {
            throw std::invalid_argument("Channel endpoint is not in the correct format: expected 'endpoint=HOST:PORT'");
        }
    }

private:
    ClusterClientConfig config_;
    std::shared_ptr<aeron::Aeron> aeron_;
    std::shared_ptr<aeron::ExclusivePublication> ingressPublication_;
    
    int64_t correlationId_;
    int64_t sessionId_;
    int32_t leaderMemberId_;
    bool connected_;
    
    std::mt19937 rng_;

    // FIXED: Add timeout to individual member connection attempts
    bool tryConnectToMemberWithTimeout(int memberId) {
        if (memberId >= static_cast<int>(config_.cluster_endpoints.size())) {
            return false;
        }

        std::string endpoint = config_.cluster_endpoints[memberId];
        
        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " 🎯 Attempting connection to member " 
                     << memberId << ": " << endpoint << std::endl;
        }

        auto memberStartTime = std::chrono::steady_clock::now();
        auto memberTimeout = std::chrono::seconds(5); // 5 second timeout per member

        try {
            // Create ingress publication
            std::string ingressChannel = "aeron:udp?endpoint=" + endpoint;
            
            if (config_.debug_logging) {
                std::cout << config_.logging.log_prefix << " 📤 Creating ingress publication: " 
                         << ingressChannel << std::endl;
            }

            int64_t pubId = aeron_->addExclusivePublication(ingressChannel, config_.ingress_stream_id);

            // FIXED: Wait for publication to connect with timeout
            bool publicationConnected = false;
            while ((std::chrono::steady_clock::now() - memberStartTime) < memberTimeout) {
                ingressPublication_ = aeron_->findExclusivePublication(pubId);
                if (ingressPublication_ && ingressPublication_->isConnected()) {
                    publicationConnected = true;
                    if (config_.debug_logging) {
                        std::cout << config_.logging.log_prefix << " ✅ Ingress publication connected" << std::endl;
                    }
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            if (!publicationConnected) {
                if (config_.enable_console_warnings) {
                    std::cout << config_.logging.log_prefix << " ⏰ Ingress publication connection timeout for " 
                             << endpoint << std::endl;
                }
                return false;
            }

            // FIXED: Send SessionConnectRequest with remaining timeout
            auto remainingTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                memberTimeout - (std::chrono::steady_clock::now() - memberStartTime));
            if (remainingTime <= std::chrono::milliseconds(100)) {
                if (config_.enable_console_warnings) {
                    std::cout << config_.logging.log_prefix << " ⏰ Not enough time to send SessionConnectRequest" << std::endl;
                }
                return false;
            }

            if (!sendSessionConnectRequestWithTimeout(remainingTime)) {
                return false;
            }

            std::cout << config_.logging.log_prefix << " 📡 Waiting for SessionEvent response..." << std::endl;
            // Update current leader
            leaderMemberId_ = memberId;
            return false;

        } catch (const std::exception& e) {
            if (config_.enable_console_warnings) {
                std::cout << config_.logging.log_prefix << " ⚠️  Error connecting to member " 
                         << memberId << ": " << e.what() << std::endl;
            }
            return false;
        }
    }

    int getAvailablePortInRange(int minPort = 40000, int maxPort = 59000) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dist(minPort, maxPort);
        std::set<int> tried;

        while (tried.size() < (maxPort - minPort + 1)) {
            int port = dist(gen);
            if (tried.count(port)) continue;
            tried.insert(port);

            int sock = socket(AF_INET, SOCK_DGRAM, 0);
            if (sock < 0) continue;

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(port);

            if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                close(sock);
                return port;  // port is available
            }

            close(sock);
        }

        throw std::runtime_error("No available UDP port found in the range");
    }

    // FIXED: Add timeout to SessionConnectRequest sending
    bool sendSessionConnectRequestWithTimeout(std::chrono::milliseconds timeout) {
        try {

            // std::string response_channel = resolveEgressEndpoint(config_.response_channel);

            if (config_.debug_logging) {
                std::cout << config_.logging.log_prefix << " 📤 Sending SessionConnectRequest..." << std::endl;
                std::cout << config_.logging.log_prefix << "   Correlation ID: " << correlationId_ << std::endl;
                std::cout << config_.logging.log_prefix << "   Response Stream ID: " << config_.egress_stream_id << std::endl;
                std::cout << config_.logging.log_prefix << "   Response Channel: " << config_.response_channel << std::endl;
            }

            // Encode SessionConnectRequest using SBE
            std::vector<uint8_t> encodedMessage = SBEEncoder::encodeSessionConnectRequest(
                correlationId_,
                config_.egress_stream_id,
                config_.response_channel,
                config_.protocol_semantic_version
            );

            if (config_.debug_logging) {
                std::cout << config_.logging.log_prefix << " 📦 Encoded SessionConnectRequest: " 
                         << encodedMessage.size() << " bytes" << std::endl;
                
                if (config_.enable_hex_dumps) {
                    SBEUtils::printHexDump(encodedMessage.data(), encodedMessage.size(), 
                                          config_.logging.log_prefix + "   ");
                }
            }

            // FIXED: Try sending with backpressure handling and timeout
            auto sendStartTime = std::chrono::steady_clock::now();
            aeron::AtomicBuffer buffer(encodedMessage.data(), encodedMessage.size());
            std::cout << config_.logging.log_prefix << " ⏳⏳⏳⏳⏳⏳⏳⏳⏳⏳⏳⏳⏳⏳⏳ Sending SessionConnectRequest..." << std::endl;
            // while ((std::chrono::steady_clock::now() - sendStartTime) < timeout) {
                int64_t result = ingressPublication_->offer(buffer, 0, encodedMessage.size());
            std::cout << config_.logging.log_prefix << " ⏳⏳⏳⏳⏳⏳⏳----------⏳⏳⏳⏳⏳⏳⏳ Sending SessionConnectRequest..." << std::endl;
            if (result > 0) {
                if (config_.debug_logging) {
                    std::cout << config_.logging.log_prefix << " ✅ SessionConnectRequest sent, position: " 
                                << result << std::endl;
                }
                return true;
            } else if (result == aeron::BACK_PRESSURED) {
                // Backpressure - wait a bit and retry
                if (config_.debug_logging) {
                    std::cout << config_.logging.log_prefix << " ⚠️  Backpressure, retrying..." << std::endl;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } else if (result == aeron::NOT_CONNECTED) {
                if (config_.enable_console_warnings) {
                    std::cout << config_.logging.log_prefix << " ⚠️  Publication not connected" << std::endl;
                }
                return false;
            } else {
                if (config_.enable_console_errors) {
                    std::cerr << config_.logging.log_prefix << " ❌ Failed to send SessionConnectRequest: " 
                                << result << std::endl;
                }
                return false;
            }
            // }

            if (config_.enable_console_warnings) {
                std::cout << config_.logging.log_prefix << " ⏰ SessionConnectRequest send timeout" << std::endl;
            }
            return false;

        } catch (const std::exception& e) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix << " ❌ Error sending SessionConnectRequest: " 
                         << e.what() << std::endl;
            }
            return false;
        }
    }

    // FIXED: Original sendSessionConnectRequest method for compatibility
    bool sendSessionConnectRequest() {
        return sendSessionConnectRequestWithTimeout(std::chrono::seconds(5));
    }

    void handleSessionOK(const ParseResult& result) {
        sessionId_ = result.sessionId;
        connected_ = true;
        
        if (config_.enable_console_info) {
            std::cout << config_.logging.log_prefix << " 🎉 Session established successfully!" << std::endl;
            std::cout << config_.logging.log_prefix << "   Session ID: " << sessionId_ << std::endl;
            std::cout << config_.logging.log_prefix << "   Leader Member: " << result.leaderMemberId << std::endl;
        }
    }

    void handleSessionRedirect(const ParseResult& result) {
        if (config_.enable_console_info) {
            std::cout << config_.logging.log_prefix << " 🔄 Redirected to leader member: " 
                     << result.leaderMemberId << std::endl;
        }

        // Disconnect current publication
        ingressPublication_.reset();
        connected_ = false;

        // FIXED: Add timeout to redirect attempt
        if (result.leaderMemberId >= 0 && 
            result.leaderMemberId < static_cast<int32_t>(config_.cluster_endpoints.size())) {
            
            // Small delay before redirect
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
            if (tryConnectToMemberWithTimeout(result.leaderMemberId)) {
                if (config_.debug_logging) {
                    std::cout << config_.logging.log_prefix << " ✅ Successfully redirected to new leader" << std::endl;
                }
            } else {
                if (config_.enable_console_warnings) {
                    std::cout << config_.logging.log_prefix << " ⚠️  Failed to connect to redirected leader" << std::endl;
                }
            }
        } else {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix << " ❌ Invalid leader member ID in redirect: " 
                         << result.leaderMemberId << std::endl;
            }
        }
    }

    void handleSessionError(const ParseResult& result) {
        if (config_.enable_console_errors) {
            std::cerr << config_.logging.log_prefix << " ❌ Session error: " 
                     << SBEUtils::getSessionEventCodeString(result.eventCode) << std::endl;
            
            if (!result.payload.empty()) {
                std::cerr << config_.logging.log_prefix << "   Details: " << result.payload << std::endl;
            }
        }
        
        connected_ = false;
        
        // Analyze error and provide suggestions
        switch (result.eventCode) {
            case SBEConstants::SESSION_EVENT_AUTHENTICATION_REJECTED:
                if (config_.enable_console_info) {
                    std::cout << config_.logging.log_prefix << " 💡 Authentication rejected. Check:" << std::endl;
                    std::cout << config_.logging.log_prefix << "   • Protocol version compatibility" << std::endl;
                    std::cout << config_.logging.log_prefix << "   • Cluster authentication settings" << std::endl;
                }
                break;
                
            default:
                if (config_.enable_console_info) {
                    std::cout << config_.logging.log_prefix << " 💡 Session error. Check:" << std::endl;
                    std::cout << config_.logging.log_prefix << "   • SBE message format" << std::endl;
                    std::cout << config_.logging.log_prefix << "   • Cluster configuration" << std::endl;
                    std::cout << config_.logging.log_prefix << "   • Network connectivity" << std::endl;
                }
                break;
        }
    }

    void handleSessionClosed(const ParseResult& result) {
        if (config_.enable_console_info) {
            std::cout << config_.logging.log_prefix << " 👋 Session closed by cluster" << std::endl;
            
            if (!result.payload.empty()) {
                std::cout << config_.logging.log_prefix << "   Reason: " << result.payload << std::endl;
            }
        }
        
        connected_ = false;
        sessionId_ = -1;
    }

    int64_t generateCorrelationId() {
        // Generate a unique correlation ID using timestamp and random component
        auto now = std::chrono::high_resolution_clock::now();
        int64_t timestamp = now.time_since_epoch().count();
        
        // Use only lower 32 bits of timestamp to avoid overflow issues
        int64_t truncatedTimestamp = timestamp & 0x7FFFFFFF;
        
        // Add random component to avoid collisions
        std::uniform_int_distribution<int32_t> dist(1, 65535);
        int32_t randomPart = dist(rng_);
        
        // Combine timestamp and random part
        int64_t correlationId = (truncatedTimestamp << 16) | randomPart;
        
        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " 🎲 Generated correlation ID: " << correlationId 
                     << " (timestamp: " << truncatedTimestamp << ", random: " << randomPart << ")" << std::endl;
        }
        
        return correlationId;
    }
};

// SessionManager public interface implementation

SessionManager::SessionManager(const ClusterClientConfig& config)
    : pImpl(std::make_unique<Impl>(config)) {
}

SessionManager::~SessionManager() = default;

bool SessionManager::connect(std::shared_ptr<aeron::Aeron> aeron) {
    return pImpl->connect(aeron);
}

void SessionManager::disconnect() {
    pImpl->disconnect();
}

bool SessionManager::isConnected() const {
    return pImpl->isConnected();
}

int64_t SessionManager::getSessionId() const {
    return pImpl->getSessionId();
}

int32_t SessionManager::getLeaderMemberId() const {
    return pImpl->getLeaderMemberId();
}

bool SessionManager::publishMessage(const std::string& topic,
                                   const std::string& messageType,
                                   const std::string& messageId,
                                   const std::string& payload,
                                   const std::string& headers) {
    return pImpl->publishMessage(topic, messageType, messageId, payload, headers);
}

void SessionManager::handleSessionEvent(const ParseResult& result) {
    pImpl->handleSessionEvent(result);
}

std::string SessionManager::resolveEgressEndpoint(const std::string& channel) {
    return pImpl->resolveEgressEndpoint(channel);
}

} // namespace aeron_cluster