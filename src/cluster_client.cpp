#include "aeron_cluster/cluster_client.hpp"

#include <Aeron.h>
#include <json/json.h>

#include <chrono>
#include <functional>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>
#include <unordered_map>
#include <cstdio>

#include "aeron_cluster/ack_decoder.hpp"  // dual-path ACK decoding (simple + full SBE)
#include "aeron_cluster/protocol.hpp"     // constants: template ids, schema id, etc.

// Removed conflicting declaration header
#include "aeron_cluster/protocol.hpp"
#include "aeron_cluster/sbe_messages.hpp"
#include "aeron_cluster/session_manager.hpp"

// ---- Local, header-free fragment reassembler (BEGIN+END flags) ----------------
class LocalFragmentReassembler {
   public:
    // Sink signature: same params as a Subscription fragment handler
    using Sink = std::function<void(aeron::AtomicBuffer&, aeron::util::index_t,
                                    aeron::util::index_t, aeron::logbuffer::Header&)>;

    explicit LocalFragmentReassembler(Sink sink) : sink_(std::move(sink)) {}

    void onFragment(aeron::AtomicBuffer& buffer, aeron::util::index_t offset,
                    aeron::util::index_t length, aeron::logbuffer::Header& header) {
        // Aeron flags: BEGIN=0x80, END=0x40 (BEGIN|END == single-fragment message)
        static constexpr std::uint8_t BEGIN_FLAG = 0x80;
        static constexpr std::uint8_t END_FLAG = 0x40;

        const std::uint8_t flags = static_cast<std::uint8_t>(header.flags());
        const std::uint8_t* src = reinterpret_cast<const std::uint8_t*>(buffer.buffer()) + offset;

        // Single-fragment fast path
        if ((flags & (BEGIN_FLAG | END_FLAG)) == (BEGIN_FLAG | END_FLAG)) {
            sink_(buffer, offset, length, header);
            return;
        }

        // Begin or middle fragment: append
        if (flags & BEGIN_FLAG) {
            acc_.clear();
            acc_.reserve(static_cast<std::size_t>(length) * 2);
        }
        acc_.insert(acc_.end(), src, src + length);

        // End fragment: deliver assembled
        if (flags & END_FLAG) {
            aeron::AtomicBuffer assembled(const_cast<std::uint8_t*>(acc_.data()),
                                          static_cast<std::int32_t>(acc_.size()));
            sink_(assembled, 0, static_cast<aeron::util::index_t>(acc_.size()), header);
            acc_.clear();
        }
    }

   private:
    std::vector<std::uint8_t> acc_;
    Sink sink_;
};
// ------------------------------------------------------------------------------

namespace aeron_cluster {

class ClusterClient::Impl {
   public:
    explicit Impl(const ClusterClientConfig& config)
        : config_(config),
          session_manager_(std::make_unique<SessionManager>(config)),
          connection_state_(ConnectionState::DISCONNECTED),
          auto_reconnect_enabled_(false) {
        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " Creating ClusterClient" << std::endl;
        }
    }

    ~Impl() {
        stop_polling();
        disconnect();
    }

    std::future<bool> connect_async() {
        return std::async(std::launch::async, [this]() { return connect(); });
    }

    bool connect() {
        if (connection_state_ == ConnectionState::CONNECTED) {
            return true;
        }

        set_connection_state(ConnectionState::CONNECTING);

        try {
            stats_.connection_attempts++;

            std::cout << config_.logging.log_prefix << " Connecting to Aeron Cluster..."
                      << std::endl;
            // Initialize Aeron
            if (!initialize_aeron()) {
                set_connection_state(ConnectionState::FAILED);
                return false;
            }

            start_polling();

            std::cout << config_.logging.log_prefix << " Aeron initialized successfully"
                      << std::endl;

            // Create egress subscription
            if (!create_egress_subscription()) {
                set_connection_state(ConnectionState::FAILED);
                return false;
            }

            std::cout << config_.logging.log_prefix << " Egress subscription created successfully"
                      << std::endl;

            // Connect session manager
            SessionConnectionResult result = session_manager_->connect(aeron_);
            if (!result.success) {
                set_connection_state(ConnectionState::FAILED);
                return false;
            }

            std::cout << config_.logging.log_prefix << " Session manager connected successfully"
                      << std::endl;

            // Update statistics
            stats_.successful_connections++;
            stats_.current_session_id = result.session_id;
            stats_.current_leader_id = result.leader_member_id;
            stats_.connection_established_time = std::chrono::steady_clock::now();
            stats_.is_connected = true;

            set_connection_state(ConnectionState::CONNECTED);

            if (config_.enable_console_info) {
                std::cout << config_.logging.log_prefix << " Connected to Aeron Cluster!"
                          << std::endl;
                std::cout << config_.logging.log_prefix
                          << "    Session ID: " << stats_.current_session_id << std::endl;
                std::cout << config_.logging.log_prefix
                          << "    Leader Member: " << stats_.current_leader_id << std::endl;
            }

            return true;

        } catch (const std::exception& e) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix << " Connection failed: " << e.what()
                          << std::endl;
            }
            set_connection_state(ConnectionState::FAILED);
            return false;
        }
    }

    bool connect_with_timeout(std::chrono::milliseconds timeout) {
        auto future = connect_async();
        return future.wait_for(timeout) == std::future_status::ready && future.get();
    }

    void disconnect() {
        if (connection_state_ == ConnectionState::DISCONNECTED) {
            return;
        }

        set_connection_state(ConnectionState::DISCONNECTED);

        if (session_manager_) {
            session_manager_->disconnect();
        }

        egress_subscription_.reset();
        aeron_.reset();

        stats_.current_session_id = -1;
        stats_.current_leader_id = -1;
        stats_.is_connected = false;

        if (config_.enable_console_info) {
            std::cout << config_.logging.log_prefix << " Disconnected from cluster" << std::endl;
        }
    }

    bool is_connected() const {
        return connection_state_ == ConnectionState::CONNECTED && session_manager_ &&
               session_manager_->is_connected();
    }

    ConnectionState get_connection_state() const {
        return connection_state_;
    }

    std::int64_t get_session_id() const {
        return session_manager_ ? session_manager_->get_session_id() : -1;
    }

    std::int32_t get_leader_member_id() const {
        return session_manager_ ? session_manager_->get_leader_member_id() : -1;
    }

    std::string publish_order(const Order& order) {
        if (!is_connected()) {
            throw NotConnectedException();
        }

        auto validation_errors = order.validate();
        if (!validation_errors.empty()) {
            throw InvalidMessageException("Order validation failed: " + validation_errors[0]);
        }

        std::string message_id = OrderUtils::generate_message_id("msg");
        std::string message_type = "CREATE_ORDER";
        if (order.status == "UPDATED" || order.status == "CANCELLED") {
            message_type = "UPDATE_ORDER";
        }

        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " Publishing order: " << order.id
                      << " (MessageID: " << message_id << ")" << std::endl;
        }

        std::string order_json = order.to_json();

        Json::Value headers;
        headers["messageId"] = message_id;
        headers["messageType"] = message_type;
        headers["orderId"] = order.id;

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::string headers_json = Json::writeString(builder, headers);

        if (!session_manager_->publish_message(config_.default_topic, message_type, message_id,
                                               order_json, headers_json)) {
            throw ClusterClientException("Failed to publish order message");
        }

        stats_.messages_sent++;
        return message_id;
    }

    std::future<std::string> publish_order_async(const Order& order) {
        return std::async(std::launch::async, [this, order]() { return publish_order(order); });
    }

    std::string publish_message(const std::string& message_type, const std::string& payload,
                                const std::string& headers) {
        if (!is_connected()) {
            throw NotConnectedException();
        }

        std::string message_id = OrderUtils::generate_message_id("msg");

        if (!session_manager_->publish_message(config_.default_topic, message_type, message_id,
                                               payload, headers)) {
            throw ClusterClientException("Failed to publish message");
        }

        stats_.messages_sent++;
        return message_id;
    }

    std::future<std::string> publish_message_async(const std::string& message_type,
                                                   const std::string& payload,
                                                   const std::string& headers) {
        return std::async(std::launch::async, [this, message_type, payload, headers]() {
            return publish_message(message_type, payload, headers);
        });
    }

    std::string send_subscription_request(const std::string& topic) {
        if (!is_connected()) {
            throw NotConnectedException();
        }

        std::string message_id = OrderUtils::generate_message_id("msg");
        std::string message_type = "SUBSCRIBE";

        Json::Value headers;
        headers["messageId"] = message_id;
        headers["topic"] = "_subscriptions";

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::string headers_json = Json::writeString(builder, headers);

        // bool publish_message(const std::string& topic, const std::string& message_type,
        //   const std::string& message_id, const std::string& payload,
        //   const std::string& headers) {

        /*
            sample message payload in golang

            Message: map[string]interface{}{
            "topic":          topic,
            "action":         "SUBSCRIBE",
            "resumeStrategy": "FROM_LAST",
            "lastMessageId":  "1000",
            }

            Headers: map[string]interface{}{
                "clientId": "subscriberClient",
            },
        */

        Json::Value payload;
        payload["topic"] = topic;
        payload["action"] = "SUBSCRIBE";
        payload["resumeStrategy"] = "FROM_LAST";
        payload["lastMessageId"] = "0";  // Placeholder, can be updated later
        std::string payload_json = Json::writeString(builder, payload);
        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix
                      << " Sending subscription request for topic: " << topic
                      << " (MessageID: " << message_id << ")" << std::endl;
        }

        if (!session_manager_->publish_message("_subscriptions", message_type, message_id,
                                               payload_json, headers_json)) {
            throw ClusterClientException("Failed to send subscription request");
        }

        stats_.messages_sent++;
        return message_id;
    }

    bool wait_for_acknowledgment(const std::string& message_id, std::chrono::milliseconds timeout) {
        // This would require tracking pending messages and their acknowledgments
        // For now, return true as a placeholder
        (void)message_id;  // Suppress unused parameter warning
        (void)timeout;     // Suppress unused parameter warning
        return true;
    }

    void set_message_callback(MessageCallback callback) {
        message_callback_ = std::move(callback);
    }

    void set_connection_state_callback(ConnectionStateCallback callback) {
        connection_state_callback_ = std::move(callback);
    }

    void set_error_callback(ErrorCallback callback) {
        error_callback_ = std::move(callback);
    }

    void start_polling() {
        if (polling_active_) {
            return;
        }

        polling_active_ = true;
        polling_thread_ = std::thread(&ClusterClient::Impl::polling_worker, this);
    }

    void stop_polling() {
        if (!polling_active_) {
            return;
        }

        polling_active_ = false;
        if (polling_thread_.joinable()) {
            polling_thread_.join();
        }
    }

    int poll_messages(int max_messages) {
        if (!egress_subscription_ || egress_subscription_->imageCount() <= 0) {
            return 0;
        }

        int messages_processed = 0;

        try {
            const int fragments = egress_subscription_->poll(
                [this](aeron::AtomicBuffer& buffer, aeron::util::index_t offset,
                       aeron::util::index_t length, aeron::logbuffer::Header& header) {
                    // Reassemble (BEGIN/MIDDLE/END) and invoke handle_incoming_message
                    egress_reassembler_->onFragment(buffer, offset, length, header);
                },
                max_messages);
            messages_processed += fragments;
            stats_.messages_received += fragments;
        } catch (const std::exception& e) {
            if (config_.debug_logging) {
                std::cerr << config_.logging.log_prefix << " Error polling messages: " << e.what()
                          << std::endl;
            }
        }
        return messages_processed;
    }

    ConnectionStats get_connection_stats() const {
        return stats_;
    }

    const ClusterClientConfig& get_config() const {
        return config_;
    }

    void set_auto_reconnect(bool enabled) {
        auto_reconnect_enabled_ = enabled;
    }

    bool is_auto_reconnect_enabled() const {
        return auto_reconnect_enabled_;
    }

    std::future<bool> reconnect_async() {
        return std::async(std::launch::async, [this]() {
            disconnect();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            return connect();
        });
    }

   private:
    ClusterClientConfig config_;
    std::unique_ptr<SessionManager> session_manager_;
    std::shared_ptr<aeron::Aeron> aeron_;
    std::shared_ptr<aeron::Subscription> egress_subscription_;

    std::atomic<ConnectionState> connection_state_;
    ConnectionStats stats_;
    bool auto_reconnect_enabled_;

    // Callbacks
    MessageCallback message_callback_;
    ConnectionStateCallback connection_state_callback_;
    ErrorCallback error_callback_;

    // Polling
    std::atomic<bool> polling_active_{false};
    std::thread polling_thread_;

    std::unique_ptr<LocalFragmentReassembler> egress_reassembler_;

    bool initialize_aeron() {
        try {
            aeron::Context context;
            context.aeronDir(config_.aeron_dir);
            aeron_ = aeron::Aeron::connect(context);
            return true;
        } catch (const std::exception& e) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix
                          << " Aeron initialization error: " << e.what() << std::endl;
            }
            return false;
        }
    }

    bool create_egress_subscription() {
        try {
            std::int64_t sub_id =
                aeron_->addSubscription(config_.response_channel, config_.egress_stream_id);

            auto timeout = std::chrono::seconds(10);
            auto start_time = std::chrono::steady_clock::now();

            while ((std::chrono::steady_clock::now() - start_time) < timeout) {
                egress_subscription_ = aeron_->findSubscription(sub_id);
                if (egress_subscription_) {
                    egress_reassembler_ = std::make_unique<LocalFragmentReassembler>(
                        [this](aeron::AtomicBuffer& buffer, aeron::util::index_t offset,
                               aeron::util::index_t length, aeron::logbuffer::Header&) {
                            this->handle_incoming_message(buffer, offset, length);
                        });
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            return false;
        } catch (const std::exception& e) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix
                          << " Egress subscription error: " << e.what() << std::endl;
            }
            return false;
        }
    }

    void handle_incoming_message(const aeron::AtomicBuffer& buffer, aeron::util::index_t offset,
                                 aeron::util::index_t length) {
        try {
            // const std::uint8_t* data = buffer.buffer() + offset;
            const std::uint8_t* data =
                reinterpret_cast<const std::uint8_t*>(buffer.buffer()) + offset;
            ParseResult result = MessageParser::parse_message(data, length);

            if (result.success) {
                if (result.is_session_event()) {
                    session_manager_->handle_session_event(result);
                }

                if (message_callback_) {
                    message_callback_(result);
                }
            }
        } catch (const std::exception& e) {
            if (config_.debug_logging) {
                std::cerr << config_.logging.log_prefix
                          << " Error handling incoming message: " << e.what() << std::endl;
            }
        }
    }

    void set_connection_state(ConnectionState new_state) {
        ConnectionState old_state = connection_state_.exchange(new_state);
        if (old_state != new_state && connection_state_callback_) {
            connection_state_callback_(old_state, new_state);
        }
    }

    void polling_worker() {
        while (polling_active_) {
            try {
                int processed = poll_messages(10);
                if (processed == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            } catch (const std::exception& e) {
                if (error_callback_) {
                    error_callback_("Polling error: " + std::string(e.what()));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }
};

// --- minimal manual TopicMessage decode (only used as fallback) ---
static inline std::uint16_t le16(const std::uint8_t* p) {
    return (std::uint16_t)p[0] | ((std::uint16_t)p[1] << 8);
}
static inline std::uint64_t le64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= (std::uint64_t)p[i] << (8 * i);
    return v;
}
struct TMDecoded {
    std::uint64_t ts{};
    std::string topic, mtype, uuid, payload, headers;
};
static bool manual_decode_topic_message(const std::uint8_t* data, std::size_t len,
                                        TMDecoded& out) {
    if (len < 16)
        return false;  // header(8) + ts(8)
    // header sanity: 8/1/1/1
    if (le16(data + 0) != 8 || le16(data + 2) != 1 || le16(data + 4) != 1 ||
        le16(data + 6) != 1)
        return false;
    std::size_t pos = 8;
    out.ts = le64(data + pos);
    pos += 8;

    auto readVar = [&](std::string& s) -> bool {
        if (pos + 2 > len)
            return false;
        const std::uint16_t L = le16(data + pos);
        pos += 2;
        if (pos + L > len)
            return false;
        s.assign(reinterpret_cast<const char*>(data + pos), L);
        pos += L;
        return true;
    };
    return readVar(out.topic) && readVar(out.mtype) && readVar(out.uuid) &&
           readVar(out.payload) && readVar(out.headers);
}

// ClusterClient public interface implementation
ClusterClient::ClusterClient(const ClusterClientConfig& config)
    : cfg_(config), pImpl_(std::make_unique<Impl>(config)) {
    handler_.set_ack_callback([this](const AckInfo& ack) { this->on_ack_default(ack); });
}

ClusterClient::~ClusterClient() = default;

std::future<bool> ClusterClient::connect_async() {
    return pImpl_->connect_async();
}

bool ClusterClient::connect() {
    return pImpl_->connect();
}

bool ClusterClient::connect_with_timeout(std::chrono::milliseconds timeout) {
    return pImpl_->connect_with_timeout(timeout);
}

void ClusterClient::disconnect() {
    pImpl_->disconnect();
}

bool ClusterClient::is_connected() const {
    return pImpl_->is_connected();
}

ConnectionState ClusterClient::get_connection_state() const {
    return pImpl_->get_connection_state();
}

std::int64_t ClusterClient::get_session_id() const {
    return pImpl_->get_session_id();
}

std::int32_t ClusterClient::get_leader_member_id() const {
    return pImpl_->get_leader_member_id();
}

std::string ClusterClient::publish_order(const Order& order) {
    return pImpl_->publish_order(order);
}

std::future<std::string> ClusterClient::publish_order_async(const Order& order) {
    return pImpl_->publish_order_async(order);
}

std::string ClusterClient::send_subscription_request(const std::string& topic) {
    return pImpl_->send_subscription_request(topic);
}

std::string ClusterClient::publish_message(const std::string& message_type,
                                           const std::string& payload,
                                           const std::string& headers) {
    return pImpl_->publish_message(message_type, payload, headers);
}

std::future<std::string> ClusterClient::publish_message_async(const std::string& message_type,
                                                              const std::string& payload,
                                                              const std::string& headers) {
    return pImpl_->publish_message_async(message_type, payload, headers);
}

bool ClusterClient::wait_for_acknowledgment(const std::string& message_id,
                                            std::chrono::milliseconds timeout) {
    return pImpl_->wait_for_acknowledgment(message_id, timeout);
}

void ClusterClient::set_message_callback(MessageCallback callback) {
    pImpl_->set_message_callback(std::move(callback));
}

void ClusterClient::set_connection_state_callback(ConnectionStateCallback callback) {
    pImpl_->set_connection_state_callback(std::move(callback));
}

void ClusterClient::set_error_callback(ErrorCallback callback) {
    pImpl_->set_error_callback(std::move(callback));
}

void ClusterClient::start_polling() {
    pImpl_->start_polling();
}

void ClusterClient::stop_polling() {
    pImpl_->stop_polling();
}

int ClusterClient::poll_messages(int max_messages) {
    return pImpl_->poll_messages(max_messages);
}

ConnectionStats ClusterClient::get_connection_stats() const {
    return pImpl_->get_connection_stats();
}

const ClusterClientConfig& ClusterClient::get_config() const {
    return pImpl_->get_config();
}

void ClusterClient::set_auto_reconnect(bool enabled) {
    pImpl_->set_auto_reconnect(enabled);
}

bool ClusterClient::is_auto_reconnect_enabled() const {
    return pImpl_->is_auto_reconnect_enabled();
}

std::future<bool> ClusterClient::reconnect_async() {
    return pImpl_->reconnect_async();
}

std::string ClusterClient::publish_topic(std::string_view topic, std::string_view message_type,
                                         std::string_view json_payload,
                                         std::string_view headers_json) {
    // Correlation UUID
    std::string uuid = std::string("pub_") + std::to_string(now_nanos());

    // Ensure headers are non-empty JSON
    std::string hdrs = headers_json.empty() ? std::string("{}") : std::string(headers_json);

    // Pre-size buffer generously: header(8) + ts(8) + varstrings + slack
    std::vector<std::uint8_t> buf;
    buf.resize(8 + 8 + topic.size() + message_type.size() + uuid.size() + json_payload.size() +
               hdrs.size() + 128);

    // ---- SBE header ----
    sbe::MessageHeader hdr;
    // Signature of wrap in your codegen typically is (buffer, offset, actingVersion,
    // bufferLength).
    hdr.wrap(reinterpret_cast<char*>(buf.data()), 0, SBE_VERSION,
             static_cast<std::uint64_t>(buf.size()));
    hdr.blockLength(TOPIC_MESSAGE_BLOCK_LENGTH);
    hdr.templateId(TOPIC_MESSAGE_TEMPLATE_ID);
    hdr.schemaId(SBE_SCHEMA_ID);
    hdr.version(SBE_VERSION);

    // ---- TopicMessage body ----
    sbe::TopicMessage msg;
    // Signature is (buffer, offset, bufferLength)
    msg.wrapForEncode(reinterpret_cast<char*>(buf.data()), 8,
                      static_cast<std::uint64_t>(buf.size() - 8));

    const auto ts = now_nanos();
    msg.timestamp(ts);

    // IMPORTANT: pass 'int' lengths; the codegen writes uint16 under the hood.
    msg.putTopic(topic.data(), static_cast<int>(topic.size()));
    msg.putMessageType(message_type.data(), static_cast<int>(message_type.size()));
    msg.putUuid(uuid.data(), static_cast<int>(uuid.size()));
    msg.putPayload(json_payload.data(), static_cast<int>(json_payload.size()));
    msg.putHeaders(hdrs.data(), static_cast<int>(hdrs.size()));

    // Finalize size to actual encoded length
    const int encodedLen = 8 + msg.encodedLength();
    buf.resize(encodedLen);

    if (offer_ingress(buf.data(), buf.size())) {
        remember_outgoing(uuid, ts);
    }
    return uuid;
}

bool ClusterClient::subscribe_topic(std::string_view topic, std::string_view resume_strategy) {
    auto frame = build_subscription_message(topic, cfg_.client_id, resume_strategy);
    return offer_ingress(frame.data(), frame.size());
}

void ClusterClient::remember_outgoing(const std::string& uuid, std::uint64_t ts_nanos) {
    inflight_.emplace(uuid, ts_nanos);
}

void ClusterClient::on_ack_default(const AckInfo& ack) {
    // If ACK has correlation/message_id, use it to compute RTT. If not, just log.
    auto it = (!ack.message_id.empty()) ? inflight_.find(ack.message_id) : inflight_.end();
    if (it != inflight_.end()) {
        const auto send_ns = it->second;
        inflight_.erase(it);
        const auto rtt_ns = (std::int64_t)now_nanos() - (std::int64_t)send_ns;
        std::printf("[ACK] corr=%s topic=%s simple=%d rtt=%.3fms (ack_ts=%llu)\n",
                    ack.message_id.c_str(), ack.topic.c_str(), ack.simple_control_ack ? 1 : 0,
                    (double)rtt_ns / 1e6, (unsigned long long)ack.timestamp_nanos);
    } else {
        // Fallback latency using ack.timestamp_nanos (server clock)
        const auto rtt_est_ns = (std::int64_t)now_nanos() - (std::int64_t)ack.timestamp_nanos;
        std::printf("[ACK] corr=? topic=%s simple=%d est_rtt=%.3fms (ack_ts=%llu)\n",
                    ack.topic.c_str(), ack.simple_control_ack ? 1 : 0, (double)rtt_est_ns / 1e6,
                    (unsigned long long)ack.timestamp_nanos);
    }
}

bool ClusterClient::offer_ingress(const std::uint8_t* data, std::size_t len) {
    // This is a placeholder implementation
    // In a real implementation, this would use the session manager to offer the message
    (void)data;  // Suppress unused parameter warning
    (void)len;   // Suppress unused parameter warning
    if (pImpl_ && pImpl_->is_connected()) {
        // Use the session manager to publish the message
        // For now, return true as a placeholder
        return true;
    }
    return false;
}

// Static factory methods
Order ClusterClient::create_sample_limit_order(const std::string& base_token,
                                               const std::string& quote_token,
                                               const std::string& side, double quantity,
                                               double limit_price) {
    // Generate unique identifiers
    std::string uuid = OrderUtils::generate_uuid();
    std::string client_order_id = uuid;
    
    // Get current timestamp
    auto now = std::chrono::system_clock::now();
    auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::string create_ts = std::to_string(timestamp_ms);
    
    // Create order with new structure
    Order order(base_token, quote_token, side, quantity, "LIMIT");
    order.id = OrderUtils::generate_order_id();
    order.client_order_uuid = client_order_id;
    order.limit_price = limit_price;
    order.time_in_force = "GTC";
    order.status = "CREATED";
    order.request_source = "FIX_GATEWAY";
    order.request_channel = "cpp_client";

    // Set account information
    order.customer_id = 75;  // From the example
    order.user_id = 67890;
    order.account_id = 11111;
    order.base_token_usd_rate = limit_price;
    order.quote_token_usd_rate = 1.0;
    
    // Set quantity token to match the base token
    order.quantity_token = base_token;
    
    // Initialize timestamps
    order.initialize_timestamps();
    
    // Store the message structure in metadata for serialization
    order.metadata["uuid"] = uuid;
    order.metadata["msg_type"] = "D";
    order.metadata["origin"] = "fix";
    order.metadata["origin_name"] = "FIX_GATEWAY";
    order.metadata["origin_id"] = "DEV_FIX01_TX";
    order.metadata["connection_uuid"] = "129725";
    order.metadata["customer_id"] = "75";
    order.metadata["ip_address"] = "13.127.186.179";
    order.metadata["create_ts"] = create_ts;
    order.metadata["auth_token"] = "Bearer eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9.eyJhayI6IjZTYXZHSFJkMzJCVWdXZ05NdEdBQnBZV05aaFFkMUxQZ1dsbE1oOHh4RXdqUjNOMTFvaWNMcXY2STltVkZvcUYiLCJhdWQiOiJERVZfRklYMDFfVFgiLCJleHAiOjE3NTgxMDE1NzEsImlhdCI6MTc1NzkyODc3MSwic2NvcGUiOiJ3cml0ZTp0cmFkZSBjcnVkOnNlc3Npb24ifQ.UPrSbS80TZDf_0ATdMCwPHf3DUhDDqqIiszBKjTL0emMSFtP4Qq9Bl1OxjXYrFI9kUpqtwBI3qyd1SwR3n2qgqh94Wi42E4OW8QeJD-R_KTgL5rAH0Fmo0IhG4EEwtYVpISYAXkuEbIV74mjBC1uzfNaaB7SXbMUuejovZpLpWkeMFDXhbjovQjWbzde0NIjtJ5LBXSRkqe39IjFEdWpheYEUZp7gV9HcuGklkwqBA3VFVUW_OgFKxtP7BXbgmijYwb1ojxvOB_rLSewk6mCABAUZaosbi5fgZGtv6R35p0VpBPUZ3-9hRcpaPveq9XROzeTx4J-9Bgz3GseGPXcfvkUhA2qqPihh8-W79ztXLwsx3bupnizaMHFdq7b07Ta8ILrVwH8son5AX1s0aOmuHp0fykGyjVM07QGyYYia33FIE1HOnJzzK1GqEy_vAucCgc59wJiSTQsgH2hP69P7PkVhbnCm-jf9j6DIAgEhABscpm6G59E6zqMz3M8qR9luHubm6j7f-FcLAohsTOa2HCp18BNzvrsBUmY3oDz9HRWCpDPkpeWZRNGauXiSZ12Ziany4iRSIWZnnO4Je6-iDbv4xw5lGA3bT-ljowUey_hm6sXx22LXCSFe_x1V9VOInJwZGL_nGLiQSSGG0mZhhO7fiL4IfCy7gTBNigW8ww";
    order.metadata["action"] = "CREATE";
    order.metadata["order_type"] = "limit";
    order.metadata["quantity_value_str"] = std::to_string(quantity);

    return order;
}

Order ClusterClient::create_sample_market_order(const std::string& base_token,
                                                const std::string& quote_token,
                                                const std::string& side, double quantity) {
    Order order(base_token, quote_token, side, quantity, "MARKET");
    order.id = OrderUtils::generate_order_id();
    order.time_in_force = "IOC";
    order.status = "CREATED";
    order.request_source = "API";
    order.request_channel = "cpp_client";

    order.customer_id = 12345;
    order.user_id = 67890;
    order.account_id = 11111;

    return order;
}

std::string ClusterClient::create_order_message(const std::string& base_token,
                                               const std::string& quote_token,
                                               const std::string& side, double quantity,
                                               double limit_price) {
    // Generate unique identifiers
    std::string uuid = OrderUtils::generate_uuid();
    std::string client_order_id = uuid;
    
    // Get current timestamp
    auto now = std::chrono::system_clock::now();
    auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::string create_ts = std::to_string(timestamp_ms);
    
    // Create the complete message structure
    Json::Value message;
    
    // Top level structure
    message["uuid"] = uuid;
    message["msg_type"] = "D";
    
    // Headers
    Json::Value headers;
    headers["origin"] = "fix";
    headers["origin_name"] = "FIX_GATEWAY";
    headers["origin_id"] = "DEV_FIX01_TX";
    headers["connection_uuid"] = "129725";
    headers["customer_id"] = "75";
    headers["ip_address"] = "13.127.186.179";
    headers["create_ts"] = create_ts;
    headers["auth_token"] = "Bearer eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9.eyJhayI6IjZTYXZHSFJkMzJCVWdXZ05NdEdBQnBZV05aaFFkMUxQZ1dsbE1oOHh4RXdqUjNOMTFvaWNMcXY2STltVkZvcUYiLCJhdWQiOiJERVZfRklYMDFfVFgiLCJleHAiOjE3NTgxMDE1NzEsImlhdCI6MTc1NzkyODc3MSwic2NvcGUiOiJ3cml0ZTp0cmFkZSBjcnVkOnNlc3Npb24ifQ.UPrSbS80TZDf_0ATdMCwPHf3DUhDDqqIiszBKjTL0emMSFtP4Qq9Bl1OxjXYrFI9kUpqtwBI3qyd1SwR3n2qgqh94Wi42E4OW8QeJD-R_KTgL5rAH0Fmo0IhG4EEwtYVpISYAXkuEbIV74mjBC1uzfNaaB7SXbMUuejovZpLpWkeMFDXhbjovQjWbzde0NIjtJ5LBXSRkqe39IjFEdWpheYEUZp7gV9HcuGklkwqBA3VFVUW_OgFKxtP7BXbgmijYwb1ojxvOB_rLSewk6mCABAUZaosbi5fgZGtv6R35p0VpBPUZ3-9hRcpaPveq9XROzeTx4J-9Bgz3GseGPXcfvkUhA2qqPihh8-W79ztXLwsx3bupnizaMHFdq7b07Ta8ILrVwH8son5AX1s0aOmuHp0fykGyjVM07QGyYYia33FIE1HOnJzzK1GqEy_vAucCgc59wJiSTQsgH2hP69P7PkVhbnCm-jf9j6DIAgEhABscpm6G59E6zqMz3M8qR9luHubm6j7f-FcLAohsTOa2HCp18BNzvrsBUmY3oDz9HRWCpDPkpeWZRNGauXiSZ12Ziany4iRSIWZnnO4Je6-iDbv4xw5lGA3bT-ljowUey_hm6sXx22LXCSFe_x1V9VOInJwZGL_nGLiQSSGG0mZhhO7fiL4IfCy7gTBNigW8ww";
    
    // Message content
    Json::Value message_content;
    message_content["action"] = "CREATE";
    
    // Order details
    Json::Value order_details;
    
    // Token pair
    Json::Value token_pair;
    token_pair["base_token"] = base_token;
    token_pair["quote_token"] = quote_token;
    order_details["token_pair"] = token_pair;
    
    // Quantity
    Json::Value quantity_obj;
    quantity_obj["token"] = base_token;
    quantity_obj["value"] = quantity;
    order_details["quantity"] = quantity_obj;
    
    // Other order details
    order_details["side"] = side;
    order_details["order_type"] = "limit";
    order_details["time_in_force"] = "gtc";
    order_details["limit_price"] = limit_price;
    order_details["quantity_value_str"] = std::to_string(quantity);
    order_details["client_order_id"] = client_order_id;
    
    message_content["order_details"] = order_details;
    
    // Assemble the complete message
    Json::Value complete_message;
    complete_message["headers"] = headers;
    complete_message["message"] = message_content;
    
    message["message"] = complete_message;
    
    // Convert to JSON string
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
    
    std::ostringstream oss;
    writer->write(message, &oss);
    
    return oss.str();
}

// ClusterClientConfigBuilder implementation
ClusterClientConfigBuilder::ClusterClientConfigBuilder() = default;

ClusterClientConfigBuilder& ClusterClientConfigBuilder::with_cluster_endpoints(
    const std::vector<std::string>& endpoints) {
    config_.cluster_endpoints = endpoints;
    return *this;
}

ClusterClientConfigBuilder& ClusterClientConfigBuilder::with_response_channel(
    const std::string& channel) {
    config_.response_channel = channel;
    return *this;
}

ClusterClientConfigBuilder& ClusterClientConfigBuilder::with_aeron_dir(
    const std::string& aeron_dir) {
    config_.aeron_dir = aeron_dir;
    return *this;
}

ClusterClientConfigBuilder& ClusterClientConfigBuilder::with_response_timeout(
    std::chrono::milliseconds timeout) {
    config_.response_timeout = timeout;
    return *this;
}

ClusterClientConfigBuilder& ClusterClientConfigBuilder::with_max_retries(int max_retries) {
    config_.max_retries = max_retries;
    return *this;
}

ClusterClientConfigBuilder& ClusterClientConfigBuilder::with_retry_delay(
    std::chrono::milliseconds delay) {
    config_.retry_delay = delay;
    return *this;
}

ClusterClientConfigBuilder& ClusterClientConfigBuilder::with_keepalive_enabled(bool enabled) {
    config_.enable_keepalive = enabled;
    return *this;
}

ClusterClientConfigBuilder& ClusterClientConfigBuilder::with_keepalive_interval(
    std::chrono::milliseconds interval) {
    config_.keepalive_interval = interval;
    return *this;
}

ClusterClientConfigBuilder& ClusterClientConfigBuilder::with_debug_logging(bool enabled) {
    config_.debug_logging = enabled;
    return *this;
}

ClusterClientConfigBuilder& ClusterClientConfigBuilder::with_application_name(
    const std::string& name) {
    config_.application_name = name;
    return *this;
}

ClusterClientConfigBuilder& ClusterClientConfigBuilder::with_default_topic(
    const std::string& topic) {
    config_.default_topic = topic;
    return *this;
}

ClusterClientConfig ClusterClientConfigBuilder::build() const {
    // Validate the configuration before returning
    ClusterClientConfig config = config_;
    config.validate();
    return config;
}

// Helper functions are already defined in protocol.hpp and subscription.hpp

}  // namespace aeron_cluster