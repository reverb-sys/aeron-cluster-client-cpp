#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <cstdint>

namespace aeron_cluster {

/**
 * @brief Logging configuration for the cluster client
 */
struct LoggingConfig {
    bool enable_console_info = true;
    bool enable_console_warnings = true;
    bool enable_console_errors = true;
    bool enable_protocol_debug = false;
    bool enable_hex_dumps = false;
    std::string log_prefix = "[AeronCluster]";
    bool include_timestamps = true;
    bool include_thread_ids = false;
};

/**
 * @brief Configuration parameters for ClusterClient
 */
struct ClusterClientConfig {
    // Connection settings
    std::vector<std::string> cluster_endpoints = {
        "localhost:9002", "localhost:9102", "localhost:9202"
    };
    std::string client_id = "cpp_client";
    std::string instance_identifier = ""; // Auto-generated unique identifier for load balancing
    std::string response_channel = "aeron:udp?endpoint=localhost:0";
    std::string aeron_dir = "/dev/shm/aeron";
    
    // Stream IDs
    std::int32_t ingress_stream_id = 101;
    std::int32_t egress_stream_id = 102;
    
    // Timeout and retry settings
    std::chrono::milliseconds response_timeout{10000};
    std::chrono::milliseconds retry_delay{500};
    int max_retries = 3;
    
    // Protocol settings
    std::string application_name = "aeron-cluster-cpp";
    std::int32_t protocol_semantic_version = 1;
    std::size_t max_message_size = 64 * 1024; // 64KB
    std::string default_topic = "orders";
    
    // Keepalive settings
    bool enable_keepalive = true;
    std::chrono::milliseconds keepalive_interval{1000};
    int keepalive_max_retries = 3;
    
    // Publish offer retry/backoff behavior
    int publish_max_retry_attempts = 50;
    std::chrono::microseconds publish_retry_idle_base{500};   // 0.5 ms baseline
    std::chrono::microseconds publish_retry_idle_max{5000};   // clamp at 5 ms
    std::chrono::microseconds publish_rate_limit_delay{0};    // optional global throttle
    
    // Logging
    LoggingConfig logging;
    bool debug_logging = false;
    bool enable_console_info = true;
    bool enable_console_warnings = true;
    bool enable_console_errors = true;
    bool enable_hex_dumps = false;
    
    // Commit logging
    bool commit_log_enabled = false;
    bool enable_auto_commit = true;

    // Delivery stall detection (0 disables the respective guard)
    std::chrono::milliseconds delivery_stall_warning_timeout{5000};
    std::chrono::milliseconds delivery_stall_disconnect_timeout{15000};

    /**
     * @brief Validate configuration parameters
     * @throws std::invalid_argument if configuration is invalid
     */
    void validate();

    /**
     * @brief Get the local IP address for this machine
     * @return IP address string, or empty string if not found
     */
    static std::string get_local_ip_address();

    /**
     * @brief Find an available UDP port in the specified range
     * @param min_port Minimum port number
     * @param max_port Maximum port number
     * @return Available port number
     * @throws std::runtime_error if no port is available
     */
    static int find_available_port(int min_port = 40000, int max_port = 59000);

    /**
     * @brief Resolve the egress endpoint, replacing port 0 with an available port
     * @param base_channel The base channel string
     * @return Resolved channel string with actual port
     */
    std::string resolve_egress_endpoint(const std::string& base_channel) const;

private:
    /**
     * @brief Check if channel needs port resolution
     * @param channel Channel string to check
     * @return true if channel contains port 0
     */
    bool needs_port_resolution(const std::string& channel) const;
};

/**
 * @brief Connection and performance statistics
 */
struct ConnectionStats {
    // Message counters
    std::uint64_t messages_sent = 0;
    std::uint64_t messages_received = 0;
    std::uint64_t messages_acknowledged = 0;
    std::uint64_t messages_failed = 0;
    
    // Connection statistics
    std::uint64_t connection_attempts = 0;
    std::uint64_t successful_connections = 0;
    std::uint64_t leader_redirects = 0;
    
    // Session information
    std::int64_t current_session_id = -1;
    std::int32_t current_leader_id = -1;
    bool is_connected = false;
    
    // Timing information
    std::chrono::steady_clock::time_point connection_established_time;
    std::chrono::milliseconds total_uptime{0};
    std::chrono::microseconds average_rtt{0};
    
    // Keepalive statistics
    std::uint64_t keepalives_sent = 0;
    std::uint64_t keepalives_failed = 0;

    /**
     * @brief Calculate current uptime if connected
     * @return Current uptime duration
     */
    std::chrono::milliseconds get_current_uptime() const;

    /**
     * @brief Get connection success rate
     * @return Success rate as a percentage (0.0 - 1.0)
     */
    double get_connection_success_rate() const;

    /**
     * @brief Get message success rate
     * @return Success rate as a percentage (0.0 - 1.0)
     */
    double get_message_success_rate() const;
};

/**
 * @brief SBE protocol constants matching Aeron Cluster specification
 */
namespace SBEConstants {
    // Schema information
    constexpr std::uint16_t CLUSTER_SCHEMA_ID = 111;
    constexpr std::uint16_t CLUSTER_SCHEMA_VERSION = 8;
    constexpr std::uint16_t TOPIC_SCHEMA_ID = 1;
    constexpr std::uint16_t TOPIC_SCHEMA_VERSION = 1;
    
    // Template IDs
    constexpr std::uint16_t SESSION_CONNECT_TEMPLATE_ID = 3;
    constexpr std::uint16_t SESSION_EVENT_TEMPLATE_ID = 2;
    constexpr std::uint16_t SESSION_CLOSE_TEMPLATE_ID = 4;
    constexpr std::uint16_t SESSION_KEEPALIVE_TEMPLATE_ID = 5;
    constexpr std::uint16_t TOPIC_MESSAGE_TEMPLATE_ID = 1;
    constexpr std::uint16_t ACKNOWLEDGMENT_TEMPLATE_ID = 2;
    
    // Block lengths
    constexpr std::uint16_t SESSION_CONNECT_BLOCK_LENGTH = 16;
    constexpr std::uint16_t SESSION_EVENT_BLOCK_LENGTH = 32;
    constexpr std::uint16_t TOPIC_MESSAGE_BLOCK_LENGTH = 48;
    constexpr std::uint16_t ACKNOWLEDGMENT_BLOCK_LENGTH = 8;
    
    // Session event codes (matching Java EventCode enum)
    constexpr std::int32_t SESSION_EVENT_OK = 0;
    constexpr std::int32_t SESSION_EVENT_ERROR = 1;
    constexpr std::int32_t SESSION_EVENT_REDIRECT = 2;  // Redirect to leader member
    constexpr std::int32_t SESSION_EVENT_AUTHENTICATION_REJECTED = 3;
    constexpr std::int32_t SESSION_EVENT_CLOSED = 4;
    
    // Header length
    constexpr std::size_t SBE_HEADER_LENGTH = 8;
} // namespace SBEConstants

} // namespace aeron_cluster