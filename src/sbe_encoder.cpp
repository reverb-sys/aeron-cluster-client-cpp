#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "aeron_cluster/config.hpp"
#include "aeron_cluster/sbe_messages.hpp"

namespace aeron_cluster {

// SBEEncoder implementation
void write_uint_16(std::vector<uint8_t>& buffer, size_t offset, uint16_t value) {
    buffer[offset] = static_cast<uint8_t>(value & 0xFF);
    buffer[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

// static void SBEDecoder::write_int_64(std::vector<uint8_t>& buffer, size_t offset, int64_t
// value) {

void write_int_64(std::vector<uint8_t>& buffer, size_t offset, int64_t value) {
    for (int i = 0; i < 8; ++i) {
        buffer[offset + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
    }
}

std::vector<uint8_t> SBEEncoder::encode_session_connect_request(int64_t correlationId,
                                                                int32_t responseStreamId,
                                                                const std::string& responseChannel,
                                                                int32_t protocolVersion) {
    std::vector<uint8_t> responseChannelBytes(responseChannel.begin(), responseChannel.end());

    // Calculate total message size
    size_t totalSize = sizeof(MessageHeader) + SessionConnectRequest::sbe_block_length() + 4 +
                       responseChannelBytes.size();  // length prefix + channel data

    std::vector<uint8_t> buffer(totalSize);
    uint8_t* ptr = buffer.data();

    // Encode SBE header
    MessageHeader header;
    header.block_length = SessionConnectRequest::sbe_block_length();
    header.template_id = SessionConnectRequest::sbe_template_id();
    header.schema_id = SessionConnectRequest::sbe_schema_id();
    header.version = SessionConnectRequest::sbe_schema_version();

    std::memcpy(ptr, &header, sizeof(MessageHeader));
    ptr += sizeof(MessageHeader);

    // Encode fixed block
    std::memcpy(ptr, &correlationId, sizeof(int64_t));
    ptr += sizeof(int64_t);

    std::memcpy(ptr, &responseStreamId, sizeof(int32_t));
    ptr += sizeof(int32_t);

    std::memcpy(ptr, &protocolVersion, sizeof(int32_t));
    ptr += sizeof(int32_t);

    // Encode variable length response channel
    uint32_t channelLength = static_cast<uint32_t>(responseChannelBytes.size());
    std::memcpy(ptr, &channelLength, sizeof(uint32_t));
    ptr += sizeof(uint32_t);

    std::memcpy(ptr, responseChannelBytes.data(), responseChannelBytes.size());

    return buffer;
}

std::vector<uint8_t> SBEEncoder::encode_session_keep_alive(int64_t leadershipTermId,
                                                           int64_t clusterSessionId) {
    // This should match the Go code's keepalive message structure
    // Based on the Go code: SessionKeepAliveTemplateId with 16 bytes block length

    std::vector<uint8_t> message;
    message.resize(SBEConstants::SBE_HEADER_LENGTH + 16);  // Header + keepalive data

    size_t offset = 0;

    // SBE Header
    write_uint_16(message, offset, 16);  // block length
    offset += 2;
    write_uint_16(message, offset, SBEConstants::SESSION_KEEPALIVE_TEMPLATE_ID);  // template ID
    offset += 2;
    write_uint_16(message, offset, SBEConstants::CLUSTER_SCHEMA_ID);  // schema ID
    offset += 2;
    write_uint_16(message, offset, SBEConstants::CLUSTER_SCHEMA_VERSION);  // schema version
    offset += 2;

    // Keepalive data (16 bytes)
    write_int_64(message, offset, leadershipTermId);
    offset += 8;
    write_int_64(message, offset, clusterSessionId);
    offset += 8;

    return message;
}

std::vector<uint8_t> SBEEncoder::encode_topic_message(
    const std::string& topic, const std::string& messageType, const std::string& uuid,
    const std::string& payload, const std::string& headers, int64_t timestamp) {
    // Use current timestamp if not provided
    if (timestamp == 0) {
        timestamp = get_current_timestamp();
    }

    // Calculate variable field sizes
    std::vector<std::string> variableFields = {topic, messageType, uuid, payload, headers};
    size_t variableFieldsSize = calculate_variable_fields_size(variableFields);

    // Calculate total message size
    size_t totalSize =
        sizeof(MessageHeader) + TopicMessage::sbe_block_length() + variableFieldsSize;

    std::vector<uint8_t> buffer(totalSize);
    uint8_t* ptr = buffer.data();

    // Encode SBE header
    MessageHeader header;
    header.block_length = TopicMessage::sbe_block_length();
    header.template_id = TopicMessage::sbe_template_id();
    header.schema_id = TopicMessage::sbe_schema_id();
    header.version = TopicMessage::sbe_schema_version();

    std::memcpy(ptr, &header, sizeof(MessageHeader));
    ptr += sizeof(MessageHeader);

    // Encode fixed block (timestamp + padding)
    std::memcpy(ptr, &timestamp, sizeof(int64_t));
    ptr += sizeof(int64_t);

    // Pad remaining fixed block space (48 - 8 = 40 bytes)
    std::memset(ptr, 0, 40);
    ptr += 40;

    // Encode variable length fields in order
    for (const auto& field : variableFields) {
        ptr += encode_variable_string(field, buffer, ptr - buffer.data()) - (ptr - buffer.data());
    }

    return buffer;
}

int64_t SBEEncoder::get_current_timestamp() {
    auto now = std::chrono::high_resolution_clock::now();
    return now.time_since_epoch().count();
}

size_t SBEEncoder::encode_variable_string(const std::string& str, std::vector<uint8_t>& buffer,
                                          size_t offset) {
    uint8_t* ptr = buffer.data() + offset;

    // Encode length prefix
    uint32_t length = static_cast<uint32_t>(str.length());
    std::memcpy(ptr, &length, sizeof(uint32_t));
    ptr += sizeof(uint32_t);

    // Encode string data
    std::memcpy(ptr, str.c_str(), str.length());
    ptr += str.length();

    return ptr - buffer.data();
}

size_t SBEEncoder::calculate_variable_fields_size(const std::vector<std::string>& fields) {
    size_t totalSize = 0;
    for (const auto& field : fields) {
        totalSize += sizeof(uint32_t) + field.length();  // length prefix + data
    }
    return totalSize;
}

// SBEDecoder implementation

bool SBEDecoder::decode_message_header(const uint8_t* data, size_t length, MessageHeader& header) {
    if (!data || length < sizeof(MessageHeader)) {
        return false;
    }

    std::memcpy(&header, data, sizeof(MessageHeader));
    return true;
}

bool SBEDecoder::decode_session_event(const uint8_t* data, size_t length, SessionEvent& event,
                                      std::string& detail) {
    if (!data || length < sizeof(MessageHeader) + SessionEvent::sbe_block_length()) {
        return false;
    }

    // Show the data in str format for debugging
    // Show the data in str format for debugging
    // Show raw binary data with better formatting
    std::cout << "Raw data analysis (" << length << " bytes):" << std::endl;
    
    // Show hex dump for first 64 bytes
    std::cout << "Hex dump: ";
    for (size_t i = 0; i < length && i < 64; ++i) {
        std::cout << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(data[i]) << " ";
        if ((i + 1) % 16 == 0) std::cout << std::endl << "          ";
    }
    std::cout << std::dec << std::endl;
    
    // Try to extract any readable strings
    std::cout << "Readable content: ";
    std::string readable;
    for (size_t i = 0; i < length && i < 256; ++i) {
        char c = static_cast<char>(data[i]);
        if (c >= 32 && c <= 126) {
            readable += c;
        } else if (!readable.empty()) {
            if (readable.length() >= 3) {
                std::cout << "\"" << readable << "\" ";
            }
            readable.clear();
        }
    }
    if (readable.length() >= 3) {
        std::cout << "\"" << readable << "\"";
    }
    std::cout << std::endl;

    // Verify header
    MessageHeader header;
    if (!decode_message_header(data, length, header)) {
        return false;
    }

    if (!validate_header(header, SessionEvent::sbe_template_id(), SessionEvent::sbe_schema_id())) {
        return false;
    }

    // Decode fixed block
    const uint8_t* ptr = data + sizeof(MessageHeader);

    std::memcpy(&event, ptr, SessionEvent::sbe_block_length());
    ptr += SessionEvent::sbe_block_length();

    // Decode variable length detail if present
    size_t remaining = length - sizeof(MessageHeader) - SessionEvent::sbe_block_length();
    if (remaining > 0) {
        extract_variable_string(ptr, 0, remaining, detail);
    }

    return true;
}

bool SBEDecoder::decode_topic_message(const uint8_t* data, size_t length, std::string& topic,
                                      std::string& messageType, std::string& uuid,
                                      std::string& payload, std::string& headers,
                                      int64_t& timestamp) {
    if (!data || length < sizeof(MessageHeader) + TopicMessage::sbe_block_length()) {
        return false;
    }

    // Verify header
    MessageHeader header;
    if (!decode_message_header(data, length, header)) {
        return false;
    }

    if (!validate_header(header, TopicMessage::sbe_template_id(), TopicMessage::sbe_schema_id())) {
        return false;
    }

    // Decode fixed block
    const uint8_t* ptr = data + sizeof(MessageHeader);
    std::memcpy(&timestamp, ptr, sizeof(int64_t));
    ptr += TopicMessage::sbe_block_length();  // Skip entire fixed block including padding

    // Decode variable length fields
    size_t remaining = length - sizeof(MessageHeader) - TopicMessage::sbe_block_length();
    size_t offset = 0;

    // Extract fields in order: topic, messageType, uuid, payload, headers
    offset = extract_variable_string(ptr, offset, remaining, topic);
    if (offset == 0)
        return false;

    offset = extract_variable_string(ptr, offset, remaining, messageType);
    if (offset == 0)
        return false;

    offset = extract_variable_string(ptr, offset, remaining, uuid);
    if (offset == 0)
        return false;

    offset = extract_variable_string(ptr, offset, remaining, payload);
    if (offset == 0)
        return false;

    extract_variable_string(ptr, offset, remaining, headers);

    return true;
}

bool SBEDecoder::decode_acknowledgment(const uint8_t* data, size_t length, std::string& messageId,
                                       std::string& status, std::string& error,
                                       int64_t& timestamp) {
    if (!data || length < sizeof(MessageHeader) + Acknowledgment::sbe_block_length()) {
        return false;
    }

    // Verify header
    MessageHeader header;
    if (!decode_message_header(data, length, header)) {
        return false;
    }

    if (!validate_header(header, Acknowledgment::sbe_template_id(),
                         Acknowledgment::sbe_schema_id())) {
        return false;
    }

    // Decode fixed block
    const uint8_t* ptr = data + sizeof(MessageHeader);
    std::memcpy(&timestamp, ptr, sizeof(int64_t));
    ptr += Acknowledgment::sbe_block_length();

    // Decode variable length fields
    size_t remaining = length - sizeof(MessageHeader) - Acknowledgment::sbe_block_length();
    size_t offset = 0;

    // Extract fields in order: messageId, status, error (optional)
    offset = extract_variable_string(ptr, offset, remaining, messageId);
    if (offset == 0)
        return false;

    offset = extract_variable_string(ptr, offset, remaining, status);
    if (offset == 0)
        return false;

    // Error field is optional
    if (offset < remaining) {
        extract_variable_string(ptr, offset, remaining, error);
    }

    return true;
}

size_t SBEDecoder::extract_variable_string(const uint8_t* data, size_t offset,
                                           size_t remainingLength, std::string& output) {
    if (offset + sizeof(uint32_t) > remainingLength) {
        return 0;  // Not enough data for length prefix
    }

    const uint8_t* ptr = data + offset;

    // Read length prefix
    uint32_t length;
    std::memcpy(&length, ptr, sizeof(uint32_t));
    ptr += sizeof(uint32_t);
    offset += sizeof(uint32_t);

    // Sanity check
    if (length > remainingLength - sizeof(uint32_t) || length > 1024 * 1024) {  // 1MB limit
        return 0;
    }

    // Extract string data
    if (length > 0) {
        output.assign(reinterpret_cast<const char*>(ptr), length);
        offset += length;
    } else {
        output.clear();
    }

    return offset;
}

bool SBEDecoder::validate_header(const MessageHeader& header, uint16_t expectedTemplateId,
                                 uint16_t expectedSchemaId) {
    return header.template_id == expectedTemplateId && header.schema_id == expectedSchemaId;
}

// SBEUtils implementation

namespace SBEUtils {

void print_hex_dump(const uint8_t* data, size_t length, const std::string& prefix, size_t maxBytes) {
    if (!data || length == 0) {
        return;
    }

    size_t bytesToPrint = (maxBytes > 0) ? std::min(length, maxBytes) : length;

    for (size_t i = 0; i < bytesToPrint; i += 16) {
        std::cout << prefix;

        // Print offset
        std::cout << std::setfill('0') << std::setw(4) << std::hex << i << ": ";

        // Print hex bytes
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < bytesToPrint) {
                std::cout << std::setfill('0') << std::setw(2) << std::hex
                          << static_cast<unsigned>(data[i + j]) << " ";
            } else {
                std::cout << "   ";
            }
            if (j == 7)
                std::cout << " ";  // Extra space in middle
        }

        std::cout << " |";

        // Print ASCII representation
        for (size_t j = 0; j < 16 && i + j < bytesToPrint; ++j) {
            char c = static_cast<char>(data[i + j]);
            std::cout << ((c >= 32 && c <= 126) ? c : '.');
        }

        std::cout << "|" << std::dec << std::endl;
    }

    if (maxBytes > 0 && length > maxBytes) {
        std::cout << prefix << "... (" << (length - maxBytes) << " more bytes)" << std::endl;
    }
}

std::string get_session_event_code_string(int32_t code) {
    switch (code) {
        case SBEConstants::SESSION_EVENT_OK:
            return "OK";
        case SBEConstants::SESSION_EVENT_ERROR:
            return "ERROR";
        case SBEConstants::SESSION_EVENT_AUTHENTICATION_REJECTED:
            return "AUTHENTICATION_REJECTED";
        case SBEConstants::SESSION_EVENT_REDIRECT:
            return "REDIRECT";
        case SBEConstants::SESSION_EVENT_CLOSED:
            return "CLOSED";
        default:
            return "UNKNOWN(" + std::to_string(code) + ")";
    }
}

std::string get_message_type_name(uint16_t templateId, uint16_t schemaId) {
    if (schemaId == SBEConstants::CLUSTER_SCHEMA_ID) {
        switch (templateId) {
            case SBEConstants::SESSION_CONNECT_TEMPLATE_ID:
                return "SessionConnectRequest";
            case SBEConstants::SESSION_EVENT_TEMPLATE_ID:
                return "SessionEvent";
            default:
                return "UnknownClusterMessage(" + std::to_string(templateId) + ")";
        }
    } else if (schemaId == SBEConstants::TOPIC_SCHEMA_ID) {
        switch (templateId) {
            case SBEConstants::TOPIC_MESSAGE_TEMPLATE_ID:
                return "TopicMessage";
            case SBEConstants::ACKNOWLEDGMENT_TEMPLATE_ID:
                return "Acknowledgment";
            default:
                return "UnknownTopicMessage(" + std::to_string(templateId) + ")";
        }
    } else {
        return "UnknownSchema(" + std::to_string(schemaId) + "," + std::to_string(templateId) + ")";
    }
}

bool is_valid_correlation_id(int64_t correlationId) {
    // Basic validation - should be positive and within reasonable range
    return correlationId > 0 && correlationId <= 0x7FFFFFFFFFFFFFFFLL;
}

int64_t generate_correlation_id() {
    auto now = std::chrono::high_resolution_clock::now();
    int64_t timestamp = now.time_since_epoch().count();

    // Use only lower 47 bits of timestamp to ensure positive value
    return timestamp & 0x7FFFFFFFFFFFFFFFLL;
}

std::string format_timestamp(int64_t timestamp) {
    // Convert nanoseconds to time_point
    auto timePoint =
        std::chrono::time_point<std::chrono::system_clock>(std::chrono::nanoseconds(timestamp));
    auto time_t_val = std::chrono::system_clock::to_time_t(timePoint);

    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_t_val), "%Y-%m-%d %H:%M:%S UTC");

    // Add nanoseconds
    auto nanos = timestamp % 1000000000;
    ss << "." << std::setfill('0') << std::setw(9) << nanos;

    return ss.str();
}

bool is_valid_sbe_message(const uint8_t* data, size_t length) {
    if (!data || length < sizeof(MessageHeader)) {
        return false;
    }

    MessageHeader header;
    std::memcpy(&header, data, sizeof(MessageHeader));

    // Basic sanity checks
    if (header.block_length == 0 || header.block_length > 10000) {
        return false;
    }

    if (length < sizeof(MessageHeader) + header.block_length) {
        return false;
    }

    // Check for known schema IDs
    if (header.schema_id != SBEConstants::CLUSTER_SCHEMA_ID &&
        header.schema_id != SBEConstants::TOPIC_SCHEMA_ID) {
        return false;
    }

    return true;
}

std::vector<std::string> extract_readable_strings(const uint8_t* data, size_t length,
                                                size_t minLength) {
    std::vector<std::string> strings;

    if (!data || length == 0) {
        return strings;
    }

    std::string current;
    for (size_t i = 0; i < length; ++i) {
        char c = static_cast<char>(data[i]);
        if (c >= 32 && c <= 126) {  // Printable ASCII
            current += c;
        } else {
            if (current.length() >= minLength) {
                strings.push_back(current);
            }
            current.clear();
        }
    }

    // Don't forget the last string
    if (current.length() >= minLength) {
        strings.push_back(current);
    }

    return strings;
}

}  // namespace SBEUtils

// ParseResult implementation

std::string ParseResult::get_description() const {
    std::stringstream ss;

    if (success) {
        ss << SBEUtils::get_message_type_name(template_id, schema_id);

        if (is_session_event()) {
            ss << " (code: " << SBEUtils::get_session_event_code_string(event_code) << ")";
        } else if (!message_type.empty()) {
            ss << " (type: " << message_type << ")";
        }

        if (!message_id.empty()) {
            ss << " [ID: " << message_id.substr(0, 8) << "...]";
        }
    } else {
        ss << "Parse Error: " << error_message;
    }

    return ss.str();
}

// MessageParser implementation

ParseResult MessageParser::parse_message(const uint8_t* data, size_t length) {
    ParseResult result;

    if (!data || length == 0) {
        result.error_message = "Null or empty data";
        return result;
    }

    // Decode header first
    if (!SBEDecoder::decode_message_header(data, length,
                                           reinterpret_cast<MessageHeader&>(result))) {
        result.error_message = "Failed to decode message header";
        return result;
    }

    // Extract header fields
    MessageHeader header;
    std::memcpy(&header, data, sizeof(MessageHeader));
    result.template_id = header.template_id;
    result.schema_id = header.schema_id;
    result.version = header.version;
    result.block_length = header.block_length;

    // Parse based on message type
    if (result.is_session_event()) {
        return parse_session_event(data, length);
    } else if (result.is_topic_message()) {
        return parse_topic_message(data, length);
    } else if (result.is_acknowledgment()) {
        return parse_acknowledgment(data, length);
    } else {
        result.error_message =
            "Unknown message type: template=" + std::to_string(result.template_id) +
            ", schema=" + std::to_string(result.schema_id);
        return result;
    }
}

ParseResult MessageParser::parse_message_debug(const uint8_t* data, size_t length,
                                               const std::string& debugPrefix) {
    std::cout << debugPrefix << "📋 Parsing message (" << length << " bytes)" << std::endl;

    if (length > 0 && length <= 200) {
        std::cout << debugPrefix << "📋 Hex dump:" << std::endl;
        SBEUtils::print_hex_dump(data, length, debugPrefix + "  ", 64);
    }

    ParseResult result = parse_message(data, length);

    std::cout << debugPrefix << "📋 Parse result: " << (result.success ? "SUCCESS" : "FAILED")
              << std::endl;
    std::cout << debugPrefix << "📋 Description: " << result.get_description() << std::endl;

    if (!result.success) {
        std::cout << debugPrefix << "📋 Error: " << result.error_message << std::endl;

        // Try to extract readable strings for debugging
        auto strings = SBEUtils::extract_readable_strings(data, length, 3);
        if (!strings.empty()) {
            std::cout << debugPrefix << "📋 Readable strings found:" << std::endl;
            for (const auto& str : strings) {
                std::cout << debugPrefix << "  \"" << str << "\"" << std::endl;
            }
        }
    }

    return result;
}

std::string MessageParser::get_message_type(const uint8_t* data, size_t length) {
    if (!data || length < sizeof(MessageHeader)) {
        return "INVALID";
    }

    MessageHeader header;
    std::memcpy(&header, data, sizeof(MessageHeader));

    return SBEUtils::get_message_type_name(header.template_id, header.schema_id);
}

int64_t MessageParser::extract_correlation_id(const uint8_t* data, size_t length) {
    if (!data || length < sizeof(MessageHeader) + sizeof(int64_t)) {
        return 0;
    }

    MessageHeader header;
    std::memcpy(&header, data, sizeof(MessageHeader));

    // Correlation ID is typically the first field after header for cluster messages
    if (header.schema_id == SBEConstants::CLUSTER_SCHEMA_ID) {
        int64_t correlationId;
        std::memcpy(&correlationId, data + sizeof(MessageHeader), sizeof(int64_t));
        return correlationId;
    }

    return 0;
}

bool MessageParser::is_acknowledgment_for(const uint8_t* data, size_t length,
                                          const std::string& message_id) {
    ParseResult result = parse_message(data, length);

    if (!result.success || !result.is_acknowledgment()) {
        return false;
    }

    // Check if the acknowledgment contains our message ID
    return result.message_id == message_id ||
           result.payload.find(message_id) != std::string::npos ||
           result.headers.find(message_id) != std::string::npos;
}

ParseResult MessageParser::parse_session_event(const uint8_t* data, size_t length) {
    ParseResult result;

    SessionEvent event;
    std::string detail;

    if (!SBEDecoder::decode_session_event(data, length, event, detail)) {
        result.error_message = "Failed to decode SessionEvent";
        return result;
    }

    // Fill result structure
    result.success = true;
    result.message_type = "SessionEvent";
    result.correlation_id = event.correlation_id;
    result.session_id = event.cluster_session_id;
    result.leader_member_id = event.leader_member_id;
    result.event_code = event.code;
    result.payload = detail;
    result.timestamp = 0;  // SessionEvent doesn't have a timestamp field

    // Extract header info
    MessageHeader header;
    std::memcpy(&header, data, sizeof(MessageHeader));
    result.template_id = header.template_id;
    result.schema_id = header.schema_id;
    result.version = header.version;
    result.block_length = header.block_length;

    return result;
}

ParseResult MessageParser::parse_topic_message(const uint8_t* data, size_t length) {
    ParseResult result;

    std::string topic, message_type, uuid, payload, headers;
    int64_t timestamp;

    if (!SBEDecoder::decode_topic_message(data, length, topic, message_type, uuid, payload, headers,
                                          timestamp)) {
        result.error_message = "Failed to decode TopicMessage";
        return result;
    }

    // Fill result structure
    result.success = true;
    result.message_type = message_type;
    result.message_id = uuid;
    result.payload = payload;
    result.headers = headers;
    result.timestamp = timestamp;

    // Extract header info
    MessageHeader header;
    std::memcpy(&header, data, sizeof(MessageHeader));
    result.template_id = header.template_id;
    result.schema_id = header.schema_id;
    result.version = header.version;
    result.block_length = header.block_length;
    // result.topic = topic; // Store topic for easy access
    return result;
}

ParseResult MessageParser::parse_acknowledgment(const uint8_t* data, size_t length) {
    ParseResult result;

    std::string message_id, status, error;
    int64_t timestamp;

    if (!SBEDecoder::decode_acknowledgment(data, length, message_id, status, error, timestamp)) {
        result.error_message = "Failed to decode Acknowledgment";
        return result;
    }

    // Fill result structure
    result.success = true;
    result.message_type = "Acknowledgment";
    result.message_id = message_id;
    result.payload = status;
    result.headers = error;  // Use headers field for error message
    result.timestamp = timestamp;

    // Extract header info
    MessageHeader header;
    std::memcpy(&header, data, sizeof(MessageHeader));
    result.template_id = header.template_id;
    result.schema_id = header.schema_id;
    result.version = header.version;
    result.block_length = header.block_length;
    // result.event_code = 0; // Acknowledgment doesn't have an event code
    return result;
}

void MessageParser::add_debug_info(ParseResult& result, const uint8_t* data, size_t length) {
    // This function could add additional debugging information to the result
    // For now, it's a placeholder for future debugging enhancements
    (void)result;  // Suppress unused parameter warning
    (void)data;
    (void)length;
}

}  // namespace aeron_cluster