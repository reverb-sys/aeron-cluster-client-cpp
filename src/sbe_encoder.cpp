#include "aeron_cluster/sbe_messages.hpp"
#include "aeron_cluster/config.hpp"
#include <chrono>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <json/json.h>

namespace aeron_cluster {

// SBEEncoder implementation

std::vector<uint8_t> SBEEncoder::encodeSessionConnectRequest(
    int64_t correlationId,
    int32_t responseStreamId,
    const std::string& responseChannel,
    int32_t protocolVersion) {
    
    // Calculate total message size
    size_t totalSize = sizeof(MessageHeader) + 
                      SessionConnectRequest::sbeBlockLength() + 
                      4 + responseChannel.length(); // length prefix + channel data
    
    std::vector<uint8_t> buffer(totalSize);
    uint8_t* ptr = buffer.data();
    
    // Encode SBE header
    MessageHeader header;
    header.blockLength = SessionConnectRequest::sbeBlockLength();
    header.templateId = SessionConnectRequest::sbeTemplateId();
    header.schemaId = SessionConnectRequest::sbeSchemaId();
    header.version = SessionConnectRequest::sbeSchemaVersion();
    
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
    uint32_t channelLength = static_cast<uint32_t>(responseChannel.length());
    std::memcpy(ptr, &channelLength, sizeof(uint32_t));
    ptr += sizeof(uint32_t);
    
    std::memcpy(ptr, responseChannel.c_str(), responseChannel.length());
    
    return buffer;
}

std::vector<uint8_t> SBEEncoder::encodeTopicMessage(
    const std::string& topic,
    const std::string& messageType,
    const std::string& uuid,
    const std::string& payload,
    const std::string& headers,
    int64_t timestamp) {
    
    // Use current timestamp if not provided
    if (timestamp == 0) {
        timestamp = getCurrentTimestamp();
    }
    
    // Calculate variable field sizes
    std::vector<std::string> variableFields = {topic, messageType, uuid, payload, headers};
    size_t variableFieldsSize = calculateVariableFieldsSize(variableFields);
    
    // Calculate total message size
    size_t totalSize = sizeof(MessageHeader) + 
                      TopicMessage::sbeBlockLength() + 
                      variableFieldsSize;
    
    std::vector<uint8_t> buffer(totalSize);
    uint8_t* ptr = buffer.data();
    
    // Encode SBE header
    MessageHeader header;
    header.blockLength = TopicMessage::sbeBlockLength();
    header.templateId = TopicMessage::sbeTemplateId();
    header.schemaId = TopicMessage::sbeSchemaId();
    header.version = TopicMessage::sbeSchemaVersion();
    
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
        ptr += encodeVariableString(field, buffer, ptr - buffer.data()) - (ptr - buffer.data());
    }
    
    return buffer;
}

int64_t SBEEncoder::getCurrentTimestamp() {
    auto now = std::chrono::high_resolution_clock::now();
    return now.time_since_epoch().count();
}

size_t SBEEncoder::encodeVariableString(const std::string& str, 
                                       std::vector<uint8_t>& buffer, 
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

size_t SBEEncoder::calculateVariableFieldsSize(const std::vector<std::string>& fields) {
    size_t totalSize = 0;
    for (const auto& field : fields) {
        totalSize += sizeof(uint32_t) + field.length(); // length prefix + data
    }
    return totalSize;
}

// SBEDecoder implementation

bool SBEDecoder::decodeMessageHeader(const uint8_t* data, 
                                    size_t length, 
                                    MessageHeader& header) {
    if (!data || length < sizeof(MessageHeader)) {
        return false;
    }
    
    std::memcpy(&header, data, sizeof(MessageHeader));
    return true;
}

bool SBEDecoder::decodeSessionEvent(const uint8_t* data, 
                                   size_t length, 
                                   SessionEvent& event,
                                   std::string& detail) {
    if (!data || length < sizeof(MessageHeader) + SessionEvent::sbeBlockLength()) {
        return false;
    }
    
    // Verify header
    MessageHeader header;
    if (!decodeMessageHeader(data, length, header)) {
        return false;
    }
    
    if (!validateHeader(header, SessionEvent::sbeTemplateId(), SessionEvent::sbeSchemaId())) {
        return false;
    }
    
    // Decode fixed block
    const uint8_t* ptr = data + sizeof(MessageHeader);
    std::memcpy(&event, ptr, SessionEvent::sbeBlockLength());
    ptr += SessionEvent::sbeBlockLength();
    
    // Decode variable length detail if present
    size_t remaining = length - sizeof(MessageHeader) - SessionEvent::sbeBlockLength();
    if (remaining > 0) {
        extractVariableString(ptr, 0, remaining, detail);
    }
    
    return true;
}

bool SBEDecoder::decodeTopicMessage(const uint8_t* data,
                                   size_t length,
                                   std::string& topic,
                                   std::string& messageType,
                                   std::string& uuid,
                                   std::string& payload,
                                   std::string& headers,
                                   int64_t& timestamp) {
    if (!data || length < sizeof(MessageHeader) + TopicMessage::sbeBlockLength()) {
        return false;
    }
    
    // Verify header
    MessageHeader header;
    if (!decodeMessageHeader(data, length, header)) {
        return false;
    }
    
    if (!validateHeader(header, TopicMessage::sbeTemplateId(), TopicMessage::sbeSchemaId())) {
        return false;
    }
    
    // Decode fixed block
    const uint8_t* ptr = data + sizeof(MessageHeader);
    std::memcpy(&timestamp, ptr, sizeof(int64_t));
    ptr += TopicMessage::sbeBlockLength(); // Skip entire fixed block including padding
    
    // Decode variable length fields
    size_t remaining = length - sizeof(MessageHeader) - TopicMessage::sbeBlockLength();
    size_t offset = 0;
    
    // Extract fields in order: topic, messageType, uuid, payload, headers
    offset = extractVariableString(ptr, offset, remaining, topic);
    if (offset == 0) return false;
    
    offset = extractVariableString(ptr, offset, remaining, messageType);
    if (offset == 0) return false;
    
    offset = extractVariableString(ptr, offset, remaining, uuid);
    if (offset == 0) return false;
    
    offset = extractVariableString(ptr, offset, remaining, payload);
    if (offset == 0) return false;
    
    extractVariableString(ptr, offset, remaining, headers);
    
    return true;
}

bool SBEDecoder::decodeAcknowledgment(const uint8_t* data,
                                     size_t length,
                                     std::string& messageId,
                                     std::string& status,
                                     std::string& error,
                                     int64_t& timestamp) {
    if (!data || length < sizeof(MessageHeader) + Acknowledgment::sbeBlockLength()) {
        return false;
    }
    
    // Verify header
    MessageHeader header;
    if (!decodeMessageHeader(data, length, header)) {
        return false;
    }
    
    if (!validateHeader(header, Acknowledgment::sbeTemplateId(), Acknowledgment::sbeSchemaId())) {
        return false;
    }
    
    // Decode fixed block
    const uint8_t* ptr = data + sizeof(MessageHeader);
    std::memcpy(&timestamp, ptr, sizeof(int64_t));
    ptr += Acknowledgment::sbeBlockLength();
    
    // Decode variable length fields
    size_t remaining = length - sizeof(MessageHeader) - Acknowledgment::sbeBlockLength();
    size_t offset = 0;
    
    // Extract fields in order: messageId, status, error (optional)
    offset = extractVariableString(ptr, offset, remaining, messageId);
    if (offset == 0) return false;
    
    offset = extractVariableString(ptr, offset, remaining, status);
    if (offset == 0) return false;
    
    // Error field is optional
    if (offset < remaining) {
        extractVariableString(ptr, offset, remaining, error);
    }
    
    return true;
}

size_t SBEDecoder::extractVariableString(const uint8_t* data,
                                        size_t offset,
                                        size_t remainingLength,
                                        std::string& output) {
    if (offset + sizeof(uint32_t) > remainingLength) {
        return 0; // Not enough data for length prefix
    }
    
    const uint8_t* ptr = data + offset;
    
    // Read length prefix
    uint32_t length;
    std::memcpy(&length, ptr, sizeof(uint32_t));
    ptr += sizeof(uint32_t);
    offset += sizeof(uint32_t);
    
    // Sanity check
    if (length > remainingLength - sizeof(uint32_t) || length > 1024 * 1024) { // 1MB limit
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

bool SBEDecoder::validateHeader(const MessageHeader& header,
                               uint16_t expectedTemplateId,
                               uint16_t expectedSchemaId) {
    return header.templateId == expectedTemplateId && 
           header.schemaId == expectedSchemaId;
}

// SBEUtils implementation

namespace SBEUtils {

void printHexDump(const uint8_t* data, 
                  size_t length, 
                  const std::string& prefix,
                  size_t maxBytes) {
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
            if (j == 7) std::cout << " "; // Extra space in middle
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

std::string getSessionEventCodeString(int32_t code) {
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

std::string getMessageTypeName(uint16_t templateId, uint16_t schemaId) {
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

bool isValidCorrelationId(int64_t correlationId) {
    // Basic validation - should be positive and within reasonable range
    return correlationId > 0 && correlationId <= 0x7FFFFFFFFFFFFFFFLL;
}

int64_t generateCorrelationId() {
    auto now = std::chrono::high_resolution_clock::now();
    int64_t timestamp = now.time_since_epoch().count();
    
    // Use only lower 47 bits of timestamp to ensure positive value
    return timestamp & 0x7FFFFFFFFFFFFFFFLL;
}

std::string formatTimestamp(int64_t timestamp) {
    // Convert nanoseconds to time_point
    auto timePoint = std::chrono::time_point<std::chrono::system_clock>(
        std::chrono::nanoseconds(timestamp));
    auto time_t_val = std::chrono::system_clock::to_time_t(timePoint);
    
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_t_val), "%Y-%m-%d %H:%M:%S UTC");
    
    // Add nanoseconds
    auto nanos = timestamp % 1000000000;
    ss << "." << std::setfill('0') << std::setw(9) << nanos;
    
    return ss.str();
}

bool isValidSBEMessage(const uint8_t* data, size_t length) {
    if (!data || length < sizeof(MessageHeader)) {
        return false;
    }
    
    MessageHeader header;
    std::memcpy(&header, data, sizeof(MessageHeader));
    
    // Basic sanity checks
    if (header.blockLength == 0 || header.blockLength > 10000) {
        return false;
    }
    
    if (length < sizeof(MessageHeader) + header.blockLength) {
        return false;
    }
    
    // Check for known schema IDs
    if (header.schemaId != SBEConstants::CLUSTER_SCHEMA_ID && 
        header.schemaId != SBEConstants::TOPIC_SCHEMA_ID) {
        return false;
    }
    
    return true;
}

std::vector<std::string> extractReadableStrings(const uint8_t* data, 
                                                size_t length, 
                                                size_t minLength) {
    std::vector<std::string> strings;
    
    if (!data || length == 0) {
        return strings;
    }
    
    std::string current;
    for (size_t i = 0; i < length; ++i) {
        char c = static_cast<char>(data[i]);
        if (c >= 32 && c <= 126) { // Printable ASCII
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

} // namespace SBEUtils

// ParseResult implementation

std::string ParseResult::getDescription() const {
    std::stringstream ss;
    
    if (success) {
        ss << SBEUtils::getMessageTypeName(templateId, schemaId);
        
        if (isSessionEvent()) {
            ss << " (code: " << SBEUtils::getSessionEventCodeString(eventCode) << ")";
        } else if (!messageType.empty()) {
            ss << " (type: " << messageType << ")";
        }
        
        if (!messageId.empty()) {
            ss << " [ID: " << messageId.substr(0, 8) << "...]";
        }
    } else {
        ss << "Parse Error: " << errorMessage;
    }
    
    return ss.str();
}

// MessageParser implementation

ParseResult MessageParser::parseMessage(const uint8_t* data, size_t length) {
    ParseResult result;
    
    if (!data || length == 0) {
        result.errorMessage = "Null or empty data";
        return result;
    }
    
    // Decode header first
    if (!SBEDecoder::decodeMessageHeader(data, length, reinterpret_cast<MessageHeader&>(result))) {
        result.errorMessage = "Failed to decode message header";
        return result;
    }
    
    // Extract header fields
    MessageHeader header;
    std::memcpy(&header, data, sizeof(MessageHeader));
    result.templateId = header.templateId;
    result.schemaId = header.schemaId;
    result.version = header.version;
    result.blockLength = header.blockLength;
    
    // Parse based on message type
    if (result.isSessionEvent()) {
        return parseSessionEvent(data, length);
    } else if (result.isTopicMessage()) {
        return parseTopicMessage(data, length);
    } else if (result.isAcknowledgment()) {
        return parseAcknowledgment(data, length);
    } else {
        result.errorMessage = "Unknown message type: template=" + std::to_string(result.templateId) + 
                             ", schema=" + std::to_string(result.schemaId);
        return result;
    }
}

ParseResult MessageParser::parseMessageDebug(const uint8_t* data, 
                                            size_t length,
                                            const std::string& debugPrefix) {
    std::cout << debugPrefix << "📋 Parsing message (" << length << " bytes)" << std::endl;
    
    if (length > 0 && length <= 200) {
        std::cout << debugPrefix << "📋 Hex dump:" << std::endl;
        SBEUtils::printHexDump(data, length, debugPrefix + "  ");
    }
    
    ParseResult result = parseMessage(data, length);
    
    std::cout << debugPrefix << "📋 Parse result: " << (result.success ? "SUCCESS" : "FAILED") << std::endl;
    std::cout << debugPrefix << "📋 Description: " << result.getDescription() << std::endl;
    
    if (!result.success) {
        std::cout << debugPrefix << "📋 Error: " << result.errorMessage << std::endl;
        
        // Try to extract readable strings for debugging
        auto strings = SBEUtils::extractReadableStrings(data, length, 3);
        if (!strings.empty()) {
            std::cout << debugPrefix << "📋 Readable strings found:" << std::endl;
            for (const auto& str : strings) {
                std::cout << debugPrefix << "  \"" << str << "\"" << std::endl;
            }
        }
    }
    
    return result;
}

std::string MessageParser::getMessageType(const uint8_t* data, size_t length) {
    if (!data || length < sizeof(MessageHeader)) {
        return "INVALID";
    }
    
    MessageHeader header;
    std::memcpy(&header, data, sizeof(MessageHeader));
    
    return SBEUtils::getMessageTypeName(header.templateId, header.schemaId);
}

int64_t MessageParser::extractCorrelationId(const uint8_t* data, size_t length) {
    if (!data || length < sizeof(MessageHeader) + sizeof(int64_t)) {
        return 0;
    }
    
    MessageHeader header;
    std::memcpy(&header, data, sizeof(MessageHeader));
    
    // Correlation ID is typically the first field after header for cluster messages
    if (header.schemaId == SBEConstants::CLUSTER_SCHEMA_ID) {
        int64_t correlationId;
        std::memcpy(&correlationId, data + sizeof(MessageHeader), sizeof(int64_t));
        return correlationId;
    }
    
    return 0;
}

bool MessageParser::isAcknowledgmentFor(const uint8_t* data, 
                                       size_t length, 
                                       const std::string& messageId) {
    ParseResult result = parseMessage(data, length);
    
    if (!result.success || !result.isAcknowledgment()) {
        return false;
    }
    
    // Check if the acknowledgment contains our message ID
    return result.messageId == messageId || 
           result.payload.find(messageId) != std::string::npos ||
           result.headers.find(messageId) != std::string::npos;
}

ParseResult MessageParser::parseSessionEvent(const uint8_t* data, size_t length) {
    ParseResult result;
    
    SessionEvent event;
    std::string detail;
    
    if (!SBEDecoder::decodeSessionEvent(data, length, event, detail)) {
        result.errorMessage = "Failed to decode SessionEvent";
        return result;
    }
    
    // Fill result structure
    result.success = true;
    result.messageType = "SessionEvent";
    result.correlationId = event.correlationId;
    result.sessionId = event.clusterSessionId;
    result.leaderMemberId = event.leaderMemberId;
    result.eventCode = event.code;
    result.payload = detail;
    result.timestamp = 0; // SessionEvent doesn't have a timestamp field
    
    // Extract header info
    MessageHeader header;
    std::memcpy(&header, data, sizeof(MessageHeader));
    result.templateId = header.templateId;
    result.schemaId = header.schemaId;
    result.version = header.version;
    result.blockLength = header.blockLength;
    
    return result;
}

ParseResult MessageParser::parseTopicMessage(const uint8_t* data, size_t length) {
    ParseResult result;
    
    std::string topic, messageType, uuid, payload, headers;
    int64_t timestamp;
    
    if (!SBEDecoder::decodeTopicMessage(data, length, topic, messageType, uuid, payload, headers, timestamp)) {
        result.errorMessage = "Failed to decode TopicMessage";
        return result;
    }
    
    // Fill result structure
    result.success = true;
    result.messageType = messageType;
    result.messageId = uuid;
    result.payload = payload;
    result.headers = headers;
    result.timestamp = timestamp;
    
    // Extract header info
    MessageHeader header;
    std::memcpy(&header, data, sizeof(MessageHeader));
    result.templateId = header.templateId;
    result.schemaId = header.schemaId;
    result.version = header.version;
    result.blockLength = header.blockLength;
    
    return result;
}

ParseResult MessageParser::parseAcknowledgment(const uint8_t* data, size_t length) {
    ParseResult result;
    
    std::string messageId, status, error;
    int64_t timestamp;
    
    if (!SBEDecoder::decodeAcknowledgment(data, length, messageId, status, error, timestamp)) {
        result.errorMessage = "Failed to decode Acknowledgment";
        return result;
    }
    
    // Fill result structure
    result.success = true;
    result.messageType = "Acknowledgment";
    result.messageId = messageId;
    result.payload = status;
    result.headers = error; // Use headers field for error message
    result.timestamp = timestamp;
    
    // Extract header info
    MessageHeader header;
    std::memcpy(&header, data, sizeof(MessageHeader));
    result.templateId = header.templateId;
    result.schemaId = header.schemaId;
    result.version = header.version;
    result.blockLength = header.blockLength;
    
    return result;
}

void MessageParser::addDebugInfo(ParseResult& result, const uint8_t* data, size_t length) {
    // This function could add additional debugging information to the result
    // For now, it's a placeholder for future debugging enhancements
    (void)result; // Suppress unused parameter warning
    (void)data;
    (void)length;
}

} // namespace aeron_cluster