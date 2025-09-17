#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <optional>
#include <functional>
#include "aeron_cluster/protocol.hpp"
#include "aeron_cluster/topic_router.hpp"
#include "aeron_cluster/ack_decoder.hpp"

// SBE generated
#include "model/MessageHeader.h"
#include "model/TopicMessage.h"

namespace aeron_cluster {

// App-facing callbacks
using TopicMessageCallback = std::function<void(std::string_view topic,
                                                std::string_view msg_type,
                                                std::string_view uuid,
                                                std::string_view payload,
                                                std::string_view headers)>;

using AckCallback = std::function<void(const AckInfo&)>;

class MessageHandler {
public:
    void on_egress(const std::uint8_t* data, std::size_t len) {
        if (len < 8) return;

        // Try ACK first (fast path for small control frames)
        if (auto ack = decode_ack(data, len)) {
            if (ack_cb_) ack_cb_(*ack);
            return;
        }

        // Else, check if TopicMessage
        if (!is_topic_message(data, len)) return;

        // Full decode TopicMessage
        sbe::MessageHeader hdr;
        hdr.wrap(const_cast<char*>(reinterpret_cast<const char*>(data)), 0, SBE_VERSION, static_cast<std::uint64_t>(len));
        sbe::TopicMessage msg;
        msg.wrapForDecode(
            const_cast<char*>(reinterpret_cast<const char*>(data)),
            8,
            hdr.blockLength(),
            hdr.version(),
            static_cast<std::uint64_t>(len - 8));

        // Extract fields
        auto topic   = read_var_string([&]{ return static_cast<int>(msg.topicLength()); }, [&](char* b, int l){ msg.getTopic(b, static_cast<std::uint64_t>(l)); });
        auto msgType = read_var_string([&]{ return static_cast<int>(msg.messageTypeLength()); }, [&](char* b, int l){ msg.getMessageType(b, static_cast<std::uint64_t>(l)); });
        auto uuid    = read_var_string([&]{ return static_cast<int>(msg.uuidLength()); }, [&](char* b, int l){ msg.getUuid(b, static_cast<std::uint64_t>(l)); });
        auto payload = read_var_string([&]{ return static_cast<int>(msg.payloadLength()); }, [&](char* b, int l){ msg.getPayload(b, static_cast<std::uint64_t>(l)); });
        auto headers = read_var_string([&]{ return static_cast<int>(msg.headersLength()); }, [&](char* b, int l){ msg.getHeaders(b, static_cast<std::uint64_t>(l)); });

        if (topic.empty()) return;

        if (tm_cb_) tm_cb_(topic, msgType, uuid, payload, headers);
    }

    void set_topic_message_callback(TopicMessageCallback cb) { tm_cb_ = std::move(cb); }
    void set_ack_callback(AckCallback cb) { ack_cb_ = std::move(cb); }

private:
    static bool is_topic_message(const std::uint8_t* data, std::size_t len) {
        if (len < 8) return false;
        const auto templateId = (std::uint16_t)data[2] | ((std::uint16_t)data[3] << 8);
        const auto schemaId   = (std::uint16_t)data[4] | ((std::uint16_t)data[5] << 8);
        return templateId == TOPIC_MESSAGE_TEMPLATE_ID && schemaId == SBE_SCHEMA_ID;
    }

    template <typename LenGetter, typename Getter>
    static std::string read_var_string(LenGetter lenGetter, Getter getter) {
        const int len = lenGetter();
        if (len <= 0) return {};
        std::string out;
        out.resize(len);
        getter(out.data(), len);
        return out;
    }

private:
    TopicMessageCallback tm_cb_;
    AckCallback          ack_cb_;
};

} // namespace aeron_cluster
