#pragma once

#include "aeron_cluster/config.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aeron_cluster {

/**
 * @brief SBE Message Header (8 bytes, packed)
 *
 * Standard SBE message header used by all Aeron Cluster messages.
 * Must be in little-endian format.
 */
struct MessageHeader {
    uint16_t blockLength;  // Length of the fixed-size message block
    uint16_t templateId;   // Message template identifier
    uint16_t schemaId;     // SBE schema identifier
    uint16_t version;      // Schema version
} __attribute__((packed));

static_assert(sizeof(MessageHeader) == 8, "MessageHeader must be exactly 8 bytes");

/**
 * @brief Session Connect Request message (Template ID 3)
 *
 * Sent by client to initiate a cluster session.
 * Fixed block is 16 bytes, followed by variable-length response channel.
 */
struct SessionConnectRequest {
    int64_t correlationId;     // Correlation ID for matching response
    int32_t responseStreamId;  // Stream ID for client responses
    int32_t version;           // Protocol semantic version (not schema version!)

    // Variable length response channel follows

    static constexpr uint16_t sbeBlockLength() {
        return 16;
    }
    static constexpr uint16_t sbeTemplateId() {
        return 3;
    }
    static constexpr uint16_t sbeSchemaId() {
        return 111;
    }
    static constexpr uint16_t sbeSchemaVersion() {
        return 8;
    }
} __attribute__((packed));

static_assert(sizeof(SessionConnectRequest) == 16,
              "SessionConnectRequest must be exactly 16 bytes");

/**
 * @brief Session Event message (Template ID 2)
 *
 * Response from cluster containing session establishment result.
 * Fixed block is 32 bytes, followed by optional variable-length detail.
 */
struct SessionEvent {
    int64_t correlationId;     // Matching correlation ID from request
    int64_t clusterSessionId;  // Assigned session ID
    int64_t leadershipTermId;  // Current leadership term
    int32_t leaderMemberId;    // Current leader member ID
    int32_t code;              // Event code (0=OK, 1=ERROR, 3=REDIRECT, 4=CLOSED)

    // Variable length detail message follows

    static constexpr uint16_t sbeBlockLength() {
        return 32;
    }
    static constexpr uint16_t sbeTemplateId() {
        return 2;
    }
    static constexpr uint16_t sbeSchemaId() {
        return 111;
    }
    static constexpr uint16_t sbeSchemaVersion() {
        return 8;
    }
} __attribute__((packed));

static_assert(sizeof(SessionEvent) == 32, "SessionEvent must be exactly 32 bytes");

/**
 * @brief Topic Message for business data (Template ID 1)
 *
 * Used for sending business messages like orders to the cluster.
 * Fixed block is 48 bytes, followed by variable-length fields.
 */
struct TopicMessage {
    int64_t timestamp;  // Message timestamp
    // 40 bytes of padding/reserved space to reach 48 bytes
    uint8_t reserved[40];

    // Variable length fields follow in order:
    // - topic (length-prefixed string)
    // - messageType (length-prefixed string)
    // - uuid (length-prefixed string)
    // - payload (length-prefixed string)
    // - headers (length-prefixed string)

    static constexpr uint16_t sbeBlockLength() {
        return 24;
    }
    static constexpr uint16_t sbeTemplateId() {
        return 1;
    }
    static constexpr uint16_t sbeSchemaId() {
        return 1;
    }
    static constexpr uint16_t sbeSchemaVersion() {
        return 8;
    }
} __attribute__((packed));

static_assert(sizeof(TopicMessage) == 48, "TopicMessage must be exactly 48 bytes");

/**
 * @brief Acknowledgment message for responses (Template ID 2, Schema ID 1)
 *
 * Response message containing acknowledgment of received business messages.
 */
struct Acknowledgment {
    int64_t timestamp;  // Acknowledgment timestamp

    // Variable length fields follow:
    // - messageId (length-prefixed string)
    // - status (length-prefixed string)
    // - error (length-prefixed string, optional)

    static constexpr uint16_t sbeBlockLength() {
        return 8;
    }
    static constexpr uint16_t sbeTemplateId() {
        return 2;
    }
    static constexpr uint16_t sbeSchemaId() {
        return 1;
    }
    static constexpr uint16_t sbeSchemaVersion() {
        return 1;
    }
} __attribute__((packed));

static_assert(sizeof(Acknowledgment) == 8, "Acknowledgment must be exactly 8 bytes");

/**
 * @brief SBE Encoder class for creating properly formatted messages
 *
 * Handles the encoding of C++ structures into SBE binary format
 * with proper byte ordering and field alignment.
 */
class SBEEncoder {
   public:
    /**
     * @brief Encode a SessionConnectRequest message
     *
     * @param correlationId Correlation ID for matching responses
     * @param responseStreamId Stream ID for receiving responses
     * @param responseChannel Channel string for responses
     * @param protocolVersion Protocol semantic version
     * @return Encoded message as byte vector
     */
    static std::vector<uint8_t> encodeSessionConnectRequest(int64_t correlationId,
                                                            int32_t responseStreamId,
                                                            const std::string& responseChannel,
                                                            int32_t protocolVersion = 1);

    /**
     * @brief Encode a TopicMessage for business data
     *
     * @param topic Topic name (e.g., "orders")
     * @param messageType Message type (e.g., "CREATE_ORDER")
     * @param uuid Message UUID for tracking
     * @param payload JSON payload string
     * @param headers JSON headers string
     * @param timestamp Message timestamp (0 = current time)
     * @return Encoded message as byte vector
     */
    static std::vector<uint8_t> encodeTopicMessage(
        const std::string& topic, const std::string& messageType, const std::string& uuid,
        const std::string& payload, const std::string& headers, int64_t timestamp = 0);

    static std::vector<uint8_t> encodeSessionKeepAlive(int64_t leadershipTermId,
                                                    int64_t clusterSessionId);

    static void writeUint16(std::vector<uint8_t>& buffer, size_t offset, uint16_t value) {
        buffer[offset] = static_cast<uint8_t>(value & 0xFF);
        buffer[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    }

    // static void SBEDecoder::writeInt64(std::vector<uint8_t>& buffer, size_t offset, int64_t
    // value) {

    static void writeInt64(std::vector<uint8_t>& buffer, size_t offset, int64_t value) {
        for (int i = 0; i < 8; ++i) {
            buffer[offset + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
        }
    }
    /**
     * @brief Get current timestamp in nanoseconds since epoch
     * @return Current timestamp suitable for SBE messages
     */
    static int64_t getCurrentTimestamp();

   private:
    /**
     * @brief Encode variable length string with length prefix
     * @param str String to encode
     * @param buffer Buffer to write to
     * @param offset Current offset in buffer
     * @return New offset after encoding
     */
    static size_t encodeVariableString(const std::string& str, std::vector<uint8_t>& buffer,
                                       size_t offset);

    /**
     * @brief Calculate total size needed for variable length fields
     * @param fields Vector of strings to encode
     * @return Total size in bytes including length prefixes
     */
    static size_t calculateVariableFieldsSize(const std::vector<std::string>& fields);
};

/**
 * @brief SBE Decoder class for parsing received messages
 *
 * Handles the decoding of SBE binary messages back into C++ structures
 * with proper error handling and validation.
 */
class SBEDecoder {
   public:
    /**
     * @brief Decode and validate message header
     *
     * @param data Raw message data
     * @param length Message length
     * @param header Output header structure
     * @return true if header is valid, false otherwise
     */
    static bool decodeMessageHeader(const uint8_t* data, size_t length, MessageHeader& header);

    /**
     * @brief Decode SessionEvent message
     *
     * @param data Raw message data (including header)
     * @param length Message length
     * @param event Output event structure
     * @param detail Output detail string (if present)
     * @return true if successfully decoded, false otherwise
     */
    static bool decodeSessionEvent(const uint8_t* data, size_t length, SessionEvent& event,
                                   std::string& detail);

    /**
     * @brief Decode TopicMessage
     *
     * @param data Raw message data (including header)
     * @param length Message length
     * @param topic Output topic string
     * @param messageType Output message type string
     * @param uuid Output UUID string
     * @param payload Output payload string
     * @param headers Output headers string
     * @param timestamp Output timestamp
     * @return true if successfully decoded, false otherwise
     */
    static bool decodeTopicMessage(const uint8_t* data, size_t length, std::string& topic,
                                   std::string& messageType, std::string& uuid,
                                   std::string& payload, std::string& headers, int64_t& timestamp);

    /**
     * @brief Decode Acknowledgment message
     *
     * @param data Raw message data (including header)
     * @param length Message length
     * @param messageId Output message ID
     * @param status Output status string
     * @param error Output error string (if present)
     * @param timestamp Output timestamp
     * @return true if successfully decoded, false otherwise
     */
    static bool decodeAcknowledgment(const uint8_t* data, size_t length, std::string& messageId,
                                     std::string& status, std::string& error, int64_t& timestamp);

    /**
     * @brief Extract variable length string from message
     *
     * @param data Message data
     * @param offset Current offset in message
     * @param remainingLength Remaining message length
     * @param output Output string
     * @return New offset after extraction, or 0 on error
     */
    static size_t extractVariableString(const uint8_t* data, size_t offset, size_t remainingLength,
                                        std::string& output);

    /**
     * @brief Validate message header fields
     *
     * @param header Header to validate
     * @param expectedTemplateId Expected template ID
     * @param expectedSchemaId Expected schema ID
     * @return true if header is valid for expected message type
     */
    static bool validateHeader(const MessageHeader& header, uint16_t expectedTemplateId,
                               uint16_t expectedSchemaId);

};

/**
 * @brief Utility functions for SBE message handling
 */
namespace SBEUtils {

/**
 * @brief Print hex dump of binary data for debugging
 *
 * @param data Binary data to dump
 * @param length Length of data
 * @param prefix Prefix for each line
 * @param maxBytes Maximum bytes to dump (0 = all)
 */
void printHexDump(const uint8_t* data, size_t length, const std::string& prefix = "",
                  size_t maxBytes = 0);

/**
 * @brief Get human-readable string for session event code
 *
 * @param code Session event code
 * @return Human-readable description
 */
std::string getSessionEventCodeString(int32_t code);

/**
 * @brief Get human-readable string for template ID
 *
 * @param templateId SBE template ID
 * @param schemaId SBE schema ID
 * @return Human-readable message type name
 */
std::string getMessageTypeName(uint16_t templateId, uint16_t schemaId);

/**
 * @brief Validate correlation ID format
 *
 * @param correlationId Correlation ID to validate
 * @return true if format is valid
 */
bool isValidCorrelationId(int64_t correlationId);

/**
 * @brief Generate a unique correlation ID
 *
 * @return New correlation ID suitable for session requests
 */
int64_t generateCorrelationId();

/**
 * @brief Convert timestamp to human-readable string
 *
 * @param timestamp Timestamp in nanoseconds since epoch
 * @return Formatted timestamp string
 */
std::string formatTimestamp(int64_t timestamp);

/**
 * @brief Check if message data appears to be valid SBE format
 *
 * @param data Message data
 * @param length Message length
 * @return true if data looks like valid SBE message
 */
bool isValidSBEMessage(const uint8_t* data, size_t length);

/**
 * @brief Extract all readable strings from binary data
 *
 * Utility function for debugging - extracts any printable ASCII
 * strings from binary message data.
 *
 * @param data Binary data
 * @param length Data length
 * @param minLength Minimum string length to extract
 * @return Vector of extracted strings
 */
std::vector<std::string> extractReadableStrings(const uint8_t* data, size_t length,
                                                size_t minLength = 3);

}  // namespace SBEUtils

/**
 * @brief Message parsing result with detailed information
 */
struct ParseResult {
    bool success = false;
    std::string errorMessage;
    std::string messageType;
    std::string messageId;
    std::string payload;
    std::string headers;
    int64_t timestamp = 0;

    // Header information
    uint16_t templateId = 0;
    uint16_t schemaId = 0;
    uint16_t version = 0;
    uint16_t blockLength = 0;

    // Session-specific fields
    int64_t correlationId = 0;
    int64_t sessionId = 0;
    int32_t leaderMemberId = 0;
    int32_t eventCode = 0;
    int64_t leadershipTermId = 0;  // For session events

    /**
     * @brief Check if this is a session event message
     */
    bool isSessionEvent() const {
        return templateId == 2 && schemaId == 111;
    }

    /**
     * @brief Check if this is a topic message
     */
    bool isTopicMessage() const {
        return templateId == 1 && schemaId == 1;
    }

    /**
     * @brief Check if this is an acknowledgment
     */
    bool isAcknowledgment() const {
        return templateId == 2 && schemaId == 1;
    }

    /**
     * @brief Get human-readable description of the message
     */
    std::string getDescription() const;
};

/**
 * @brief Comprehensive message parser for all SBE message types
 *
 * This class provides a high-level interface for parsing any SBE message
 * received from the Aeron Cluster, automatically detecting the message type
 * and extracting relevant fields.
 */
class MessageParser {
   public:
    /**
     * @brief Parse any SBE message and return detailed result
     *
     * @param data Raw message data
     * @param length Message length
     * @return ParseResult with all extracted information
     */
    static ParseResult parseMessage(const uint8_t* data, size_t length);

    /**
     * @brief Parse message with detailed debugging output
     *
     * Same as parseMessage but also outputs detailed debugging information
     * including hex dumps and field-by-field parsing details.
     *
     * @param data Raw message data
     * @param length Message length
     * @param debugPrefix Prefix for debug output lines
     * @return ParseResult with all extracted information
     */
    static ParseResult parseMessageDebug(const uint8_t* data, size_t length,
                                         const std::string& debugPrefix = "");

    /**
     * @brief Quick check for message type without full parsing
     *
     * @param data Raw message data
     * @param length Message length
     * @return Message type string or "UNKNOWN"
     */
    static std::string getMessageType(const uint8_t* data, size_t length);

    /**
     * @brief Extract just the correlation ID from a message
     *
     * Useful for quickly checking if a message belongs to this client.
     *
     * @param data Raw message data
     * @param length Message length
     * @return Correlation ID, or 0 if not found/applicable
     */
    static int64_t extractCorrelationId(const uint8_t* data, size_t length);

    /**
     * @brief Check if message is likely an acknowledgment for given message ID
     *
     * @param data Raw message data
     * @param length Message length
     * @param messageId Message ID to check for
     * @return true if this appears to be an ACK for the given message
     */
    static bool isAcknowledgmentFor(const uint8_t* data, size_t length,
                                    const std::string& messageId);

   private:
    static ParseResult parseSessionEvent(const uint8_t* data, size_t length);
    static ParseResult parseTopicMessage(const uint8_t* data, size_t length);
    static ParseResult parseAcknowledgment(const uint8_t* data, size_t length);
    static void addDebugInfo(ParseResult& result, const uint8_t* data, size_t length);
};

}  // namespace aeron_cluster