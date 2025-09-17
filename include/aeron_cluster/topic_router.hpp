#pragma once
#include <string>
#include <string_view>

namespace aeron_cluster {

enum class TopicClass { Orders, Control, Unknown };

inline TopicClass classify_topic(std::string_view topic) {
    if (topic == TOPIC_ORDERS || topic == TOPIC_ORDER_REQUEST) return TopicClass::Orders;
    if (topic == TOPIC_SUBSCRIPTIONS || topic == TOPIC_ACK || topic == TOPIC_CONTROL || topic == TOPIC_INTERNAL)
        return TopicClass::Control;
    return TopicClass::Unknown;
}

} // namespace aeron_cluster
