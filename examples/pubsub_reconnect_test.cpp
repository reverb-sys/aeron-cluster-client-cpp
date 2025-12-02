#include <aeron_cluster/cluster_client.hpp>
#include <aeron_cluster/logging.hpp>
#include <aeron_cluster/signal_handler.hpp>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <functional>
#include <thread>
#include <unistd.h>
#include <ctime>
#include <set>
#include <map>
#include <unordered_set>
#include <sstream>
#include <string>
#include <mutex>
#include <vector>
#include <json/json.h>
#include <algorithm>

using namespace aeron_cluster;

// Global flag for graceful shutdown
std::atomic<bool> running{true};

// Global logger instance
std::shared_ptr<Logger> logger;

// Message tracking
std::atomic<int> messages_published{0};
std::atomic<int> messages_received{0};
std::mutex message_mutex;
std::set<std::string> received_message_ids;

// Message comparison tracking
std::set<std::string> published_message_ids;
std::set<std::string> received_message_ids_comparison;
std::mutex comparison_mutex;

// Publisher completion tracking
std::atomic<bool> publisher_finished{false};
std::chrono::steady_clock::time_point publisher_finish_time;

// Track client_order_id -> message_id and sent IDs for correlation
std::mutex client_order_mutex;
std::set<std::string> sent_client_order_ids;
std::map<std::string, std::string> client_order_id_to_message_id;
std::set<std::string> received_client_order_ids;
std::mutex received_client_order_mutex;
std::map<std::string, std::string> client_order_id_to_response_message_id;
std::map<std::string, std::string> client_order_id_to_order_id;
std::map<std::string, std::chrono::steady_clock::time_point> client_order_send_time;
std::map<std::string, std::chrono::steady_clock::time_point> client_order_receive_time;
std::map<std::string, long long> client_order_latency_ms;

// For multi-identifier test
std::map<std::string, int> messages_published_by_identifier;
std::map<std::string, int> messages_received_by_identifier;
std::mutex identifier_mutex;

// Signal handling is completely automatic via SignalHandlerManager.
// When clients connect(), they automatically register with SignalHandlerManager.
// SignalHandlerManager installs signal handlers for SIGINT and SIGTERM that:
// 1. Disconnect all registered clients (sending SessionCloseRequest for each)
// 2. Re-raise the signal to allow normal termination
// No manual signal handling needed - it's all automatic!

void printMessageComparison() {
    std::lock_guard<std::mutex> lock(comparison_mutex);
    
    logger->info("========================================");
    logger->info("CLIENT ORDER ID COMPARISON");
    logger->info("========================================");
    
    // For final comparison use client_order_id sets
    std::size_t sent_count;
    std::size_t received_count;
    {
        std::lock_guard<std::mutex> s_lock(client_order_mutex);
        std::lock_guard<std::mutex> r_lock(received_client_order_mutex);
        sent_count = sent_client_order_ids.size();
        received_count = received_client_order_ids.size();
        logger->info("Client Order IDs Sent: {}", sent_count);
        logger->info("Client Order IDs Received: {}", received_count);
    }
    
    // Compute missing and extra using client_order_id
    std::set<std::string> missing_client_orders;
    std::set<std::string> extra_client_orders;
    {
        std::lock_guard<std::mutex> s_lock(client_order_mutex);
        std::lock_guard<std::mutex> r_lock(received_client_order_mutex);
        for (const auto& cid : sent_client_order_ids) {
            if (received_client_order_ids.find(cid) == received_client_order_ids.end()) {
                missing_client_orders.insert(cid);
            }
        }
        for (const auto& cid : received_client_order_ids) {
            if (sent_client_order_ids.find(cid) == sent_client_order_ids.end()) {
                extra_client_orders.insert(cid);
            }
        }
    }
    
    logger->info("Missing Client Orders: {}", missing_client_orders.size());
    if (!missing_client_orders.empty()) {
        logger->error("MISSING client_order_id values:");
        int count = 0;
        for (const auto& missing_id : missing_client_orders) {
            logger->error("  {}: {}...", count + 1, missing_id.substr(0, 20));
            count++;
            if (count >= 10) { // Limit to first 10
                logger->error("  ... and {} more missing client orders", missing_client_orders.size() - 10);
                break;
            }
        }
    }
    
    logger->info("Extra Client Orders: {}", extra_client_orders.size());
    if (!extra_client_orders.empty()) {
        logger->warn("EXTRA client_order_id values (received but not sent):");
        int count = 0;
        for (const auto& extra_id : extra_client_orders) {
            logger->warn("  {}: {}...", count + 1, extra_id.substr(0, 20));
            count++;
            if (count >= 10) { // Limit to first 10
                logger->warn("  ... and {} more extra client orders", extra_client_orders.size() - 10);
                break;
            }
        }
    }
    
    // Calculate success rate
    {
        std::lock_guard<std::mutex> s_lock(client_order_mutex);
        double denom = static_cast<double>(sent_client_order_ids.size());
        double success_rate = denom > 0.0
            ? (static_cast<double>(sent_client_order_ids.size() - missing_client_orders.size()) / denom) * 100.0
            : 100.0;
        logger->info("Success Rate: {:.2f}%", success_rate);
    }
    
    logger->info("========================================");
}

void printLatencyReport() {
    logger->info("========================================");
    logger->info("ROUND-TRIP LATENCY (client_order_id)");
    logger->info("========================================");
    
    struct Row {
        long long latency_ms;
        std::string client_order_id;
        std::string order_id;
        std::string req_msg_id;
        std::string resp_msg_id;
    };
    std::vector<Row> rows;
    {
        std::lock_guard<std::mutex> lock(client_order_mutex);
        rows.reserve(client_order_latency_ms.size());
        for (const auto& kv : client_order_latency_ms) {
            const std::string& cid = kv.first;
            long long lat = kv.second;
            const std::string orderId = client_order_id_to_order_id.count(cid) ? client_order_id_to_order_id.at(cid) : "";
            const std::string reqId = client_order_id_to_message_id.count(cid) ? client_order_id_to_message_id.at(cid) : "";
            const std::string respId = client_order_id_to_response_message_id.count(cid) ? client_order_id_to_response_message_id.at(cid) : "";
            rows.push_back(Row{lat, cid, orderId, reqId, respId});
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b){ return a.latency_ms > b.latency_ms; });
    
    logger->info("Total measured: {}", rows.size());
    for (const auto& r : rows) {
        std::string reqShort = r.req_msg_id.size() > 12 ? r.req_msg_id.substr(0, 12) : r.req_msg_id;
        std::string respShort = r.resp_msg_id.size() > 12 ? r.resp_msg_id.substr(0, 12) : r.resp_msg_id;
        std::string ordShort = r.order_id.size() > 12 ? r.order_id.substr(0, 12) : r.order_id;
        logger->info("latencyMs={} client_order_id={} order_id={} req_msg_id={}... resp_msg_id={}...", 
            r.latency_ms, r.client_order_id, ordShort.empty() ? "<unknown>" : r.order_id, 
            reqShort.empty() ? "<n/a>" : r.req_msg_id, respShort.empty() ? "<n/a>" : r.resp_msg_id);
    }
    logger->info("========================================");
}

void printUsage() {
    std::cout << "Aeron Cluster C++ - Pub/Sub Test Suite" << std::endl;
    std::cout << "=======================================" << std::endl;
    std::cout << "\nThis test suite includes two test modes:" << std::endl;
    std::cout << "\n1. RECONNECT TEST (default):" << std::endl;
    std::cout << "   - Publisher sends messages to order_notification_topic" << std::endl;
    std::cout << "   - Subscriber receives messages with messageIdentifier ROHIT_AERON01_TX" << std::endl;
    std::cout << "   - Subscriber disconnects while publisher continues" << std::endl;
    std::cout << "   - Subscriber reconnects and receives all messages" << std::endl;
    std::cout << "   - Verifies total published == total received" << std::endl;
    std::cout << "\n2. IDENTIFIER FILTER TEST:" << std::endl;
    std::cout << "   - Publisher sends messages to MULTIPLE identifiers (ID_A, ID_B, ID_C)" << std::endl;
    std::cout << "   - Subscriber subscribes to ONLY ONE identifier (e.g., ID_A)" << std::endl;
    std::cout << "   - Verifies subscriber ONLY receives messages for subscribed identifier" << std::endl;
    std::cout << "   - Logs WARNING if any messages from other identifiers are received" << std::endl;
    std::cout << "\nUsage: ./pubsub_reconnect_test [options]" << std::endl;
    std::cout << "\nOptions:" << std::endl;
    std::cout << "  --help              Show this help message" << std::endl;
    std::cout << "  --test-mode MODE    Test mode: 'reconnect' or 'identifier-filter' (default: reconnect)" << std::endl;
    std::cout << "  --endpoints LIST    Comma-separated cluster endpoints (default: localhost:9002,localhost:9102,localhost:9202)" << std::endl;
    std::cout << "  --aeron-dir PATH    Aeron media driver directory (default: /dev/shm/aeron)" << std::endl;
    std::cout << "  --debug             Enable debug logging" << std::endl;
    std::cout << "  --messages COUNT    Number of messages to publish (default: 100)" << std::endl;
    std::cout << "  --interval MS       Interval between messages in milliseconds (default: 100)" << std::endl;
    std::cout << "\nReconnect Test Options:" << std::endl;
    std::cout << "  --disconnect-at N   Disconnect subscriber after N messages (default: 30)" << std::endl;
    std::cout << "  --reconnect-delay MS Delay before reconnecting subscriber (default: 2000)" << std::endl;
    std::cout << "  --timeout MS        Timeout after publisher finishes (default: 5000)" << std::endl;
    std::cout << "\nIdentifier Filter Test Options:" << std::endl;
    std::cout << "  --subscribe-to ID   Identifier to subscribe to (default: IDENTIFIER_A)" << std::endl;
    std::cout << "  --identifiers LIST  Comma-separated list of identifiers to publish to" << std::endl;
    std::cout << "                      (default: IDENTIFIER_A,IDENTIFIER_B,IDENTIFIER_C)" << std::endl;
    std::cout << std::endl;
}

std::vector<std::string> parseEndpoints(const std::string& endpointList) {
    std::vector<std::string> endpoints;
    std::stringstream ss(endpointList);
    std::string endpoint;

    while (std::getline(ss, endpoint, ',')) {
        // Trim whitespace
        endpoint.erase(0, endpoint.find_first_not_of(" \t"));
        endpoint.erase(endpoint.find_last_not_of(" \t") + 1);
        if (!endpoint.empty()) {
            endpoints.push_back(endpoint);
        }
    }

    return endpoints;
}

std::string joinIdentifiers(const std::vector<std::string>& identifiers) {
    if (identifiers.empty()) {
        return "(none)";
    }

    std::ostringstream oss;
    for (size_t i = 0; i < identifiers.size(); ++i) {
        if (i > 0) {
            oss << ", ";
        }
        oss << identifiers[i];
    }
    return oss.str();
}

// Publisher thread function
void publisherThread(const ClusterClientConfig& config, int messageCount, int intervalMs, const std::string& identifier) {
    auto pub_logger = LoggerFactory::instance().getLogger("publisher");
    
    try {
        pub_logger->info("Starting publisher thread...");
        
        // Create publisher client
        auto publisher = std::make_shared<ClusterClient>(config);
        
        pub_logger->info("Connecting publisher to cluster...");
        if (!publisher->connect()) {
            pub_logger->error("Failed to connect publisher to cluster");
            return;
        }
        
        pub_logger->info("Publisher connected successfully!");
        pub_logger->info("Session ID: {}", publisher->get_session_id());
        
        // Publish messages
        pub_logger->info("Publishing {} messages to order_notification_topic...", messageCount);
        
        for (int i = 0; i < messageCount && running; ++i) {
            try {
                // Create a sample order
                std::string side = (i % 2 == 0) ? "buy" : "sell";
                double quantity = 1.0 + (i * 0.1);
                double price = 3500.0 + (i * 10.0);
                
                Order order = ClusterClient::create_sample_limit_order("ETH", "USD", side, quantity, price, identifier);
                order.account_id = 10000 + i;
                order.customer_id = 50000 + i;
                const std::string clientOrderId = order.client_order_uuid;
                {
                    std::lock_guard<std::mutex> lock(client_order_mutex);
                    sent_client_order_ids.insert(clientOrderId);
                }
                
                // Generate message ID
                std::string messageId = "msg_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "_" + std::to_string(i);
                std::string messageType = "CREATE_ORDER";
                
                // Create headers with messageIdentifier
                std::stringstream headers_ss;
                headers_ss << "{"
                          << "\"messageId\":\"" << messageId << "\","
                          << "\"messageType\":\"" << messageType << "\","
                          << "\"identifier\":\"" << identifier << "\","
                          << "\"orderId\":\"" << order.id << "\""
                          << "}";
                std::string headers = headers_ss.str();
                
                // Convert order to JSON payload and add identifier at root level
                std::string base_payload = order.to_json();
                std::string payload;
                
                // Parse the JSON and add identifier field
                Json::Value payload_json;
                Json::CharReaderBuilder reader_builder;
                std::string errs;
                std::istringstream payload_stream(base_payload);
                if (Json::parseFromStream(reader_builder, payload_stream, &payload_json, &errs)) {
                    // Add identifier at root level for cluster/subscriber to find
                    payload_json["identifier"] = identifier;
                    payload_json["messageIdentifier"] = identifier;
                    
                    // Re-serialize
                    Json::StreamWriterBuilder writer_builder;
                    writer_builder["indentation"] = "";
                    payload = Json::writeString(writer_builder, payload_json);
                } else {
                    payload = base_payload; // Fallback to original
                }
                
                // Publish to order_notification_topic with messageIdentifier in headers
                {
                    std::lock_guard<std::mutex> lock(client_order_mutex);
                    client_order_send_time[clientOrderId] = std::chrono::steady_clock::now();
                }
                std::string actualMessageId = publisher->publish_message_to_topic(messageType, payload, headers, "order_request_topic");
                
                messages_published++;
                {
                    std::lock_guard<std::mutex> lock(client_order_mutex);
                    client_order_id_to_message_id[clientOrderId] = actualMessageId;
                }
                
                // Track published message ID for comparison (use the actual ID returned by cluster client)
                {
                    std::lock_guard<std::mutex> lock(comparison_mutex);
                    published_message_ids.insert(actualMessageId);
                }
                
                pub_logger->info("Published message {}/{}: {} with identifier {} (client_order_id={}, req_msg_id={}...)", 
                              messages_published.load(), messageCount, side, identifier, clientOrderId, actualMessageId.substr(0, 12));
                
                // Wait between messages
                if (i < messageCount - 1 && running) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
                }
                
                // Poll for any responses
                publisher->poll_messages(5);
                
            } catch (const std::exception& e) {
                pub_logger->error("Failed to publish message {}: {}", i + 1, e.what());
            }
        }
        
        pub_logger->info("Publisher finished! Total published: {}", messages_published.load());
        
        // Mark publisher as finished
        publisher_finished = true;
        publisher_finish_time = std::chrono::steady_clock::now();
        
        // Disconnect
        publisher->disconnect();
        
    } catch (const std::exception& e) {
        pub_logger->error("Publisher thread error: {}", e.what());
    }
}

// Subscriber thread function
void subscriberThread(const ClusterClientConfig& config, int disconnectAt, int reconnectDelayMs, int totalMessages, int timeoutMs, const std::vector<std::string>& identifiers) {
    auto sub_logger = LoggerFactory::instance().getLogger("subscriber");
    
    std::vector<std::string> subscriptionIdentifiers;
    {
        std::unordered_set<std::string> seen_identifiers;
        for (const auto& id : identifiers) {
            if (id.empty()) {
                continue;
            }
            if (seen_identifiers.insert(id).second) {
                subscriptionIdentifiers.push_back(id);
            }
        }
    }
    if (subscriptionIdentifiers.empty()) {
        subscriptionIdentifiers.emplace_back("");
    }

    try {
        sub_logger->info("Starting subscriber thread...");
        if (subscriptionIdentifiers.size() == 1 && subscriptionIdentifiers.front().empty()) {
            sub_logger->info("Subscribing without identifier filter (all messages).");
        } else {
            sub_logger->info("Subscribing to identifiers: {}", joinIdentifiers(subscriptionIdentifiers));
        }
        
        // Create subscriber client
        auto subscriber = std::make_shared<ClusterClient>(config);
        
        // Set up message callback
        subscriber->set_message_callback([&](const aeron_cluster::ParseResult& result) {
            // Only count ORDER messages, not acknowledgments
            if (result.is_order_message() || 
                (result.is_topic_message() && result.message_type == "CREATE_ORDER")) {
                // Try to extract client_order_id and order_id from payload
                std::string extracted_client_order_id;
                std::string extracted_order_id;
                try {
                    if (!result.payload.empty()) {
                        Json::CharReaderBuilder rb;
                        std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
                        Json::Value root;
                        std::string errs;
                        const char* begin = result.payload.data();
                        const char* end = begin + result.payload.size();
                        if (reader->parse(begin, end, &root, &errs)) {
                            // Common locations:
                            // - client order id may be at: message.message.client_order_id
                            // - or message.message.order_details.client_order_id
                            // - order id may be at: message.message.order_id
                            auto find_str = [](const Json::Value& obj, const char* key) -> std::string {
                                return obj.isMember(key) && obj[key].isString() ? obj[key].asString() : "";
                            };
                            const Json::Value& msg = root.isMember("message") ? root["message"] : Json::Value();
                            const Json::Value& inner = (msg.isObject() && msg.isMember("message")) ? msg["message"] : Json::Value();
                            if (inner.isObject()) {
                                extracted_client_order_id = find_str(inner, "client_order_id");
                                extracted_order_id = find_str(inner, "order_id");
                                if (extracted_client_order_id.empty() && inner.isMember("order_details") && inner["order_details"].isObject()) {
                                    extracted_client_order_id = find_str(inner["order_details"], "client_order_id");
                                }
                            } else if (msg.isObject() && msg.isMember("order_details") && msg["order_details"].isObject()) {
                                // Older format
                                extracted_client_order_id = find_str(msg["order_details"], "client_order_id");
                            }
                            // Some servers place order id at top-level "uuid"
                            if (extracted_order_id.empty()) {
                                std::string maybe_uuid = find_str(root, "uuid");
                                if (!maybe_uuid.empty()) {
                                    extracted_order_id = maybe_uuid;
                                }
                            }
                        }
                    }
                } catch (...) {
                    // Ignore JSON parse errors; fall back to generic logging
                }
                
                // Track received message
                std::lock_guard<std::mutex> lock(message_mutex);
                
                // Check if we've seen this message before
                if (received_message_ids.find(result.message_id) == received_message_ids.end()) {
                    received_message_ids.insert(result.message_id);
                    messages_received++;
                    if (!extracted_client_order_id.empty()) {
                        std::lock_guard<std::mutex> lock_rcv(received_client_order_mutex);
                        received_client_order_ids.insert(extracted_client_order_id);
                    }
                    
                    // Track received message ID for comparison
                    {
                        std::lock_guard<std::mutex> lock(comparison_mutex);
                        received_message_ids_comparison.insert(result.message_id);
                    }
                    
                    if (!extracted_client_order_id.empty()) {
                        bool was_sent = false;
                        std::string sent_msg_id;
                        {
                            std::lock_guard<std::mutex> lock2(client_order_mutex);
                            was_sent = sent_client_order_ids.find(extracted_client_order_id) != sent_client_order_ids.end();
                            auto it = client_order_id_to_message_id.find(extracted_client_order_id);
                            if (it != client_order_id_to_message_id.end()) {
                                sent_msg_id = it->second;
                            }
                            // Compute latency if we have not recorded it yet and have a send time
                            auto itSend = client_order_send_time.find(extracted_client_order_id);
                            auto itLat = client_order_latency_ms.find(extracted_client_order_id);
                            if (itSend != client_order_send_time.end() && itLat == client_order_latency_ms.end()) {
                                auto now = std::chrono::steady_clock::now();
                                client_order_receive_time[extracted_client_order_id] = now;
                                long long latency_ms_val = std::chrono::duration_cast<std::chrono::milliseconds>(now - itSend->second).count();
                                client_order_latency_ms[extracted_client_order_id] = latency_ms_val;
                                client_order_id_to_response_message_id[extracted_client_order_id] = result.message_id;
                                if (!extracted_order_id.empty()) {
                                    client_order_id_to_order_id[extracted_client_order_id] = extracted_order_id;
                                }
                            }
                        }
                        if (was_sent) {
                            long long latency_out = -1;
                            auto itLat2 = client_order_latency_ms.find(extracted_client_order_id);
                            if (itLat2 != client_order_latency_ms.end()) latency_out = itLat2->second;
                            sub_logger->info("Received ORDER {}: client_order_id={} -> order_id={} (matched, req_msg_id={}..., resp_msg_id={}..., latencyMs={})", 
                                          messages_received.load(),
                                          extracted_client_order_id,
                                          extracted_order_id.empty() ? "<unknown>" : extracted_order_id,
                                          sent_msg_id.empty() ? "<n/a>" : sent_msg_id.substr(0, 12),
                                          result.message_id.substr(0, 12),
                                          latency_out);
                        } else {
                            sub_logger->warn("Received ORDER {}: client_order_id={} not recognized (order_id={}, resp_msg_id={}...)", 
                                          messages_received.load(),
                                          extracted_client_order_id,
                                          extracted_order_id.empty() ? "<unknown>" : extracted_order_id,
                                          result.message_id.substr(0, 12));
                        }
                    } else {
                        sub_logger->info("Received ORDER message {}: Type={}, ID={}...", 
                                      messages_received.load(), 
                                      result.message_type, 
                                      result.message_id.substr(0, 12));
                    }
                } else {
                    sub_logger->debug("Duplicate ORDER message detected (already received): {}...", 
                                   result.message_id.substr(0, 12));
                }
            } else if (result.is_acknowledgment() || result.message_type == "Acknowledgment") {
                // Log acknowledgments but don't count them
                sub_logger->debug("Received ACK: ID={}...", result.message_id.substr(0, 12));
            } else if (result.message_type == "REPLAY_COMPLETE") {
                sub_logger->info("REPLAY_COMPLETE received");
            }
        });
        
        // Set up connection state callback to detect session closures
        bool session_closed_by_cluster = false;
        subscriber->set_connection_state_callback([&](aeron_cluster::ConnectionState old_state, 
                                                     aeron_cluster::ConnectionState new_state) {
            sub_logger->info("Connection state changed: {} -> {}", 
                          static_cast<int>(old_state), static_cast<int>(new_state));
            if (new_state == aeron_cluster::ConnectionState::DISCONNECTED) {
                sub_logger->warn("Session closed/disconnected!");
                session_closed_by_cluster = true;
            }
        });
        
        bool first_connection = true;
        bool disconnected_intentionally = false;
        
        int connection_attempt = 0;
        
        while (running) {
            connection_attempt++;
            
            // Generate a NEW instance ID for each connection attempt to avoid cluster rejecting reconnections
            // The cluster needs time to clean up the previous session with the same instance ID
            std::string instance_id = "cpp_test_subscriber_" + std::to_string(getpid()) + "_" + std::to_string(connection_attempt);
            sub_logger->info("Using instance ID: {}", instance_id);
            // Connect to cluster
            sub_logger->info("Connecting subscriber to cluster...");
            if (!subscriber->connect()) {
                sub_logger->error("Failed to connect subscriber to cluster");
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            
            sub_logger->info("Subscriber connected successfully!");
            
            // Wait for session to be fully established
            int64_t session_id = subscriber->get_session_id();
            int wait_attempts = 0;
            const int max_wait_attempts = 50; // 5 seconds max
            
            while (session_id == -1 && wait_attempts < max_wait_attempts && running) {
                sub_logger->debug("Waiting for session to be established... (attempt {})", wait_attempts + 1);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                subscriber->poll_messages(5); // Poll to process session messages
                session_id = subscriber->get_session_id();
                wait_attempts++;
            }
            
            if (session_id == -1) {
                sub_logger->error("Failed to establish session (Session ID still -1)");
                sub_logger->warn("Disconnecting and will retry...");
                subscriber->disconnect();
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            
            sub_logger->info("Session established! Session ID: {}", session_id);
            
            // Verify connection is still active before sending subscription
            if (!subscriber->is_connected()) {
                sub_logger->error("Connection lost after session establishment");
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            
            std::string replay_position = first_connection ? "LAST_COMMIT" : "LAST_COMMIT";

            bool subscription_error = false;
            for (const auto& subscription_identifier : subscriptionIdentifiers) {
                try {
                    if (subscription_identifier.empty()) {
                        sub_logger->info("Sending subscription request for order_notification_topic without identifier filter (instance {})...", instance_id);
                    } else {
                        sub_logger->info("Sending subscription request for order_notification_topic with identifier {} and instance {}...", subscription_identifier, instance_id);
                    }
                    std::string message_id = subscriber->send_subscription_request("order_notification_topic", subscription_identifier, replay_position, instance_id);
                    sub_logger->debug("Subscription request accepted with message ID: {}", message_id);
                } catch (const std::exception& e) {
                    sub_logger->error("Failed to send subscription request for identifier {}: {}", subscription_identifier.empty() ? "<none>" : subscription_identifier, e.what());
                    subscription_error = true;
                    break;
                }
            }

            if (subscription_error) {
                sub_logger->warn("Disconnecting and will retry...");
                subscriber->disconnect();
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }

            sub_logger->info("Subscription request(s) sent with instance: {}", instance_id);
            
            // Proactively send commit request for the topic so cluster will accept commit offsets
            try {
                bool cr = subscriber->send_commit_request("order_notification_topic");
                sub_logger->info("Commit request sent for topic order_notification_topic: {}", cr ? "OK" : "FAILED");
            } catch (const std::exception& e) {
                sub_logger->warn("Failed to send commit request: {}", e.what());
            }
            
            first_connection = false;
            
            // Reset session closure flag for this connection
            session_closed_by_cluster = false;
            
            // Poll messages until disconnect condition
            int poll_count = 0;
            while (running && subscriber->is_connected()) {
                int messagesPolled = subscriber->poll_messages(10);
                
                if (messagesPolled > 0) {
                    sub_logger->debug("Polled {} messages", messagesPolled);
                }
                
                poll_count++;
                
                // Check if cluster closed the session (back pressure, etc.)
                if (session_closed_by_cluster) {
                    sub_logger->warn("Detected session closure by cluster, will reconnect...");
                    subscriber->disconnect();
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    break; // Break to reconnect
                }
                
                // Check if we should disconnect intentionally
                if (!disconnected_intentionally && messages_received >= disconnectAt) {
                    sub_logger->warn("Reached disconnect threshold ({} messages), disconnecting...", disconnectAt);
                    subscriber->disconnect();
                    disconnected_intentionally = true;
                    
                    // Wait before reconnecting - cluster needs time to clean up the previous session
                    // Minimum recommended: 2000ms (2 seconds)
                    int actual_delay = std::max(reconnectDelayMs, 2000);
                    if (reconnectDelayMs < 2000) {
                        sub_logger->warn("Reconnect delay {}ms is too short, using {}ms instead", reconnectDelayMs, actual_delay);
                    }
                    sub_logger->info("Waiting {}ms before reconnecting to allow cluster cleanup...", actual_delay);
                    std::this_thread::sleep_for(std::chrono::milliseconds(actual_delay));
                    break; // Break inner loop to reconnect
                }
                
                // Check if publisher is finished and timeout has elapsed
                if (publisher_finished.load()) {
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - publisher_finish_time).count();
                    
                    if (elapsed >= timeoutMs) {
                        sub_logger->info("Publisher finished {}ms ago, timeout reached. Stopping subscriber.", elapsed);
                        sub_logger->info("Final message count: Received={}, Published={}", messages_received.load(), messages_published.load());
                        running = false;
                        break;
                    }
                }
                
                // Check if we've received all messages
                if (messages_received >= totalMessages && messages_published >= totalMessages) {
                    sub_logger->info("Received all messages! ({}/{})", messages_received.load(), totalMessages);
                    // Wait a bit more to ensure no more messages
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    
                    // Final check
                    subscriber->poll_messages(10);
                    
                    if (messages_received >= totalMessages) {
                        sub_logger->info("Test complete! All messages received.");
                        running = false;
                        break;
                    }
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            
            // If we're done, exit
            if (!running) {
                break;
            }
            
            // If disconnected but still running, reconnect
            if (!subscriber->is_connected() && running) {
                sub_logger->info("Subscriber disconnected, will reconnect...");
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
        
        sub_logger->info("Subscriber finished! Total received: {}", messages_received.load());
        
        // Disconnect
        if (subscriber->is_connected()) {
            subscriber->disconnect();
        }
        
    } catch (const std::exception& e) {
        sub_logger->error("Subscriber thread error: {}", e.what());
    }
}

// Multi-identifier publisher thread function
void multiIdentifierPublisherThread(const ClusterClientConfig& config, int messageCount, int intervalMs, const std::vector<std::string>& identifiers) {
    auto pub_logger = LoggerFactory::instance().getLogger("multi-publisher");
    
    try {
        pub_logger->info("Starting multi-identifier publisher thread...");
        
        // Create publisher client
        ClusterClient publisher(config);
        
        pub_logger->info("Connecting publisher to cluster...");
        if (!publisher.connect()) {
            pub_logger->error("Failed to connect publisher to cluster");
            return;
        }
        
        pub_logger->info("Publisher connected successfully!");
        pub_logger->info("Session ID: {}", publisher.get_session_id());
        
        // Publish messages to different identifiers
        pub_logger->info("Publishing {} messages across {} identifiers to order_notification_topic...", messageCount, identifiers.size());
        
        for (int i = 0; i < messageCount && running; ++i) {
            try {
                // Rotate through identifiers
                std::string identifier = identifiers[i % identifiers.size()];
                
                // Create a sample order
                std::string side = (i % 2 == 0) ? "buy" : "sell";
                double quantity = 1.0 + (i * 0.1);
                double price = 3500.0 + (i * 10.0);
                
                Order order = ClusterClient::create_sample_limit_order("ETH", "USD", side, quantity, price, identifier);
                order.account_id = 10000 + i;
                order.customer_id = 50000 + i;
                const std::string clientOrderId = order.client_order_uuid;
                {
                    std::lock_guard<std::mutex> lock(client_order_mutex);
                    sent_client_order_ids.insert(clientOrderId);
                }
                
                // Generate message ID
                std::string messageId = "msg_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "_" + std::to_string(i);
                std::string messageType = "CREATE_ORDER";
                
                // Create headers with messageIdentifier
                std::stringstream headers_ss;
                headers_ss << "{"
                          << "\"messageId\":\"" << messageId << "\","
                          << "\"messageType\":\"" << messageType << "\","
                          << "\"identifier\":\"" << identifier << "\","
                          << "\"orderId\":\"" << order.id << "\""
                          << "}";
                std::string headers = headers_ss.str();
                
                // Convert order to JSON payload and add identifier at root level
                std::string base_payload = order.to_json();
                std::string payload;
                
                // Parse the JSON and add identifier field
                Json::Value payload_json;
                Json::CharReaderBuilder reader_builder;
                std::string errs;
                std::istringstream payload_stream(base_payload);
                if (Json::parseFromStream(reader_builder, payload_stream, &payload_json, &errs)) {
                    // Add identifier at root level for cluster/subscriber to find
                    payload_json["identifier"] = identifier;
                    payload_json["messageIdentifier"] = identifier;
                    
                    // Re-serialize
                    Json::StreamWriterBuilder writer_builder;
                    writer_builder["indentation"] = "";
                    payload = Json::writeString(writer_builder, payload_json);
                } else {
                    payload = base_payload; // Fallback to original
                }
                
                // Publish to order_notification_topic with messageIdentifier in headers
                // Store send time just before publish to capture network/cluster latency
                {
                    std::lock_guard<std::mutex> lock(client_order_mutex);
                    client_order_send_time[clientOrderId] = std::chrono::steady_clock::now();
                }
                std::string actualMessageId = publisher.publish_message_to_topic(messageType, payload, headers, "order_request_topic");
                
                messages_published++;
                {
                    std::lock_guard<std::mutex> lock(client_order_mutex);
                    client_order_id_to_message_id[clientOrderId] = actualMessageId;
                }
                
                // Track published message ID for comparison (use the actual ID returned by cluster client)
                {
                    std::lock_guard<std::mutex> lock(comparison_mutex);
                    published_message_ids.insert(actualMessageId);
                }
                
                // Track by identifier
                {
                    std::lock_guard<std::mutex> lock(identifier_mutex);
                    messages_published_by_identifier[identifier]++;
                }
                
                pub_logger->info("Published message {}/{}: {} with identifier {} (client_order_id={}, req_msg_id={}...)", 
                              messages_published.load(), messageCount, side, identifier, clientOrderId, actualMessageId.substr(0, 12));
                
                // Wait between messages
                if (i < messageCount - 1 && running) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
                }
                
                // Poll for any responses
                publisher.poll_messages(5);
                
            } catch (const std::exception& e) {
                pub_logger->error("Failed to publish message {}: {}", i + 1, e.what());
            }
        }
        
        pub_logger->info("Publisher finished! Total published: {}", messages_published.load());
        
        // Mark publisher as finished
        publisher_finished = true;
        publisher_finish_time = std::chrono::steady_clock::now();
        
        // Log breakdown by identifier
        {
            std::lock_guard<std::mutex> lock(identifier_mutex);
            pub_logger->info("Messages published by identifier:");
            for (const auto& pair : messages_published_by_identifier) {
                pub_logger->info("  {}: {} messages", pair.first, pair.second);
            }
        }
        
        // Disconnect
        publisher.disconnect();
        
    } catch (const std::exception& e) {
        pub_logger->error("Publisher thread error: {}", e.what());
    }
}

// Multi-identifier subscriber thread function - subscribes to only ONE specific identifier
void multiIdentifierSubscriberThread(const ClusterClientConfig& config, const std::string& subscribeToIdentifier, int totalMessages, int timeoutMs) {
    auto sub_logger = LoggerFactory::instance().getLogger("multi-subscriber");
    
    try {
        sub_logger->info("Starting multi-identifier subscriber thread...");
        sub_logger->info("Subscribing ONLY to identifier: {}", subscribeToIdentifier);
        
        // Create subscriber client
        ClusterClient subscriber(config);
        
        // Set up message callback
        subscriber.set_message_callback([&](const aeron_cluster::ParseResult& result) {
            // Only count ORDER messages, not acknowledgments
            if (result.is_order_message() || 
                (result.is_topic_message() && result.message_type == "CREATE_ORDER")) {
                
                // Extract identifier and IDs for correlation
                std::string extracted_client_order_id;
                std::string extracted_order_id;
                // Extract identifier from headers
                std::string received_identifier = "UNKNOWN";
                try {
                    // Parse payload for client/order ids
                    if (!result.payload.empty()) {
                        Json::CharReaderBuilder rb;
                        std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
                        Json::Value root;
                        std::string errs;
                        const char* begin = result.payload.data();
                        const char* end = begin + result.payload.size();
                        if (reader->parse(begin, end, &root, &errs)) {
                            const Json::Value& msg = root.isMember("message") ? root["message"] : Json::Value();
                            const Json::Value& inner = (msg.isObject() && msg.isMember("message")) ? msg["message"] : Json::Value();
                            auto find_str = [](const Json::Value& obj, const char* key) -> std::string {
                                return obj.isMember(key) && obj[key].isString() ? obj[key].asString() : "";
                            };
                            if (inner.isObject()) {
                                extracted_client_order_id = find_str(inner, "client_order_id");
                                extracted_order_id = find_str(inner, "order_id");
                                if (extracted_client_order_id.empty() && inner.isMember("order_details") && inner["order_details"].isObject()) {
                                    extracted_client_order_id = find_str(inner["order_details"], "client_order_id");
                                }
                            }
                            if (extracted_order_id.empty()) {
                                std::string maybe_uuid = find_str(root, "uuid");
                                if (!maybe_uuid.empty()) {
                                    extracted_order_id = maybe_uuid;
                                }
                            }
                        }
                    }
                    
                    // Try parsing from payload first (cluster adds it there during replay/forward)
                    size_t payload_id_pos = result.payload.find("\"identifier\":\"");
                    if (payload_id_pos != std::string::npos) {
                        payload_id_pos += 14; // Length of "identifier":"
                        size_t end_pos = result.payload.find("\"", payload_id_pos);
                        if (end_pos != std::string::npos) {
                            received_identifier = result.payload.substr(payload_id_pos, end_pos - payload_id_pos);
                        }
                    } else {
                        // Fallback: try parsing from headers
                        size_t identifier_pos = result.headers.find("\"identifier\":\"");
                        if (identifier_pos != std::string::npos) {
                            identifier_pos += 14; // Length of "identifier":""
                            size_t end_pos = result.headers.find("\"", identifier_pos);
                            if (end_pos != std::string::npos) {
                                received_identifier = result.headers.substr(identifier_pos, end_pos - identifier_pos);
                            }
                        }
                    }
                } catch (...) {
                    sub_logger->warn("Failed to extract identifier from headers/payload");
                }
                
                // Track received message
                std::lock_guard<std::mutex> lock(message_mutex);
                
                // Check if we've seen this message before
                if (received_message_ids.find(result.message_id) == received_message_ids.end()) {
                    received_message_ids.insert(result.message_id);
                    messages_received++;
                    if (!extracted_client_order_id.empty()) {
                        std::lock_guard<std::mutex> lock_rcv(received_client_order_mutex);
                        received_client_order_ids.insert(extracted_client_order_id);
                    }
                    
                    // Track received message ID for comparison
                    {
                        std::lock_guard<std::mutex> lock(comparison_mutex);
                        received_message_ids_comparison.insert(result.message_id);
                    }
                    
                    // Track by identifier
                    {
                        std::lock_guard<std::mutex> id_lock(identifier_mutex);
                        messages_received_by_identifier[received_identifier]++;
                    }
                    
                    // Log with WARNING if we received message for wrong identifier
                    if (received_identifier != subscribeToIdentifier) {
                        sub_logger->warn("*** RECEIVED MESSAGE FOR WRONG IDENTIFIER ***: Expected={}, Received={}, ID={}...", 
                                      subscribeToIdentifier, received_identifier, result.message_id.substr(0, 12));
                    } else {
                        if (!extracted_client_order_id.empty()) {
                            bool was_sent = false;
                            std::string sent_msg_id;
                            {
                                std::lock_guard<std::mutex> lock2(client_order_mutex);
                                was_sent = sent_client_order_ids.find(extracted_client_order_id) != sent_client_order_ids.end();
                                auto it = client_order_id_to_message_id.find(extracted_client_order_id);
                                if (it != client_order_id_to_message_id.end()) {
                                    sent_msg_id = it->second;
                                }
                                // Compute latency if available and not yet recorded
                                auto itSend = client_order_send_time.find(extracted_client_order_id);
                                auto itLat = client_order_latency_ms.find(extracted_client_order_id);
                                if (itSend != client_order_send_time.end() && itLat == client_order_latency_ms.end()) {
                                    auto now = std::chrono::steady_clock::now();
                                    client_order_receive_time[extracted_client_order_id] = now;
                                    long long latency_ms_val = std::chrono::duration_cast<std::chrono::milliseconds>(now - itSend->second).count();
                                    client_order_latency_ms[extracted_client_order_id] = latency_ms_val;
                                    client_order_id_to_response_message_id[extracted_client_order_id] = result.message_id;
                                    if (!extracted_order_id.empty()) {
                                        client_order_id_to_order_id[extracted_client_order_id] = extracted_order_id;
                                    }
                                }
                            }
                            if (was_sent) {
                                long long latency_out = -1;
                                auto itLat2 = client_order_latency_ms.find(extracted_client_order_id);
                                if (itLat2 != client_order_latency_ms.end()) latency_out = itLat2->second;
                                sub_logger->info("Received ORDER {}: Identifier={}, client_order_id={} -> order_id={} (matched, req_msg_id={}..., resp_msg_id={}..., latencyMs={})", 
                                              messages_received.load(), 
                                              received_identifier,
                                              extracted_client_order_id,
                                              extracted_order_id.empty() ? "<unknown>" : extracted_order_id,
                                              sent_msg_id.empty() ? "<n/a>" : sent_msg_id.substr(0, 12),
                                              result.message_id.substr(0, 12),
                                              latency_out);
                            } else {
                                sub_logger->warn("Received ORDER {}: Identifier={}, client_order_id={} not recognized (order_id={}, resp_msg_id={}...)", 
                                              messages_received.load(), 
                                              received_identifier,
                                              extracted_client_order_id,
                                              extracted_order_id.empty() ? "<unknown>" : extracted_order_id,
                                              result.message_id.substr(0, 12));
                            }
                        } else {
                            sub_logger->info("Received ORDER message {}: Type={}, Identifier={}, ID={}...", 
                                          messages_received.load(), 
                                          result.message_type,
                                          received_identifier,
                                          result.message_id.substr(0, 12));
                        }
                    }
                } else {
                    sub_logger->debug("Duplicate ORDER message detected (already received): {}...", 
                                   result.message_id.substr(0, 12));
                }
            } else if (result.is_acknowledgment() || result.message_type == "Acknowledgment") {
                // Log acknowledgments but don't count them
                sub_logger->debug("Received ACK: ID={}...", result.message_id.substr(0, 12));
            } else if (result.message_type == "REPLAY_COMPLETE") {
                sub_logger->info("REPLAY_COMPLETE received");
            }
        });
        
        // Set up connection state callback to detect session closures
        bool session_closed_by_cluster = false;
        subscriber.set_connection_state_callback([&](aeron_cluster::ConnectionState old_state, 
                                                     aeron_cluster::ConnectionState new_state) {
            sub_logger->info("Connection state changed: {} -> {}", 
                          static_cast<int>(old_state), static_cast<int>(new_state));
            if (new_state == aeron_cluster::ConnectionState::DISCONNECTED) {
                sub_logger->warn("Session closed/disconnected!");
                session_closed_by_cluster = true;
            }
        });
        
        bool first_connection = true;
        
        int connection_attempt = 0;
        
        while (running) {
            connection_attempt++;
            
            // Generate a NEW instance ID for each connection attempt to avoid cluster rejecting reconnections
            std::string instance_id = "cpp_test_multi_subscriber_" + std::to_string(getpid()) + "_" + std::to_string(connection_attempt);
            sub_logger->info("Using instance ID: {}", instance_id);
            // Connect to cluster
            sub_logger->info("Connecting subscriber to cluster...");
            if (!subscriber.connect()) {
                sub_logger->error("Failed to connect subscriber to cluster");
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            
            sub_logger->info("Subscriber connected successfully!");
            
            // Wait for session to be fully established
            int64_t session_id = subscriber.get_session_id();
            int wait_attempts = 0;
            const int max_wait_attempts = 50; // 5 seconds max
            
            while (session_id == -1 && wait_attempts < max_wait_attempts && running) {
                sub_logger->debug("Waiting for session to be established... (attempt {})", wait_attempts + 1);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                subscriber.poll_messages(10); // Poll to process session messages
                session_id = subscriber.get_session_id();
                wait_attempts++;
            }
            
            if (session_id == -1) {
                sub_logger->error("Failed to establish session (Session ID still -1)");
                sub_logger->warn("Disconnecting and will retry...");
                subscriber.disconnect();
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            
            sub_logger->info("Session established! Session ID: {}", session_id);
            
            // Verify connection is still active before sending subscription
            if (!subscriber.is_connected()) {
                sub_logger->error("Connection lost after session establishment");
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            
            // Send subscription request with SPECIFIC identifier only
            try {
                std::string replay_position = first_connection ? "LAST_COMMIT" : "LAST_COMMIT";
                
                sub_logger->info("Sending subscription request for order_notification_topic with identifier {} and instance {}...", 
                              subscribeToIdentifier, instance_id);
                subscriber.send_subscription_request("order_notification_topic", subscribeToIdentifier, replay_position, instance_id);
                sub_logger->info("Subscription request sent - should ONLY receive messages with identifier: {}", subscribeToIdentifier);
            } catch (const std::exception& e) {
                sub_logger->error("Failed to send subscription request: {}", e.what());
                sub_logger->warn("Disconnecting and will retry...");
                subscriber.disconnect();
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            
            first_connection = false;
            
            // Reset session closure flag for this connection
            session_closed_by_cluster = false;
            
            // Proactively send commit request for the topic so cluster will accept commit offsets
            try {
                bool cr = subscriber.send_commit_request("order_notification_topic");
                sub_logger->info("Commit request sent for topic order_notification_topic: {}", cr ? "OK" : "FAILED");
            } catch (const std::exception& e) {
                sub_logger->warn("Failed to send commit request: {}", e.what());
            }
            
            // Poll messages
            int poll_count = 0;
            int no_message_polls = 0;
            const int max_no_message_polls = 50; // 5 seconds of no messages before checking completion
            
            while (running && subscriber.is_connected()) {
                int messagesPolled = subscriber.poll_messages(10);
                
                if (messagesPolled > 0) {
                    sub_logger->debug("Polled {} messages", messagesPolled);
                    no_message_polls = 0;
                } else {
                    no_message_polls++;
                }
                
                poll_count++;
                
                // Check if cluster closed the session (back pressure, etc.)
                if (session_closed_by_cluster) {
                    sub_logger->warn("Detected session closure by cluster, will reconnect...");
                    subscriber.disconnect();
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    break; // Break to reconnect
                }
                
                // Check if publisher is done and we haven't received messages in a while
                if (messages_published >= totalMessages && no_message_polls >= max_no_message_polls) {
                    sub_logger->info("Publisher finished and no new messages for {} polls", no_message_polls);
                    sub_logger->info("Test complete!");
                    running = false;
                    break;
                }
                
                // Check if publisher is finished and timeout has elapsed
                if (publisher_finished.load()) {
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - publisher_finish_time).count();
                    
                    if (elapsed >= timeoutMs) {
                        sub_logger->info("Publisher finished {}ms ago, timeout reached. Stopping subscriber.", elapsed);
                        sub_logger->info("Final message count: Received={}, Published={}", messages_received.load(), messages_published.load());
                        running = false;
                        break;
                    }
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            
            // If we're done, exit
            if (!running) {
                break;
            }
            
            // If disconnected but still running, reconnect
            if (!subscriber.is_connected() && running) {
                sub_logger->info("Subscriber disconnected, will reconnect...");
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
        
        sub_logger->info("Subscriber finished! Total received: {}", messages_received.load());
        
        // Log breakdown by identifier
        {
            std::lock_guard<std::mutex> lock(identifier_mutex);
            sub_logger->info("Messages received by identifier:");
            for (const auto& pair : messages_received_by_identifier) {
                sub_logger->info("  {}: {} messages", pair.first, pair.second);
            }
        }
        
        // Disconnect
        if (subscriber.is_connected()) {
            subscriber.disconnect();
        }
        
    } catch (const std::exception& e) {
        sub_logger->error("Subscriber thread error: {}", e.what());
    }
}

int main(int argc, char* argv[]) {
    // Initialize logger
    logger = LoggerFactory::instance().getLogger("pubsub_test");
    
    // Parse command line arguments
    std::string testMode = "reconnect";  // Default test mode
    std::vector<std::string> clusterEndpoints = {"localhost:9002", "localhost:9102", "localhost:9202"};
    std::string aeronDir = "/dev/shm/aeron";
    bool debugMode = false;
    int messageCount = 100;
    int messageInterval = 100;  // milliseconds
    int disconnectAt = 30;
    int reconnectDelay = 2000;  // milliseconds
    int timeoutMs = 500000;  // milliseconds
    
    // Multi-identifier test options
    std::vector<std::string> subscribeIdentifiers = {"IDENTIFIER_A"};
    std::vector<std::string> publishIdentifiers = {"IDENTIFIER_A", "IDENTIFIER_B", "IDENTIFIER_C"};
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (arg == "--test-mode" && i + 1 < argc) {
            testMode = argv[++i];
        } else if (arg == "--endpoints" && i + 1 < argc) {
            clusterEndpoints = parseEndpoints(argv[++i]);
        } else if (arg == "--aeron-dir" && i + 1 < argc) {
            aeronDir = argv[++i];
        } else if (arg == "--debug") {
            debugMode = true;
        } else if (arg == "--messages" && i + 1 < argc) {
            messageCount = std::stoi(argv[++i]);
        } else if (arg == "--interval" && i + 1 < argc) {
            messageInterval = std::stoi(argv[++i]);
        } else if (arg == "--disconnect-at" && i + 1 < argc) {
            disconnectAt = std::stoi(argv[++i]);
        } else if (arg == "--reconnect-delay" && i + 1 < argc) {
            reconnectDelay = std::stoi(argv[++i]);
        } else if (arg == "--timeout" && i + 1 < argc) {
            timeoutMs = std::stoi(argv[++i]);
        } else if (arg == "--subscribe-to" && i + 1 < argc) {
            subscribeIdentifiers = parseEndpoints(argv[++i]);
        } else if (arg == "--identifiers" && i + 1 < argc) {
            publishIdentifiers = parseEndpoints(argv[++i]);  // Reuse parseEndpoints for comma-separated list
        } else {
            logger->error("Unknown option: {}", arg);
            printUsage();
            return 1;
        }
    }
    
    if (subscribeIdentifiers.empty()) {
        subscribeIdentifiers.emplace_back("");
    }
    std::string primarySubscribeIdentifier = subscribeIdentifiers.front();

    // NOTE: Signal handlers are automatically installed by SignalHandlerManager
    // when clients register (on connect()). No manual signal handler installation needed.
    // SignalHandlerManager will handle SIGINT and SIGTERM automatically to gracefully
    // disconnect all registered clients.
    
    // Validate test mode
    if (testMode != "reconnect" && testMode != "identifier-filter") {
        logger->error("Invalid test mode: {}. Must be 'reconnect' or 'identifier-filter'", testMode);
        printUsage();
        return 1;
    }
    
    logger->info("========================================");
    if (testMode == "reconnect") {
        logger->info("Pub/Sub Reconnect Test Suite Starting");
    } else {
        logger->info("Pub/Sub Identifier Filter Test Starting");
    }
    logger->info("========================================");
    logger->info("Configuration:");
    logger->info("Test mode: {}", testMode);
    
    std::string endpointsStr;
    for (size_t i = 0; i < clusterEndpoints.size(); ++i) {
        endpointsStr += clusterEndpoints[i];
        if (i < clusterEndpoints.size() - 1)
            endpointsStr += ", ";
    }
    logger->info("Cluster endpoints: {}", endpointsStr);
    logger->info("Aeron directory: {}", aeronDir);
    logger->info("Debug mode: {}", debugMode ? "enabled" : "disabled");
    logger->info("Total messages: {}", messageCount);
    logger->info("Message interval: {}ms", messageInterval);
    
    if (testMode == "reconnect") {
        logger->info("Disconnect after: {} messages", disconnectAt);
        logger->info("Reconnect delay: {}ms", reconnectDelay);
        logger->info("Timeout after publisher finishes: {}ms", timeoutMs);
        if (subscribeIdentifiers.size() == 1 && subscribeIdentifiers.front().empty()) {
            logger->info("Subscribe identifiers: ALL (no identifier filter)");
        } else {
            logger->info("Subscribe identifiers: {}", joinIdentifiers(subscribeIdentifiers));
        }
    } else {
        logger->info("Subscribe to identifier: {}", primarySubscribeIdentifier.empty() ? "(none)" : primarySubscribeIdentifier);
        if (subscribeIdentifiers.size() > 1) {
            logger->warn("Identifier filter test received multiple identifiers; only '{}' will be used for filtering.", primarySubscribeIdentifier.empty() ? "(none)" : primarySubscribeIdentifier);
        }
        std::string identifiersStr;
        for (size_t i = 0; i < publishIdentifiers.size(); ++i) {
            identifiersStr += publishIdentifiers[i];
            if (i < publishIdentifiers.size() - 1)
                identifiersStr += ", ";
        }
        logger->info("Publish to identifiers: {}", identifiersStr);
    }
    logger->info("========================================");
    
    try {
        // Create configurations for publisher and subscriber
        auto pub_config = ClusterClientConfigBuilder()
                            .with_cluster_endpoints(clusterEndpoints)
                            .with_aeron_dir(aeronDir)
                            .with_debug_logging(debugMode)
                            .with_commit_logging(true)
                            .with_response_timeout(std::chrono::milliseconds(10000))
                            .with_max_retries(3)
                            .with_default_topic("order_request_topic")
                            .build();
        
        pub_config.debug_logging = debugMode;
        pub_config.enable_console_info = true;
        pub_config.enable_console_warnings = true;
        pub_config.enable_console_errors = true;
        
        auto sub_config = ClusterClientConfigBuilder()
                            .with_cluster_endpoints(clusterEndpoints)
                            .with_aeron_dir(aeronDir)
                            .with_debug_logging(debugMode)
                            .with_commit_logging(true)
                            .with_response_timeout(std::chrono::milliseconds(10000))
                            .with_max_retries(3)
                            .with_default_topic("order_notification_topic")
                            .build();
        
        sub_config.debug_logging = debugMode;
        sub_config.enable_console_info = true;
        sub_config.enable_console_warnings = true;
        sub_config.enable_console_errors = true;
        
        if (testMode == "reconnect") {
            // ======== RECONNECT TEST ========
            
            // Start subscriber thread first
            logger->info("Starting subscriber thread...");
            std::thread subscriber(subscriberThread, std::ref(sub_config), disconnectAt, reconnectDelay, messageCount, timeoutMs, std::cref(subscribeIdentifiers));
            
            // Wait a bit for subscriber to connect and subscribe
            std::this_thread::sleep_for(std::chrono::seconds(3));
            
            // Start publisher thread
            logger->info("Starting publisher thread...");
            std::vector<std::thread> publishers;
            publishers.reserve(publishIdentifiers.size());
            for (const auto& identifier : publishIdentifiers) {
                publishers.emplace_back(publisherThread, std::ref(pub_config), messageCount, messageInterval, identifier);
            }
            // Wait for threads to complete
            std::this_thread::sleep_for(std::chrono::seconds(10));
            for (auto& publisher : publishers) {
                if (publisher.joinable()) {
                    // logger->info("Joining publisher thread for identifier: {}", identifier);
                    //sleep for 1 second
                    publisher.join();
                }
            }
            logger->info("Publisher thread completed");
            
            subscriber.join();
            logger->info("Subscriber thread completed");
            
            // Final results
            logger->info("========================================");
            logger->info("Test Results:");
            logger->info("========================================");
            logger->info("Messages Published: {}", messages_published.load());
            logger->info("Messages Received: {}", messages_received.load());
            logger->info("Unique Messages Received: {}", received_message_ids.size());
            {
                std::lock_guard<std::mutex> s_lock(client_order_mutex);
                std::lock_guard<std::mutex> r_lock(received_client_order_mutex);
                logger->info("Unique Client Order IDs Sent: {}", sent_client_order_ids.size());
                logger->info("Unique Client Order IDs Received: {}", received_client_order_ids.size());
            }
            
            // Print detailed message comparison
            printMessageComparison();
            // Print latency report
            printLatencyReport();
            
            bool success;
            {
                std::lock_guard<std::mutex> s_lock(client_order_mutex);
                std::lock_guard<std::mutex> r_lock(received_client_order_mutex);
                success = (sent_client_order_ids.size() == received_client_order_ids.size());
            }
            
            if (success) {
                logger->info("========================================");
                logger->info("TEST PASSED! ✓");
                logger->info("All client_order_id values were received correctly!");
                logger->info("Subscriber successfully reconnected and received all messages.");
                logger->info("========================================");
                return 0;
            } else {
                logger->error("========================================");
                logger->error("TEST FAILED! ✗");
                {
                    std::lock_guard<std::mutex> s_lock(client_order_mutex);
                    std::lock_guard<std::mutex> r_lock(received_client_order_mutex);
                    logger->error("Client order ID mismatch!");
                    logger->error("Sent (unique client_order_id): {}", sent_client_order_ids.size());
                    logger->error("Received (unique client_order_id): {}", received_client_order_ids.size());
                }
                logger->error("========================================");
                return 1;
            }
            
        } else if (testMode == "identifier-filter") {
            // ======== IDENTIFIER FILTER TEST ========
            
            // Start subscriber thread first
            logger->info("Starting multi-identifier subscriber thread...");
            std::thread subscriber(multiIdentifierSubscriberThread, std::ref(sub_config), primarySubscribeIdentifier, messageCount, timeoutMs);
            
            // Wait a bit for subscriber to connect and subscribe
            std::this_thread::sleep_for(std::chrono::seconds(3));
            
            // Start publisher thread
            logger->info("Starting multi-identifier publisher thread...");
            std::thread publisher(multiIdentifierPublisherThread, std::ref(pub_config), messageCount, messageInterval, std::ref(publishIdentifiers));
            
            // Wait for threads to complete
            publisher.join();
            logger->info("Publisher thread completed");
            
            subscriber.join();
            logger->info("Subscriber thread completed");
            
            // Final results
            logger->info("========================================");
            logger->info("Test Results:");
            logger->info("========================================");
            logger->info("Messages Published: {}", messages_published.load());
            logger->info("Messages Received: {}", messages_received.load());
            logger->info("Unique Messages Received: {}", received_message_ids.size());
            
            // Print detailed message comparison
            printMessageComparison();
            // Print latency report
            printLatencyReport();
            
            logger->info("\nBreakdown by Identifier:");
            logger->info("Published:");
            int expected_for_subscribed_id = 0;
            {
                std::lock_guard<std::mutex> lock(identifier_mutex);
                for (const auto& pair : messages_published_by_identifier) {
                    logger->info("  {}: {} messages", pair.first, pair.second);
                    if (pair.first == primarySubscribeIdentifier) {
                        expected_for_subscribed_id = pair.second;
                    }
                }
            }
            
            logger->info("Received:");
            int received_for_subscribed_id = 0;
            int received_for_other_ids = 0;
            {
                std::lock_guard<std::mutex> lock(identifier_mutex);
                for (const auto& pair : messages_received_by_identifier) {
                    logger->info("  {}: {} messages", pair.first, pair.second);
                    if (pair.first == primarySubscribeIdentifier) {
                        received_for_subscribed_id = pair.second;
                    } else {
                        received_for_other_ids += pair.second;
                    }
                }
            }
            
            bool success = (messages_received == expected_for_subscribed_id) &&
                          (received_for_subscribed_id == expected_for_subscribed_id) &&
                          (received_for_other_ids == 0);
            
            if (success) {
                logger->info("========================================");
                logger->info("TEST PASSED! ✓");
                logger->info("Identifier filtering works correctly!");
                logger->info("Subscriber ONLY received messages for identifier: {}", primarySubscribeIdentifier);
                logger->info("Expected: {}, Received: {}", expected_for_subscribed_id, received_for_subscribed_id);
                logger->info("Messages from other identifiers: 0");
                logger->info("========================================");
                return 0;
            } else {
                logger->error("========================================");
                logger->error("TEST FAILED! ✗");
                if (received_for_other_ids > 0) {
                    logger->error("CRITICAL: Received {} messages from WRONG identifiers!", received_for_other_ids);
                    logger->error("Subscriber should ONLY receive messages for identifier: {}", primarySubscribeIdentifier);
                }
                logger->error("Expected for {}: {}", primarySubscribeIdentifier, expected_for_subscribed_id);
                logger->error("Received for {}: {}", primarySubscribeIdentifier, received_for_subscribed_id);
                logger->error("Received from other identifiers: {}", received_for_other_ids);
                logger->error("========================================");
                return 1;
            }
        }
        
    } catch (const std::exception& e) {
        logger->fatal("Fatal error: {}", e.what());
        logger->error("Common solutions:");
        logger->error("- Check that Aeron Media Driver is running");
        logger->error("- Verify cluster endpoints are correct and accessible");
        logger->error("- Ensure proper permissions for Aeron directory");
        return 1;
    }
    
    return 0;
}

