#include "aeron_cluster/ack_decoder.hpp"
#include "aeron_cluster/protocol.hpp"

// Include your SBE generated headers (adjust include paths to your repo)
#include "sbe/MessageHeader.h"
#include "sbe/Acknowledgment.h"

#include <cstring>

namespace aeron_cluster {

// Little-endian helpers
static inline std::uint16_t le16_at(const std::uint8_t* p) {
    return (std::uint16_t)p[0] | ((std::uint16_t)p[1] << 8);
}
static inline std::uint64_t le64_at(const std::uint8_t* p) {
    std::uint64_t v = 0;
    v |= (std::uint64_t)p[0];
    v |= (std::uint64_t)p[1] << 8;
    v |= (std::uint64_t)p[2] << 16;
    v |= (std::uint64_t)p[3] << 24;
    v |= (std::uint64_t)p[4] << 32;
    v |= (std::uint64_t)p[5] << 40;
    v |= (std::uint64_t)p[6] << 48;
    v |= (std::uint64_t)p[7] << 56;
    return v;
}

std::optional<AckInfo> decode_ack(const std::uint8_t* data, std::size_t len) {
    if (len < 8) return std::nullopt;

    // Read header minimally: blockLength, templateId, schemaId, version
    if (len >= 8) {
        const auto blockLength = le16_at(data + 0);
        const auto templateId  = le16_at(data + 2);
        const auto schemaId    = le16_at(data + 4);
        const auto version     = le16_at(data + 6);
        (void)version;

        if (schemaId != SBE_SCHEMA_ID) return std::nullopt;

        if (templateId == ACKNOWLEDGMENT_TEMPLATE_ID) {
            // Two possibilities:
            //  (1) "Simple control ack" => total len == 16: header(8) + timestamp(8-ms)
            //  (2) Full SBE Acknowledgment => header + SBE fields
            if (len == 16 && blockLength == 8) {
                const auto ts_ms = le64_at(data + 8);
                AckInfo info;
                info.timestamp_nanos = to_nanos_auto(ts_ms);
                info.simple_control_ack = true;
                return info;
            }

            // Full SBE ACK
            try {
                // Wrap header flyweight
                sbe::MessageHeader hdr;
                hdr.wrap(const_cast<std::uint8_t*>(data), 0, SBE_VERSION, len);

                if (hdr.templateId() != ACKNOWLEDGMENT_TEMPLATE_ID) {
                    return std::nullopt;
                }

                // Acknowledgment decoder
                sbe::Acknowledgment ack;
                // Body starts after header: 8 bytes
                ack.wrapForDecode(const_cast<std::uint8_t*>(data), 8, hdr.blockLength(), hdr.version(), len - 8);

                AckInfo info;
                info.timestamp_nanos = to_nanos_auto(ack.timestamp());

                // Extract varStringEncoding fields
                {
                    const auto messageIdLen = ack.getMessageIdLength();
                    std::vector<char> buf(messageIdLen);
                    if (messageIdLen > 0) {
                        ack.getMessageId(buf.data(), messageIdLen);
                        info.message_id.assign(buf.data(), messageIdLen);
                    }
                }
                {
                    const auto topicLen = ack.getTopicLength();
                    std::vector<char> buf(topicLen);
                    if (topicLen > 0) {
                        ack.getTopic(buf.data(), topicLen);
                        info.topic.assign(buf.data(), topicLen);
                    }
                }
                {
                    const auto corrLen = ack.getCorrelationIdLength();
                    std::vector<char> buf(corrLen);
                    if (corrLen > 0) {
                        ack.getCorrelationId(buf.data(), corrLen);
                        info.correlation_id.assign(buf.data(), corrLen);
                    }
                }

                return info;
            } catch (...) {
                return std::nullopt;
            }
        }
    }
    return std::nullopt;
}

} // namespace aeron_cluster
