#pragma once

#include "config.hpp"
#include "sbe_messages.hpp"

#include <Aeron.h>
#include <memory>
#include <atomic>
#include <future>
#include <functional>

namespace aeron_cluster {

/**
 * @brief Exception thrown by session manager operations
 */
class SessionManagerException : public std::runtime_error {
public:
    explicit SessionManagerException(const std::string& message) 
        : std::runtime_error(message) {}
};

/**
 * @brief Session connection result
 */
struct SessionConnectionResult {
    bool success = false;
    std::int64_t session_id = -1;
    std::int32_t leader_member_id = -1;
    std::int64_t leadership_term_id = -1;
    std::string error_message;
    std::chrono::milliseconds connection_time{0};
};

/**
 * @brief Session manager handles the low-level cluster session protocol
 */
class SessionManager {
public:
    /**
     * @brief Callback for session events
     */
    using SessionEventCallback = std::function<void(const ParseResult& event)>;

    /**
     * @brief Callback for connection state changes
     */
    using ConnectionStateCallback = std::function<void(bool connected)>;

    /**
     * @brief Construct session manager with configuration
     */
    explicit SessionManager(const ClusterClientConfig& config);

    /**
     * @brief Destructor
     */
    ~SessionManager();

    // Non-copyable, non-movable
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;
    SessionManager(SessionManager&&) = delete;
    SessionManager& operator=(SessionManager&&) = delete;

    /**
     * @brief Connect to cluster synchronously
     * @param aeron Shared Aeron instance
     * @return Connection result with detailed information
     */
    SessionConnectionResult connect(std::shared_ptr<aeron::Aeron> aeron);

    /**
     * @brief Connect to cluster asynchronously
     * @param aeron Shared Aeron instance
     * @return Future that resolves to connection result
     */
    std::future<SessionConnectionResult> connect_async(std::shared_ptr<aeron::Aeron> aeron);

    /**
     * @brief Connect with timeout
     * @param aeron Shared Aeron instance
     * @param timeout Maximum time to wait for connection
     * @return Connection result
     */
    SessionConnectionResult connect_with_timeout(
        std::shared_ptr<aeron::Aeron> aeron,
        std::chrono::milliseconds timeout);

    /**
     * @brief Disconnect from cluster
     */
    void disconnect();

    /**
     * @brief Check if session is connected and active
     */
    bool is_connected() const;

    /**
     * @brief Get session ID
     */
    std::int64_t get_session_id() const;

    /**
     * @brief Get current leader member ID
     */
    std::int32_t get_leader_member_id() const;

    /**
     * @brief Get current leadership term ID
     */
    std::int64_t get_leadership_term_id() const;

    /**
     * @brief Publish a message to the cluster
     * @param topic Topic name
     * @param message_type Message type
     * @param message_id Unique message identifier
     * @param payload Message payload (usually JSON)
     * @param headers Message headers (usually JSON)
     * @return true if message was successfully offered to Aeron
     */
    bool publish_message(
        const std::string& topic,
        const std::string& message_type,
        const std::string& message_id,
        const std::string& payload,
        const std::string& headers);

    /**
     * @brief Send raw SBE-encoded message
     * @param encoded_message Pre-encoded SBE message
     * @return true if message was successfully offered
     */
    bool send_raw_message(const std::vector<std::uint8_t>& encoded_message);

    /**
     * @brief Send combined message (with session envelope wrapper)
     * @param business_message Pre-encoded business message
     * @return true if message was successfully offered
     */
    bool send_combined_message(const std::vector<std::uint8_t>& business_message);

    /**
     * @brief Send keepalive message
     * @return true if keepalive was sent successfully
     */
    bool send_keepalive();

    /**
     * @brief Get the last Aeron publication error code observed when offering to ingress.
     * @return Negative Aeron error code or 0 if the last offer succeeded.
     */
    std::int64_t get_last_publication_error_code() const;

    /**
     * @brief Get the human-readable description for the last Aeron publication error.
     * @return Description string or empty if the last offer succeeded.
     */
    std::string get_last_publication_error_message() const;

    /**
     * @brief Handle incoming session event
     * @param result Parsed session event
     */
    void handle_session_event(const ParseResult& result);

    /**
     * @brief Set callback for session events
     */
    void set_session_event_callback(SessionEventCallback callback);

    /**
     * @brief Set callback for connection state changes
     */
    void set_connection_state_callback(ConnectionStateCallback callback);

    /**
     * @brief Get session statistics
     */
    struct SessionStats {
        std::uint64_t messages_sent = 0;
        std::uint64_t keepalives_sent = 0;
        std::uint64_t keepalives_failed = 0;
        std::uint64_t session_events_received = 0;
        std::uint64_t redirects_received = 0;
        std::chrono::steady_clock::time_point last_keepalive_time;
        std::chrono::steady_clock::time_point last_message_time;
        std::chrono::steady_clock::time_point session_start_time;
    };

    SessionStats get_session_stats() const;

    /**
     * @brief Enable or disable automatic keepalives
     */
    void set_keepalive_enabled(bool enabled);

    /**
     * @brief Check if automatic keepalives are enabled
     */
    bool is_keepalive_enabled() const;

    /**
     * @brief Force a session reconnection
     * @return Future that resolves to reconnection result
     */
    std::future<SessionConnectionResult> reconnect_async();

private:
    class Impl;
    std::unique_ptr<Impl> pImpl_;
};

/**
 * @brief Utility functions for session management
 */
namespace SessionUtils {

/**
 * @brief Calculate leader endpoint from member ID
 * @param member_id Leader member ID
 * @param base_ip Base IP address
 * @return Leader endpoint string
 */
std::string calculate_leader_endpoint(std::int32_t member_id, const std::string& base_ip = "localhost");

/**
 * @brief Extract base IP from endpoint string
 * @param endpoint Endpoint in format "host:port"
 * @return Base IP address
 */
std::string extract_base_ip(const std::string& endpoint);

/**
 * @brief Validate endpoint format
 * @param endpoint Endpoint string to validate
 * @return true if endpoint format is valid
 */
bool is_valid_endpoint(const std::string& endpoint);

/**
 * @brief Parse endpoint into host and port
 * @param endpoint Endpoint string
 * @return Pair of (host, port), or empty strings if invalid
 */
std::pair<std::string, int> parse_endpoint(const std::string& endpoint);

/**
 * @brief Generate unique correlation ID
 * @return Unique correlation ID
 */
std::int64_t generate_correlation_id();

} // namespace SessionUtils

} // namespace aeron_cluster