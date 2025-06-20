#include "message_handlers.hpp"
#include "aeron_cluster/cluster_client.hpp"
#include <json/json.h>
#include <iostream>
#include <chrono>
#include <unordered_map>

namespace aeron_cluster {

// Forward declaration for ClusterClient::Impl access
class ClusterClient::Impl;

/**
 * @brief Private implementation for MessageHandler (PIMPL pattern)
 */
class MessageHandler::Impl {
public:
    explicit Impl(ClusterClient::Impl& client)
        : client_(client)
        , messageCount_(0)
        , lastMessageTime_(std::chrono::steady_clock::now())
    {
    }

    void handleMessage(const ParseResult& result) {
        messageCount_++;
        lastMessageTime_ = std::chrono::steady_clock::now();
        
        if (result.success) {
            if (result.isSessionEvent()) {
                handleSessionEventMessage(result);
            } else if (result.isAcknowledgment()) {
                handleAcknowledgmentMessage(result);
            } else if (result.isTopicMessage()) {
                handleTopicMessage(result);
            } else {
                handleUnknownMessage(result);
            }
        } else {
            handleParseError(result);
        }
        
        // Update statistics
        updateMessageStats(result);
    }

private:
    ClusterClient::Impl& client_;
    uint64_t messageCount_;
    std::chrono::steady_clock::time_point lastMessageTime_;
    
    // Message tracking for acknowledgments
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> pendingMessages_;
    std::unordered_map<std::string, std::string> messageTypes_;

    void handleSessionEventMessage(const ParseResult& result) {
        logMessage("📨 SessionEvent", result);
        
        if (isDebugLogging()) {
            std::cout << getLogPrefix() << "   Session Event Details:" << std::endl;
            std::cout << getLogPrefix() << "     Correlation ID: " << result.correlationId << std::endl;
            std::cout << getLogPrefix() << "     Session ID: " << result.sessionId << std::endl;
            std::cout << getLogPrefix() << "     Leader Member: " << result.leaderMemberId << std::endl;
            std::cout << getLogPrefix() << "     Event Code: " << result.eventCode 
                     << " (" << SBEUtils::getSessionEventCodeString(result.eventCode) << ")" << std::endl;
            
            if (!result.payload.empty()) {
                std::cout << getLogPrefix() << "     Detail: " << result.payload << std::endl;
            }
        }
        
        // SessionEvent messages are handled by SessionManager, but we can log them here
        switch (result.eventCode) {
            case SBEConstants::SESSION_EVENT_OK:
                if (isInfoLogging()) {
                    std::cout << getLogPrefix() << " ✅ Session established successfully" << std::endl;
                }
                break;
                
            case SBEConstants::SESSION_EVENT_REDIRECT:
                if (isInfoLogging()) {
                    std::cout << getLogPrefix() << " 🔄 Redirected to leader member: " 
                             << result.leaderMemberId << std::endl;
                }
                break;
                
            case SBEConstants::SESSION_EVENT_ERROR:
            case SBEConstants::SESSION_EVENT_AUTHENTICATION_REJECTED:
                if (isErrorLogging()) {
                    std::cerr << getLogPrefix() << " ❌ Session error: " 
                             << SBEUtils::getSessionEventCodeString(result.eventCode) << std::endl;
                }
                break;
                
            case SBEConstants::SESSION_EVENT_CLOSED:
                if (isInfoLogging()) {
                    std::cout << getLogPrefix() << " 👋 Session closed by cluster" << std::endl;
                }
                break;
        }
    }

    void handleAcknowledgmentMessage(const ParseResult& result) {
        logMessage("📨 Acknowledgment", result);
        
        // Parse acknowledgment details
        AckInfo ackInfo = parseAcknowledgment(result);
        
        if (isDebugLogging()) {
            std::cout << getLogPrefix() << "   Acknowledgment Details:" << std::endl;
            std::cout << getLogPrefix() << "     Message ID: " << ackInfo.messageId << std::endl;
            std::cout << getLogPrefix() << "     Status: " << ackInfo.status << std::endl;
            std::cout << getLogPrefix() << "     Success: " << (ackInfo.success ? "true" : "false") << std::endl;
            
            if (!ackInfo.error.empty()) {
                std::cout << getLogPrefix() << "     Error: " << ackInfo.error << std::endl;
            }
            
            if (ackInfo.processingTimeMs > 0) {
                std::cout << getLogPrefix() << "     Processing Time: " << ackInfo.processingTimeMs << "ms" << std::endl;
            }
        }
        
        // Check if this acknowledgment is for a message we sent
        auto pendingIt = pendingMessages_.find(ackInfo.messageId);
        if (pendingIt != pendingMessages_.end()) {
            auto sendTime = pendingIt->second;
            auto now = std::chrono::steady_clock::now();
            auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(now - sendTime);
            
            if (isInfoLogging()) {
                if (ackInfo.success) {
                    std::cout << getLogPrefix() << " ✅ Message acknowledged: " 
                             << ackInfo.messageId.substr(0, 12) << "... (RTT: " << rtt.count() << "ms)" << std::endl;
                } else {
                    std::cout << getLogPrefix() << " ❌ Message failed: " 
                             << ackInfo.messageId.substr(0, 12) << "... - " << ackInfo.error << std::endl;
                }
            }
            
            // Remove from pending list
            pendingMessages_.erase(pendingIt);
            messageTypes_.erase(ackInfo.messageId);
        } else if (isDebugLogging()) {
            std::cout << getLogPrefix() << " 👻 Acknowledgment for unknown message: " 
                     << ackInfo.messageId.substr(0, 12) << "..." << std::endl;
        }
    }

    void handleTopicMessage(const ParseResult& result) {
        logMessage("📨 TopicMessage", result);
        
        if (isDebugLogging()) {
            std::cout << getLogPrefix() << "   Topic Message Details:" << std::endl;
            std::cout << getLogPrefix() << "     Message Type: " << result.messageType << std::endl;
            std::cout << getLogPrefix() << "     Message ID: " << result.messageId << std::endl;
            std::cout << getLogPrefix() << "     Timestamp: " << SBEUtils::formatTimestamp(result.timestamp) << std::endl;
            
            if (!result.payload.empty()) {
                // Try to parse payload as JSON for better display
                std::string displayPayload = formatJsonPayload(result.payload);
                std::cout << getLogPrefix() << "     Payload: " << displayPayload.substr(0, 200);
                if (displayPayload.length() > 200) {
                    std::cout << "...";
                }
                std::cout << std::endl;
            }
            
            if (!result.headers.empty()) {
                std::cout << getLogPrefix() << "     Headers: " << result.headers << std::endl;
            }
        }
        
        // This might be an echoed message or a cluster broadcast
        if (isInfoLogging() && result.messageType != "CREATE_ORDER" && result.messageType != "UPDATE_ORDER") {
            std::cout << getLogPrefix() << " 📢 Received topic message: " << result.messageType << std::endl;
        }
    }

    void handleUnknownMessage(const ParseResult& result) {
        if (isWarningLogging()) {
            std::cout << getLogPrefix() << " ⚠️  Unknown message type: " 
                     << result.getDescription() << std::endl;
        }
        
        if (isDebugLogging()) {
            std::cout << getLogPrefix() << "   Template ID: " << result.templateId << std::endl;
            std::cout << getLogPrefix() << "   Schema ID: " << result.schemaId << std::endl;
            std::cout << getLogPrefix() << "   Version: " << result.version << std::endl;
            std::cout << getLogPrefix() << "   Block Length: " << result.blockLength << std::endl;
        }
    }

    void handleParseError(const ParseResult& result) {
        if (isErrorLogging()) {
            std::cerr << getLogPrefix() << " ❌ Failed to parse message: " << result.errorMessage << std::endl;
        }
        
        // This might indicate a protocol mismatch or corrupt message
        if (isWarningLogging()) {
            std::cout << getLogPrefix() << " 💡 Possible causes:" << std::endl;
            std::cout << getLogPrefix() << "   • SBE schema version mismatch" << std::endl;
            std::cout << getLogPrefix() << "   • Corrupt or truncated message" << std::endl;
            std::cout << getLogPrefix() << "   • Wrong message format" << std::endl;
        }
    }

    struct AckInfo {
        std::string messageId;
        std::string status;
        std::string error;
        bool success = false;
        int64_t processingTimeMs = 0;
    };

    AckInfo parseAcknowledgment(const ParseResult& result) {
        AckInfo info;
        info.messageId = result.messageId;
        
        // Try to parse the payload as JSON for detailed acknowledgment info
        try {
            Json::Reader reader;
            Json::Value root;
            
            if (reader.parse(result.payload, root)) {
                // Standard acknowledgment fields
                if (root.isMember("status")) {
                    info.status = root["status"].asString();
                    info.success = (info.status == "success" || info.status == "SUCCESS");
                } else if (root.isMember("success")) {
                    info.success = root["success"].asBool();
                    info.status = info.success ? "success" : "failed";
                }
                
                if (root.isMember("error") && !root["error"].isNull()) {
                    info.error = root["error"].asString();
                }
                
                if (root.isMember("processingTimeMs")) {
                    info.processingTimeMs = root["processingTimeMs"].asInt64();
                }
                
                // Order-specific fields
                if (root.isMember("orderId")) {
                    std::string orderId = root["orderId"].asString();
                    if (isDebugLogging()) {
                        std::cout << getLogPrefix() << "     Order ID: " << orderId << std::endl;
                    }
                }
                
                if (root.isMember("orderStatus")) {
                    std::string orderStatus = root["orderStatus"].asString();
                    if (isDebugLogging()) {
                        std::cout << getLogPrefix() << "     Order Status: " << orderStatus << std::endl;
                    }
                }
            } else {
                // Not JSON, treat as simple string status
                info.status = result.payload;
                info.success = (result.payload.find("success") != std::string::npos ||
                               result.payload.find("SUCCESS") != std::string::npos ||
                               result.payload.find("OK") != std::string::npos);
            }
        } catch (const std::exception& e) {
            // Fallback: treat payload as status string
            info.status = result.payload;
            info.success = (result.payload.find("success") != std::string::npos);
            
            if (isDebugLogging()) {
                std::cout << getLogPrefix() << "   JSON parse error: " << e.what() << std::endl;
            }
        }
        
        // Use headers field for error if status parsing didn't find one
        if (info.error.empty() && !result.headers.empty() && !info.success) {
            info.error = result.headers;
        }
        
        return info;
    }

    std::string formatJsonPayload(const std::string& payload) {
        try {
            Json::Reader reader;
            Json::Value root;
            
            if (reader.parse(payload, root)) {
                Json::StreamWriterBuilder builder;
                builder["indentation"] = "  ";
                return Json::writeString(builder, root);
            }
        } catch (const std::exception&) {
            // Not JSON or parsing failed, return as-is
        }
        
        return payload;
    }

    void updateMessageStats(const ParseResult& result) {
        // This would update internal statistics
        // For now, just track basic info
        (void)result; // Suppress unused parameter warning
    }

    void logMessage(const std::string& type, const ParseResult& result) {
        if (isDebugLogging()) {
            std::cout << getLogPrefix() << " " << type << " received" << std::endl;
            std::cout << getLogPrefix() << "   Description: " << result.getDescription() << std::endl;
        }
    }

    // Helper methods to check logging levels
    bool isDebugLogging() const {
        return getConfig().debug_logging;
    }

    bool isInfoLogging() const {
        return getConfig().enable_console_info;
    }

    bool isWarningLogging() const {
        return getConfig().enable_console_warnings;
    }

    bool isErrorLogging() const {
        return getConfig().enable_console_errors;
    }

    std::string getLogPrefix() const {
        return getConfig().logging.log_prefix;
    }

    // Access to client configuration (would need to be implemented in actual integration)
    const ClusterClientConfig& getConfig() const;
};

// MessageHandler public interface implementation

MessageHandler::MessageHandler(ClusterClient::Impl& client)
    : pImpl(std::make_unique<Impl>(client)) {
}

MessageHandler::~MessageHandler() = default;

void MessageHandler::handleMessage(const ParseResult& result) {
    pImpl->handleMessage(result);
}

// Stub implementation for getConfig - this would need proper integration
const ClusterClientConfig& MessageHandler::Impl::getConfig() const {
    // This is a placeholder - in the real implementation, this would access
    // the configuration from the ClusterClient::Impl
    static ClusterClientConfig defaultConfig;
    return defaultConfig;
}

} // namespace aeron_cluster