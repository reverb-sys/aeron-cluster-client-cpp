#include "aeron_cluster/cluster_client.hpp"
#include "aeron_cluster/performance_config.hpp"
#include "aeron_cluster/logging.hpp"
#include <Aeron.h>
#include <json/json.h>

#include <chrono>
#include <atomic>
#include <cstdio>
#include <functional>
#include <iostream>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// For generating unique instance identifiers
#ifdef _WIN32
    #include <windows.h>
#else
    #include <unistd.h>  // for getpid()
#endif

#include "aeron_cluster/ack_decoder.hpp"  // dual-path ACK decoding (simple + full SBE)
#include "aeron_cluster/debug_utils.hpp"
#include "aeron_cluster/protocol.hpp"  // constants: template ids, schema id, etc.
#include "aeron_cluster/commit_manager.hpp"
#include <memory>
#include <mutex>
#include <unordered_set>

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
            // Create a copy to avoid dangling pointer issues
            std::vector<std::uint8_t> assembled_data(acc_.begin(), acc_.end());
            aeron::AtomicBuffer assembled(const_cast<std::uint8_t*>(assembled_data.data()),
                                          static_cast<std::int32_t>(assembled_data.size()));
            sink_(assembled, 0, static_cast<aeron::util::index_t>(assembled_data.size()), header);
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
          logger_(LoggerFactory::instance().getLogger("cluster_client")),
          commit_manager_(std::make_unique<CommitManager>()),
          connection_state_(ConnectionState::DISCONNECTED),
          auto_reconnect_enabled_(false) {
        // Generate unique instance identifier for load balancing if not provided
        if (config_.instance_identifier.empty()) {
            config_.instance_identifier = generate_instance_identifier();
        }

        session_manager_->set_session_event_callback(
            [this](const ParseResult& event) { this->on_session_event(event); });

        session_manager_->set_connection_state_callback(
            [this](bool connected) { this->on_session_connection_state_changed(connected); });
    }

    ~Impl() {
        try {
            stop_polling();
            disconnect();
            if (auto_reconnect_thread_.joinable()) {
                auto_reconnect_thread_.join();
            }
        } catch (const std::exception& e) {
            DEBUG_LOG("Error in destructor: ", e.what());
        }
    }
    
    // Generate a unique instance identifier for load balancing
    static std::string generate_instance_identifier() {
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        
        // Get process ID
        #ifdef _WIN32
            auto pid = GetCurrentProcessId();
        #else
            auto pid = getpid();
        #endif
        
        std::ostringstream oss;
        oss << "cpp_client_" << pid << "_" << timestamp;
        return oss.str();
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

            if (!initialize_aeron()) {
                set_connection_state(ConnectionState::FAILED);
                return false;
            }

            start_polling();

            if (!create_egress_subscription()) {
                set_connection_state(ConnectionState::FAILED);
                return false;
            }

            SessionConnectionResult result = session_manager_->connect(aeron_);
            if (!result.success) {
                set_connection_state(ConnectionState::FAILED);
                return false;
            }

            stats_.successful_connections++;
            stats_.current_session_id = result.session_id;
            stats_.current_leader_id = result.leader_member_id;
            stats_.connection_established_time = std::chrono::steady_clock::now();
            stats_.is_connected = true;
            disconnect_notified_.store(false);
            reconnect_in_progress_.store(false);

            // Resume from last commit for subscribed topics
            resume_subscribed_topics();

            set_connection_state(ConnectionState::CONNECTED);
            return true;

        } catch (const std::exception& e) {
            DEBUG_LOG("Connection failed: ", e.what());
            set_connection_state(ConnectionState::FAILED);
            return false;
        }
    }

    bool connect_with_timeout(std::chrono::milliseconds timeout) {
        auto future = connect_async();
        return future.wait_for(timeout) == std::future_status::ready && future.get();
    }

    void disconnect() {
        // CRITICAL FIX: Always try to send close request, even if already disconnected
        // This ensures the server is notified when signal handler calls disconnect()
        bool was_connected = (connection_state_ == ConnectionState::CONNECTED);
        suppress_disconnect_notifications_.store(true);
        struct NotificationReset {
            std::atomic<bool>& flag;
            ~NotificationReset() { flag.store(false); }
        } reset_guard{suppress_disconnect_notifications_};
        
        if (was_connected) {
            DEBUG_LOG("Disconnecting client...");
        } else {
            DEBUG_LOG("Disconnect called but client already disconnected, will still attempt to send close request");
        }
        
        // CRITICAL FIX: Send SessionCloseRequest BEFORE stopping polling and resetting resources
        // This ensures the server is notified of the disconnection
        if (session_manager_) {
            // Always try to disconnect session manager (it will send close request if connected)
            session_manager_->disconnect();
        }

        // Only do cleanup if we were actually connected
        if (was_connected) {
            // Stop polling first to prevent interference with cleanup
            stop_polling();
            
            set_connection_state(ConnectionState::DISCONNECTED);

            egress_subscription_.reset();
            aeron_.reset();

            stats_.current_session_id = -1;
            stats_.current_leader_id = -1;
            stats_.is_connected = false;
        }
        
        DEBUG_LOG("Client disconnect completed (was_connected={})", was_connected);
    }

    bool is_connected() const {
        // Basic connection state check
        if (connection_state_ != ConnectionState::CONNECTED) {
            return false;
        }
        
        // Check session manager
        if (!session_manager_ || !session_manager_->is_connected()) {
            return false;
        }
        
        // Only check egress subscription if it exists and we're past initial connection
        // During initial connection, egress subscription might not be ready yet
        if (egress_subscription_) {
            return egress_subscription_->imageCount() > 0;
        }
        
        // If egress subscription doesn't exist yet, consider connected if session manager is connected
        return true;
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

    std::string publish_order_to_topic(const Order& order, const std::string& topic) {
        if (!is_connected()) {
            throw NotConnectedException();
        }

        if (topic.empty()) {
            throw InvalidMessageException("Topic is empty");
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

        std::string order_json = order.to_json();

        Json::Value headers;
        headers["messageId"] = message_id;
        headers["messageType"] = message_type;
        headers["orderId"] = order.id;

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::string headers_json = Json::writeString(builder, headers);

        if (!session_manager_->publish_message(topic, message_type, message_id,
                                               order_json, headers_json)) {
            throw ClusterClientException("Failed to publish order message");
        }

        stats_.messages_sent++;
        return message_id;
    }


    std::future<std::string> publish_order_async(const Order& order) {
        return std::async(std::launch::async, [this, order]() { return publish_order(order); });
    }

    std::future<std::string> publish_order_to_topic_async(const Order& order, const std::string& topic) {
        return std::async(std::launch::async, [this, order, topic]() { return publish_order_to_topic(order, topic); });
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

    std::string publish_message_to_topic(const std::string& message_type, const std::string& payload,
        const std::string& headers, const std::string& topic) {
        if (!is_connected()) {
        throw NotConnectedException();
        }

        if (topic.empty()) {
            throw InvalidMessageException("Topic is empty");
        }

        std::string message_id = OrderUtils::generate_message_id("msg");

        if (!session_manager_->publish_message(topic, message_type, message_id,
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

    std::future<std::string> publish_message_to_topic_async(const std::string& message_type,
        const std::string& payload, const std::string& headers, const std::string& topic) {
        return std::async(std::launch::async, [this, message_type, payload, headers, topic]() {
            return publish_message_to_topic(message_type, payload, headers, topic);
        });
    }

    std::string send_subscription_request(const std::string& topic,
                                          const std::string& messageIdentifier,
                                          const std::string& resumeStrategy,
                                          const std::string& instanceIdentifier = "") {
        if (config_.debug_logging) {
            DEBUG_LOG("send_subscription_request called: topic=", topic, " messageIdentifier=", messageIdentifier, 
                     " resumeStrategy=", resumeStrategy, " instanceIdentifier=", instanceIdentifier);
        }
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

        Json::Value payload;
        payload["topic"] = topic;
        payload["action"] = "SUBSCRIBE";
        payload["messageIdentifier"] = messageIdentifier;
        payload["resumeStrategy"] = resumeStrategy;
        
        // Add instance identifier for load balancing - use auto-generated one or provided one
        std::string effective_instance_id = instanceIdentifier.empty() ? config_.instance_identifier : instanceIdentifier;
        payload["instanceIdentifier"] = effective_instance_id;
        
        std::string payload_json = Json::writeString(builder, payload);
        if (config_.debug_logging) {
            logger_->debug("Sending subscription request for topic: ", topic, " (MessageID: ", message_id, ", InstanceID: ", effective_instance_id, ")");
        }

        if (!session_manager_->publish_message("_subscriptions", message_type, message_id,
                                               payload_json, headers_json)) {
            throw ClusterClientException("Failed to send subscription request");
        }

        // Track subscribed topic
        add_subscribed_topic(topic);
        
        // Store the message identifier and instance identifier for this subscription
        {
            std::lock_guard<std::mutex> lock(subscription_mutex_);
            subscription_message_identifiers_[topic] = messageIdentifier;
            if (config_.debug_logging) {
                DEBUG_LOG("Stored message identifier for topic ", topic, ": ", messageIdentifier);
                DEBUG_LOG("Stored instance identifier for topic ", topic, ": ", effective_instance_id);
            }
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
        polling_active_.store(false);
        if (polling_thread_.joinable() && polling_thread_.get_id() != std::this_thread::get_id()) {
            polling_thread_.join();
        }
    }

    int poll_messages(int max_messages) {
        if (!egress_subscription_ || egress_subscription_->imageCount() <= 0) {
            return 0;
        }

        int messages_processed = 0;
        int max_to_process = (max_messages > 0) ? max_messages : PerformanceConfig::DEFAULT_POLL_BATCH_SIZE;

        try {
            // Use optimized batch processing with message limit
            const int fragments = MessageBatchProcessor::process_batch(
                *egress_subscription_,
                [this](aeron::AtomicBuffer& buffer, aeron::util::index_t offset,
                       aeron::util::index_t length, aeron::logbuffer::Header& header) {
                    egress_reassembler_->onFragment(buffer, offset, length, header);
                });
            
            // Limit the number of messages processed
            messages_processed = std::min(fragments, max_to_process);
            stats_.messages_received += messages_processed;
        } catch (const std::exception& e) {
            DEBUG_LOG("Error polling messages: ", e.what());
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
            std::this_thread::sleep_for(PerformanceConfig::RECONNECT_DELAY);
            return connect();
        });
    }

    std::string send_unsubscription_request(const std::string& topic, const std::string& messageIdentifier = "") {
        if (!is_connected()) {
            throw NotConnectedException();
        }

        std::string message_id = OrderUtils::generate_message_id("msg");
        std::string message_type = "UNSUBSCRIBE";

        Json::Value headers;
        headers["messageId"] = message_id;
        headers["topic"] = "_subscriptions";

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::string headers_json = Json::writeString(builder, headers);

        Json::Value payload;
        payload["topic"] = topic;
        payload["action"] = "UNSUBSCRIBE";
        payload["messageIdentifier"] = messageIdentifier.empty() ? Json::Value::null : Json::Value(messageIdentifier);
        std::string payload_json = Json::writeString(builder, payload);
        
        if (config_.debug_logging) {
            logger_->debug("Sending unsubscription request for topic: ", topic, " (MessageID: ", message_id, ")");
        }

        if (!session_manager_->publish_message("_subscriptions", message_type, message_id,
                                               payload_json, headers_json)) {
            throw ClusterClientException("Failed to send unsubscription request");
        }

        // Remove topic from tracking
        remove_subscribed_topic(topic);
        
        // Clean up stored message identifier for this subscription
        {
            std::lock_guard<std::mutex> lock(subscription_mutex_);
            subscription_message_identifiers_.erase(topic);
        }

        stats_.messages_sent++;
        return message_id;
    }

    void commit_message(const std::string& topic, const std::string& message_identifier,
                       const std::string& message_id, std::uint64_t timestamp_nanos, 
                       std::uint64_t sequence_number) {
        if (commit_manager_) {
            commit_manager_->commit_message(topic, message_identifier, message_id, timestamp_nanos, sequence_number);
        }
    }

    std::shared_ptr<CommitOffset> get_last_commit(const std::string& topic, 
                                                const std::string& message_identifier) const {
        if (commit_manager_) {
            return commit_manager_->get_last_commit(topic, message_identifier);
        }
        return nullptr;
    }

    bool send_commit_request(const std::string& topic) {
        if (!is_connected() || !commit_manager_) {
            return false;
        }

        auto frame = commit_manager_->build_commit_message(topic, config_.client_id);
        return session_manager_->send_raw_message(frame);
    }

    bool send_commit_offset(const std::string& topic, const CommitOffset& offset) {
        if (config_.debug_logging) {
            DEBUG_LOG("send_commit_offset called: is_connected=", is_connected(), 
                     " has_commit_manager=", (commit_manager_ != nullptr),
                     " has_session_manager=", (session_manager_ != nullptr),
                     " session_id=", get_session_id());
        }
        
        if (!is_connected() || !commit_manager_) {
            if (config_.debug_logging) {
                DEBUG_LOG("send_commit_offset failed: not connected or no commit manager");
            }
            return false;
        }

        // Use the message identifier from the offset, not the default client_id
        auto frame = commit_manager_->build_commit_offset_message(topic, offset.message_identifier, offset);
        
        // CRITICAL FIX: Use send_combined_message() instead of send_raw_message()
        // to include the session envelope wrapper that the cluster expects
        bool result = session_manager_->send_combined_message(frame);
        
        if (config_.debug_logging) {
            DEBUG_LOG("send_commit_offset result: ", result, " session_id: ", get_session_id());
        }
        
        return result;
    }

    bool resume_from_last_commit(const std::string& topic, const std::string& message_identifier) {
        if (!is_connected() || !commit_manager_) {
            return false;
        }

        // Get the last commit for this topic and message identifier
        auto last_commit = commit_manager_->get_last_commit(topic, message_identifier);
        if (!last_commit) {
            // No previous commit, subscribe from latest
            return send_subscription_request(topic, message_identifier, "LATEST") != "";
        }

        // Send commit offset to resume from last known position
        return send_commit_offset(topic, *last_commit);
    }
    
    // Load balancing and instance management implementation
    void set_instance_identifier(const std::string& instance_id) {
        config_.instance_identifier = instance_id;
    }
    
    std::string get_instance_identifier() const {
        return config_.instance_identifier;
    }
    
    bool subscribe_topic_with_load_balancing(const std::string& topic, const std::string& message_identifier,
                                          const std::string& resume_strategy) {
        return send_subscription_request(topic, message_identifier, resume_strategy, config_.instance_identifier) != "";
    }
    
    bool unsubscribe_topic_with_cleanup(const std::string& topic, const std::string& message_identifier) {
        return send_unsubscription_request(topic, message_identifier) != "";
    }

    // Message deduplication helpers
    bool is_message_processed(const std::string& message_id) {
        std::lock_guard<std::mutex> lock(message_mutex_);
        return processed_messages_.find(message_id) != processed_messages_.end();
    }

    void mark_message_processed(const std::string& message_id) {
        std::lock_guard<std::mutex> lock(message_mutex_);
        processed_messages_.insert(message_id);
        
        // Cleanup old messages to prevent memory leak (keep last 1000)
        if (processed_messages_.size() > 1000) {
            // Remove oldest entries (simple cleanup - remove first 100)
            int count = 0;
            for (auto it = processed_messages_.begin(); it != processed_messages_.end() && count < 100; ) {
                it = processed_messages_.erase(it);
                count++;
            }
        }
    }

    // Extract topic from message - try multiple sources
    std::string extract_topic_from_message(const ParseResult& result) {
        // First try to extract from headers JSON
        if (!result.headers.empty()) {
            try {
                Json::Value root;
                Json::CharReaderBuilder builder;
                std::string errors;
                std::istringstream stream(result.headers);
                
                if (Json::parseFromStream(builder, stream, &root, &errors)) {
                    if (root.isMember("topic")) {
                        return root["topic"].asString();
                    }
                }
            } catch (const std::exception& e) {
                DEBUG_LOG("Failed to parse headers for topic: ", e.what());
            }
        }
        
        // If headers don't contain topic, try to extract from payload
        if (!result.payload.empty()) {
            try {
                Json::Value root;
                Json::CharReaderBuilder builder;
                std::string errors;
                std::istringstream stream(result.payload);
                
                if (Json::parseFromStream(builder, stream, &root, &errors)) {
                    // Look for topic in various places in the payload
                    if (root.isMember("topic")) {
                        return root["topic"].asString();
                    }
                    if (root.isMember("message") && root["message"].isMember("topic")) {
                        return root["message"]["topic"].asString();
                    }
                }
            } catch (const std::exception& e) {
                DEBUG_LOG("Failed to parse payload for topic: ", e.what());
            }
        }
        
        // If we have subscribed topics, use the first one as fallback
        // This is a reasonable assumption since the client typically subscribes to one topic at a time
        std::lock_guard<std::mutex> lock(topics_mutex_);
        if (!subscribed_topics_.empty()) {
            return *subscribed_topics_.begin();
        }
        
        // Fallback to default
        return "default";
    }

    std::string extract_message_identifier_from_headers(const std::string& headers_json, const std::string& topic = "") {
        // Check if headers_json is empty or just "{}" (empty JSON object)
        if (headers_json.empty() || headers_json == "{}") {
            // Use stored message identifier for this subscription if available
            if (!topic.empty()) {
                std::lock_guard<std::mutex> lock(subscription_mutex_);
                auto it = subscription_message_identifiers_.find(topic);
                if (it != subscription_message_identifiers_.end()) {
                    logger_->debug("Using stored message identifier for topic ", topic, ": ", it->second);
                    return it->second;
                } else {
                    logger_->debug("No stored message identifier found for topic: ", topic, ", using default: ", config_.client_id);
                }
            }
            return config_.client_id;
        }
        
        try {
            Json::Value root;
            Json::CharReaderBuilder builder;
            std::string errors;
            std::istringstream stream(headers_json);
            
            if (Json::parseFromStream(builder, stream, &root, &errors)) {
                if (root.isMember("messageIdentifier")) {
                    return root["messageIdentifier"].asString();
                }
                if (root.isMember("clientId")) {
                    return root["clientId"].asString();
                }
            }
        } catch (const std::exception& e) {
            DEBUG_LOG("Failed to parse headers for message identifier: ", e.what());
        }
        
        return config_.client_id;
    }

    // Check if message should be filtered out from auto-commit
    bool should_skip_auto_commit(const ParseResult& result) {
        // Skip acknowledgments and control messages
        if (result.message_type == "acknowledgment" || 
            result.message_type == "acknowledgment_legacy" ||
            result.message_type == "Acknowledgment" ||
            result.message_type == "Acknowledgment_legacy" ||
            result.message_type == "COMMIT_RESPONSE" ||
            result.message_type == "SUBSCRIBE") {
            return true;
        }

        // Skip acknowledgment messages by checking message ID pattern
        if (!result.message_id.empty() && result.message_id.find("ack_") == 0) {
            return true;
        }

        // Skip control topics (extract from message) - but allow REPLAY_COMPLETE messages
        std::string topic = extract_topic_from_message(result);
        if (topic == "_ack" || 
            topic == "_subscriptions") {
            return true;
        }
        
        // Allow _control messages that are REPLAY_COMPLETE
        if (topic == "_control") {
            // Check if this is a REPLAY_COMPLETE message
            if (result.message_type == "REPLAY_COMPLETE" || 
                result.payload.find("REPLAY_COMPLETE") != std::string::npos) {
                return false; // Don't skip REPLAY_COMPLETE messages
            }
            return true; // Skip other control messages
        }

        // Skip messages without payload (likely control messages)
        if (result.payload.empty()) {
            return true;
        }

        return false;
    }

    // Send commit offset to server asynchronously
    void send_commit_offset_async(const std::string& topic, 
                                const std::string& message_identifier,
                                const std::string& message_id,
                                std::uint64_t timestamp_nanos,
                                std::uint64_t sequence_number) {
        if (config_.debug_logging) {
            DEBUG_LOG("Starting async commit offset send for topic: ", topic, " messageID: ", message_id);
        }
        
        std::thread([this, topic, message_identifier, message_id, timestamp_nanos, sequence_number]() {
            try {
                CommitOffset offset(topic, message_identifier, message_id, timestamp_nanos, sequence_number);
                if (!send_commit_offset(topic, offset)) {
                    DEBUG_LOG("Failed to send commit offset to server for topic: ", topic);
                } else {
                    DEBUG_LOG("Sent commit offset to server for topic: ", topic, " messageID: ", message_id);
                }
            } catch (const std::exception& e) {
                DEBUG_LOG("Error sending commit offset: ", e.what());
            }
        }).detach();
    }

    // Resume subscribed topics from last commit
    void resume_subscribed_topics() {
        std::lock_guard<std::mutex> lock(topics_mutex_);
        
        for (const auto& topic : subscribed_topics_) {
            if (config_.debug_logging) {
                DEBUG_LOG("Resuming subscription for topic: ", topic);
            }
            
            // Get the stored message identifier for this topic
            std::string message_identifier = config_.client_id; // fallback to default
            {
                std::lock_guard<std::mutex> sub_lock(subscription_mutex_);
                auto it = subscription_message_identifiers_.find(topic);
                if (it != subscription_message_identifiers_.end()) {
                    message_identifier = it->second;
                }
            }
            
            // Get the last commit for this topic with the correct message identifier
            auto last_commit = commit_manager_->get_last_commit(topic, message_identifier);
            if (last_commit) {
                // Resume from last commit
                if (send_commit_offset(topic, *last_commit)) {
                    DEBUG_LOG("Resumed from last commit for topic: ", topic, " messageID: ", last_commit->message_id);
                } else {
                    DEBUG_LOG("Failed to send commit offset for topic: ", topic);
                }
            } else {
                // No previous commit, subscribe from latest
                if (send_subscription_request(topic, message_identifier, "LATEST") != "") {
                    DEBUG_LOG("Subscribed to topic from latest: ", topic);
                } else {
                    DEBUG_LOG("Failed to subscribe to topic: ", topic);
                }
            }
        }
    }

    // Add topic to subscribed topics tracking
    void add_subscribed_topic(const std::string& topic) {
        std::lock_guard<std::mutex> lock(topics_mutex_);
        subscribed_topics_.insert(topic);
    }

    // Remove topic from subscribed topics tracking
    void remove_subscribed_topic(const std::string& topic) {
        std::lock_guard<std::mutex> lock(topics_mutex_);
        subscribed_topics_.erase(topic);
    }

   private:
    ClusterClientConfig config_;
    std::unique_ptr<SessionManager> session_manager_;
    std::shared_ptr<Logger> logger_;
    std::shared_ptr<aeron::Aeron> aeron_;
    std::shared_ptr<aeron::Subscription> egress_subscription_;
    std::unique_ptr<CommitManager> commit_manager_;

    std::atomic<ConnectionState> connection_state_;
    ConnectionStats stats_;
    bool auto_reconnect_enabled_;
    std::atomic<bool> reconnect_in_progress_{false};
    std::atomic<bool> disconnect_notified_{false};
    std::atomic<bool> suppress_disconnect_notifications_{false};

    // Callbacks
    MessageCallback message_callback_;
    ConnectionStateCallback connection_state_callback_;
    ErrorCallback error_callback_;

    // Polling
    std::atomic<bool> polling_active_{false};
    std::thread polling_thread_;
    std::thread auto_reconnect_thread_;

    std::unique_ptr<LocalFragmentReassembler> egress_reassembler_;
    
    // Subscription tracking: topic -> message identifier mapping
    std::unordered_map<std::string, std::string> subscription_message_identifiers_;
    std::mutex subscription_mutex_;

    // Message deduplication
    std::unordered_set<std::string> processed_messages_;
    mutable std::mutex message_mutex_;

    // Track subscribed topics for resume functionality
    std::unordered_set<std::string> subscribed_topics_;
    mutable std::mutex topics_mutex_;

    bool initialize_aeron() {
        try {
            aeron::Context context;
            context.aeronDir(config_.aeron_dir);
            
            // Note: Aeron doesn't have client-side keepalive configuration in Context
            // Keepalive is handled at the Media Driver level on the server side
            
            aeron_ = aeron::Aeron::connect(context);
            return true;
        } catch (const std::exception& e) {
            DEBUG_LOG("Aeron initialization error: ", e.what());
            return false;
        }
    }

    bool create_egress_subscription() {
        try {
            std::int64_t sub_id =
                aeron_->addSubscription(config_.response_channel, config_.egress_stream_id);

            auto timeout = PerformanceConfig::CONNECTION_TIMEOUT;
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
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            return false;
        } catch (const std::exception& e) {
            DEBUG_LOG("Egress subscription error: ", e.what());
            return false;
        }
    }

    void handle_incoming_message(const aeron::AtomicBuffer& buffer, aeron::util::index_t offset,
                                 aeron::util::index_t length) {
        try {
            const std::uint8_t* data =
                reinterpret_cast<const std::uint8_t*>(buffer.buffer()) + offset;
            ParseResult result = MessageParser::parse_message(data, length);
            
            if (config_.debug_logging) {
                logger_->info(
                    "Received message: {} {} {} - {} - {} - {}",
                    result.message_type,
                    result.message_id,
                    result.error_message,
                    result.headers,
                    result.payload,
                    result.sequence_number);
            }
            
            if (result.success) {
                // Handle session events first
                if (result.is_session_event()) {
                    session_manager_->handle_session_event(result);
                }

                // Check for message deduplication
                if (!result.message_id.empty() && is_message_processed(result.message_id)) {
                    // Message already processed, skip
                    logger_->debug("Message already processed, skipping: ", result.message_id);
                    // return;
                }

                // Basic debug output to see if we reach this point
                logger_->debug("Processing message: ", result.message_id, " type: ", result.message_type, " payload length: ", result.payload.length());
                // Check if message should be filtered out from auto-commit
                bool should_skip = should_skip_auto_commit(result);
                
                if (config_.debug_logging) {
                    DEBUG_LOG("Message processing: ID=", result.message_id, 
                             " type=", result.message_type, 
                             " should_skip=", should_skip,
                             " has_commit_manager=", (commit_manager_ != nullptr));
                }
                
                // Call the message callback for all successful messages
                bool callback_success = false;
                if (message_callback_) {
                    try {
                        message_callback_(result);
                        callback_success = true;
                    } catch (const std::exception& e) {
                        DEBUG_LOG("Message callback failed: ", e.what());
                        callback_success = false;
                    }
                } else {
                    callback_success = true; // No callback means success
                }

                if (config_.debug_logging) {
                    DEBUG_LOG("Callback success: ", callback_success, 
                             " should_skip: ", should_skip,
                             " has_message_id: ", !result.message_id.empty());
                }

                // Auto-commit only if callback succeeded and message should not be skipped
                if (config_.debug_logging) {
                    DEBUG_LOG("Commit check: callback_success=", callback_success, " should_skip=", should_skip, " has_commit_manager=", (commit_manager_ != nullptr), " has_message_id=", !result.message_id.empty());
                }
                if (callback_success && !should_skip && commit_manager_ && !result.message_id.empty()) {
                    // Mark message as processed
                    mark_message_processed(result.message_id);
                    
                    // Extract proper topic and message identifier from message
                    std::string topic = extract_topic_from_message(result);
                    if (config_.debug_logging) {
                        DEBUG_LOG("Extracting message identifier for topic: ", topic, " headers: ", result.headers);
                    }
                    
                    // Use the stored message identifier for this subscription, not from headers
                    std::string message_identifier = config_.client_id; // fallback to default
                    {
                        std::lock_guard<std::mutex> lock(subscription_mutex_);
                        auto it = subscription_message_identifiers_.find(topic);
                        if (it != subscription_message_identifiers_.end()) {
                            message_identifier = it->second;
                            if (config_.debug_logging) {
                                DEBUG_LOG("Using stored message identifier for topic ", topic, ": ", message_identifier);
                            }
                        } else {
                            if (config_.debug_logging) {
                                DEBUG_LOG("No stored message identifier found for topic: ", topic, ", using default: ", config_.client_id);
                            }
                        }
                    }
                    std::uint64_t timestamp_nanos = result.timestamp;
                    std::uint64_t sequence_number = result.sequence_number;
                    
                    // Commit locally
                    commit_manager_->commit_message(topic, message_identifier, result.message_id, 
                                                   timestamp_nanos, sequence_number);
                    
                    // Send commit offset to server asynchronously
                    send_commit_offset_async(topic, message_identifier, result.message_id, 
                                           timestamp_nanos, sequence_number);
                    
                    if (config_.debug_logging) {
                        DEBUG_LOG("Message processed and committed successfully for topic: ", topic);
                    }
                } else if (!callback_success) {
                    DEBUG_LOG("Message callback returned error, not committing: ", result.message_id);
                } else if (should_skip) {
                    if (config_.debug_logging) {
                        DEBUG_LOG("Skipping auto-commit for control message: ", result.message_type);
                    }
                }
            } else {
                DEBUG_LOG("Failed to parse message: ", result.error_message);
            }
        } catch (const std::exception& e) {
            DEBUG_LOG("Error handling incoming message: ", e.what());
            if (error_callback_) {
                error_callback_("Message handling error: " + std::string(e.what()));
            }
        }
    }

    void on_session_event(const ParseResult& event) {
        if (config_.debug_logging) {
            DEBUG_LOG("Session event received: code=", event.event_code,
                      " message=", event.error_message,
                      " leader=", event.leader_member_id);
        }

        switch (event.event_code) {
            case SBEConstants::SESSION_EVENT_REDIRECT:
                stats_.current_leader_id = event.leader_member_id;
                break;
            case SBEConstants::SESSION_EVENT_CLOSED: {
                std::string reason = "Cluster session closed";
                if (!event.error_message.empty()) {
                    reason += ": " + event.error_message;
                }
                notify_connection_loss(reason);
                break;
            }
            case SBEConstants::SESSION_EVENT_ERROR: {
                std::string reason = "Cluster session error";
                if (!event.error_message.empty()) {
                    reason += ": " + event.error_message;
                }
                notify_connection_loss(reason);
                break;
            }
            case SBEConstants::SESSION_EVENT_AUTHENTICATION_REJECTED: {
                std::string reason = "Cluster session authentication rejected";
                if (!event.error_message.empty()) {
                    reason += ": " + event.error_message;
                }
                notify_connection_loss(reason, false);
                break;
            }
            default:
                break;
        }
    }

    void on_session_connection_state_changed(bool connected) {
        if (connected) {
            stats_.is_connected = true;
            disconnect_notified_.store(false);
            return;
        }

        stats_.is_connected = false;
        notify_connection_loss("Cluster session disconnected");
    }

    void notify_connection_loss(const std::string& reason, bool allow_auto_reconnect = true) {
        if (suppress_disconnect_notifications_.load()) {
            if (config_.debug_logging) {
                DEBUG_LOG("Disconnect notification suppressed (reason: ", reason, ")");
            }
            return;
        }

        if (disconnect_notified_.exchange(true)) {
            return;
        }

        if (config_.enable_console_info || config_.debug_logging) {
            logger_->info("Connection lost: ", reason);
        }

        stats_.is_connected = false;
        stats_.current_session_id = -1;
        stats_.current_leader_id = -1;

        polling_active_ = false;

        set_connection_state(ConnectionState::DISCONNECTED);

        if (error_callback_) {
            error_callback_(reason);
        }

        if (allow_auto_reconnect && auto_reconnect_enabled_) {
            schedule_auto_reconnect(reason);
        }
    }

    void schedule_auto_reconnect(const std::string& reason) {
        if (!auto_reconnect_enabled_) {
            return;
        }

        if (reconnect_in_progress_.exchange(true)) {
            if (config_.debug_logging) {
                DEBUG_LOG("Auto-reconnect already in progress, skipping new request");
            }
            return;
        }

        if (auto_reconnect_thread_.joinable()) {
            auto_reconnect_thread_.join();
        }

        auto_reconnect_thread_ = std::thread([this, reason]() {
            // Ensure previous polling loop is stopped
            stop_polling();

            for (int attempt = 1;
                 auto_reconnect_enabled_ && attempt <= PerformanceConfig::MAX_RECONNECT_ATTEMPTS;
                 ++attempt) {
                if (!auto_reconnect_enabled_) {
                    break;
                }

                if (config_.debug_logging) {
                    DEBUG_LOG("Auto-reconnect attempt ", attempt, " (reason: ", reason, ")");
                }

                std::this_thread::sleep_for(PerformanceConfig::RECONNECT_DELAY);

                if (!auto_reconnect_enabled_) {
                    break;
                }

                suppress_disconnect_notifications_.store(true);
                try {
                    if (session_manager_) {
                        session_manager_->disconnect();
                    }
                } catch (const std::exception& e) {
                    if (config_.enable_console_errors || config_.debug_logging) {
                        DEBUG_LOG("Auto-reconnect cleanup error: ", e.what());
                    }
                }
                suppress_disconnect_notifications_.store(false);

                egress_subscription_.reset();
                aeron_.reset();

                if (connect()) {
                    if (config_.debug_logging) {
                        DEBUG_LOG("Auto-reconnect successful on attempt ", attempt);
                    }
                    disconnect_notified_.store(false);
                    reconnect_in_progress_.store(false);
                    return;
                }

                stop_polling();
            }

            reconnect_in_progress_.store(false);
            if (auto_reconnect_enabled_ && error_callback_) {
                error_callback_(std::string("Auto-reconnect failed after ") +
                                std::to_string(PerformanceConfig::MAX_RECONNECT_ATTEMPTS) +
                                " attempts. Reason: " + reason);
            }
        });
    }

    void set_connection_state(ConnectionState new_state) {
        ConnectionState old_state = connection_state_.exchange(new_state);
        if (old_state != new_state && connection_state_callback_) {
            connection_state_callback_(old_state, new_state);
        }
    }
    
    bool perform_connection_health_check() {
        try {
            // Check if Aeron is still connected
            if (!aeron_) {
                DEBUG_LOG("Health check failed: Aeron is null");
                return false;
            }
            
            // Check if session manager is still connected
            if (!session_manager_ || !session_manager_->is_connected()) {
                DEBUG_LOG("Health check failed: Session manager disconnected");
                return false;
            }
            
            // Only check egress subscription if it exists
            if (egress_subscription_) {
                if (egress_subscription_->imageCount() <= 0) {
                    DEBUG_LOG("Health check failed: No active egress images");
                    return false;
                }
            } else {
                DEBUG_LOG("Health check warning: Egress subscription not yet created");
                // Don't fail health check if egress subscription doesn't exist yet
            }
            
            return true;
        } catch (const std::exception& e) {
            DEBUG_LOG("Health check exception: ", e.what());
            return false;
        }
    }

    void polling_worker() {
        auto last_health_check = std::chrono::steady_clock::now();
        auto last_activity = std::chrono::steady_clock::now();
        auto connection_start_time = std::chrono::steady_clock::now();
        const auto health_check_interval = std::chrono::seconds(10); // Less frequent checks
        const auto max_idle_time = std::chrono::seconds(60); // Longer idle timeout
        const auto connection_grace_period = std::chrono::seconds(15); // Grace period for initial connection
        
        while (polling_active_.load()) {
            try {
                auto now = std::chrono::steady_clock::now();
                bool is_in_grace_period = (now - connection_start_time) < connection_grace_period;
                
                // Reset grace period if we just connected
                if (connection_state_ == ConnectionState::CONNECTED && is_in_grace_period) {
                    // Check if this is a fresh connection (session manager just connected)
                    if (session_manager_ && session_manager_->is_connected()) {
                        // DEBUG_LOG("Fresh connection detected, extending grace period");
                        connection_start_time = now; // Reset grace period
                        is_in_grace_period = true;
                    }
                }
                
                // During grace period, only check basic connection state
                // After grace period, do full connection validation
                if (!is_in_grace_period) {
                    // Full connection check only after grace period
                    if (!is_connected() || !egress_subscription_ || egress_subscription_->imageCount() <= 0) {
                        DEBUG_LOG("Connection lost or no active images, stopping polling");
                        notify_connection_loss("Connection lost or no active images");
                        break; // Exit polling loop
                    }
                } else {
                    // During grace period, only check if we're still trying to connect
                    if (connection_state_ == ConnectionState::DISCONNECTED) {
                        DEBUG_LOG("Connection state changed to DISCONNECTED during grace period");
                        break;
                    }
                }
                
                // Periodic health check (less frequent)
                if (now - last_health_check >= health_check_interval) {
                    if (!perform_connection_health_check()) {
                        // Only disconnect if we're past the grace period
                        if (!is_in_grace_period) {
                            DEBUG_LOG("Connection health check failed, disconnecting");
                            notify_connection_loss("Connection health check failed");
                            break;
                        } else {
                            DEBUG_LOG("Connection health check failed during grace period, continuing...");
                        }
                    }
                    last_health_check = now;
                }
                
                int processed = poll_messages(PerformanceConfig::DEFAULT_POLL_BATCH_SIZE);
                if (processed > 0) {
                    last_activity = now; // Update activity timestamp
                } else {
                    // Check for idle timeout (only after grace period)
                    if (!is_in_grace_period && (now - last_activity >= max_idle_time)) {
                        DEBUG_LOG("Client idle for too long, disconnecting");
                        notify_connection_loss("Client idle for too long");
                        break;
                    }
                    std::this_thread::sleep_for(PerformanceConfig::POLL_INTERVAL);
                }
            } catch (const std::exception& e) {
                DEBUG_LOG("Polling error: ", e.what());
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
// Removed unused manual_decode_topic_message function

// ClusterClient public interface implementation
ClusterClient::ClusterClient(const ClusterClientConfig& config)
    : cfg_(config), pImpl_(std::make_unique<Impl>(config)) {
    handler_.set_ack_callback([this](const AckInfo& ack) { this->on_ack_default(ack); });
}

ClusterClient::~ClusterClient() {
    // Unregister this client from signal handling
    auto logger = LoggerFactory::instance().getLogger("cluster_client");
    logger->info("Destructor called, unregistering client from signal handling (session_id={})",
                 pImpl_->get_session_id());
    SignalHandlerManager::instance().unregister_client(this);
}

std::future<bool> ClusterClient::connect_async() {
    // Enable automatic signal handling on first connect
    enable_automatic_signal_handling();
    return pImpl_->connect_async();
}

bool ClusterClient::connect() {
    // Enable automatic signal handling on first connect
    enable_automatic_signal_handling();
    return pImpl_->connect();
}

bool ClusterClient::connect_with_timeout(std::chrono::milliseconds timeout) {
    // Enable automatic signal handling on first connect
    enable_automatic_signal_handling();
    return pImpl_->connect_with_timeout(timeout);
}

void ClusterClient::enable_automatic_signal_handling() {
    auto logger = LoggerFactory::instance().getLogger("cluster_client");
    logger->info("Enabling automatic signal handling");
    // Register this client for automatic signal handling
    // shared_from_this() throws std::bad_weak_ptr if not owned by std::shared_ptr
    try {
        auto self = shared_from_this();
        SignalHandlerManager::instance().register_client(self);
        logger->info("Successfully registered client for automatic signal handling");
    } catch (const std::bad_weak_ptr&) {
        logger->error("Client not managed by std::shared_ptr! Cannot register for automatic signal handling.");
        logger->error("Make sure to create clients with std::make_shared<ClusterClient>(...)");
        // Client not managed by std::shared_ptr; skip auto signal handling
        // Users can still register manually by creating the client in a shared_ptr
    } catch (const std::exception& e) {
        std::cerr << "[SignalHandler] ERROR: Exception while registering client: " << e.what() << std::endl;
    }
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

std::string ClusterClient::publish_order_to_topic(const Order& order, const std::string& topic) {
    return pImpl_->publish_order_to_topic(order, topic);
}

std::future<std::string> ClusterClient::publish_order_async(const Order& order) {
    return pImpl_->publish_order_async(order);
}

std::future<std::string> ClusterClient::publish_order_to_topic_async(const Order& order, const std::string& topic) {
    return pImpl_->publish_order_to_topic_async(order, topic);
}

std::string ClusterClient::send_subscription_request(const std::string& topic,
                                                     const std::string& messageIdentifier,
                                                     const std::string& resumeStrategy,
                                                     const std::string& instanceIdentifier) {
    return pImpl_->send_subscription_request(topic, messageIdentifier, resumeStrategy, instanceIdentifier);
}

std::string ClusterClient::publish_message(const std::string& message_type,
                                           const std::string& payload, const std::string& headers) {
    return pImpl_->publish_message(message_type, payload, headers);
}

std::string ClusterClient::publish_message_to_topic(const std::string& message_type,
                                                   const std::string& payload, const std::string& headers, const std::string& topic) {
    return pImpl_->publish_message_to_topic(message_type, payload, headers, topic);
}

std::future<std::string> ClusterClient::publish_message_async(const std::string& message_type,
                                                              const std::string& payload,
                                                              const std::string& headers) {
    return pImpl_->publish_message_async(message_type, payload, headers);
}

std::future<std::string> ClusterClient::publish_message_to_topic_async(const std::string& message_type,
    const std::string& payload, const std::string& headers, const std::string& topic) {
    return pImpl_->publish_message_to_topic_async(message_type, payload, headers, topic);
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
    msg.sequenceNumber(0); // Will be set by server, client sends 0

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
    std::string message_id = pImpl_->send_subscription_request(std::string(topic), "", std::string(resume_strategy));
    return !message_id.empty();
}

bool ClusterClient::unsubscribe_topic(std::string_view topic) {
    std::string message_id = pImpl_->send_unsubscription_request(std::string(topic), "");
    return !message_id.empty();
}

std::string ClusterClient::send_unsubscription_request(const std::string& topic, const std::string& messageIdentifier) {
    return pImpl_->send_unsubscription_request(topic, messageIdentifier);
}

void ClusterClient::commit_message(const std::string& topic, const std::string& message_identifier,
                                  const std::string& message_id, std::uint64_t timestamp_nanos, 
                                  std::uint64_t sequence_number) {
    pImpl_->commit_message(topic, message_identifier, message_id, timestamp_nanos, sequence_number);
}

std::shared_ptr<CommitOffset> ClusterClient::get_last_commit(const std::string& topic, 
                                                            const std::string& message_identifier) const {
    return pImpl_->get_last_commit(topic, message_identifier);
}

bool ClusterClient::send_commit_request(const std::string& topic) {
    return pImpl_->send_commit_request(topic);
}

bool ClusterClient::send_commit_offset(const std::string& topic, const CommitOffset& offset) {
    return pImpl_->send_commit_offset(topic, offset);
}

bool ClusterClient::resume_from_last_commit(const std::string& topic, const std::string& message_identifier) {
    return pImpl_->resume_from_last_commit(topic, message_identifier);
}

void ClusterClient::set_instance_identifier(const std::string& instance_id) {
    pImpl_->set_instance_identifier(instance_id);
}

std::string ClusterClient::get_instance_identifier() const {
    return pImpl_->get_instance_identifier();
}

bool ClusterClient::subscribe_topic_with_load_balancing(const std::string& topic, const std::string& message_identifier,
                                                      const std::string& resume_strategy) {
    return pImpl_->subscribe_topic_with_load_balancing(topic, message_identifier, resume_strategy);
}

bool ClusterClient::unsubscribe_topic_with_cleanup(const std::string& topic, const std::string& message_identifier) {
    return pImpl_->unsubscribe_topic_with_cleanup(topic, message_identifier);
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
                                               double limit_price,
                                               const std::string& identifier) {
    // Generate unique identifiers
    std::string uuid = OrderUtils::generate_uuid();
    std::string client_order_id = uuid;

    // Get current timestamp
    auto now = std::chrono::system_clock::now();
    auto timestamp_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::string create_ts = std::to_string(timestamp_ms);

    // Create order with new structure
    Order order(base_token, quote_token, side, quantity, "LIMIT", identifier);
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
                                                double /* limit_price */) {
    // Generate unique identifiers
    std::string uuid = OrderUtils::generate_uuid();
    std::string client_order_id = uuid;

    // Get current timestamp
    auto now = std::chrono::system_clock::now();
    auto timestamp_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::string create_ts = std::to_string(timestamp_ms);

    // Create the complete message structure to match terminal output exactly
    Json::Value message;

    // Top level structure - matches terminal output
    message["uuid"] = client_order_id;
    message["msg_type"] = "D";

    // Nested message structure
    Json::Value nested_message;

    // Headers
    Json::Value headers;
    headers["origin"] = "fix";
    headers["origin_name"] = "FIX_GATEWAY";
    headers["origin_id"] = "SEKAR_AERON01_TX";
    headers["connection_uuid"] = "130032";
    headers["customer_id"] = "75";
    headers["ip_address"] = "10.37.62.251";
    headers["create_ts"] = create_ts;
    headers["auth_token"] =
        "Bearer "
        "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9."
        "eyJhayI6IjZTYXZHSFJkMzJCVWdXZ05NdEdBQnBZV05aaFFkMUxQZ1dsbE1oOHh4RXdqUjNOMTFvaWNMcXY2STltVk"
        "ZvcUYiLCJhdWQiOiJTRUtBUl9BRVJPTjAxX1RYIiwiZXhwIjoxNzU4NzgxMzI5LCJpYXQiOjE3NTg2MDg1MjksInNj"
        "b3BlIjoid3JpdGU6dHJhZGUgY3J1ZDpzZXNzaW9uIn0.X9h-"
        "jz1NeoKJLCFtDPmcEuqOhrCdZIzWCyxOprQ1OD07TBPRIwz0hGRM2jwIrIzBkeLJ0lFuaJPA-"
        "McjGMAkdzSozcf1d61HFK56ORfCerR_9Omgaw2EsRZv7qCkwZmBdYBbf0_7vr_"
        "YdNiL5J7a77efZdso4Ac2k9RqmsbnDMNaPtt1nC5eMwJhdwbwzKS9NdqDXGhmuXBpVj3YcweWY2uiYYC0cpILiEcFD"
        "-j0OGsDqM8QWC29cwyFYryjU16YGLesD7qluWzSBmbeqCHAg2F9oMKZO886hdHqtN3rqd6Oo8oDsd1F7yN00AJzICb"
        "qKbFq8m6RzAgBxh9kNQgdwbzgkQIY-eDLPZsRf6kNLJvA-dMjuHHLu7VssrY-kRd_WX_CWnnhwP0yfQDB-"
        "1nHHqvCjIUY_lWElnyWHtKW_7xQSDIoc8CJQ4P9xY0KPEEC-qv0kHGfvbXVEN1cqXIWdY-"
        "vRLTQxb4Rw2YDK0yZvMJggT-"
        "C68g6pFdTQzd5pSUVg45hDO8KIu0O90wvATvljWrfGfCryzkwWSWRKYRvUGXcBCgVipiEt-"
        "CT7OzfeX4mZQwU56lH4_OT4DK-Sw-lw46pjxUHgKXyENnvm8cd8xf0o2CgrKX6ChJTkHExpNuOp-lHRul7V_"
        "20MtkRVIz6Le0YtOHZK-wsFcc4UhO_r3c";

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
    order_details["order_type"] = "market";
    order_details["quantity_value_str"] = std::to_string(quantity);
    order_details["client_order_id"] = client_order_id;

    message_content["order_details"] = order_details;

    // Assemble the nested message structure
    nested_message["headers"] = headers;
    nested_message["message"] = message_content;

    // Final message structure
    message["message"] = nested_message;

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

ClusterClientConfigBuilder& ClusterClientConfigBuilder::with_client_id(
    const std::string& client_id) {
    config_.client_id = client_id;
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