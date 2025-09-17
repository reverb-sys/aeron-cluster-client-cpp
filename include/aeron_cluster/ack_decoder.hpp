#pragma once
#include <cstdint>
#include <string>
#include <optional>
#include <vector>

namespace aeron_cluster {

struct AckInfo {
    std::uint64_t timestamp_nanos{};
    std::string   message_id;
    std::string   topic;
    std::string   correlation_id;
    bool          simple_control_ack{false}; // true if 16-byte variant
};

// Decodes either a full SBE Acknowledgment or 16-byte "simple control ack".
// Returns std::nullopt if the buffer is not an ACK or is malformed.
std::optional<AckInfo> decode_ack(const std::uint8_t* data, std::size_t len);

} // namespace aeron_cluster
