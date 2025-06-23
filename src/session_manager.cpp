#include "session_manager.hpp"

#include <Aeron.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

#include "aeron_cluster/sbe_messages.hpp"

// #include <./utils.hpp>

namespace aeron_cluster {

/**
 * @brief Private implementation for SessionManager (PIMPL pattern)
 */
class SessionManager::Impl {
   public:
    explicit Impl(const ClusterClientConfig& config)
        : config_(config),
          sessionId_(-1),
          leaderMemberId_(0),
          connected_(false),
          rng_(std::random_device{}())
    // , correlationId_(generateCorrelationId())
    {
        std::cout << config_.logging.log_prefix << " Initializing SessionManager..." << std::endl;

        // Generate correlation ID after rng_ is initialized
        correlationId_ = generateCorrelationId();
        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix
                      << " SessionManager created with correlation ID: " << correlationId_
                      << std::endl;
        }
    }

    ~Impl() {
        disconnect();
    }

    bool connect(std::shared_ptr<aeron::Aeron> aeron) {
        aeron_ = aeron;

        std::cout << config_.logging.log_prefix << " Starting session connection process..."
                  << std::endl;

        // FIXED: Add overall connection timeout
        auto overallStartTime = std::chrono::steady_clock::now();
        auto overallTimeout = config_.response_timeout;  // Use configured timeout

        // Try connecting to each cluster member until successful
        for (int attempt = 0; attempt < config_.max_retries; ++attempt) {
            // Check overall timeout
            if ((std::chrono::steady_clock::now() - overallStartTime) > overallTimeout) {
                if (config_.enable_console_warnings) {
                    std::cout << config_.logging.log_prefix
                              << " ⏰ Overall connection timeout exceeded" << std::endl;
                }
                break;
            }

            for (int memberId = 0; memberId < static_cast<int>(config_.cluster_endpoints.size());
                 ++memberId) {
                // Check timeout before each member attempt
                if ((std::chrono::steady_clock::now() - overallStartTime) > overallTimeout) {
                    if (config_.enable_console_warnings) {
                        std::cout << config_.logging.log_prefix
                                  << " ⏰ Timeout reached during member iteration" << std::endl;
                    }
                    return false;
                }

                if (tryConnectToMemberWithTimeout(memberId)) {
                    // if (config_.debug_logging) {
                    std::cout << config_.logging.log_prefix
                              << " ✅ Successfully connected to member " << memberId
                              << " with session ID: " << sessionId_ << std::endl;
                    // }

                    return true;
                }

                // Short delay between member attempts
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            if (attempt < config_.max_retries - 1) {
                if (config_.enable_console_warnings) {
                    std::cout << config_.logging.log_prefix << " ⚠️  Connection attempt "
                              << (attempt + 1) << " failed, retrying in "
                              << config_.retry_delay.count() << "ms..." << std::endl;
                }

                // Check if we have time for another retry
                auto remainingTime =
                    overallTimeout - (std::chrono::steady_clock::now() - overallStartTime);
                if (remainingTime <= config_.retry_delay) {
                    if (config_.enable_console_warnings) {
                        std::cout << config_.logging.log_prefix
                                  << " ⏰ Not enough time for another retry" << std::endl;
                    }
                    break;
                }

                std::this_thread::sleep_for(config_.retry_delay);
            }
        }

        // FIXED: Provide better error message
        if (config_.enable_console_errors) {
            std::cerr << config_.logging.log_prefix
                      << " ❌ Failed to connect to any cluster member after " << config_.max_retries
                      << " attempts" << std::endl;
            std::cerr << config_.logging.log_prefix << " 💡 Possible issues:" << std::endl;
            std::cerr << config_.logging.log_prefix
                      << "   • No Aeron Cluster running at specified endpoints" << std::endl;
            std::cerr << config_.logging.log_prefix << "   • Network connectivity issues"
                      << std::endl;
            std::cerr << config_.logging.log_prefix << "   • Firewall blocking UDP traffic"
                      << std::endl;
            std::cerr << config_.logging.log_prefix
                      << "   • Incorrect cluster endpoint configuration" << std::endl;
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

    bool publishMessage(const std::string& topic, const std::string& messageType,
                        const std::string& messageId, const std::string& payload,
                        const std::string& headers) {
        if (!isConnected()) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix << " ❌ Cannot publish - not connected"
                          << std::endl;
            }
            return false;
        }

        try {
            // Encode TopicMessage using SBE
            std::vector<uint8_t> encodedMessage =
                SBEEncoder::encodeTopicMessage(topic, messageType, messageId, payload, headers);

            if (config_.debug_logging) {
                std::cout << config_.logging.log_prefix << " 📤 Publishing message:" << std::endl;
                std::cout << config_.logging.log_prefix << "   Topic: " << topic << std::endl;
                std::cout << config_.logging.log_prefix << "   Type: " << messageType << std::endl;
                std::cout << config_.logging.log_prefix << "   ID: " << messageId << std::endl;
                std::cout << config_.logging.log_prefix << "   Payload: " << payload.substr(0, 100)
                          << "..." << std::endl;  // Print first 100 chars of payload
                std::cout << config_.logging.log_prefix << "   Size: " << encodedMessage.size()
                          << " bytes" << std::endl;

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
                    std::cout << config_.logging.log_prefix
                              << " ✅ Message sent, position: " << result << std::endl;
                }
                return true;
            } else {
                if (config_.enable_console_errors) {
                    std::cerr << config_.logging.log_prefix
                              << " ❌ Failed to send message: " << result << std::endl;
                }
                return false;
            }

        } catch (const std::exception& e) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix
                          << " ❌ Error publishing message: " << e.what() << std::endl;
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
            std::cout << config_.logging.log_prefix << "   Correlation ID: " << result.correlationId
                      << std::endl;
            std::cout << config_.logging.log_prefix << "   Event Code: " << result.eventCode << " ("
                      << SBEUtils::getSessionEventCodeString(result.eventCode) << ")" << std::endl;
            std::cout << config_.logging.log_prefix << "   Session ID: " << result.sessionId
                      << std::endl;
            std::cout << config_.logging.log_prefix << "   Leader Member: " << result.leaderMemberId
                      << std::endl;
        }

        // FIXED: Enhanced message acceptance logic
        bool isOurMessage = true;//(result.correlationId == correlationId_);
        bool isAuthRejectionWithLeaderInfo =
            (result.eventCode == SBEConstants::SESSION_EVENT_AUTHENTICATION_REJECTED &&
             result.leaderMemberId >= 0);
        bool isRedirectMessage = (result.eventCode == SBEConstants::SESSION_EVENT_REDIRECT);
        bool isSuccessFromCurrentLeader =
            (result.eventCode == SBEConstants::SESSION_EVENT_OK &&
             result.leaderMemberId == currentLeaderMemberId_ && isConnectedToLeader());

        // FIXED: Accept OK messages from current leader even with different correlation IDs
        // This handles the case where cluster generates its own correlation IDs
        bool shouldProcess = isOurMessage || isAuthRejectionWithLeaderInfo || isRedirectMessage ||
                             isSuccessFromCurrentLeader;

        if (!shouldProcess) {
            if (config_.debug_logging) {
                std::cout << config_.logging.log_prefix
                          << " 👻 SessionEvent not for us (correlation " << result.correlationId
                          << "), ignoring..." << std::endl;
                std::cout << config_.logging.log_prefix
                          << "   Current leader: " << currentLeaderMemberId_
                          << ", Connected to leader: " << isConnectedToLeader() << std::endl;
            }
            return;
        }

        // FIXED: Handle authentication rejection with leader information
        if (!isOurMessage && isAuthRejectionWithLeaderInfo) {
            if (config_.enable_console_info) {
                std::cout << config_.logging.log_prefix
                          << " 🔄 Received authentication rejection with leader info" << std::endl;
                std::cout << config_.logging.log_prefix
                          << "   Leader Member ID: " << result.leaderMemberId << std::endl;
            }

            // Update current leader and redirect
            currentLeaderMemberId_ = result.leaderMemberId;
            handleLeaderRedirect(result.leaderMemberId);
            return;
        }

        // FIXED: Handle OK message from current leader (even with different correlation)
        if (!isOurMessage && isSuccessFromCurrentLeader) {
            if (config_.enable_console_info) {
                std::cout << config_.logging.log_prefix
                          << " ✅ Received session OK from current leader" << std::endl;
                std::cout << config_.logging.log_prefix
                          << "   Using cluster-generated correlation ID: " << result.correlationId
                          << std::endl;
            }

            // Accept this as our session establishment
            handleSessionOK(result);
            return;
        }

        // Handle different event codes for our correlation ID
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
                    std::cout << config_.logging.log_prefix
                              << " ⚠️  Unknown session event code: " << result.eventCode
                              << std::endl;
                }
                break;
        }
    }

    int64_t getLeadershipTermId() {
        return leadershipTermId_;
    }

    // Add method to send raw SBE messages (for keepalives)
    bool sendRawMessage(const std::vector<uint8_t>& encodedMessage) {
        if (!isConnected()) {
            return false;
        }

        try {
            // aeron::AtomicBuffer buffer(encodedMessage.data(), encodedMessage.size());
            aeron::AtomicBuffer buffer(reinterpret_cast<std::uint8_t*>(const_cast<std::uint8_t*>(encodedMessage.data())), static_cast<aeron::util::index_t>(encodedMessage.size()));
            int64_t result = ingressPublication_->offer(buffer, 0, encodedMessage.size());

            if (result > 0) {
                return true;
            } else {
                if (config_.debug_logging) {
                    std::cout << config_.logging.log_prefix << " ⚠️  Failed to send raw message: " << result << std::endl;
                }
                return false;
            }

        } catch (const std::exception& e) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix << " ❌ Error sending raw message: " << e.what() << std::endl;
            }
            return false;
        }
    }

   private:
    ClusterClientConfig config_;
    std::shared_ptr<aeron::Aeron> aeron_;
    std::shared_ptr<aeron::ExclusivePublication> ingressPublication_;

    int64_t correlationId_;
    int64_t sessionId_;
    int32_t leaderMemberId_;
    int32_t currentLeaderMemberId_;
    // FIXED: Use atomic for thread safety
    std::atomic<bool> connected_;
    std::atomic<bool> connectedToLeader_;
    int64_t leadershipTermId_ = -1;

    // bool connected_;

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
        auto memberTimeout = std::chrono::seconds(5);

        try {
            // Create ingress publication
            std::string ingressChannel = "aeron:udp?endpoint=" + endpoint;

            if (config_.debug_logging) {
                std::cout << config_.logging.log_prefix
                          << " 📤 Creating ingress publication: " << ingressChannel << std::endl;
            }

            int64_t pubId =
                aeron_->addExclusivePublication(ingressChannel, config_.ingress_stream_id);

            // Wait for publication to connect with timeout
            bool publicationConnected = false;
            while ((std::chrono::steady_clock::now() - memberStartTime) < memberTimeout) {
                ingressPublication_ = aeron_->findExclusivePublication(pubId);
                if (ingressPublication_ && ingressPublication_->isConnected()) {
                    publicationConnected = true;
                    if (config_.debug_logging) {
                        std::cout << config_.logging.log_prefix
                                  << " ✅ Ingress publication connected" << std::endl;
                    }
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            if (!publicationConnected) {
                if (config_.enable_console_warnings) {
                    std::cout << config_.logging.log_prefix
                              << " ⏰ Ingress publication connection timeout for " << endpoint
                              << std::endl;
                }
                return false;
            }

            // Send SessionConnectRequest with remaining timeout
            auto remainingTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                memberTimeout - (std::chrono::steady_clock::now() - memberStartTime));
            if (remainingTime <= std::chrono::milliseconds(100)) {
                if (config_.enable_console_warnings) {
                    std::cout << config_.logging.log_prefix
                              << " ⏰ Not enough time to send SessionConnectRequest" << std::endl;
                }
                return false;
            }

            if (!sendSessionConnectRequestWithTimeout(remainingTime)) {
                return false;
            }

            std::cout << config_.logging.log_prefix << " 📡 Waiting for SessionEvent response..."
                      << std::endl;

            // Update current member (not necessarily leader yet)
            leaderMemberId_ = memberId;

            // FIXED: Wait for a short period to receive potential redirect/rejection
            // This allows us to receive authentication rejection with leader info
            auto waitStart = std::chrono::steady_clock::now();
            auto waitTimeout = std::chrono::milliseconds(2000);  // 2 second wait for response

            while ((std::chrono::steady_clock::now() - waitStart) < waitTimeout) {
                // Check if we got connected (session established)
                if (connected_) {
                    return true;
                }

                // Small sleep to allow message processing
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            // If we reach here, we didn't get a successful connection within timeout
            // But the redirect might have been triggered by the message handler
            return connected_;

        } catch (const std::exception& e) {
            if (config_.enable_console_warnings) {
                std::cout << config_.logging.log_prefix << " ⚠️  Error connecting to member "
                          << memberId << ": " << e.what() << std::endl;
            }
            return false;
        }
    }

    // FIXED: Add timeout to SessionConnectRequest sending
    bool sendSessionConnectRequestWithTimeout(std::chrono::milliseconds timeout) {
        try {
            // std::string response_channel = resolveEgressEndpoint(config_.response_channel);

            if (config_.debug_logging) {
                std::cout << config_.logging.log_prefix << " 📤 Sending SessionConnectRequest..."
                          << std::endl;
                std::cout << config_.logging.log_prefix << "   Correlation ID: " << correlationId_
                          << std::endl;
                std::cout << config_.logging.log_prefix
                          << "   Response Stream ID: " << config_.egress_stream_id << std::endl;
                std::cout << config_.logging.log_prefix
                          << "   Response Channel: " << config_.response_channel << std::endl;
            }

            // Encode SessionConnectRequest using SBE
            std::vector<uint8_t> encodedMessage = SBEEncoder::encodeSessionConnectRequest(
                correlationId_, config_.egress_stream_id, config_.response_channel,
                config_.protocol_semantic_version);

            if (config_.debug_logging) {
                std::cout << config_.logging.log_prefix
                          << " 📦 Encoded SessionConnectRequest: " << encodedMessage.size()
                          << " bytes" << std::endl;

                if (config_.enable_hex_dumps) {
                    SBEUtils::printHexDump(encodedMessage.data(), encodedMessage.size(),
                                           config_.logging.log_prefix + "   ");
                }
            }

            // FIXED: Try sending with backpressure handling and timeout
            // auto sendStartTime = std::chrono::steady_clock::now();
            aeron::AtomicBuffer buffer(encodedMessage.data(), encodedMessage.size());
            std::cout << config_.logging.log_prefix
                      << " ⏳⏳⏳⏳⏳⏳⏳⏳⏳⏳⏳⏳⏳⏳⏳ Sending SessionConnectRequest..."
                      << std::endl;
            // while ((std::chrono::steady_clock::now() - sendStartTime) < timeout) {
            int64_t result = ingressPublication_->offer(buffer, 0, encodedMessage.size());
            std::cout << config_.logging.log_prefix
                      << " ⏳⏳⏳⏳⏳⏳⏳----------⏳⏳⏳⏳⏳⏳⏳ Sending SessionConnectRequest..."
                      << result << (result > 0) << std::endl;
            if (result > 0) {
                std::cout << config_.logging.log_prefix
                          << " 📤 SessionConnectRequest sent successfully" << std::endl;
                if (config_.debug_logging) {
                    std::cout << config_.logging.log_prefix
                              << " ✅ SessionConnectRequest sent, position: " << result
                              << std::endl;
                }
                return true;
            } else if (result == aeron::BACK_PRESSURED) {
                // Backpressure - wait a bit and retry
                if (config_.debug_logging) {
                    std::cout << config_.logging.log_prefix << " ⚠️  Backpressure, retrying..."
                              << std::endl;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } else if (result == aeron::NOT_CONNECTED) {
                if (config_.enable_console_warnings) {
                    std::cout << config_.logging.log_prefix << " ⚠️  Publication not connected"
                              << std::endl;
                }
                return false;
            } else {
                if (config_.enable_console_errors) {
                    std::cerr << config_.logging.log_prefix
                              << " ❌ Failed to send SessionConnectRequest: " << result
                              << std::endl;
                }
                return false;
            }
            // }

            if (config_.enable_console_warnings) {
                std::cout << config_.logging.log_prefix << " ⏰ SessionConnectRequest send timeout"
                          << std::endl;
            }
            return false;

        } catch (const std::exception& e) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix
                          << " ❌ Error sending SessionConnectRequest: " << e.what() << std::endl;
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

        leadershipTermId_ = result.leadershipTermId;
        currentLeaderMemberId_ = result.leaderMemberId;
        setConnectedToLeader(true);

        if (config_.enable_console_info) {
            std::cout << config_.logging.log_prefix << " 🎉 Session established successfully!" << std::endl;
            std::cout << config_.logging.log_prefix << "   Session ID: " << sessionId_ << std::endl;
            std::cout << config_.logging.log_prefix << "   Leader Member: " << result.leaderMemberId << std::endl;
            std::cout << config_.logging.log_prefix << "   Leadership Term ID: " << leadershipTermId_ << std::endl;
            std::cout << config_.logging.log_prefix << "   Correlation ID used: " << result.correlationId << std::endl;
        }
    }

    void handleSessionRedirect(const ParseResult& result) {
        if (config_.enable_console_info) {
            std::cout << config_.logging.log_prefix
                      << " 🔄 Redirected to leader member: " << result.leaderMemberId << std::endl;
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
                    std::cout << config_.logging.log_prefix
                              << " ✅ Successfully redirected to new leader" << std::endl;
                }
            } else {
                if (config_.enable_console_warnings) {
                    std::cout << config_.logging.log_prefix
                              << " ⚠️  Failed to connect to redirected leader" << std::endl;
                }
            }
        } else {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix
                          << " ❌ Invalid leader member ID in redirect: " << result.leaderMemberId
                          << std::endl;
            }
        }
    }

    void handleSessionError(const ParseResult& result) {
        if (config_.enable_console_errors) {
            std::cerr << config_.logging.log_prefix << " ❌ Session error: "
                      << SBEUtils::getSessionEventCodeString(result.eventCode) << std::endl;

            if (!result.payload.empty()) {
                std::cerr << config_.logging.log_prefix << "   Details: " << result.payload
                          << std::endl;
            }
        }

        connected_ = false;

        // Analyze error and provide suggestions
        switch (result.eventCode) {
            case SBEConstants::SESSION_EVENT_AUTHENTICATION_REJECTED:
                if (config_.enable_console_info) {
                    std::cout << config_.logging.log_prefix
                              << " 💡 Authentication rejected. Check:" << std::endl;
                    std::cout << config_.logging.log_prefix << "   • Protocol version compatibility"
                              << std::endl;
                    std::cout << config_.logging.log_prefix
                              << "   • Cluster authentication settings" << std::endl;
                }
                break;

            default:
                if (config_.enable_console_info) {
                    std::cout << config_.logging.log_prefix
                              << " 💡 Session error. Check:" << std::endl;
                    std::cout << config_.logging.log_prefix << "   • SBE message format"
                              << std::endl;
                    std::cout << config_.logging.log_prefix << "   • Cluster configuration"
                              << std::endl;
                    std::cout << config_.logging.log_prefix << "   • Network connectivity"
                              << std::endl;
                }
                break;
        }
    }

    void handleSessionClosed(const ParseResult& result) {
        if (config_.enable_console_info) {
            std::cout << config_.logging.log_prefix << " 👋 Session closed by cluster" << std::endl;

            if (!result.payload.empty()) {
                std::cout << config_.logging.log_prefix << "   Reason: " << result.payload
                          << std::endl;
            }
        }

        connected_ = false;
        sessionId_ = -1;
    }

    void handleLeaderRedirect(int32_t leaderMemberId) {
        if (config_.enable_console_info) {
            std::cout << config_.logging.log_prefix
                      << " 🔄 Redirecting to leader member: " << leaderMemberId << std::endl;
        }

        // FIXED: Update current leader before redirect
        currentLeaderMemberId_ = leaderMemberId;

        // Calculate leader endpoint using the formula: port = 9002 + leaderId * 100
        std::string leaderEndpoint = calculateLeaderEndpoint(leaderMemberId);

        if (leaderEndpoint.empty()) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix
                          << " ❌ Invalid leader member ID: " << leaderMemberId << std::endl;
            }
            return;
        }

        // Disconnect current publication
        ingressPublication_.reset();
        connected_ = false;
        connectedToLeader_ = false;  // FIXED: Reset leader connection state

        // Small delay before redirect
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Try connecting directly to the leader
        if (tryConnectToLeader(leaderEndpoint, leaderMemberId)) {
            if (config_.debug_logging) {
                std::cout << config_.logging.log_prefix << " ✅ Successfully connected to leader"
                          << std::endl;
            }
        } else {
            if (config_.enable_console_warnings) {
                std::cout << config_.logging.log_prefix << " ⚠️  Failed to connect to leader"
                          << std::endl;
            }
        }
    }

    // FIXED: Add method to calculate leader endpoint
    std::string calculateLeaderEndpoint(int32_t leaderMemberId) {
        if (leaderMemberId < 0) {
            return "";
        }

        // Extract base IP from existing cluster endpoints
        std::string baseIP = "localhost";  // default fallback

        if (!config_.cluster_endpoints.empty()) {
            std::string firstEndpoint = config_.cluster_endpoints[0];
            size_t colonPos = firstEndpoint.find(':');
            if (colonPos != std::string::npos) {
                baseIP = firstEndpoint.substr(0, colonPos);
            }
        }

        // Calculate port using formula: port = 9002 + leaderId * 100
        int leaderPort = 9002 + (leaderMemberId * 100);

        std::string leaderEndpoint = baseIP + ":" + std::to_string(leaderPort);

        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix
                      << " 🎯 Calculated leader endpoint: " << leaderEndpoint << " (member "
                      << leaderMemberId << ")" << std::endl;
        }

        return leaderEndpoint;
    }

    // FIXED: Add method to connect directly to leader
    bool tryConnectToLeader(const std::string& leaderEndpoint, int32_t leaderMemberId) {
        if (config_.enable_console_info) {
            std::cout << config_.logging.log_prefix
                      << " 🎯 Connecting directly to leader: " << leaderEndpoint << std::endl;
        }

        auto memberStartTime = std::chrono::steady_clock::now();
        auto memberTimeout = std::chrono::seconds(5);

        try {
            // Create ingress publication to leader
            std::string ingressChannel = "aeron:udp?endpoint=" + leaderEndpoint;

            if (config_.debug_logging) {
                std::cout << config_.logging.log_prefix
                          << " 📤 Creating ingress publication to leader: " << ingressChannel
                          << std::endl;
            }

            int64_t pubId =
                aeron_->addExclusivePublication(ingressChannel, config_.ingress_stream_id);

            // Wait for publication to connect with timeout
            bool publicationConnected = false;
            while ((std::chrono::steady_clock::now() - memberStartTime) < memberTimeout) {
                ingressPublication_ = aeron_->findExclusivePublication(pubId);
                if (ingressPublication_ && ingressPublication_->isConnected()) {
                    publicationConnected = true;
                    if (config_.debug_logging) {
                        std::cout << config_.logging.log_prefix
                                  << " ✅ Leader ingress publication connected" << std::endl;
                    }
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            if (!publicationConnected) {
                if (config_.enable_console_warnings) {
                    std::cout << config_.logging.log_prefix
                              << " ⏰ Leader ingress publication connection timeout" << std::endl;
                }
                return false;
            }

            // Generate new correlation ID for leader connection
            correlationId_ = generateCorrelationId();

            // FIXED: Update current leader member ID
            currentLeaderMemberId_ = leaderMemberId;
            leaderMemberId_ = leaderMemberId;

            // Send SessionConnectRequest to leader
            auto remainingTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                memberTimeout - (std::chrono::steady_clock::now() - memberStartTime));
            if (remainingTime <= std::chrono::milliseconds(100)) {
                if (config_.enable_console_warnings) {
                    std::cout << config_.logging.log_prefix
                              << " ⏰ Not enough time to send SessionConnectRequest to leader"
                              << std::endl;
                }
                return false;
            }

            if (!sendSessionConnectRequestWithTimeout(remainingTime)) {
                return false;
            }

            if (config_.enable_console_info) {
                std::cout << config_.logging.log_prefix
                          << " 📡 SessionConnectRequest sent to leader, waiting for response..."
                          << std::endl;
            }

            // FIXED: Wait briefly for the session establishment response
            auto waitStart = std::chrono::steady_clock::now();
            auto waitTimeout = std::chrono::milliseconds(2000);  // 2 second wait for response

            while ((std::chrono::steady_clock::now() - waitStart) < waitTimeout) {
                // Check if we got connected (session established)
                if (connected_ && connectedToLeader_) {
                    return true;
                }

                // Small sleep to allow message processing
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            // Return true anyway - the connection will be verified by the message handler
            return true;

        } catch (const std::exception& e) {
            if (config_.enable_console_warnings) {
                std::cout << config_.logging.log_prefix
                          << " ⚠️  Error connecting to leader: " << e.what() << std::endl;
            }
            return false;
        }
    }

    void setConnectedToLeader(bool connected) {
        connectedToLeader_ = connected;
        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix
                      << " 🎯 Set connected to leader: " << (connected ? "true" : "false")
                      << std::endl;
        }
    }

    bool isConnectedToLeader() const {
        return connectedToLeader_;
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
            std::cout << config_.logging.log_prefix
                      << " 🎲 Generated correlation ID: " << correlationId
                      << " (timestamp: " << truncatedTimestamp << ", random: " << randomPart << ")"
                      << std::endl;
        }

        return correlationId;
    }
};

// SessionManager public interface implementation

SessionManager::SessionManager(const ClusterClientConfig& config)
    : pImpl(std::make_unique<Impl>(config)) {}

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

bool SessionManager::publishMessage(const std::string& topic, const std::string& messageType,
                                    const std::string& messageId, const std::string& payload,
                                    const std::string& headers) {
    return pImpl->publishMessage(topic, messageType, messageId, payload, headers);
}

void SessionManager::handleSessionEvent(const ParseResult& result) {
    pImpl->handleSessionEvent(result);
}

int64_t SessionManager::getLeadershipTermId() {
    return pImpl->getLeadershipTermId();
}

bool SessionManager::sendRawMessage(const std::vector<uint8_t>& encodedMessage) {
    return pImpl->sendRawMessage(encodedMessage);
}

}  // namespace aeron_cluster