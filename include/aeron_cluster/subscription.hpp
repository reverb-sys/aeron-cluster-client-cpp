#pragma once
#include <string>
#include <string_view>
#include <vector>
#include "aeron_cluster/protocol.hpp"

// SBE generated
#include "model/MessageHeader.h"
#include "model/TopicMessage.h"

namespace aeron_cluster {

// Build a TopicMessage that requests subscription on _subscriptions
inline std::vector<std::uint8_t>
build_subscription_message(std::string_view target_topic, std::string_view client_id, std::string_view resume_strategy = "LATEST")
{
    // JSON payload: {"topic": "...", "action": "SUBSCRIBE", "client_id": "...", "resumeStrategy":"..."}
    // (keep minimal keys the server expects)
    std::string payload = std::string("{\"topic\":\"") + std::string(target_topic) + "\","
                           "\"action\":\"SUBSCRIBE\","
                           "\"client_id\":\"" + std::string(client_id) + "\"";

    if (!resume_strategy.empty()) {
        payload += ",\"resumeStrategy\":\"" + std::string(resume_strategy) + "\"";
    }
    payload += "}";

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
    msg.putMessageType(MSGTYPE_SUBSCRIPTION, static_cast<std::uint16_t>(std::char_traits<char>::length(MSGTYPE_SUBSCRIPTION)));

    // UUID: sub_<client>_<ts>
    {
        std::string uuid = std::string("sub_") + std::string(client_id) + "_" + std::to_string(now_nanos());
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

// Build a TopicMessage that requests unsubscription from _subscriptions
inline std::vector<std::uint8_t>
build_unsubscription_message(std::string_view target_topic, std::string_view client_id)
{
    // JSON payload: {"topic": "...", "action": "UNSUBSCRIBE", "client_id": "..."}
    std::string payload = std::string("{\"topic\":\"") + std::string(target_topic) + "\","
                           "\"action\":\"UNSUBSCRIBE\","
                           "\"client_id\":\"" + std::string(client_id) + "\"}";

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
    msg.putMessageType(MSGTYPE_UNSUBSCRIBE, static_cast<std::uint16_t>(std::char_traits<char>::length(MSGTYPE_UNSUBSCRIBE)));

    // UUID: unsub_<client>_<ts>
    {
        std::string uuid = std::string("unsub_") + std::string(client_id) + "_" + std::to_string(now_nanos());
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

} // namespace aeron_cluster
