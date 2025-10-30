#pragma once

#include "config.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <stdexcept>

namespace aeron_cluster {

/**
 * @brief SBE Message Header (8 bytes, packed)
 */
struct MessageHeader {
    std::uint16_t block_length;  // Length of the fixed-size message block
    std::uint16_t template_id;   // Message template identifier
    std::uint16_t schema_id;     // SBE schema identifier
    std::uint16_t version;       // Schema version
} __attribute__((packed));

static_assert(sizeof(MessageHeader) == 8, "MessageHeader must be exactly 8 bytes");

/**
 * @brief Session Connect Request message (Template ID 3)
 */
struct SessionConnectRequest {
    std::int64_t correlation_id;     // Correlation ID for matching response
    std::int32_t response_stream_id; // Stream ID for client responses
    std::int32_t version;            // Protocol semantic version

    static constexpr std::uint16_t sbe_block_length() { return 16; }
    static constexpr std::uint16_t sbe_template_id() { return 3; }
    static constexpr std::uint16_t sbe_schema_id() { return 111; }
    static constexpr std::uint16_t sbe_schema_version() { return 8; }
} __attribute__((packed));

static_assert(sizeof(SessionConnectRequest) == 16, "SessionConnectRequest must be exactly 16 bytes");

/**
 * @brief Session Event message (Template ID 2)
 */
struct SessionEvent {
    std::int64_t correlation_id;      // Matching correlation ID from request
    std::int64_t cluster_session_id;  // Assigned session ID
    std::int64_t leadership_term_id;  // Current leadership term
    std::int32_t leader_member_id;    // Current leader member ID
    std::int32_t code;                // Event code (0=OK, 1=ERROR, 3=REDIRECT, 4=CLOSED)

    static constexpr std::uint16_t sbe_block_length() { return 32; }
    static constexpr std::uint16_t sbe_template_id() { return 2; }
    static constexpr std::uint16_t sbe_schema_id() { return 111; }
    static constexpr std::uint16_t sbe_schema_version() { return 8; }
} __attribute__((packed));

static_assert(sizeof(SessionEvent) == 32, "SessionEvent must be exactly 32 bytes");

/**
 * @brief Session Keep Alive message (Template ID 5)
 */
struct SessionKeepAlive {
    std::int64_t leadership_term_id;
    std::int64_t cluster_session_id;

    static constexpr std::uint16_t sbe_block_length() { return 16; }
    static constexpr std::uint16_t sbe_template_id() { return 5; }
    static constexpr std::uint16_t sbe_schema_id() { return 111; }
    static constexpr std::uint16_t sbe_schema_version() { return 8; }
} __attribute__((packed));

static_assert(sizeof(SessionKeepAlive) == 16, "SessionKeepAlive must be exactly 16 bytes");

/**
 * @brief Session Close Request message (Template ID 4)
 * Sent by client to gracefully close a session, allowing cluster to detect CLIENT_ACTION
 */
struct SessionCloseRequest {
    std::int64_t leadership_term_id;
    std::int64_t cluster_session_id;

    static constexpr std::uint16_t sbe_block_length() { return 16; }
    static constexpr std::uint16_t sbe_template_id() { return 4; }
    static constexpr std::uint16_t sbe_schema_id() { return 111; }
    static constexpr std::uint16_t sbe_schema_version() { return 8; }
} __attribute__((packed));

static_assert(sizeof(SessionCloseRequest) == 16, "SessionCloseRequest must be exactly 16 bytes");

/**
 * @brief Topic Message for business data (Template ID 1)
 */
struct TopicMessage {
    std::int64_t timestamp;  // Message timestamp
    std::uint8_t reserved[40]; // Padding to reach 48 bytes

    static constexpr std::uint16_t sbe_block_length() { return 48; }
    static constexpr std::uint16_t sbe_template_id() { return 1; }
    static constexpr std::uint16_t sbe_schema_id() { return 1; }
    static constexpr std::uint16_t sbe_schema_version() { return 1; }
} __attribute__((packed));

static_assert(sizeof(TopicMessage) == 48, "TopicMessage must be exactly 48 bytes");

/**
 * @brief Acknowledgment message for responses (Template ID 2, Schema ID 1)
 */
struct Acknowledgment {
    std::int64_t timestamp;  // Acknowledgment timestamp

    static constexpr std::uint16_t sbe_block_length() { return 8; }
    static constexpr std::uint16_t sbe_template_id() { return 2; }
    static constexpr std::uint16_t sbe_schema_id() { return 1; }
    static constexpr std::uint16_t sbe_schema_version() { return 1; }
} __attribute__((packed));

static_assert(sizeof(Acknowledgment) == 8, "Acknowledgment must be exactly 8 bytes");

/**
 * @brief Exception thrown during SBE encoding/decoding operations
 */
class SBEException : public std::runtime_error {
public:
    explicit SBEException(const std::string& message) : std::runtime_error(message) {}
};

/**
 * @brief SBE Encoder class for creating properly formatted messages
 */
class SBEEncoder {
public:
    /**
     * @brief Encode a SessionConnectRequest message
     */
    static std::vector<std::uint8_t> encode_session_connect_request(
        std::int64_t correlation_id,
        std::int32_t response_stream_id,
        const std::string& response_channel,
        std::int32_t protocol_version = 1);

    /**
     * @brief Encode a SessionKeepAlive message
     */
    static std::vector<std::uint8_t> encode_session_keep_alive(
        std::int64_t leadership_term_id,
        std::int64_t cluster_session_id);

    /**
     * @brief Encode a SessionCloseRequest message
     * This allows the cluster to detect disconnection as CLIENT_ACTION and properly clean up
     */
    static std::vector<std::uint8_t> encode_session_close_request(
        std::int64_t leadership_term_id,
        std::int64_t cluster_session_id);

    /**
     * @brief Encode a TopicMessage for business data
     */
    static std::vector<std::uint8_t> encode_topic_message(
        const std::string& topic,
        const std::string& message_type,
        const std::string& uuid,
        const std::string& payload,
        const std::string& headers,
        std::int64_t timestamp = 0);

    /**
     * @brief Get current timestamp in nanoseconds since epoch
     */
    static std::int64_t get_current_timestamp();

    static void write_uint16_le(std::vector<std::uint8_t>& buffer, std::size_t offset, std::uint16_t value);
    static void write_int64_le(std::vector<std::uint8_t>& buffer, std::size_t offset, std::int64_t value);
    static void write_uint32_le(std::vector<std::uint8_t>& buffer, std::size_t offset, std::uint32_t value);

private:
    static std::size_t encode_variable_string(
        const std::string& str,
        std::vector<std::uint8_t>& buffer,
        std::size_t offset);

    static std::size_t calculate_variable_fields_size(
        const std::vector<std::string>& fields);

};

/**
 * @brief SBE Decoder class for parsing received messages
 */
class SBEDecoder {
public:
    /**
     * @brief Decode and validate message header
     */
    static bool decode_message_header(
        const std::uint8_t* data,
        std::size_t length,
        MessageHeader& header);

    /**
     * @brief Decode SessionEvent message
     */
    static bool decode_session_event(
        const std::uint8_t* data,
        std::size_t length,
        SessionEvent& event,
        std::string& detail);

    /**
     * @brief Decode TopicMessage
     */
    static bool decode_topic_message(
        const std::uint8_t* data,
        std::size_t length,
        std::string& topic,
        std::string& message_type,
        std::string& uuid,
        std::string& payload,
        std::string& headers,
        std::int64_t& timestamp);

    /**
     * @brief Decode Acknowledgment message
     */
    static bool decode_acknowledgment(
        const std::uint8_t* data,
        std::size_t length,
        std::string& message_id,
        std::string& status,
        std::string& error,
        std::int64_t& timestamp);

private:
    static std::size_t extract_variable_string(
        const std::uint8_t* data,
        std::size_t offset,
        std::size_t remaining_length,
        std::string& output);

    static bool validate_header(
        const MessageHeader& header,
        std::uint16_t expected_template_id,
        std::uint16_t expected_schema_id);

    static std::uint16_t read_uint16_le(const std::uint8_t* data, std::size_t offset);
    static std::int64_t read_int64_le(const std::uint8_t* data, std::size_t offset);
    static std::uint32_t read_uint32_le(const std::uint8_t* data, std::size_t offset);
};

/**
 * @brief Utility functions for SBE message handling
 */
namespace SBEUtils {

/**
 * @brief Print hex dump of binary data for debugging
 */
void print_hex_dump(
    const std::uint8_t* data,
    std::size_t length,
    const std::string& prefix = "",
    std::size_t max_bytes = 0);

/**
 * @brief Get human-readable string for session event code
 */
std::string get_session_event_code_string(std::int32_t code);

/**
 * @brief Get human-readable string for template ID
 */
std::string get_message_type_name(std::uint16_t template_id, std::uint16_t schema_id);

/**
 * @brief Validate correlation ID format
 */
bool is_valid_correlation_id(std::int64_t correlation_id);

/**
 * @brief Generate a unique correlation ID
 */
std::int64_t generate_correlation_id();

/**
 * @brief Convert timestamp to human-readable string
 */
std::string format_timestamp(std::int64_t timestamp);

/**
 * @brief Check if message data appears to be valid SBE format
 */
bool is_valid_sbe_message(const std::uint8_t* data, std::size_t length);

/**
 * @brief Extract all readable strings from binary data
 */
std::vector<std::string> extract_readable_strings(
    const std::uint8_t* data,
    std::size_t length,
    std::size_t min_length = 3);

} // namespace SBEUtils

/**
 * @brief Message parsing result with detailed information
 */
struct ParseResult {
    bool success = false;
    std::string error_message;
    std::string message_type;
    std::string message_id;
    std::string payload;
    std::string headers;
    std::int64_t timestamp = 0;
    std::uint64_t sequence_number = 0;

    // Header information
    std::uint16_t template_id = 0;
    std::uint16_t schema_id = 0;
    std::uint16_t version = 0;
    std::uint16_t block_length = 0;

    // Session-specific fields
    std::int64_t correlation_id = 0;
    std::int64_t session_id = 0;
    std::int32_t leader_member_id = 0;
    std::int32_t event_code = 0;
    std::int64_t leadership_term_id = 0;

    /**
     * @brief Check if this is a session event message
     */
    bool is_session_event() const {
        return template_id == SBEConstants::SESSION_EVENT_TEMPLATE_ID && 
               schema_id == SBEConstants::CLUSTER_SCHEMA_ID;
    }

    /**
     * @brief Check if this is a topic message (including ORDER_MESSAGE)
     */
    bool is_topic_message() const {
        // Primary check: direct TopicMessage
        if (template_id == SBEConstants::TOPIC_MESSAGE_TEMPLATE_ID && 
            (schema_id == SBEConstants::TOPIC_SCHEMA_ID || schema_id == 1)) {
            return true;
        }

        // Check for embedded TopicMessage in cluster session messages
        if (schema_id == SBEConstants::CLUSTER_SCHEMA_ID &&
            template_id == SBEConstants::TOPIC_MESSAGE_TEMPLATE_ID) {
            return true;
        }

        // Check for embedded TopicMessage in session events
        if (schema_id == SBEConstants::TOPIC_SCHEMA_ID && 
            template_id == SBEConstants::SESSION_EVENT_TEMPLATE_ID) {
            return true;
        }

        // Additional check: if message_type indicates it's a topic message
        if (!message_type.empty() && 
            (message_type.find("ORDER") != std::string::npos ||
             message_type.find("TopicMessage") != std::string::npos ||
             message_type.find("CREATE_ORDER") != std::string::npos ||
             message_type.find("UPDATE_ORDER") != std::string::npos)) {
            return true;
        }

        return false;
    }

    /**
     * @brief Check if this is an acknowledgment
     */
    bool is_acknowledgment() const {
        return template_id == SBEConstants::ACKNOWLEDGMENT_TEMPLATE_ID && 
               schema_id == SBEConstants::TOPIC_SCHEMA_ID;
    }

    /**
     * @brief Check if this is an ORDER_MESSAGE (TopicMessage with order content)
     */
    bool is_order_message() const {
        // Check if it's a TopicMessage with order-related content
        if (is_topic_message()) {
            // Check message type for order-related keywords
            if (!message_type.empty() && 
                (message_type == "CREATE_ORDER" ||
                 message_type.find("ORDER") != std::string::npos ||
                 message_type.find("CREATE_ORDER") != std::string::npos ||
                 message_type.find("UPDATE_ORDER") != std::string::npos ||
                 message_type.find("CANCEL_ORDER") != std::string::npos)) {
                return true;
            }
            
            // Check payload for order-related content
            if (!payload.empty() && 
                (payload.find("order_details") != std::string::npos ||
                 payload.find("token_pair") != std::string::npos ||
                 payload.find("quantity") != std::string::npos ||
                 payload.find("side") != std::string::npos ||
                 payload.find("client_order_id") != std::string::npos)) {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Get human-readable description of the message
     */
    std::string get_description() const;
};

/**
 * @brief Comprehensive message parser for all SBE message types
 */
class MessageParser {
public:
    /**
     * @brief Parse any SBE message and return detailed result
     */
    static ParseResult parse_message(const std::uint8_t* data, std::size_t length);
    static ParseResult decode_topic_message_with_sbe(const uint8_t* data, size_t length);
    static ParseResult decode_acknowledgment_with_sbe(const uint8_t* data, size_t length);

    /**
     * @brief Parse message with detailed debugging output
     */
    static ParseResult parse_message_debug(
        const std::uint8_t* data,
        std::size_t length,
        const std::string& debug_prefix = "");

    /**
     * @brief Quick check for message type without full parsing
     */
    static std::string get_message_type(const std::uint8_t* data, std::size_t length);

    /**
     * @brief Extract just the correlation ID from a message
     */
    static std::int64_t extract_correlation_id(const std::uint8_t* data, std::size_t length);

    /**
     * @brief Check if message is likely an acknowledgment for given message ID
     */
    static bool is_acknowledgment_for(
        const std::uint8_t* data,
        std::size_t length,
        const std::string& message_id);

private:
    static ParseResult parse_session_event(const std::uint8_t* data, std::size_t length);
    static ParseResult parse_topic_message(const std::uint8_t* data, std::size_t length);
    static ParseResult parse_acknowledgment(const std::uint8_t* data, std::size_t length);
    static void add_debug_info(ParseResult& result, const std::uint8_t* data, std::size_t length);
};

} // namespace aeron_cluster