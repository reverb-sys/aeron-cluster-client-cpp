#include "aeron_cluster/commit_manager.hpp"
#include "aeron_cluster/subscription.hpp"
#include "model/CommitOffsetLite.h"
#include "model/MessageHeader.h"
#include <json/json.h>
#include <iostream>
#include <sstream>
#include <memory>
#include <unordered_map>
#include <mutex>

namespace aeron_cluster {

namespace {
// Map canonical topic string to numeric topicId used by server
inline std::uint32_t topic_to_id(const std::string& topic) {
    if (topic == "order_request_topic") return 1;
    if (topic == "order_notification_topic") return 2;
    if (topic == "orders") return 3;
    if (topic == "order_status_request_topic") return 4;
    return 0; // unknown
}
} // namespace

CommitManager::CommitManager() = default;

CommitManager::~CommitManager() = default;

void CommitManager::commit_message(const std::string& topic, const std::string& message_identifier,
                                  const std::string& message_id, std::uint64_t timestamp_nanos, 
                                  std::uint64_t sequence_number) {
    std::lock_guard<std::mutex> lock(commits_mutex_);
    std::string commit_key = topic + ":" + message_identifier;
    commits_[commit_key] = CommitOffset(topic, message_identifier, message_id, timestamp_nanos, sequence_number);
}

std::shared_ptr<CommitOffset> CommitManager::get_last_commit(const std::string& topic, 
                                                           const std::string& message_identifier) const {
    std::lock_guard<std::mutex> lock(commits_mutex_);
    std::string commit_key = topic + ":" + message_identifier;
    auto it = commits_.find(commit_key);
    if (it != commits_.end()) {
        return std::make_shared<CommitOffset>(it->second);
    }
    return nullptr;
}

std::unordered_map<std::string, CommitOffset> CommitManager::get_all_commits() const {
    std::lock_guard<std::mutex> lock(commits_mutex_);
    return commits_;
}

void CommitManager::clear_commits(const std::string& topic) {
    std::lock_guard<std::mutex> lock(commits_mutex_);
    commits_.erase(topic);
}

void CommitManager::clear_all_commits() {
    std::lock_guard<std::mutex> lock(commits_mutex_);
    commits_.clear();
}

std::vector<std::uint8_t> CommitManager::build_commit_message(const std::string& topic, const std::string& client_id) const {
    // JSON payload: {"topic": "...", "action": "COMMIT", "client_id": "..."}
    std::string payload = std::string("{\"topic\":\"") + topic + "\","
                          "\"action\":\"COMMIT\","
                          "\"client_id\":\"" + client_id + "\"}";

    // Prepare buffer (oversized to avoid reallocs)
    std::vector<std::uint8_t> buf;
    buf.resize(8 /*header*/ + 16 /*timestamp + sequenceNumber*/ + 512);

    // Encode header
    sbe::MessageHeader hdr;
    hdr.wrap(reinterpret_cast<char*>(buf.data()), 0, SBE_VERSION, static_cast<std::uint64_t>(buf.size()));
    hdr.blockLength(TOPIC_MESSAGE_BLOCK_LENGTH);
    hdr.templateId(TOPIC_MESSAGE_TEMPLATE_ID);
    hdr.schemaId(SBE_SCHEMA_ID);
    hdr.version(SBE_VERSION);

    // Encode TopicMessage
    sbe::TopicMessage msg;
    msg.wrapForEncode(reinterpret_cast<char*>(buf.data()), 8, static_cast<std::uint64_t>(buf.size() - 8));

    msg.timestamp(now_nanos());
    msg.sequenceNumber(0); // Will be set by server, client sends 0

    msg.putTopic(TOPIC_SUBSCRIPTIONS, static_cast<std::uint16_t>(std::char_traits<char>::length(TOPIC_SUBSCRIPTIONS)));
    msg.putMessageType(MSGTYPE_COMMIT, static_cast<std::uint16_t>(std::char_traits<char>::length(MSGTYPE_COMMIT)));

    // UUID: commit_<client>_<ts>
    {
        std::string uuid = std::string("commit_") + client_id + "_" + std::to_string(now_nanos());
        msg.putUuid(uuid.c_str(), static_cast<std::uint16_t>(uuid.size()));
    }

    msg.putPayload(payload.data(), static_cast<std::uint16_t>(payload.size()));
    static const char emptyHeaders[] = "{}";
    msg.putHeaders(emptyHeaders, 2);

    // Compute encoded length
    const int encodedLen = 8 + msg.encodedLength();
    buf.resize(encodedLen);
    return buf;
}

std::vector<std::uint8_t> CommitManager::build_commit_offset_message(const std::string& topic, const std::string& client_id, 
                                                                    const CommitOffset& offset) const {
    // Prefer ultra-fast CommitOffsetLite SBE template
    const std::uint32_t topicId = topic_to_id(offset.topic);
    if (topicId != 0) {
        const std::string& messageId = offset.message_id;
        const std::string& messageIdentifier = offset.message_identifier;
        const std::size_t headerLen = sbe::MessageHeader::encodedLength();
        const std::size_t bodyLen = sbe::CommitOffsetLite::computeLength(messageId.size(), messageIdentifier.size());
        const std::size_t total = headerLen + bodyLen;
        
        std::vector<std::uint8_t> buf(total);
        char* ptr = reinterpret_cast<char*>(buf.data());
        
        sbe::CommitOffsetLite lite;
        lite.wrapAndApplyHeader(ptr, 0, buf.size());
        lite.topicId(topicId);
        lite.sequence(offset.sequence_number);
        lite.putMessageId(messageId.c_str(), static_cast<std::uint16_t>(messageId.size()));
        lite.putMessageIdentifier(messageIdentifier.c_str(), static_cast<std::uint16_t>(messageIdentifier.size()));
        
        // Resize to actual size (header + encoded body)
        const std::size_t encoded = headerLen + static_cast<std::size_t>(lite.encodedLength());
        buf.resize(encoded);
        return buf;
    }

    // Fallback: legacy TopicMessage with JSON payload (kept for backward compatibility)
    Json::Value commitMessage;
    commitMessage["action"] = "COMMIT_OFFSET";
    commitMessage["topic"] = offset.topic;
    commitMessage["message_id"] = offset.message_id;
    commitMessage["timestamp_nanos"] = static_cast<Json::UInt64>(offset.timestamp_nanos);
    commitMessage["sequence_number"] = static_cast<Json::UInt64>(offset.sequence_number);
    commitMessage["messageIdentifier"] = offset.message_identifier;

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    std::string messageJson = Json::writeString(builder, commitMessage);

    std::vector<std::uint8_t> buf;
    buf.resize(8 /*header*/ + 16 /*timestamp + sequenceNumber*/ + 1024);

    sbe::MessageHeader hdr;
    hdr.wrap(reinterpret_cast<char*>(buf.data()), 0, SBE_VERSION, static_cast<std::uint64_t>(buf.size()));
    hdr.blockLength(TOPIC_MESSAGE_BLOCK_LENGTH);
    hdr.templateId(TOPIC_MESSAGE_TEMPLATE_ID);
    hdr.schemaId(SBE_SCHEMA_ID);
    hdr.version(SBE_VERSION);

    sbe::TopicMessage msg;
    msg.wrapForEncode(reinterpret_cast<char*>(buf.data()), 8, static_cast<std::uint64_t>(buf.size() - 8));

    msg.timestamp(now_nanos());
    msg.sequenceNumber(0);
    msg.putTopic(TOPIC_SUBSCRIPTIONS, static_cast<std::uint16_t>(std::char_traits<char>::length(TOPIC_SUBSCRIPTIONS)));
    msg.putMessageType(MSGTYPE_COMMIT_OFFSET, static_cast<std::uint16_t>(std::char_traits<char>::length(MSGTYPE_COMMIT_OFFSET)));
    {
        std::string uuid = std::string("commit_offset_") + client_id + "_" + std::to_string(now_nanos());
        msg.putUuid(uuid.c_str(), static_cast<std::uint16_t>(uuid.size()));
    }
    msg.putPayload(messageJson.data(), static_cast<std::uint16_t>(messageJson.size()));
    static const char emptyHeaders[] = "{}";
    msg.putHeaders(emptyHeaders, 2);

    const int encodedLen = 8 + msg.encodedLength();
    buf.resize(encodedLen);
    return buf;
}

CommitOffset CommitManager::parse_commit_offset(const std::string& payload) {
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream stream(payload);
    
    if (!Json::parseFromStream(builder, stream, &root, &errors)) {
        throw std::runtime_error("Failed to parse commit offset JSON: " + errors);
    }

    CommitOffset offset;
    offset.topic = root.get("topic", "").asString();
    offset.message_identifier = root.get("message_identifier", "").asString();
    offset.message_id = root.get("message_id", "").asString();
    offset.timestamp_nanos = root.get("timestamp_nanos", 0).asUInt64();
    offset.sequence_number = root.get("sequence_number", 0).asUInt64();
    
    return offset;
}

std::string CommitManager::serialize_commit_offset(const CommitOffset& offset) {
    Json::Value root;
    root["action"] = "COMMIT_OFFSET";  // Add action field like Go implementation
    root["topic"] = offset.topic;  // Use the actual topic, not _subscriptions
    root["messageIdentifier"] = offset.message_identifier;  // Use messageIdentifier like Go
    root["message_id"] = offset.message_id;
    root["timestamp_nanos"] = static_cast<Json::UInt64>(offset.timestamp_nanos);
    root["sequence_number"] = static_cast<Json::UInt64>(offset.sequence_number);
    
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, root);
}

} // namespace aeron_cluster
