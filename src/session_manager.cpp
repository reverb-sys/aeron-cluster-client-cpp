#include "aeron_cluster/session_manager.hpp"

#include <Aeron.h>
#include <json/json.h>

#include <chrono>
#include <iostream>
#include <random>
#include <regex>
#include <thread>

#include "aeron_cluster/sbe_messages.hpp"
#include "model/Acknowledgment.h"
#include "model/MessageHeader.h"
#include "model/TopicMessage.h"
#include "model/VarStringEncoding.h"

using sbe::Acknowledgment;
using sbe::MessageHeader;
using sbe::TopicMessage;

namespace aeron_cluster {

/**
 * @brief Private implementation for SessionManager (PIMPL pattern)
 */
class SessionManager::Impl {
   public:
    explicit Impl(const ClusterClientConfig& config)
        : config_(config),
          session_id_(-1),
          leader_member_id_(0),
          leadership_term_id_(-1),
          connected_(false),
          rng_(std::random_device{}()),
          correlation_id_(generate_correlation_id()) {
        create_session_message_header_buffer();
        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix
                      << " SessionManager created with correlation ID: " << correlation_id_
                      << std::endl;
        }
    }

    ~Impl() {
        disconnect();
    }

    SessionConnectionResult connect(std::shared_ptr<aeron::Aeron> aeron) {
        aeron_ = aeron;

        SessionConnectionResult result;
        auto start_time = std::chrono::steady_clock::now();

        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " Starting session connection process..."
                      << std::endl;
        }

        // Try connecting to each cluster member
        for (int attempt = 0; attempt < config_.max_retries; ++attempt) {
            for (size_t member_id = 0; member_id < config_.cluster_endpoints.size(); ++member_id) {
                if (try_connect_to_member(static_cast<int>(member_id))) {
                    result.success = true;
                    result.session_id = session_id_;
                    result.leader_member_id = leader_member_id_;
                    result.leadership_term_id = leadership_term_id_;
                    result.connection_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_time);

                    if (config_.enable_console_info) {
                        std::cout << config_.logging.log_prefix
                                  << " Successfully connected to member " << member_id
                                  << " with session ID: " << session_id_ << std::endl;
                    }
                    return result;
                }
            }

            if (attempt < config_.max_retries - 1) {
                std::this_thread::sleep_for(config_.retry_delay);
            }
        }

        result.error_message = "Failed to connect to any cluster member after " +
                               std::to_string(config_.max_retries) + " attempts";
        return result;
    }

    void disconnect() {
        if (config_.debug_logging && connected_) {
            std::cout << config_.logging.log_prefix << " Disconnecting session..." << std::endl;
        }

        ingress_publication_.reset();
        connected_ = false;
        session_id_ = -1;
    }

    bool is_connected() const {
        return connected_ && ingress_publication_ && ingress_publication_->isConnected();
    }

    std::int64_t get_session_id() const {
        return session_id_;
    }

    std::int32_t get_leader_member_id() const {
        return leader_member_id_;
    }

    std::int64_t get_leadership_term_id() const {
        return leadership_term_id_;
    }

    bool publish_message(const std::string& topic, const std::string& message_type,
                         const std::string& message_id, const std::string& payload,
                         const std::string& headers) {
        if (!is_connected()) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix << " Cannot publish - not connected"
                          << std::endl;
            }
            return false;
        }

        try {
            // Create business message (TopicMessage)
            std::vector<std::uint8_t> business_message =
                create_topic_message(topic, message_type, message_id, payload, headers);

            // Update session header with current values
            // update_session_header();

            if (config_.debug_logging) {
                std::cout << config_.logging.log_prefix
                          << " Publishing with single buffer (Go-compatible):" << std::endl;
                std::cout << config_.logging.log_prefix
                          << "   Session header size: " << session_header_buffer_.size()
                          << std::endl;
                std::cout << config_.logging.log_prefix
                          << "   Business message size: " << business_message.size() << std::endl;
                std::cout << config_.logging.log_prefix
                          << "   Leadership term: " << leadership_term_id_ << std::endl;
                std::cout << config_.logging.log_prefix << "   Session ID: " << session_id_
                          << std::endl;
            }

            // Create single combined buffer with PERFECT alignment (no extra bytes)
            return send_combined_message(business_message);

        } catch (const std::exception& e) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix
                          << " Error publishing business message: " << e.what() << std::endl;
            }
            return false;
        }
    }

    bool send_raw_message(const std::vector<std::uint8_t>& encoded_message) {
        if (!is_connected()) {
            return false;
        }

        // Debug: Print hex dump of what we're sending
        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " Sending message ("
                      << encoded_message.size() << " bytes):" << std::endl;
            SBEUtils::print_hex_dump(encoded_message.data(), encoded_message.size(),
                                     config_.logging.log_prefix + "  ", 64);
        }

        try {
            aeron::AtomicBuffer buffer(
                reinterpret_cast<std::uint8_t*>(const_cast<std::uint8_t*>(encoded_message.data())),
                static_cast<aeron::util::index_t>(encoded_message.size()));

            std::int64_t result = ingress_publication_->offer(buffer, 0, encoded_message.size());

            if (result > 0) {
                stats_.messages_sent++;
                stats_.last_message_time = std::chrono::steady_clock::now();

                if (config_.debug_logging) {
                    std::cout << config_.logging.log_prefix
                              << " Message sent successfully, position: " << result << std::endl;
                }
                return true;
            } else {
                if (config_.debug_logging) {
                    std::cout << config_.logging.log_prefix
                              << " Failed to send message, result: " << result << std::endl;
                }
                return false;
            }

        } catch (const std::exception& e) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix
                          << " Error sending raw message: " << e.what() << std::endl;
            }
            return false;
        }
    }

    bool send_keepalive() {
        if (!is_connected() || leadership_term_id_ == -1 || session_id_ == -1) {
            return false;
        }

        try {
            std::vector<std::uint8_t> encoded_message =
                SBEEncoder::encode_session_keep_alive(leadership_term_id_, session_id_);

            if (send_raw_message(encoded_message)) {
                stats_.keepalives_sent++;
                stats_.last_keepalive_time = std::chrono::steady_clock::now();
                return true;
            } else {
                stats_.keepalives_failed++;
                return false;
            }

        } catch (const std::exception& e) {
            stats_.keepalives_failed++;
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix << " Keepalive error: " << e.what()
                          << std::endl;
            }
            return false;
        }
    }

    void handle_session_event(const ParseResult& result) {
        if (!result.is_session_event()) {
            return;
        }

        stats_.session_events_received++;

        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " Handling SessionEvent:" << std::endl;
            std::cout << config_.logging.log_prefix
                      << "   Correlation ID: " << result.correlation_id << std::endl;
            std::cout << config_.logging.log_prefix << "   Event Code: " << result.event_code
                      << std::endl;
        }

        switch (result.event_code) {
            case SBEConstants::SESSION_EVENT_OK:
                handle_session_ok(result);
                break;
            case SBEConstants::SESSION_EVENT_REDIRECT:
                handle_session_redirect(result);
                break;
            case SBEConstants::SESSION_EVENT_ERROR:
            case SBEConstants::SESSION_EVENT_AUTHENTICATION_REJECTED:
                handle_session_error(result);
                break;
            case SBEConstants::SESSION_EVENT_CLOSED:
                handle_session_closed(result);
                break;
            default:
                if (config_.enable_console_warnings) {
                    std::cout << config_.logging.log_prefix
                              << " Unknown session event code: " << result.event_code << std::endl;
                }
                break;
        }

        if (session_event_callback_) {
            std::cout << config_.logging.log_prefix
                      << " Invoking session event callback for event code: " << result.event_code
                      << std::endl;
            session_event_callback_(result);
        }
    }

    void set_session_event_callback(SessionEventCallback callback) {
        session_event_callback_ = std::move(callback);
    }

    void set_connection_state_callback(ConnectionStateCallback callback) {
        connection_state_callback_ = std::move(callback);
    }

    SessionStats get_session_stats() const {
        return stats_;
    }

   private:
    ClusterClientConfig config_;
    std::shared_ptr<aeron::Aeron> aeron_;
    std::shared_ptr<aeron::ExclusivePublication> ingress_publication_;

    std::int64_t session_id_;
    std::int32_t leader_member_id_;
    std::int64_t leadership_term_id_;
    std::atomic<bool> connected_;
    std::mt19937 rng_;
    std::int64_t correlation_id_;
    SessionStats stats_;
    SessionEventCallback session_event_callback_;
    ConnectionStateCallback connection_state_callback_;
    std::vector<std::uint8_t> session_header_buffer_;  // Pre-built like Go client

    // ---- Connection management methods ----

    bool try_connect_to_member(int member_id) {
        if (member_id >= static_cast<int>(config_.cluster_endpoints.size())) {
            return false;
        }

        std::string endpoint = config_.cluster_endpoints[member_id];

        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " Attempting connection to member "
                      << member_id << ": " << endpoint << std::endl;
        }

        try {
            // Create ingress publication
            std::string ingress_channel = "aeron:udp?endpoint=" + endpoint;
            std::int64_t pub_id =
                aeron_->addExclusivePublication(ingress_channel, config_.ingress_stream_id);

            // Wait for publication to connect
            auto timeout = std::chrono::seconds(5);
            auto start_time = std::chrono::steady_clock::now();

            while ((std::chrono::steady_clock::now() - start_time) < timeout) {
                ingress_publication_ = aeron_->findExclusivePublication(pub_id);
                if (ingress_publication_ && ingress_publication_->isConnected()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            if (!ingress_publication_ || !ingress_publication_->isConnected()) {
                return false;
            }

            // Send SessionConnectRequest
            if (!send_session_connect_request()) {
                return false;
            }

            leader_member_id_ = member_id;

            // Wait briefly for session establishment
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            return connected_;

        } catch (const std::exception& e) {
            if (config_.enable_console_warnings) {
                std::cout << config_.logging.log_prefix << " Error connecting to member "
                          << member_id << ": " << e.what() << std::endl;
            }
            return false;
        }
    }

    bool send_session_connect_request() {
        try {
            std::vector<std::uint8_t> encoded_message = SBEEncoder::encode_session_connect_request(
                correlation_id_, config_.egress_stream_id, config_.response_channel,
                config_.protocol_semantic_version);

            aeron::AtomicBuffer buffer(encoded_message.data(), encoded_message.size());
            std::int64_t result = ingress_publication_->offer(buffer, 0, encoded_message.size());

            if (result > 0) {
                if (config_.debug_logging) {
                    std::cout << config_.logging.log_prefix
                              << " SessionConnectRequest sent successfully" << std::endl;
                }
                return true;
            } else {
                if (config_.enable_console_errors) {
                    std::cerr << config_.logging.log_prefix
                              << " Failed to send SessionConnectRequest: " << result << std::endl;
                }
                return false;
            }

        } catch (const std::exception& e) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix
                          << " Error sending SessionConnectRequest: " << e.what() << std::endl;
            }
            return false;
        }
    }
    // ---- End of connection management methods ----

    // ---- Session handling methods ----
    void create_session_message_header_buffer() {
        // Create exactly like Go client: 32 bytes total
        session_header_buffer_.resize(32);

        size_t offset = 0;

        // SBE Header (8 bytes) - FIXED: block_length should be 24, not variable
        write_uint16_le(session_header_buffer_, offset,
                        24);  // FIXED: Always 24 (SessionMessageHeader.BLOCK_LENGTH)
        offset += 2;
        write_uint16_le(session_header_buffer_, offset,
                        1);  // template ID = 1 (SessionMessageHeader)
        offset += 2;
        write_uint16_le(session_header_buffer_, offset, 111);  // schema ID = 111 (cluster)
        offset += 2;
        write_uint16_le(session_header_buffer_, offset, 8);  // schema version = 8
        offset += 2;

        // Fixed Block (24 bytes) - will be updated in update_session_header()
        write_int64_le(session_header_buffer_, offset, 0);  // leadership_term_id (updated later)
        offset += 8;
        write_int64_le(session_header_buffer_, offset, 0);  // cluster_session_id (updated later)
        offset += 8;
        write_int64_le(session_header_buffer_, offset, 0);  // timestamp = 0
        offset += 8;

        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix
                      << " Created session header buffer (32 bytes) with FIXED block_length=24"
                      << std::endl;
        }
    }

    void update_session_header() {
        // Update the pre-built buffer with current session values
        size_t offset = 8;  // Skip SBE header, go to fixed block

        std::cout << config_.logging.log_prefix << " Updating session header with current values..."
                  << "leadership_term_id: " << leadership_term_id_
                  << ", session_id: " << session_id_ << std::endl;
        write_int64_le(session_header_buffer_, offset,
                       leadership_term_id_);  // FIXED: Use actual value
        offset += 8;
        write_int64_le(session_header_buffer_, offset, session_id_);
        offset += 8;
        write_int64_le(session_header_buffer_, offset, 0);  // timestamp = 0

        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix << " Updated session header with:" << std::endl;
            std::cout << config_.logging.log_prefix << "   Leadership term: " << leadership_term_id_
                      << std::endl;
            std::cout << config_.logging.log_prefix << "   Session ID: " << session_id_
                      << std::endl;

            // Print the actual header bytes for verification
            std::cout << config_.logging.log_prefix << " Session header hex: ";
            for (size_t i = 0; i < session_header_buffer_.size(); ++i) {
                std::cout << std::hex << std::setfill('0') << std::setw(2)
                          << static_cast<unsigned>(session_header_buffer_[i]) << " ";
                if ((i + 1) % 8 == 0)
                    std::cout << " ";
            }
            std::cout << std::dec << std::endl;
        }
    }
    // ---- End of session handling methods ----


    std::vector<std::uint8_t> create_topic_message(const std::string& topic,
                                                   const std::string& message_type,
                                                   const std::string& message_id,
                                                   const std::string& payload,
                                                   const std::string& headers) {
        if (config_.debug_logging) {
            std::cout << config_.logging.log_prefix
                      << " === CREATING TOPIC MESSAGE WITH SBE CLASSES (FIXED) ===" << std::endl;
        }

        try {
            // Calculate total size needed
            size_t total_size = sbe::TopicMessage::sbeBlockAndHeaderLength() +
                                sbe::TopicMessage::computeLength(
                                    topic.length(), message_type.length(), message_id.length(),
                                    payload.length(), headers.length());

            std::vector<uint8_t> buffer(total_size);
            char* bufferPtr = reinterpret_cast<char*>(buffer.data());

            // Create and setup the TopicMessage
            sbe::TopicMessage topicMessage;
            topicMessage.wrapAndApplyHeader(bufferPtr, 0, buffer.size());

            // Set timestamp
            int64_t timestamp = get_current_timestamp();
            topicMessage.timestamp(static_cast<uint64_t>(timestamp));

            // Set fields in the correct order
            topicMessage.putTopic(topic);
            topicMessage.putMessageType(message_type);
            topicMessage.putUuid(message_id);
            topicMessage.putPayload(payload);
            topicMessage.putHeaders(headers);

            // Get actual encoded length
            size_t actual_length = topicMessage.encodedLength();
            buffer.resize(actual_length);

            if (config_.debug_logging) {
                std::cout << config_.logging.log_prefix << " SBE encoding successful:" << std::endl;
                std::cout << config_.logging.log_prefix << "   Encoded length: " << actual_length
                          << std::endl;
                std::cout << config_.logging.log_prefix
                          << "   Template ID: " << sbe::TopicMessage::sbeTemplateId() << std::endl;
                std::cout << config_.logging.log_prefix
                          << "   Schema ID: " << sbe::TopicMessage::sbeSchemaId() << std::endl;

                // Verify the first few fields by re-reading
                sbe::TopicMessage verifyMsg;
                char* verifyPtr = reinterpret_cast<char*>(buffer.data());
                verifyMsg.wrapForDecode(verifyPtr, 8, 8, 1,
                                        buffer.size());  // Skip header for verification

                std::string verify_topic = verifyMsg.getTopicAsString();
                std::cout << config_.logging.log_prefix << "   Verification - topic: \""
                          << verify_topic << "\"" << std::endl;
            }

            return buffer;

        } catch (const std::exception& e) {
            std::cout << "[ERROR] SBE encoding exception: " << e.what() << std::endl;
            throw;
        }
    }

    bool send_combined_message(const std::vector<std::uint8_t>& business_message) {
        if (!is_connected()) {
            return false;
        }

        try {
            // Create single buffer: SessionHeader (32 bytes) + BusinessMessage
            size_t total_size = session_header_buffer_.size() + business_message.size();
            std::vector<std::uint8_t> combined_buffer;
            combined_buffer.reserve(total_size);

            // Copy session header first (32 bytes)
            combined_buffer.insert(combined_buffer.end(), session_header_buffer_.begin(),
                                   session_header_buffer_.end());

            // Copy business message immediately after (no padding, no gap)
            combined_buffer.insert(combined_buffer.end(), business_message.begin(),
                                   business_message.end());

            if (config_.debug_logging) {
                std::cout << config_.logging.log_prefix
                          << " Combined message verification:" << std::endl;
                std::cout << config_.logging.log_prefix
                          << "   Total size: " << combined_buffer.size() << std::endl;

                // Verify the business message header starts correctly at offset 32
                if (combined_buffer.size() >= 40) {
                    std::cout << config_.logging.log_prefix << "   Business header at offset 32: ";
                    for (size_t i = 32; i < 40; ++i) {
                        std::cout << std::hex << std::setfill('0') << std::setw(2)
                                  << static_cast<unsigned>(combined_buffer[i]) << " ";
                    }
                    std::cout << std::dec << std::endl;

                    // Verify this matches the expected TopicMessage header (08 00 01 00 01 00 01
                    // 00)
                    bool header_correct =
                        (combined_buffer[32] == 0x08 && combined_buffer[33] == 0x00 &&
                         combined_buffer[34] == 0x01 && combined_buffer[35] == 0x00 &&
                         combined_buffer[36] == 0x01 && combined_buffer[37] == 0x00 &&
                         combined_buffer[38] == 0x01 && combined_buffer[39] == 0x00);
                    std::cout << config_.logging.log_prefix
                              << "   Business header valid: " << (header_correct ? "YES" : "NO")
                              << std::endl;
                }
            }

            // Send the message
            aeron::AtomicBuffer buffer(combined_buffer.data(), combined_buffer.size());
            std::int64_t result = ingress_publication_->offer(
                buffer, 0, static_cast<aeron::util::index_t>(combined_buffer.size()));

            if (result > 0) {
                stats_.messages_sent++;
                stats_.last_message_time = std::chrono::steady_clock::now();

                if (config_.debug_logging) {
                    std::cout << config_.logging.log_prefix
                              << " Combined message sent successfully, position: " << result
                              << std::endl;
                }
                return true;
            } else {
                if (config_.debug_logging) {
                    std::cout << config_.logging.log_prefix
                              << " Combined message send failed, result: " << result << std::endl;
                }
                return false;
            }

        } catch (const std::exception& e) {
            if (config_.enable_console_errors) {
                std::cerr << config_.logging.log_prefix
                          << " Error sending combined message: " << e.what() << std::endl;
            }
            return false;
        }
    }

    // ---- Session event handling methods ----

    void handle_session_redirect(const ParseResult& result) {
        stats_.redirects_received++;

        if (config_.enable_console_info) {
            std::cout << config_.logging.log_prefix
                      << " Redirected to leader member: " << result.leader_member_id << std::endl;
        }

        // Handle redirect logic here
        leader_member_id_ = result.leader_member_id;
    }

    void handle_session_ok(const ParseResult& result) {
        session_id_ = result.correlation_id;
        connected_ = true;
        leadership_term_id_ = result.leadership_term_id;
        leader_member_id_ = result.leader_member_id;

        update_session_header();
        stats_.session_start_time = std::chrono::steady_clock::now();

        if (config_.enable_console_info) {
            std::cout << config_.logging.log_prefix << " Session established successfully!"
                      << std::endl;
            std::cout << config_.logging.log_prefix << "   Session ID: " << session_id_
                      << std::endl;
            std::cout << config_.logging.log_prefix << "   Leadership Term: " << leadership_term_id_
                      << std::endl;
            std::cout << config_.logging.log_prefix << "   Leader Member: " << leader_member_id_
                      << std::endl;
        }

        if (connection_state_callback_) {
            connection_state_callback_(true);
        }
    }

    void handle_session_error(const ParseResult& result) {
        if (config_.enable_console_errors) {
            std::cerr << config_.logging.log_prefix << " Session error: "
                      << SBEUtils::get_session_event_code_string(result.event_code) << std::endl;
        }

        connected_ = false;

        if (connection_state_callback_) {
            connection_state_callback_(false);
        }
    }

    void handle_session_closed(const ParseResult& result) {
        if (config_.enable_console_info) {
            std::cout << config_.logging.log_prefix << " Session closed by cluster" << std::endl;
            std::cout << config_.logging.log_prefix << " Error ? " << result.error_message
                      << std::endl;
        }

        connected_ = false;
        session_id_ = -1;

        if (connection_state_callback_) {
            connection_state_callback_(false);
        }
    }
    // ---- End of session event handling methods ----

    // Helper methods

    // Helper function to verify SBE configuration matches Go client
    void verify_sbe_configuration() {
        std::cout << "=== SBE Configuration Verification ===" << std::endl;
        std::cout << "TopicMessage:" << std::endl;
        std::cout << "  Template ID: " << sbe::TopicMessage::sbeTemplateId() << " (should be 1)"
                  << std::endl;
        std::cout << "  Schema ID: " << sbe::TopicMessage::sbeSchemaId() << " (should be 1)"
                  << std::endl;
        std::cout << "  Schema Version: " << sbe::TopicMessage::sbeSchemaVersion()
                  << " (should be 1)" << std::endl;
        std::cout << "  Block Length: " << sbe::TopicMessage::sbeBlockLength() << " (should be 8)"
                  << std::endl;

        std::cout << "VarStringEncoding:" << std::endl;
        std::cout << "  Schema ID: " << sbe::VarStringEncoding::sbeSchemaId() << " (should be 1)"
                  << std::endl;
        std::cout << "  Schema Version: " << sbe::VarStringEncoding::sbeSchemaVersion()
                  << " (should be 1)" << std::endl;

        // Verify header lengths are correct (should all be 2 for uint16 length prefixes)
        std::cout << "Header Lengths:" << std::endl;
        std::cout << "  Topic: " << sbe::TopicMessage::topicHeaderLength() << " (should be 2)"
                  << std::endl;
        std::cout << "  MessageType: " << sbe::TopicMessage::messageTypeHeaderLength()
                  << " (should be 2)" << std::endl;
        std::cout << "  UUID: " << sbe::TopicMessage::uuidHeaderLength() << " (should be 2)"
                  << std::endl;
        std::cout << "  Payload: " << sbe::TopicMessage::payloadHeaderLength() << " (should be 2)"
                  << std::endl;
        std::cout << "  Headers: " << sbe::TopicMessage::headersHeaderLength() << " (should be 2)"
                  << std::endl;
        std::cout << "====================================" << std::endl;
    }

    void print_buffer_transition(const std::vector<std::uint8_t>& combined_buffer) {
        std::cout << config_.logging.log_prefix << " Buffer transition (bytes 28-36):" << std::endl;
        std::cout << config_.logging.log_prefix << "   ";

        // Print bytes around the transition point (session->business)
        for (size_t i = 28; i < combined_buffer.size() && i < 40; ++i) {
            if (i == 32)
                std::cout << "| ";  // Mark transition
            std::cout << std::hex << std::setfill('0') << std::setw(2)
                      << static_cast<unsigned>(combined_buffer[i]) << " ";
        }
        std::cout << std::dec << std::endl;

        // Show what this represents
        std::cout << config_.logging.log_prefix << "   ^session end    ^business start"
                  << std::endl;
    }
    void print_hex_section(const uint8_t* data, size_t start, size_t length) {
        std::cout << config_.logging.log_prefix << "   ";
        for (size_t i = 0; i < length; ++i) {
            std::cout << std::hex << std::setfill('0') << std::setw(2)
                      << static_cast<unsigned>(data[start + i]) << " ";
            if ((i + 1) % 8 == 0 && i + 1 < length) {
                std::cout << " ";
            }
        }
        std::cout << std::dec << std::endl;

        // Also print ASCII representation
        std::cout << config_.logging.log_prefix << "   ASCII: ";
        for (size_t i = 0; i < length; ++i) {
            char c = static_cast<char>(data[start + i]);
            std::cout << (c >= 32 && c <= 126 ? c : '.');
        }
        std::cout << std::endl;
    }

    void write_uint16_le(std::vector<std::uint8_t>& buffer, std::size_t offset,
                         std::uint16_t value) {
        buffer[offset] = static_cast<std::uint8_t>(value & 0xFF);
        buffer[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    }

    void write_uint32_le(std::vector<std::uint8_t>& buffer, std::size_t offset,
                         std::uint32_t value) {
        buffer[offset] = static_cast<std::uint8_t>(value & 0xFF);
        buffer[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
        buffer[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
        buffer[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    }

    void write_int64_le(std::vector<std::uint8_t>& buffer, std::size_t offset, std::int64_t value) {
        for (int i = 0; i < 8; ++i) {
            buffer[offset + i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF);
        }
    }

    int64_t get_current_timestamp() {
        auto now = std::chrono::high_resolution_clock::now();
        return now.time_since_epoch().count();
    }

    std::int64_t generate_correlation_id() {
        auto now = std::chrono::high_resolution_clock::now();
        std::int64_t timestamp = now.time_since_epoch().count();

        // Use only lower 47 bits to ensure positive value
        return timestamp & 0x7FFFFFFFFFFFFFFFLL;
    }
};

// SessionManager public interface implementation
SessionManager::SessionManager(const ClusterClientConfig& config)
    : pImpl_(std::make_unique<Impl>(config)) {}

SessionManager::~SessionManager() = default;

SessionConnectionResult SessionManager::connect(std::shared_ptr<aeron::Aeron> aeron) {
    return pImpl_->connect(aeron);
}

std::future<SessionConnectionResult> SessionManager::connect_async(
    std::shared_ptr<aeron::Aeron> aeron) {
    return std::async(std::launch::async, [this, aeron]() { return pImpl_->connect(aeron); });
}

SessionConnectionResult SessionManager::connect_with_timeout(std::shared_ptr<aeron::Aeron> aeron,
                                                             std::chrono::milliseconds timeout) {
    auto future = connect_async(aeron);

    if (future.wait_for(timeout) == std::future_status::timeout) {
        SessionConnectionResult result;
        result.error_message = "Connection timeout after " + std::to_string(timeout.count()) + "ms";
        return result;
    }

    return future.get();
}

void SessionManager::disconnect() {
    pImpl_->disconnect();
}

bool SessionManager::is_connected() const {
    return pImpl_->is_connected();
}

std::int64_t SessionManager::get_session_id() const {
    return pImpl_->get_session_id();
}

std::int32_t SessionManager::get_leader_member_id() const {
    return pImpl_->get_leader_member_id();
}

std::int64_t SessionManager::get_leadership_term_id() const {
    return pImpl_->get_leadership_term_id();
}

bool SessionManager::publish_message(const std::string& topic, const std::string& message_type,
                                     const std::string& message_id, const std::string& payload,
                                     const std::string& headers) {
    return pImpl_->publish_message(topic, message_type, message_id, payload, headers);
}

bool SessionManager::send_raw_message(const std::vector<std::uint8_t>& encoded_message) {
    return pImpl_->send_raw_message(encoded_message);
}

bool SessionManager::send_keepalive() {
    return pImpl_->send_keepalive();
}

void SessionManager::handle_session_event(const ParseResult& result) {
    pImpl_->handle_session_event(result);
}

void SessionManager::set_session_event_callback(SessionEventCallback callback) {
    pImpl_->set_session_event_callback(std::move(callback));
}

void SessionManager::set_connection_state_callback(ConnectionStateCallback callback) {
    pImpl_->set_connection_state_callback(std::move(callback));
}

SessionManager::SessionStats SessionManager::get_session_stats() const {
    return pImpl_->get_session_stats();
}

std::future<SessionConnectionResult> SessionManager::reconnect_async() {
    // Implementation would disconnect and reconnect
    return std::async(std::launch::async, []() {
        SessionConnectionResult result;
        result.error_message = "Reconnect not implemented yet";
        return result;
    });
}

// SessionUtils namespace implementation
namespace SessionUtils {

std::string calculate_leader_endpoint(std::int32_t member_id, const std::string& base_ip) {
    if (member_id < 0) {
        return "";
    }

    int leader_port = 9002 + (member_id * 100);
    return base_ip + ":" + std::to_string(leader_port);
}

std::string extract_base_ip(const std::string& endpoint) {
    std::size_t colon_pos = endpoint.find(':');
    if (colon_pos != std::string::npos) {
        return endpoint.substr(0, colon_pos);
    }
    return endpoint;
}

bool is_valid_endpoint(const std::string& endpoint) {
    std::regex pattern(R"(^[a-zA-Z0-9\.\-]+:\d+$)");
    return std::regex_match(endpoint, pattern);
}

std::pair<std::string, int> parse_endpoint(const std::string& endpoint) {
    std::size_t colon_pos = endpoint.find(':');
    if (colon_pos == std::string::npos) {
        return {"", 0};
    }

    std::string host = endpoint.substr(0, colon_pos);
    std::string port_str = endpoint.substr(colon_pos + 1);

    try {
        int port = std::stoi(port_str);
        return {host, port};
    } catch (const std::exception&) {
        return {"", 0};
    }
}

std::int64_t generate_correlation_id() {
    auto now = std::chrono::high_resolution_clock::now();
    std::int64_t timestamp = now.time_since_epoch().count();
    return timestamp & 0x7FFFFFFFFFFFFFFFLL;
}

}  // namespace SessionUtils

}  // namespace aeron_cluster