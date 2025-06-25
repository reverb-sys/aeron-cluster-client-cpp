#include "aeron_cluster/session_manager.hpp"
#include "aeron_cluster/sbe_messages.hpp"

#include <Aeron.h>
#include <chrono>
#include <iostream>
#include <random>
#include <regex>
#include <thread>

namespace aeron_cluster {

/**
 * @brief Private implementation for SessionManager (PIMPL pattern)
 */
class SessionManager::Impl {
public:
    explicit Impl(const ClusterClientConfig& config)
        : config_(config)
        , session_id_(-1)
        , leader_member_id_(0)
        , leadership_term_id_(-1)
        , connected_(false)
        , rng_(std::random_device{}())
        , correlation_id_(generate_correlation_id())
    {
        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix 
                      << " SessionManager created with correlation ID: " << correlation_id_ << std::endl;
        }
    }

    ~Impl() {
        disconnect();
    }

    SessionConnectionResult connect(std::shared_ptr<aeron::Aeron> aeron) {
        aeron_ = aeron;
        
        SessionConnectionResult result;
        auto start_time = std::chrono::steady_clock::now();
        
        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " Starting session connection process..." << std::endl;
        }

        // Try connecting to each cluster member
        for (int attempt = 0; attempt < config_.max_retries; ++attempt) {
            for (size_t member_id = 0; member_id < config_.cluster_endpoints.size(); ++member_id) {
                if (try_connect_to_member(static_cast<int>(member_id))) {
                    result.success = true;
                    result.session_id = session_id_;
                    result.leader_member_id = leader_member_id_;
                    result.leadership_term_id = leadership_term_id_;
                    result.connection_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_time);
                    
                    if (config_.enable_console_info) {
                        std::cout << config_.logging.log_prefix 
                                  << " Successfully connected to member " << member_id
                                  << " with session ID: " << session_id_ << std::endl;
                    }
                    return result;
                }
            }
            
            if (attempt < config_.max_retries - 1) {
                std::this_thread::sleep_for(config_.retry_delay);
            }
        }

        result.error_message = "Failed to connect to any cluster member after " + 
                              std::to_string(config_.max_retries) + " attempts";
        return result;
    }

    void disconnect() {
        if (config_.debug_logging && connected_) {
            std::cout << config_.logging.log_prefix << " Disconnecting session..." << std::endl;
        }

        ingress_publication_.reset();
        connected_ = false;
        session_id_ = -1;
    }

    bool is_connected() const {
        return connected_ && ingress_publication_ && ingress_publication_->isConnected();
    }

    std::int64_t get_session_id() const {
        return session_id_;
    }

    std::int32_t get_leader_member_id() const {
        return leader_member_id_;
    }

    std::int64_t get_leadership_term_id() const {
        return leadership_term_id_;
    }

    bool publish_message(const std::string& topic,
                        const std::string& message_type,
                        const std::string& message_id,
                        const std::string& payload,
                        const std::string& headers) {
        if (!is_connected()) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix << " Cannot publish - not connected" << std::endl;
            }
            return false;
        }

        try {
            // Encode TopicMessage using SBE
            std::vector<std::uint8_t> encoded_message = 
                SBEEncoder::encode_topic_message(topic, message_type, message_id, payload, headers);

            if (config_.debug_logging) {
                std::cout << config_.logging.log_prefix << " Publishing message:" << std::endl;
                std::cout << config_.logging.log_prefix << "   Topic: " << topic << std::endl;
                std::cout << config_.logging.log_prefix << "   Type: " << message_type << std::endl;
                std::cout << config_.logging.log_prefix << "   ID: " << message_id << std::endl;
            }

            return send_raw_message(encoded_message);

        } catch (const std::exception& e) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix 
                          << " Error publishing message: " << e.what() << std::endl;
            }
            return false;
        }
    }

    bool send_raw_message(const std::vector<std::uint8_t>& encoded_message) {
        if (!is_connected()) {
            return false;
        }

        try {
            aeron::AtomicBuffer buffer(
                reinterpret_cast<std::uint8_t*>(const_cast<std::uint8_t*>(encoded_message.data())),
                static_cast<aeron::util::index_t>(encoded_message.size())
            );
            
            std::int64_t result = ingress_publication_->offer(buffer, 0, encoded_message.size());

            if (result > 0) {
                stats_.messages_sent++;
                stats_.last_message_time = std::chrono::steady_clock::now();
                return true;
            } else {
                if (config_.debug_logging) {
                    std::cout << config_.logging.log_prefix 
                              << " Failed to send raw message: " << result << std::endl;
                }
                return false;
            }

        } catch (const std::exception& e) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix 
                          << " Error sending raw message: " << e.what() << std::endl;
            }
            return false;
        }
    }

    bool send_keepalive() {
        if (!is_connected() || leadership_term_id_ == -1 || session_id_ == -1) {
            return false;
        }

        try {
            std::vector<std::uint8_t> encoded_message = 
                SBEEncoder::encode_session_keep_alive(leadership_term_id_, session_id_);

            if (send_raw_message(encoded_message)) {
                stats_.keepalives_sent++;
                stats_.last_keepalive_time = std::chrono::steady_clock::now();
                return true;
            } else {
                stats_.keepalives_failed++;
                return false;
            }

        } catch (const std::exception& e) {
            stats_.keepalives_failed++;
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix 
                          << " Keepalive error: " << e.what() << std::endl;
            }
            return false;
        }
    }

    void handle_session_event(const ParseResult& result) {
        if (!result.is_session_event()) {
            return;
        }

        stats_.session_events_received++;

        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " Handling SessionEvent:" << std::endl;
            std::cout << config_.logging.log_prefix 
                      << "   Correlation ID: " << result.correlation_id << std::endl;
            std::cout << config_.logging.log_prefix 
                      << "   Event Code: " << result.event_code << std::endl;
        }

        switch (result.event_code) {
            case SBEConstants::SESSION_EVENT_OK:
                handle_session_ok(result);
                break;
            case SBEConstants::SESSION_EVENT_REDIRECT:
                handle_session_redirect(result);
                break;
            case SBEConstants::SESSION_EVENT_ERROR:
            case SBEConstants::SESSION_EVENT_AUTHENTICATION_REJECTED:
                handle_session_error(result);
                break;
            case SBEConstants::SESSION_EVENT_CLOSED:
                handle_session_closed(result);
                break;
            default:
                if (config_.enable_console_warnings) {
                    std::cout << config_.logging.log_prefix 
                              << " Unknown session event code: " << result.event_code << std::endl;
                }
                break;
        }

        if (session_event_callback_) {
            std::cout << config_.logging.log_prefix 
                      << " Invoking session event callback for event code: " << result.event_code << std::endl;
            session_event_callback_(result);
        }
    }

    void set_session_event_callback(SessionEventCallback callback) {
        session_event_callback_ = std::move(callback);
    }

    void set_connection_state_callback(ConnectionStateCallback callback) {
        connection_state_callback_ = std::move(callback);
    }

    SessionStats get_session_stats() const {
        return stats_;
    }

private:
    ClusterClientConfig config_;
    std::shared_ptr<aeron::Aeron> aeron_;
    std::shared_ptr<aeron::ExclusivePublication> ingress_publication_;

    std::int64_t session_id_;
    std::int32_t leader_member_id_;
    std::int64_t leadership_term_id_;
    std::atomic<bool> connected_;
    std::mt19937 rng_;
    std::int64_t correlation_id_;
    SessionStats stats_;
    SessionEventCallback session_event_callback_;
    ConnectionStateCallback connection_state_callback_;

    bool try_connect_to_member(int member_id) {
        if (member_id >= static_cast<int>(config_.cluster_endpoints.size())) {
            return false;
        }

        std::string endpoint = config_.cluster_endpoints[member_id];

        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix 
                      << " Attempting connection to member " << member_id 
                      << ": " << endpoint << std::endl;
        }

        try {
            // Create ingress publication
            std::string ingress_channel = "aeron:udp?endpoint=" + endpoint;
            std::int64_t pub_id = aeron_->addExclusivePublication(ingress_channel, config_.ingress_stream_id);

            // Wait for publication to connect
            auto timeout = std::chrono::seconds(5);
            auto start_time = std::chrono::steady_clock::now();
            
            while ((std::chrono::steady_clock::now() - start_time) < timeout) {
                ingress_publication_ = aeron_->findExclusivePublication(pub_id);
                if (ingress_publication_ && ingress_publication_->isConnected()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            if (!ingress_publication_ || !ingress_publication_->isConnected()) {
                return false;
            }

            // Send SessionConnectRequest
            if (!send_session_connect_request()) {
                return false;
            }

            leader_member_id_ = member_id;

            // Wait briefly for session establishment
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            return connected_;

        } catch (const std::exception& e) {
            if (config_.enable_console_warnings) {
                std::cout << config_.logging.log_prefix 
                          << " Error connecting to member " << member_id 
                          << ": " << e.what() << std::endl;
            }
            return false;
        }
    }

    bool send_session_connect_request() {
        try {
            std::vector<std::uint8_t> encoded_message = SBEEncoder::encode_session_connect_request(
                correlation_id_, 
                config_.egress_stream_id, 
                config_.response_channel,
                config_.protocol_semantic_version
            );

            aeron::AtomicBuffer buffer(encoded_message.data(), encoded_message.size());
            std::int64_t result = ingress_publication_->offer(buffer, 0, encoded_message.size());

            if (result > 0) {
                if (config_.debug_logging) {
                    std::cout << config_.logging.log_prefix 
                              << " SessionConnectRequest sent successfully" << std::endl;
                }
                return true;
            } else {
                if (config_.enable_console_errors) {
                    std::cerr << config_.logging.log_prefix 
                              << " Failed to send SessionConnectRequest: " << result << std::endl;
                }
                return false;
            }

        } catch (const std::exception& e) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix 
                          << " Error sending SessionConnectRequest: " << e.what() << std::endl;
            }
            return false;
        }
    }

    void handle_session_ok(const ParseResult& result) {
        session_id_ = result.session_id;
        connected_ = true;
        leadership_term_id_ = result.leadership_term_id;
        leader_member_id_ = result.leader_member_id;

        stats_.session_start_time = std::chrono::steady_clock::now();

        if (config_.enable_console_info) {
            // std::cout << config_.logging.log_prefix << " Session established successfully!" << std::endl;
            std::cout << config_.logging.log_prefix << "   Session ID: " << session_id_ << std::endl;
            std::cout << config_.logging.log_prefix << "   Leadership Term: " << leadership_term_id_ << std::endl;
            std::cout << config_.logging.log_prefix << "   Leader Member: " << leader_member_id_ << std::endl;
        }

        if (connection_state_callback_) {
            connection_state_callback_(true);
        }
    }

    void handle_session_redirect(const ParseResult& result) {
        stats_.redirects_received++;
        
        if (config_.enable_console_info) {
            std::cout << config_.logging.log_prefix 
                      << " Redirected to leader member: " << result.leader_member_id << std::endl;
        }

        // Handle redirect logic here
        leader_member_id_ = result.leader_member_id;
    }

    void handle_session_error(const ParseResult& result) {
        if (config_.enable_console_errors) {
            std::cerr << config_.logging.log_prefix 
                      << " Session error: " << SBEUtils::get_session_event_code_string(result.event_code) << std::endl;
        }

        connected_ = false;
        
        if (connection_state_callback_) {
            connection_state_callback_(false);
        }
    }

    void handle_session_closed(const ParseResult& result) {
        if (config_.enable_console_info) {
            std::cout << config_.logging.log_prefix << " Session closed by cluster" << std::endl;
            std::cout << config_.logging.log_prefix << " Error ? " << result.error_message << std::endl;
        }

        connected_ = false;
        session_id_ = -1;
        
        if (connection_state_callback_) {
            connection_state_callback_(false);
        }
    }

    std::int64_t generate_correlation_id() {
        auto now = std::chrono::high_resolution_clock::now();
        std::int64_t timestamp = now.time_since_epoch().count();
        
        // Use only lower 47 bits to ensure positive value
        return timestamp & 0x7FFFFFFFFFFFFFFFLL;
    }
};

// SessionManager public interface implementation
SessionManager::SessionManager(const ClusterClientConfig& config)
    : pImpl_(std::make_unique<Impl>(config)) {
}

SessionManager::~SessionManager() = default;

SessionConnectionResult SessionManager::connect(std::shared_ptr<aeron::Aeron> aeron) {
    return pImpl_->connect(aeron);
}

std::future<SessionConnectionResult> SessionManager::connect_async(std::shared_ptr<aeron::Aeron> aeron) {
    return std::async(std::launch::async, [this, aeron]() {
        return pImpl_->connect(aeron);
    });
}

SessionConnectionResult SessionManager::connect_with_timeout(
    std::shared_ptr<aeron::Aeron> aeron,
    std::chrono::milliseconds timeout) {
    
    auto future = connect_async(aeron);
    
    if (future.wait_for(timeout) == std::future_status::timeout) {
        SessionConnectionResult result;
        result.error_message = "Connection timeout after " + std::to_string(timeout.count()) + "ms";
        return result;
    }
    
    return future.get();
}

void SessionManager::disconnect() {
    pImpl_->disconnect();
}

bool SessionManager::is_connected() const {
    return pImpl_->is_connected();
}

std::int64_t SessionManager::get_session_id() const {
    return pImpl_->get_session_id();
}

std::int32_t SessionManager::get_leader_member_id() const {
    return pImpl_->get_leader_member_id();
}

std::int64_t SessionManager::get_leadership_term_id() const {
    return pImpl_->get_leadership_term_id();
}

bool SessionManager::publish_message(
    const std::string& topic,
    const std::string& message_type,
    const std::string& message_id,
    const std::string& payload,
    const std::string& headers) {
    return pImpl_->publish_message(topic, message_type, message_id, payload, headers);
}

bool SessionManager::send_raw_message(const std::vector<std::uint8_t>& encoded_message) {
    return pImpl_->send_raw_message(encoded_message);
}

bool SessionManager::send_keepalive() {
    return pImpl_->send_keepalive();
}

void SessionManager::handle_session_event(const ParseResult& result) {
    pImpl_->handle_session_event(result);
}

void SessionManager::set_session_event_callback(SessionEventCallback callback) {
    pImpl_->set_session_event_callback(std::move(callback));
}

void SessionManager::set_connection_state_callback(ConnectionStateCallback callback) {
    pImpl_->set_connection_state_callback(std::move(callback));
}



SessionManager::SessionStats SessionManager::get_session_stats() const {
    return pImpl_->get_session_stats();
}

std::future<SessionConnectionResult> SessionManager::reconnect_async() {
    // Implementation would disconnect and reconnect
    return std::async(std::launch::async, []() {
        SessionConnectionResult result;
        result.error_message = "Reconnect not implemented yet";
        return result;
    });
}

// SessionUtils namespace implementation
namespace SessionUtils {

std::string calculate_leader_endpoint(std::int32_t member_id, const std::string& base_ip) {
    if (member_id < 0) {
        return "";
    }
    
    int leader_port = 9002 + (member_id * 100);
    return base_ip + ":" + std::to_string(leader_port);
}

std::string extract_base_ip(const std::string& endpoint) {
    std::size_t colon_pos = endpoint.find(':');
    if (colon_pos != std::string::npos) {
        return endpoint.substr(0, colon_pos);
    }
    return endpoint;
}

bool is_valid_endpoint(const std::string& endpoint) {
    std::regex pattern(R"(^[a-zA-Z0-9\.\-]+:\d+$)");
    return std::regex_match(endpoint, pattern);
}

std::pair<std::string, int> parse_endpoint(const std::string& endpoint) {
    std::size_t colon_pos = endpoint.find(':');
    if (colon_pos == std::string::npos) {
        return {"", 0};
    }
    
    std::string host = endpoint.substr(0, colon_pos);
    std::string port_str = endpoint.substr(colon_pos + 1);
    
    try {
        int port = std::stoi(port_str);
        return {host, port};
    } catch (const std::exception&) {
        return {"", 0};
    }
}

std::int64_t generate_correlation_id() {
    auto now = std::chrono::high_resolution_clock::now();
    std::int64_t timestamp = now.time_since_epoch().count();
    return timestamp & 0x7FFFFFFFFFFFFFFFLL;
}

} // namespace SessionUtils

} // namespace aeron_cluster