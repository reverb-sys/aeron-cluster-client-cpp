#pragma once
#include <cstdint>
#include <chrono>

namespace aeron_cluster {

// SBE message header/schema constants (from your schema)
constexpr std::uint16_t TOPIC_MESSAGE_TEMPLATE_ID  = 1; // TopicMessage
constexpr std::uint16_t ACKNOWLEDGMENT_TEMPLATE_ID = 2; // Acknowledgment
constexpr std::uint16_t SBE_SCHEMA_ID              = 1;
constexpr std::uint16_t SBE_VERSION                = 1;
constexpr std::uint16_t TOPIC_MESSAGE_BLOCK_LENGTH = 8; // matches TopicMessage block length

// Control topics
inline constexpr const char* TOPIC_SUBSCRIPTIONS = "_subscriptions";
inline constexpr const char* TOPIC_ACK           = "_ack";
inline constexpr const char* TOPIC_CONTROL       = "_control";
inline constexpr const char* TOPIC_INTERNAL      = "_internal";

// User topics (aligns with your Go/Java setup)
inline constexpr const char* TOPIC_ORDERS            = "orders";
inline constexpr const char* TOPIC_ORDER_REQUEST     = "order_request_topic";

// Subscription wire contract
inline constexpr const char* MSGTYPE_SUBSCRIPTION = "SUBSCRIPTION";

// Time helpers
inline std::uint64_t now_nanos() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

// Heuristic: detect ms vs ns timestamp (Java simple ack uses currentTimeMillis)
inline std::uint64_t to_nanos_auto(std::uint64_t ts) {
    // if < ~ 10^14, it's milliseconds since epoch; convert to ns
    // (in 2025, ms since epoch ~ 1.7e12, ns ~ 1.7e18)
    if (ts < 100000000000000ULL) { return ts * 1000000ULL; }
    return ts;
}

} // namespace aeron_cluster
