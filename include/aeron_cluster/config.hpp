#pragma once

#include <string>
#include <vector>
#include <chrono>

namespace aeron_cluster {

/**
 * @brief Configuration parameters for ClusterClient
 */
struct ClusterClientConfig {
    /**
     * @brief List of cluster member endpoints in format "host:port"
     * 
     * The client will attempt to connect to these endpoints to find
     * the current cluster leader. Typically includes all cluster members.
     * 
     * Example: {"localhost:9002", "localhost:9102", "localhost:9202"}
     */
    std::vector<std::string> cluster_endpoints = {
        "localhost:9002",
        "localhost:9102", 
        "localhost:9202"
    };

    /**
     * @brief Aeron channel for receiving responses from cluster
     * 
     * This channel will be used to create a subscription for egress messages.
     * Use "aeron:udp?endpoint=localhost:0" to let Aeron choose a port automatically.
     * 
     * For specific network interfaces, use something like:
     * "aeron:udp?endpoint=192.168.1.100:44445"
     */
    std::string response_channel = "aeron:udp?endpoint=localhost:0";

    /**
     * @brief Aeron media driver directory
     * 
     * Path to the shared memory directory used by Aeron media driver.
     * Must match the directory used by the running media driver.
     * 
     * Common values:
     * - Linux/macOS: "/dev/shm/aeron" or "/tmp/aeron"
     * - Windows: "C:\\temp\\aeron"
     */
    std::string aeron_dir = "/dev/shm/aeron";

    /**
     * @brief Stream ID for cluster ingress (client -> cluster)
     * 
     * Must match the ingress stream ID configured in the cluster.
     * Default value matches standard Aeron Cluster configuration.
     */
    int32_t ingress_stream_id = 101;

    /**
     * @brief Stream ID for cluster egress (cluster -> client)
     * 
     * Must match the egress stream ID configured in the cluster.
     * Default value matches standard Aeron Cluster configuration.
     */
    int32_t egress_stream_id = 102;

    /**
     * @brief Timeout for waiting for session connection response
     * 
     * Maximum time to wait for SessionEvent after sending SessionConnectRequest.
     * Increase this value for slow networks or heavily loaded clusters.
     */
    std::chrono::milliseconds response_timeout = std::chrono::seconds(10);

    /**
     * @brief Maximum number of connection retry attempts
     * 
     * If connection to a cluster member fails, the client will try
     * this many times before giving up.
     */
    int max_retries = 3;

    /**
     * @brief Delay between retry attempts
     * 
     * Time to wait between connection retry attempts.
     */
    std::chrono::milliseconds retry_delay = std::chrono::milliseconds(500);

    /**
     * @brief Client application name identifier
     * 
     * Used for logging and debugging purposes. Will be included
     * in some protocol messages.
     */
    std::string application_name = "aeron-cluster-cpp";

    /**
     * @brief Protocol semantic version to use
     * 
     * Must match the protocol version expected by the cluster.
     * This is different from the SBE schema version.
     */
    int32_t protocol_semantic_version = 1;

    /**
     * @brief Enable debug logging for protocol messages
     * 
     * When true, will output detailed hex dumps and parsing information
     * for all SBE messages. Useful for debugging protocol issues.
     */
    bool debug_logging = false;

    /**
     * @brief Maximum message size for SBE encoding
     * 
     * Maximum size in bytes for encoded SBE messages. Should be large
     * enough to accommodate your largest order payloads.
     */
    size_t max_message_size = 64 * 1024; // 64KB

    /**
     * @brief Default topic name for order messages
     * 
     * Topic name used when publishing orders to the cluster.
     * Must match the topic expected by your cluster application.
     */
    std::string default_topic = "orders";
};

/**
 * @brief Connection and performance statistics
 */
struct ConnectionStats {
    /**
     * @brief Total number of messages sent to cluster
     */
    uint64_t messages_sent = 0;

    /**
     * @brief Total number of messages received from cluster
     */
    uint64_t messages_received = 0;

    /**
     * @brief Number of successful message acknowledgments
     */
    uint64_t messages_acknowledged = 0;

    /**
     * @brief Number of failed/rejected messages
     */
    uint64_t messages_failed = 0;

    /**
     * @brief Number of connection attempts made
     */
    uint64_t connection_attempts = 0;

    /**
     * @brief Number of successful connections
     */
    uint64_t successful_connections = 0;

    /**
     * @brief Number of times client was redirected to different leader
     */
    uint64_t leader_redirects = 0;

    /**
     * @brief Current session ID (or -1 if not connected)
     */
    int64_t current_session_id = -1;

    /**
     * @brief Current leader member ID (or -1 if unknown)
     */
    int32_t current_leader_id = -1;

    /**
     * @brief Time when connection was established
     */
    std::chrono::steady_clock::time_point connection_established_time;

    /**
     * @brief Total connection uptime
     */
    std::chrono::milliseconds total_uptime{0};

    /**
     * @brief Average round-trip time for acknowledged messages
     */
    std::chrono::microseconds average_rtt{0};

    /**
     * @brief Whether client is currently connected
     */
    bool is_connected = false;
};

/**
 * @brief SBE protocol constants matching Aeron Cluster specification
 */
struct SBEConstants {
    // Cluster schema constants
    static constexpr uint16_t CLUSTER_SCHEMA_ID = 111;
    static constexpr uint16_t CLUSTER_SCHEMA_VERSION = 8;
    
    // Topic message schema constants  
    static constexpr uint16_t TOPIC_SCHEMA_ID = 1;
    static constexpr uint16_t TOPIC_SCHEMA_VERSION = 1;
    
    // Template IDs
    static constexpr uint16_t SESSION_CONNECT_TEMPLATE_ID = 3;
    static constexpr uint16_t SESSION_EVENT_TEMPLATE_ID = 2;
    static constexpr uint16_t TOPIC_MESSAGE_TEMPLATE_ID = 1;
    static constexpr uint16_t ACKNOWLEDGMENT_TEMPLATE_ID = 2;
    
    // Fixed block lengths
    static constexpr uint16_t SESSION_CONNECT_BLOCK_LENGTH = 16;
    static constexpr uint16_t SESSION_EVENT_BLOCK_LENGTH = 32;
    static constexpr uint16_t TOPIC_MESSAGE_BLOCK_LENGTH = 48;
    
    // Session event codes
    static constexpr int32_t SESSION_EVENT_OK = 0;
    static constexpr int32_t SESSION_EVENT_ERROR = 1;
    static constexpr int32_t SESSION_EVENT_AUTHENTICATION_REJECTED = 2;
    static constexpr int32_t SESSION_EVENT_REDIRECT = 3;
    static constexpr int32_t SESSION_EVENT_CLOSED = 4;
};

/**
 * @brief Logging configuration for the client
 */
struct LoggingConfig {
    /**
     * @brief Enable console output for informational messages
     */
    bool enable_console_info = true;

    /**
     * @brief Enable console output for warning messages
     */
    bool enable_console_warnings = true;

    /**
     * @brief Enable console output for error messages
     */
    bool enable_console_errors = true;

    /**
     * @brief Enable detailed protocol debugging output
     */
    bool enable_protocol_debug = false;

    /**
     * @brief Enable hex dump output for all messages
     */
    bool enable_hex_dumps = false;

    /**
     * @brief Log message prefix for identification
     */
    std::string log_prefix = "[AeronCluster]";

    /**
     * @brief Whether to include timestamps in log messages
     */
    bool include_timestamps = true;

    /**
     * @brief Whether to include thread IDs in log messages
     */
    bool include_thread_ids = false;
};

} // namespace aeron_cluster