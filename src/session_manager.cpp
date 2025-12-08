#include "aeron_cluster/session_manager.hpp"

#include <Aeron.h>
#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <mutex>
#include <random>
#include <regex>
#include <set>
#include <thread>

#include "aeron_cluster/sbe_messages.hpp"
#include "model/Acknowledgment.h"
#include "model/MessageHeader.h"
#include "model/TopicMessage.h"
#include "model/VarStringEncoding.h"

using sbe::Acknowledgment;
using sbe::MessageHeader;
using sbe::TopicMessage;

namespace {

enum class OfferFailureCode {
    None = 0,
    NotConnected,
    BackPressured,
    AdminAction,
    PublicationClosed,
    MaxPositionExceeded,
    DriverError,
    Unknown
};

constexpr std::int64_t kAeronPublicationErrorCode = -6;

}  // namespace

namespace aeron_cluster {

/**
 * @brief Private implementation for SessionManager (PIMPL pattern)
 */
class SessionManager::Impl {
   public:
    explicit Impl(const ClusterClientConfig& config)
        : config_(config),
          session_id_(-1),
          leader_member_id_(0),
          leadership_term_id_(-1),
          connected_(false),
          redirected_to_member_id_(-1),
          rng_(std::random_device{}()),
          correlation_id_(generate_correlation_id()),
          stats_(),
          last_offer_error_code_(0) {
        create_session_message_header_buffer();
        create_keepalive_buffer();
        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix
                      << " SessionManager created with correlation ID: " << correlation_id_
                      << std::endl;
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
            std::cout << config_.logging.log_prefix << " Starting session connection process..."
                      << std::endl;
        }

        // Track which members we've already tried to avoid loops
        std::set<int> tried_members;

        // Try connecting to each cluster member
        for (int attempt = 0; attempt < config_.max_retries; ++attempt) {
            // Check if we got redirected to a specific member from previous attempt
            int redirect_target = redirected_to_member_id_.load();
            if (redirect_target >= 0 && 
                redirect_target < static_cast<int>(config_.cluster_endpoints.size()) &&
                tried_members.find(redirect_target) == tried_members.end()) {
                
                tried_members.insert(redirect_target);
                // Don't clear redirect flag yet - wait until connection succeeds
                
                if (config_.enable_console_info || config_.debug_logging) {
                    std::cout << config_.logging.log_prefix
                              << " Attempting connection to redirected leader member: "
                              << redirect_target << std::endl;
                }
                
                if (try_connect_to_member(redirect_target)) {
                    // Only clear redirect flag on successful connection
                    redirected_to_member_id_ = -1;
                    
                    result.success = true;
                    result.session_id = session_id_;
                    result.leader_member_id = leader_member_id_;
                    result.leadership_term_id = leadership_term_id_;
                    result.connection_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_time);

                    if (config_.enable_console_info) {
                        std::cout << config_.logging.log_prefix
                                  << " Successfully connected to redirected member " << redirect_target
                                  << " with session ID: " << session_id_ << std::endl;
                    }

                    // Start a keepalive thread that sends keepalive messages
                    std::thread keepalive_thread_ = std::thread([this]() {
                        while (is_connected()) {
                            if (!send_keepalive()) {
                                if (config_.enable_console_errors) {
                                    std::cerr << config_.logging.log_prefix
                                              << " Failed to send keepalive message" << std::endl;
                                }
                            }
                            std::this_thread::sleep_for(config_.keepalive_interval);
                        }
                    });
                    keepalive_thread_.detach();  // Detach to run independently

                    return result;
                }

                // If redirected target failed, check if we got a NEW redirect while trying
                int new_redirect = redirected_to_member_id_.load();
                if (new_redirect >= 0 && new_redirect != redirect_target) {
                    // Got a NEW redirect to a different member - keep it and remove old target
                    tried_members.erase(redirect_target);
                    // Keep new redirect flag set for next iteration
                    if (config_.debug_logging) {
                        std::cout << config_.logging.log_prefix
                                  << " Got new redirect to member " << new_redirect
                                  << " while trying member " << redirect_target << std::endl;
                    }
                } else if (new_redirect < 0) {
                    // No redirect anymore (shouldn't happen, but handle it)
                    // Remove from tried_members to allow retry
                    tried_members.erase(redirect_target);
                    redirected_to_member_id_ = -1;
                } else {
                    // Same redirect target - keep it and allow retry
                    tried_members.erase(redirect_target);
                    // Keep redirect flag set for next iteration
                    if (config_.debug_logging) {
                        std::cout << config_.logging.log_prefix
                                  << " Connection to redirected member " << redirect_target
                                  << " failed, will retry" << std::endl;
                    }
                }

                // Skip trying other members in this iteration
                // Wait a bit before retrying to avoid hammering the network
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            
            // Check if we have a pending redirect - if so, skip normal member loop
            int pending_redirect = redirected_to_member_id_.load();
            if (pending_redirect >= 0) {
                // We have a redirect that we haven't tried yet (it's in tried_members or out of range)
                // Wait a bit and continue to next iteration to handle redirect
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            
            // Try all cluster members (skip ones we've already tried)
            for (size_t member_id = 0; member_id < config_.cluster_endpoints.size(); ++member_id) {
                if (tried_members.find(static_cast<int>(member_id)) != tried_members.end()) {
                    continue;  // Skip already tried members
                }
                
                tried_members.insert(static_cast<int>(member_id));
                
                // Reset redirect flag before attempting connection
                redirected_to_member_id_ = -1;
                
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

                    // Start a keepalive thread that sends keepalive messages
                    std::thread keepalive_thread_ = std::thread([this]() {
                        while (is_connected()) {
                            if (!send_keepalive()) {
                                if (config_.enable_console_errors) {
                                    std::cerr << config_.logging.log_prefix
                                              << " Failed to send keepalive message" << std::endl;
                                }
                            }
                            std::this_thread::sleep_for(config_.keepalive_interval);
                        }
                    });
                    keepalive_thread_.detach();  // Detach to run independently

                    return result;
                }
                
                // Check if we got redirected while trying this member
                redirect_target = redirected_to_member_id_.load();
                if (redirect_target >= 0 && redirect_target != static_cast<int>(member_id)) {
                    // Got redirected, break inner loop and try redirected member in next iteration
                    if (config_.debug_logging) {
                        std::cout << config_.logging.log_prefix
                                  << " Redirect received to member " << redirect_target
                                  << ", will retry with redirected member" << std::endl;
                    }
                    break;
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
        // CRITICAL FIX: Always try to send close request if we have session info
        // Even if connected_ is false, we might still have valid session_id and can notify server
        bool should_send_close = (session_id_ > 0 && leadership_term_id_ > 0);
        
        if (config_.debug_logging) {
            if (connected_) {
                std::cout << config_.logging.log_prefix << " Disconnecting session (session_id=" 
                          << session_id_ << ", leadership_term=" << leadership_term_id_ << ")..." << std::endl;
            } else if (should_send_close) {
                std::cout << config_.logging.log_prefix << " Disconnecting session (not connected but have session info, session_id=" 
                          << session_id_ << ", leadership_term=" << leadership_term_id_ << ")..." << std::endl;
            } else {
                std::cout << config_.logging.log_prefix << " Disconnecting session (no session info available)..." << std::endl;
            }
        }

        // Send SessionCloseRequest to notify cluster of CLIENT_ACTION disconnection
        // This allows the cluster to properly detect and clean up the session
        // CRITICAL FIX: Try to send even if publication connection check fails
        if (should_send_close && ingress_publication_) {
            bool publication_connected = false;
            try {
                publication_connected = ingress_publication_->isConnected();
            } catch (const std::exception& e) {
                if (config_.enable_console_errors) {
                    std::cerr << config_.logging.log_prefix 
                              << " Error checking publication connection: " << e.what() << std::endl;
                }
                // Continue anyway - try to send close request
            }
            
            if (publication_connected || connected_) {
                try {
                    if (config_.debug_logging) {
                        std::cout << config_.logging.log_prefix 
                                  << " Sending SessionCloseRequest (leadership_term_id=" 
                                  << leadership_term_id_ << ", session_id=" << session_id_ << ")"
                                  << std::endl;
                    }
                    
                    std::vector<std::uint8_t> close_request = 
                        SBEEncoder::encode_session_close_request(leadership_term_id_, session_id_);
                    
                    // Try to send with retries (similar to how keepalive is sent)
                    int attempts = 3;
                    while (attempts > 0) {
                        if (send_raw_message(close_request)) {
                            if (config_.debug_logging) {
                                std::cout << config_.logging.log_prefix 
                                          << " SessionCloseRequest sent successfully" << std::endl;
                            }
                            break;
                        }
                        attempts--;
                        if (attempts > 0) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        }
                    }
                    
                    if (attempts == 0) {
                        if (config_.enable_console_errors || config_.debug_logging) {
                            std::cerr << config_.logging.log_prefix 
                                      << " WARNING: Failed to send SessionCloseRequest after 3 attempts "
                                      << "(session_id=" << session_id_ << ", leadership_term=" << leadership_term_id_ 
                                      << ", publication_connected=" << publication_connected << ")"
                                      << std::endl;
                        }
                    }
                } catch (const std::exception& e) {
                    if (config_.enable_console_errors) {
                        std::cerr << config_.logging.log_prefix 
                                  << " ERROR sending SessionCloseRequest: " << e.what() << std::endl;
                    }
                }
            }
        } else if (should_send_close && config_.enable_console_errors) {
            std::cerr << config_.logging.log_prefix 
                      << " WARNING: Cannot send SessionCloseRequest - no publication or publication not connected"
                      << " (session_id=" << session_id_ << ", leadership_term=" << leadership_term_id_ << ")"
                      << std::endl;
        }

        // Reset connection state AFTER attempting to send close request
        ingress_publication_.reset();
        connected_ = false;
        session_id_ = -1;
    }

    bool is_connected() const {
        try {
            return connected_ && ingress_publication_ && ingress_publication_->isConnected() &&
                   session_id_ > 0 && leadership_term_id_ > 0;
        } catch (const std::exception& e) {
            // If checking connection throws an exception, consider it disconnected
            return false;
        }
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

    bool publish_message(const std::string& topic, const std::string& message_type,
                         const std::string& message_id, const std::string& payload,
                         const std::string& headers) {
        if (!is_connected()) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix << " Cannot publish - not connected"
                          << std::endl;
            }
            return false;
        }

        try {
            // Create business message (TopicMessage)
            std::vector<std::uint8_t> business_message =
                create_topic_message(topic, message_type, message_id, payload, headers);

            // Update session header with current values
            // update_session_header();

            if (config_.debug_logging) {
                std::cout << config_.logging.log_prefix
                          << " Publishing with single buffer (Go-compatible):" << std::endl;
                std::cout << config_.logging.log_prefix
                          << "   Session header size: " << session_header_buffer_.size()
                          << std::endl;
                std::cout << config_.logging.log_prefix
                          << "   Business message size: " << business_message.size() << std::endl;
                std::cout << config_.logging.log_prefix
                          << "   Leadership term: " << leadership_term_id_ << std::endl;
                std::cout << config_.logging.log_prefix << "   Session ID: " << session_id_
                          << std::endl;
            }

            // Create single combined buffer with PERFECT alignment (no extra bytes)
            return send_combined_message(business_message);

        } catch (const std::exception& e) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix
                          << " Error publishing business message: " << e.what() << std::endl;
            }
            return false;
        }
    }

    bool send_raw_message(const std::vector<std::uint8_t>& encoded_message) {
        if (!is_connected()) {
            return false;
        }

        // Debug: Print hex dump of what we're sending
        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " Sending message ("
                      << encoded_message.size() << " bytes):" << std::endl;
            SBEUtils::print_hex_dump(encoded_message.data(), encoded_message.size(),
                                     config_.logging.log_prefix + "  ", 64);
        }

        try {
            aeron::AtomicBuffer buffer(
                reinterpret_cast<std::uint8_t*>(const_cast<std::uint8_t*>(encoded_message.data())),
                static_cast<aeron::util::index_t>(encoded_message.size()));

            const int max_attempts = effective_publish_retry_attempts();
            for (int attempt = 0; attempt < max_attempts; ++attempt) {
                std::int64_t result =
                    ingress_publication_->offer(buffer, 0, encoded_message.size());

                if (result > 0) {
                    clear_last_publication_error();
                    stats_.messages_sent++;
                    stats_.last_message_time = std::chrono::steady_clock::now();

                    if (config_.debug_logging) {
                        std::cout << config_.logging.log_prefix
                                  << " Message sent successfully, position: " << result << std::endl;
                    }

                    apply_publish_rate_limit();
                    return true;
                }

                OfferFailureCode failure_code = handle_offer_failure("Raw message offer", result);
                if (!is_transient_failure(failure_code) || attempt == max_attempts - 1) {
                    return false;
                }

                wait_before_publish_retry(attempt);
            }
            return false;

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
            // Use pre-built buffer like Go version (matches Go SendKeepAlive implementation)
            // Retry up to 3 times with idle strategy between attempts (like Go)
            for (int i = 0; i < 3; i++) {
                aeron::AtomicBuffer buffer(
                    reinterpret_cast<std::uint8_t*>(keepalive_buffer_.data()),
                    static_cast<aeron::util::index_t>(keepalive_buffer_.size()));

                std::int64_t result = ingress_publication_->offer(buffer, 0, keepalive_buffer_.size());

                if (result > 0) {
                    clear_last_publication_error();
                    stats_.keepalives_sent++;
                    stats_.last_keepalive_time = std::chrono::steady_clock::now();
                    return true;
                }

                OfferFailureCode failure_code = handle_offer_failure("Keepalive offer", result);
                if (!is_transient_failure(failure_code)) {
                    break;
                }

                if (i < 2) {  // Don't sleep on last attempt
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }

            // All retries failed
            stats_.keepalives_failed++;
            if (config_.debug_logging) {
                std::cout << config_.logging.log_prefix
                          << " Keepalive send failed after 3 attempts" << std::endl;
            }
            return false;

        } catch (const std::exception& e) {
            stats_.keepalives_failed++;
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix << " Keepalive error: " << e.what()
                          << std::endl;
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
            std::cout << config_.logging.log_prefix << "   Event Code: " << result.event_code
                      << std::endl;
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
            if (config_.debug_logging) {
                std::cout << config_.logging.log_prefix
                          << " Invoking session event callback for event code: " << result.event_code
                          << std::endl;
            }
            try {
                session_event_callback_(result);
            } catch (const std::exception& e) {
                if (config_.enable_console_errors) {
                    std::cerr << config_.logging.log_prefix
                              << " Session event callback error: " << e.what() << std::endl;
                }
            } catch (...) {
                if (config_.enable_console_errors) {
                    std::cerr << config_.logging.log_prefix
                              << " Session event callback threw unknown exception" << std::endl;
                }
            }
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

    std::int64_t get_last_publication_error_code() const {
        return last_offer_error_code_.load();
    }

    std::string get_last_publication_error_message() const {
        std::lock_guard<std::mutex> lock(last_offer_error_mutex_);
        return last_offer_error_message_;
    }

   private:
    void clear_last_publication_error() {
        last_offer_error_code_.store(0);
        std::lock_guard<std::mutex> lock(last_offer_error_mutex_);
        last_offer_error_message_.clear();
    }

    void update_last_publication_error(std::int64_t code, const std::string& message) {
        last_offer_error_code_.store(code);
        std::lock_guard<std::mutex> lock(last_offer_error_mutex_);
        last_offer_error_message_ = message;
    }

    OfferFailureCode classify_offer_failure(std::int64_t result) const {
        if (result > 0) {
            return OfferFailureCode::None;
        }

        switch (result) {
            case aeron::NOT_CONNECTED:
                return OfferFailureCode::NotConnected;
            case aeron::BACK_PRESSURED:
                return OfferFailureCode::BackPressured;
            case aeron::ADMIN_ACTION:
                return OfferFailureCode::AdminAction;
            case aeron::PUBLICATION_CLOSED:
                return OfferFailureCode::PublicationClosed;
            case aeron::MAX_POSITION_EXCEEDED:
                return OfferFailureCode::MaxPositionExceeded;
            case kAeronPublicationErrorCode:
                return OfferFailureCode::DriverError;
            default:
                return OfferFailureCode::Unknown;
        }
    }

    std::string describe_offer_failure(std::int64_t result) const {
        switch (result) {
            case aeron::NOT_CONNECTED:
                return "AERON_PUBLICATION_NOT_CONNECTED (-1): ingress publication is not connected";
            case aeron::BACK_PRESSURED:
                return "AERON_PUBLICATION_BACK_PRESSURED (-2): subscribers are applying back pressure";
            case aeron::ADMIN_ACTION:
                return "AERON_PUBLICATION_ADMIN_ACTION (-3): driver administration is in progress";
            case aeron::PUBLICATION_CLOSED:
                return "AERON_PUBLICATION_CLOSED (-4): publication is closed and must be recreated";
            case aeron::MAX_POSITION_EXCEEDED:
                return "AERON_PUBLICATION_MAX_POSITION_EXCEEDED (-5): publication reached its maximum position";
            case kAeronPublicationErrorCode:
                return "AERON_PUBLICATION_ERROR (-6): driver reported a publication error";
            case 0:
                return "Offer returned 0: publication is not ready for the message yet";
            default:
                return "Unknown Aeron publication status (" + std::to_string(result) + ")";
        }
    }

    bool is_transient_failure(OfferFailureCode code) const {
        return code == OfferFailureCode::BackPressured || code == OfferFailureCode::AdminAction;
    }

    bool is_connection_loss_failure(OfferFailureCode code) const {
        switch (code) {
            case OfferFailureCode::NotConnected:
            case OfferFailureCode::PublicationClosed:
            case OfferFailureCode::MaxPositionExceeded:
            case OfferFailureCode::DriverError:
                return true;
            default:
                return false;
        }
    }

    void handle_connection_loss(const std::string& reason) {
        bool was_connected = connected_.exchange(false);
        if (!was_connected) {
            return;
        }

        if (config_.enable_console_errors || config_.enable_console_warnings) {
            std::ostream& stream = config_.enable_console_errors ? std::cerr : std::cout;
            stream << config_.logging.log_prefix << " Connection lost: " << reason << std::endl;
        }

        if (connection_state_callback_) {
            connection_state_callback_(false);
        }
    }

    OfferFailureCode handle_offer_failure(const std::string& context, std::int64_t result) {
        OfferFailureCode code = classify_offer_failure(result);

        if (code == OfferFailureCode::None) {
            clear_last_publication_error();
            return code;
        }

        std::string description = describe_offer_failure(result);
        update_last_publication_error(result, description);

        if (config_.enable_console_errors || config_.enable_console_warnings || config_.debug_logging) {
            std::ostream& stream = config_.enable_console_errors ? std::cerr : std::cout;
            stream << config_.logging.log_prefix << context << " failed: " << description << std::endl;
        }

        if (is_connection_loss_failure(code)) {
            handle_connection_loss(description);
        }

        return code;
    }

    int effective_publish_retry_attempts() const {
        return std::max(1, config_.publish_max_retry_attempts);
    }

    void wait_before_publish_retry(int attempt) const {
        if (config_.publish_retry_idle_base.count() <= 0 &&
            config_.publish_retry_idle_max.count() <= 0) {
            std::this_thread::yield();
            return;
        }

        auto base = config_.publish_retry_idle_base.count() > 0
                        ? config_.publish_retry_idle_base
                        : std::chrono::microseconds(100);
        auto delay = base * (attempt + 1);
        if (config_.publish_retry_idle_max.count() > 0 &&
            delay > config_.publish_retry_idle_max) {
            delay = config_.publish_retry_idle_max;
        }

        if (delay.count() > 0) {
            std::this_thread::sleep_for(delay);
        } else {
            std::this_thread::yield();
        }
    }

    void apply_publish_rate_limit() const {
        if (config_.publish_rate_limit_delay.count() > 0) {
            std::this_thread::sleep_for(config_.publish_rate_limit_delay);
        }
    }

    ClusterClientConfig config_;
    std::shared_ptr<aeron::Aeron> aeron_;
    std::shared_ptr<aeron::ExclusivePublication> ingress_publication_;

    std::int64_t session_id_;
    std::int32_t leader_member_id_;
    std::int64_t leadership_term_id_;
    std::atomic<bool> connected_;
    std::atomic<int> redirected_to_member_id_;  // Track redirects (-1 means no redirect)
    std::mt19937 rng_;
    std::int64_t correlation_id_;
    SessionStats stats_;
    std::atomic<std::int64_t> last_offer_error_code_;
    std::string last_offer_error_message_;
    mutable std::mutex last_offer_error_mutex_;
    SessionEventCallback session_event_callback_;
    ConnectionStateCallback connection_state_callback_;
    std::vector<std::uint8_t> session_header_buffer_;  // Pre-built like Go client
    std::vector<std::uint8_t> keepalive_buffer_;  // Pre-built keepalive buffer like Go keepAliveBuffer
    mutable std::mutex send_mutex_;

    // ---- Connection management methods ----

    // Resolve endpoint string for a given member id.
    // Supports two formats in config_.cluster_endpoints:
    // 1) Indexed list: ["hostA:9002","hostB:9002","hostC:9002"] (index == memberId)
    // 2) Explicit map entries: ["0=hostA:9002","2=hostC:9002", ...] (id=endpoint)
    std::string resolve_endpoint_for_member(int member_id) {
        // First pass: check for explicit id mapping entries
        for (const auto &entry : config_.cluster_endpoints) {
            auto pos = entry.find('=');
            if (pos != std::string::npos) {
                // Format id=endpoint
                const std::string id_str = entry.substr(0, pos);
                const std::string endpoint_str = entry.substr(pos + 1);
                try {
                    int id = std::stoi(id_str);
                    if (id == member_id) {
                        if (config_.enable_console_info || config_.debug_logging) {
                            std::cout << config_.logging.log_prefix
                                      << " Resolved endpoint via explicit mapping for member "
                                      << member_id << ": " << endpoint_str << std::endl;
                        }
                        return endpoint_str;
                    }
                } catch (...) {
                    // Ignore malformed mapping and continue
                }
            }
        }

        // Fallback: index-based resolution
        if (member_id >= 0 && member_id < static_cast<int>(config_.cluster_endpoints.size())) {
            const std::string &endpoint = config_.cluster_endpoints[member_id];
            // If this entry itself is a mapping, skip (not matching id)
            if (endpoint.find('=') == std::string::npos) {
                return endpoint;
            }
        }
        return std::string();
    }

    bool try_connect_to_member(int member_id) {
        std::string endpoint = resolve_endpoint_for_member(member_id);
        if (endpoint.empty()) {
            if (config_.enable_console_warnings) {
                std::cout << config_.logging.log_prefix
                          << " Unable to resolve endpoint for member " << member_id << std::endl;
            }
            return false;
        }

        if (config_.enable_console_info || config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " Attempting connection to member "
                      << member_id << ": " << endpoint << std::endl;
        }

        try {
            // Ensure any previous publication is closed before switching members
            if (ingress_publication_) {
                try {
                    ingress_publication_->close();
                } catch (...) {
                    // ignore close errors
                }
                ingress_publication_.reset();
            }
            // Create ingress publication
            std::string ingress_channel = "aeron:udp?endpoint=" + endpoint;
            std::int64_t pub_id =
                aeron_->addExclusivePublication(ingress_channel, config_.ingress_stream_id);

            // Wait for publication to connect (longer timeout for ELB endpoints)
            auto timeout = std::chrono::seconds(10);
            auto start_time = std::chrono::steady_clock::now();

            while ((std::chrono::steady_clock::now() - start_time) < timeout) {
                ingress_publication_ = aeron_->findExclusivePublication(pub_id);
                if (ingress_publication_ && ingress_publication_->isConnected()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            if (!ingress_publication_ || !ingress_publication_->isConnected()) {
                if (config_.enable_console_warnings) {
                    std::cout << config_.logging.log_prefix
                              << " Ingress publication failed to connect to member "
                              << member_id << " (" << endpoint << ") after "
                              << std::chrono::duration_cast<std::chrono::seconds>(timeout).count()
                              << " seconds" << std::endl;
                }
                return false;
            }

            // Send SessionConnectRequest (with retries on back pressure)
            int connect_offer_retries = 20;
            while (connect_offer_retries-- > 0) {
                if (send_session_connect_request()) {
                    break;
                }
                // If failed due to back pressure, brief backoff and retry
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (connect_offer_retries < 0) {
                return false;
            }

            leader_member_id_ = member_id;

            // Wait for session establishment with periodic checks
            // The egress subscription polling thread needs time to process the response
            auto session_timeout = std::chrono::seconds(10);
            auto session_start = std::chrono::steady_clock::now();
            
            while ((std::chrono::steady_clock::now() - session_start) < session_timeout) {
                if (connected_) {
                    return true;  // Successfully connected
                }
                
                // Check if we got redirected - if so, return false to let connect() handle the redirect
                int redirect_target = redirected_to_member_id_.load();
                if (redirect_target >= 0 && redirect_target != member_id) {
                    if (config_.enable_console_info || config_.debug_logging) {
                        std::cout << config_.logging.log_prefix 
                                  << " Got redirect to member " << redirect_target
                                  << " while connecting to member " << member_id << std::endl;
                    }
                    return false;  // Return false so connect() can handle the redirect
                }
                
                // Check every 100ms to allow polling thread to process messages
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
            // Timeout - connection not established
            if (config_.enable_console_warnings) {
                std::cout << config_.logging.log_prefix 
                          << " Session connection timeout after " 
                          << std::chrono::duration_cast<std::chrono::seconds>(session_timeout).count()
                          << " seconds" << std::endl;
            }
            return connected_;

        } catch (const std::exception& e) {
            if (config_.enable_console_warnings) {
                std::cout << config_.logging.log_prefix << " Error connecting to member "
                          << member_id << ": " << e.what() << std::endl;
            }
            return false;
        }
    }

    bool send_session_connect_request() {
        try {
            std::vector<std::uint8_t> encoded_message = SBEEncoder::encode_session_connect_request(
                correlation_id_, config_.egress_stream_id, config_.response_channel,
                config_.protocol_semantic_version);

            aeron::AtomicBuffer buffer(encoded_message.data(), encoded_message.size());
            std::int64_t result = ingress_publication_->offer(buffer, 0, encoded_message.size());

            if (result > 0) {
                clear_last_publication_error();
                if (config_.debug_logging) {
                    std::cout << config_.logging.log_prefix
                              << " SessionConnectRequest sent successfully" << std::endl;
                }
                return true;
            }

            handle_offer_failure("SessionConnectRequest offer", result);
            return false;

        } catch (const std::exception& e) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix
                          << " Error sending SessionConnectRequest: " << e.what() << std::endl;
            }
            return false;
        }
    }
    // ---- End of connection management methods ----

    // ---- Session handling methods ----
    void create_session_message_header_buffer() {
        // Create exactly like Go client: 32 bytes total
        session_header_buffer_.resize(32);

        size_t offset = 0;

        // SBE Header (8 bytes) - FIXED: block_length should be 24, not variable
        write_uint16_le(session_header_buffer_, offset,
                        24);  // FIXED: Always 24 (SessionMessageHeader.BLOCK_LENGTH)
        offset += 2;
        write_uint16_le(session_header_buffer_, offset,
                        1);  // template ID = 1 (SessionMessageHeader)
        offset += 2;
        write_uint16_le(session_header_buffer_, offset, 111);  // schema ID = 111 (cluster)
        offset += 2;
        write_uint16_le(session_header_buffer_, offset, 8);  // schema version = 8
        offset += 2;

        // Fixed Block (24 bytes) - will be updated in update_session_header()
        write_int64_le(session_header_buffer_, offset, 0);  // leadership_term_id (updated later)
        offset += 8;
        write_int64_le(session_header_buffer_, offset, 0);  // cluster_session_id (updated later)
        offset += 8;
        write_int64_le(session_header_buffer_, offset, 0);  // timestamp = 0
        offset += 8;

        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix
                      << " Created session header buffer (32 bytes) with FIXED block_length=24"
                      << std::endl;
        }
    }

    void create_keepalive_buffer() {
        // Create exactly like Go client: 8 bytes header + 16 bytes data = 24 bytes total
        keepalive_buffer_.resize(24);

        size_t offset = 0;

        // SBE Header (8 bytes) - matching Go MakeClusterMessageBuffer
        write_uint16_le(keepalive_buffer_, offset, 16);  // block_length = 16
        offset += 2;
        write_uint16_le(keepalive_buffer_, offset, SBEConstants::SESSION_KEEPALIVE_TEMPLATE_ID);  // template ID = 5
        offset += 2;
        write_uint16_le(keepalive_buffer_, offset, SBEConstants::CLUSTER_SCHEMA_ID);  // schema ID = 111
        offset += 2;
        write_uint16_le(keepalive_buffer_, offset, SBEConstants::CLUSTER_SCHEMA_VERSION);  // schema version = 8
        offset += 2;

        // Fixed Block (16 bytes) - will be updated in update_keepalive_buffer()
        write_int64_le(keepalive_buffer_, offset, 0);  // leadership_term_id (updated later)
        offset += 8;
        write_int64_le(keepalive_buffer_, offset, 0);  // cluster_session_id (updated later)
        offset += 8;

        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix
                      << " Created keepalive buffer (24 bytes) with block_length=16"
                      << std::endl;
        }
    }

    void update_keepalive_buffer() {
        // Update the pre-built buffer with current session values (like Go does)
        size_t offset = 8;  // Skip SBE header, go to fixed block

        write_int64_le(keepalive_buffer_, offset, leadership_term_id_);
        offset += 8;
        write_int64_le(keepalive_buffer_, offset, session_id_);

        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " Updated keepalive buffer with:" << std::endl;
            std::cout << config_.logging.log_prefix << "   Leadership term: " << leadership_term_id_
                      << std::endl;
            std::cout << config_.logging.log_prefix << "   Session ID: " << session_id_
                      << std::endl;
        }
    }

    void update_session_header() {
        // Update the pre-built buffer with current session values
        size_t offset = 8;  // Skip SBE header, go to fixed block

        std::cout << config_.logging.log_prefix << " Updating session header with current values..."
                  << "leadership_term_id: " << leadership_term_id_
                  << ", session_id: " << session_id_ << std::endl;
        write_int64_le(session_header_buffer_, offset,
                       leadership_term_id_);  // FIXED: Use actual value
        offset += 8;
        write_int64_le(session_header_buffer_, offset, session_id_);
        offset += 8;
        write_int64_le(session_header_buffer_, offset, 0);  // timestamp = 0

        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " Updated session header with:" << std::endl;
            std::cout << config_.logging.log_prefix << "   Leadership term: " << leadership_term_id_
                      << std::endl;
            std::cout << config_.logging.log_prefix << "   Session ID: " << session_id_
                      << std::endl;

            // Print the actual header bytes for verification
            std::cout << config_.logging.log_prefix << " Session header hex: ";
            for (size_t i = 0; i < session_header_buffer_.size(); ++i) {
                std::cout << std::hex << std::setfill('0') << std::setw(2)
                          << static_cast<unsigned>(session_header_buffer_[i]) << " ";
                if ((i + 1) % 8 == 0)
                    std::cout << " ";
            }
            std::cout << std::dec << std::endl;
        }
    }
    // ---- End of session handling methods ----


    std::vector<std::uint8_t> create_topic_message(const std::string& topic,
                                                   const std::string& message_type,
                                                   const std::string& message_id,
                                                   const std::string& payload,
                                                   const std::string& headers) {
        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix
                      << " === CREATING TOPIC MESSAGE WITH SBE CLASSES (FIXED) ===" << std::endl;
        }

        try {
            // Calculate total size needed
            size_t total_size = sbe::TopicMessage::sbeBlockAndHeaderLength() +
                                sbe::TopicMessage::computeLength(
                                    topic.length(), message_type.length(), message_id.length(),
                                    payload.length(), headers.length());

            std::vector<uint8_t> buffer(total_size);
            char* bufferPtr = reinterpret_cast<char*>(buffer.data());

            // Create and setup the TopicMessage
            sbe::TopicMessage topicMessage;
            topicMessage.wrapAndApplyHeader(bufferPtr, 0, buffer.size());

            // Set timestamp
            int64_t timestamp = get_current_timestamp();
            topicMessage.timestamp(static_cast<uint64_t>(timestamp));

            // Set fields in the correct order
            topicMessage.putTopic(topic);
            topicMessage.putMessageType(message_type);
            topicMessage.putUuid(message_id);
            topicMessage.putPayload(payload);
            topicMessage.putHeaders(headers);

            // Get actual encoded length
            size_t actual_length = topicMessage.encodedLength();
            buffer.resize(actual_length);

            if (config_.debug_logging) {
                std::cout << config_.logging.log_prefix << " SBE encoding successful:" << std::endl;
                std::cout << config_.logging.log_prefix << "   Encoded length: " << actual_length
                          << std::endl;
                std::cout << config_.logging.log_prefix
                          << "   Template ID: " << sbe::TopicMessage::sbeTemplateId() << std::endl;
                std::cout << config_.logging.log_prefix
                          << "   Schema ID: " << sbe::TopicMessage::sbeSchemaId() << std::endl;

                // Verify the first few fields by re-reading
                sbe::TopicMessage verifyMsg;
                char* verifyPtr = reinterpret_cast<char*>(buffer.data());
                verifyMsg.wrapForDecode(verifyPtr, 8, 8, 1,
                                        buffer.size());  // Skip header for verification

                std::string verify_topic = verifyMsg.getTopicAsString();
                std::cout << config_.logging.log_prefix << "   Verification - topic: \""
                          << verify_topic << "\"" << std::endl;
            }

            return buffer;

        } catch (const std::exception& e) {
            std::cout << "[ERROR] SBE encoding exception: " << e.what() << std::endl;
            throw;
        }
    }

   public:
    bool send_combined_message(const std::vector<std::uint8_t>& business_message) {
        if (!is_connected()) {
            return false;
        }

        std::lock_guard<std::mutex> lock(send_mutex_);

        // Existing code…
        if (!is_connected()) {
            //log this for debugging now
            std::cout << config_.logging.log_prefix << " Not connected, cannot send combined message" << std::endl;
            return false;
        }

        try {
            // Create single buffer: SessionHeader (32 bytes) + BusinessMessage
            size_t total_size = session_header_buffer_.size() + business_message.size();
            std::vector<std::uint8_t> combined_buffer;
            combined_buffer.reserve(total_size);

            // Copy session header first (32 bytes)
            combined_buffer.insert(combined_buffer.end(), session_header_buffer_.begin(),
                                   session_header_buffer_.end());

            // Copy business message immediately after (no padding, no gap)
            combined_buffer.insert(combined_buffer.end(), business_message.begin(),
                                   business_message.end());

            if (config_.debug_logging) {
                std::cout << config_.logging.log_prefix
                          << " Combined message verification:" << std::endl;
                std::cout << config_.logging.log_prefix
                          << "   Total size: " << combined_buffer.size() << std::endl;

                // Verify the business message header starts correctly at offset 32
                if (combined_buffer.size() >= 40) {
                    std::cout << config_.logging.log_prefix << "   Business header at offset 32: ";
                    for (size_t i = 32; i < 40; ++i) {
                        std::cout << std::hex << std::setfill('0') << std::setw(2)
                                  << static_cast<unsigned>(combined_buffer[i]) << " ";
                    }
                    std::cout << std::dec << std::endl;

                    // Verify this matches the expected TopicMessage header
                    // Old format: block_length=8 (08 00), new format: block_length=16 (10 00)
                    // Both are valid, just check template_id=1, schema_id=1, version=1
                    uint16_t block_length = combined_buffer[32] | (combined_buffer[33] << 8);
                    bool header_correct =
                        (block_length == 8 || block_length == 16) && // Support both old and new format
                        (combined_buffer[34] == 0x01 && combined_buffer[35] == 0x00 && // template_id = 1
                         combined_buffer[36] == 0x01 && combined_buffer[37] == 0x00 && // schema_id = 1
                         combined_buffer[38] == 0x01 && combined_buffer[39] == 0x00);  // version = 1
                    std::cout << config_.logging.log_prefix
                              << "   Business header valid: " << (header_correct ? "YES" : "NO")
                              << std::endl;
                }
            }

            // Send the message
            aeron::AtomicBuffer buffer(combined_buffer.data(), combined_buffer.size());
            const int max_attempts = effective_publish_retry_attempts();
            for (int attempt = 0; attempt < max_attempts; ++attempt) {
                std::int64_t result = ingress_publication_->offer(
                    buffer, 0, static_cast<aeron::util::index_t>(combined_buffer.size()));

                if (result > 0) {
                    clear_last_publication_error();
                    stats_.messages_sent++;
                    stats_.last_message_time = std::chrono::steady_clock::now();

                    if (config_.debug_logging) {
                        std::cout << config_.logging.log_prefix
                                  << " Combined message sent successfully, position: " << result
                                  << std::endl;
                    }

                    apply_publish_rate_limit();
                    return true;
                }

                OfferFailureCode failure_code =
                    handle_offer_failure("Combined message offer", result);
                if (!is_transient_failure(failure_code) || attempt == max_attempts - 1) {
                    return false;
                }

                wait_before_publish_retry(attempt);
            }
            return false;

        } catch (const std::exception& e) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix
                          << " Error sending combined message: " << e.what() << std::endl;
            }
            return false;
        }
    }

    // ---- Session event handling methods ----

    void handle_session_redirect(const ParseResult& result) {
        stats_.redirects_received++;

        std::int32_t redirect_member_id = result.leader_member_id;
        
        if (config_.enable_console_info) {
            std::cout << config_.logging.log_prefix
                      << " Redirected to leader member: " << redirect_member_id << std::endl;
        }

        // Store the redirect target member ID so connect() can retry with that member
        redirected_to_member_id_ = redirect_member_id;
        leader_member_id_ = redirect_member_id;
    }

    void handle_session_ok(const ParseResult& result) {
        session_id_ = result.correlation_id;
        connected_ = true;
        leadership_term_id_ = result.leadership_term_id;
        leader_member_id_ = result.leader_member_id;

        update_session_header();
        update_keepalive_buffer();  // Update keepalive buffer with session values (like Go)
        stats_.session_start_time = std::chrono::steady_clock::now();

        if (config_.enable_console_info) {
            std::cout << config_.logging.log_prefix << " Session established successfully!"
                      << std::endl;
            std::cout << config_.logging.log_prefix << "   Session ID: " << session_id_
                      << std::endl;
            std::cout << config_.logging.log_prefix << "   Leadership Term: " << leadership_term_id_
                      << std::endl;
            std::cout << config_.logging.log_prefix << "   Leader Member: " << leader_member_id_
                      << std::endl;
        }

        if (connection_state_callback_) {
            connection_state_callback_(true);
        }
    }

    void handle_session_error(const ParseResult& result) {
        if (config_.enable_console_errors) {
            std::cerr << config_.logging.log_prefix << " Session error: "
                      << SBEUtils::get_session_event_code_string(result.event_code) << std::endl;
        }

        connected_ = false;

        if (connection_state_callback_) {
            connection_state_callback_(false);
        }
    }

    void handle_session_closed(const ParseResult& result) {
        if (config_.enable_console_info) {
            std::cout << config_.logging.log_prefix << " Session closed by cluster" << std::endl;
            std::cout << config_.logging.log_prefix << " Error ? " << result.error_message
                      << std::endl;
        }

        connected_ = false;
        session_id_ = -1;

        if (connection_state_callback_) {
            connection_state_callback_(false);
        }
    }
    // ---- End of session event handling methods ----

    // Helper methods

    // Helper function to verify SBE configuration matches Go client
    void verify_sbe_configuration() {
        std::cout << "=== SBE Configuration Verification ===" << std::endl;
        std::cout << "TopicMessage:" << std::endl;
        std::cout << "  Template ID: " << sbe::TopicMessage::sbeTemplateId() << " (should be 1)"
                  << std::endl;
        std::cout << "  Schema ID: " << sbe::TopicMessage::sbeSchemaId() << " (should be 1)"
                  << std::endl;
        std::cout << "  Schema Version: " << sbe::TopicMessage::sbeSchemaVersion()
                  << " (should be 1)" << std::endl;
        std::cout << "  Block Length: " << sbe::TopicMessage::sbeBlockLength() << " (should be 8)"
                  << std::endl;

        std::cout << "VarStringEncoding:" << std::endl;
        std::cout << "  Schema ID: " << sbe::VarStringEncoding::sbeSchemaId() << " (should be 1)"
                  << std::endl;
        std::cout << "  Schema Version: " << sbe::VarStringEncoding::sbeSchemaVersion()
                  << " (should be 1)" << std::endl;

        // Verify header lengths are correct (should all be 2 for uint16 length prefixes)
        std::cout << "Header Lengths:" << std::endl;
        std::cout << "  Topic: " << sbe::TopicMessage::topicHeaderLength() << " (should be 2)"
                  << std::endl;
        std::cout << "  MessageType: " << sbe::TopicMessage::messageTypeHeaderLength()
                  << " (should be 2)" << std::endl;
        std::cout << "  UUID: " << sbe::TopicMessage::uuidHeaderLength() << " (should be 2)"
                  << std::endl;
        std::cout << "  Payload: " << sbe::TopicMessage::payloadHeaderLength() << " (should be 2)"
                  << std::endl;
        std::cout << "  Headers: " << sbe::TopicMessage::headersHeaderLength() << " (should be 2)"
                  << std::endl;
        std::cout << "====================================" << std::endl;
    }

    void print_buffer_transition(const std::vector<std::uint8_t>& combined_buffer) {
        std::cout << config_.logging.log_prefix << " Buffer transition (bytes 28-36):" << std::endl;
        std::cout << config_.logging.log_prefix << "   ";

        // Print bytes around the transition point (session->business)
        for (size_t i = 28; i < combined_buffer.size() && i < 40; ++i) {
            if (i == 32)
                std::cout << "| ";  // Mark transition
            std::cout << std::hex << std::setfill('0') << std::setw(2)
                      << static_cast<unsigned>(combined_buffer[i]) << " ";
        }
        std::cout << std::dec << std::endl;

        // Show what this represents
        std::cout << config_.logging.log_prefix << "   ^session end    ^business start"
                  << std::endl;
    }
    void print_hex_section(const uint8_t* data, size_t start, size_t length) {
        std::cout << config_.logging.log_prefix << "   ";
        for (size_t i = 0; i < length; ++i) {
            std::cout << std::hex << std::setfill('0') << std::setw(2)
                      << static_cast<unsigned>(data[start + i]) << " ";
            if ((i + 1) % 8 == 0 && i + 1 < length) {
                std::cout << " ";
            }
        }
        std::cout << std::dec << std::endl;

        // Also print ASCII representation
        std::cout << config_.logging.log_prefix << "   ASCII: ";
        for (size_t i = 0; i < length; ++i) {
            char c = static_cast<char>(data[start + i]);
            std::cout << (c >= 32 && c <= 126 ? c : '.');
        }
        std::cout << std::endl;
    }

    void write_uint16_le(std::vector<std::uint8_t>& buffer, std::size_t offset,
                         std::uint16_t value) {
        buffer[offset] = static_cast<std::uint8_t>(value & 0xFF);
        buffer[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    }

    void write_uint32_le(std::vector<std::uint8_t>& buffer, std::size_t offset,
                         std::uint32_t value) {
        buffer[offset] = static_cast<std::uint8_t>(value & 0xFF);
        buffer[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
        buffer[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
        buffer[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    }

    void write_int64_le(std::vector<std::uint8_t>& buffer, std::size_t offset, std::int64_t value) {
        for (int i = 0; i < 8; ++i) {
            buffer[offset + i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF);
        }
    }

    int64_t get_current_timestamp() {
        auto now = std::chrono::high_resolution_clock::now();
        return now.time_since_epoch().count();
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
    : pImpl_(std::make_unique<Impl>(config)) {}

SessionManager::~SessionManager() = default;

SessionConnectionResult SessionManager::connect(std::shared_ptr<aeron::Aeron> aeron) {
    return pImpl_->connect(aeron);
}

std::future<SessionConnectionResult> SessionManager::connect_async(
    std::shared_ptr<aeron::Aeron> aeron) {
    return std::async(std::launch::async, [this, aeron]() { return pImpl_->connect(aeron); });
}

SessionConnectionResult SessionManager::connect_with_timeout(std::shared_ptr<aeron::Aeron> aeron,
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

bool SessionManager::publish_message(const std::string& topic, const std::string& message_type,
                                     const std::string& message_id, const std::string& payload,
                                     const std::string& headers) {
    return pImpl_->publish_message(topic, message_type, message_id, payload, headers);
}

bool SessionManager::send_raw_message(const std::vector<std::uint8_t>& encoded_message) {
    return pImpl_->send_raw_message(encoded_message);
}

bool SessionManager::send_combined_message(const std::vector<std::uint8_t>& business_message) {
    return pImpl_->send_combined_message(business_message);
}

bool SessionManager::send_keepalive() {
    return pImpl_->send_keepalive();
}

std::int64_t SessionManager::get_last_publication_error_code() const {
    return pImpl_->get_last_publication_error_code();
}

std::string SessionManager::get_last_publication_error_message() const {
    return pImpl_->get_last_publication_error_message();
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

}  // namespace SessionUtils

}  // namespace aeron_cluster