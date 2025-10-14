#include <aeron_cluster/cluster_client.hpp>
#include <aeron_cluster/logging.hpp>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>
// Helper function implementations for string parsing
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <ctime>
#include <sys/types.h>

#include <fstream>
#include <iomanip>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace aeron_cluster;

// Global flag for graceful shutdown
std::atomic<bool> running{true};

// Global logger instance
std::shared_ptr<Logger> logger;

void signalHandler(int signal) {
    logger->info("Received signal {}, shutting down gracefully...", signal);
    running = false;
}

void printUsage() {
    std::cout << "Aeron Cluster C++ Client - Basic Example" << std::endl;
    std::cout << "=========================================" << std::endl;
    std::cout << "\nThis example demonstrates:" << std::endl;
    std::cout << "- Connecting to Aeron Cluster" << std::endl;
    std::cout << "- Publishing sample trading orders" << std::endl;
    std::cout << "- Receiving and handling acknowledgments" << std::endl;
    std::cout << "- Connection statistics and monitoring" << std::endl;
    std::cout << "\nUsage: ./basic_client_example [options]" << std::endl;
    std::cout << "\nOptions:" << std::endl;
    std::cout << "  --help              Show this help message" << std::endl;
    std::cout << "  --endpoints LIST    Comma-separated cluster endpoints (default: "
                 "localhost:9002,localhost:9102,localhost:9202)"
              << std::endl;
    std::cout << "  --aeron-dir PATH    Aeron media driver directory (default: /dev/shm/aeron)"
              << std::endl;
    std::cout
        << "  --client-type TYPE  Client type: 'publisher' or 'subscriber' (default: publisher)"
        << std::endl;
    std::cout << "  --debug             Enable debug logging" << std::endl;
    std::cout << "  --orders COUNT      Number of test orders to send (default: 5, not required "
                 "for client-type 'subscriber')"
              << std::endl;
    std::cout << "  --interval MS       Interval between orders in milliseconds (default: 1000)"
              << std::endl;
    std::cout << "  --timeout MS        Connection timeout in milliseconds (default: 10000)"
              << std::endl;
    std::cout << "  --check-only        Only check cluster availability, don't send orders"
              << std::endl;
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

// FIXED: Add pre-flight checks
bool performPreflightChecks(const std::string& aeronDir,
                            const std::vector<std::string>& endpoints) {
    logger->info("Performing pre-flight checks...");

    // Check 1: Aeron directory and Media Driver
    std::string cncFile = aeronDir + "/cnc.dat";
    std::ifstream file(cncFile);
    bool aeronRunning = file.good();
    file.close();

    if (!aeronRunning) {
        logger->error("Aeron Media Driver not detected");
        return false;
    }
    logger->info("Aeron Media Driver detected at: {}", aeronDir);

    // Check 2: Warn about cluster requirements
    logger->warn("Cluster Requirements:");
    logger->warn("Make sure Aeron Cluster is running at these endpoints:");
    for (const auto& endpoint : endpoints) {
        logger->warn("  - {}", endpoint);
    }

    return true;
}

// FIXED: Add connection test with timeout
bool testClusterConnectivity(const std::vector<std::string>& endpoints, const std::string& aeronDir,
                             int timeoutMs, bool debugMode) {
    logger->info("Testing cluster connectivity...");

    try {
        // Create minimal configuration for connectivity test
        auto config = ClusterClientConfigBuilder()
                          .with_cluster_endpoints(endpoints)
                          .with_aeron_dir(aeronDir)
                          .with_response_timeout(std::chrono::milliseconds(timeoutMs))
                          .with_max_retries(1)  // Only one retry for quick test
                          .build();

        config.debug_logging = debugMode;
        config.enable_console_info = true;
        config.enable_console_warnings = true;
        config.enable_console_errors = true;

        // Create client and attempt connection
        ClusterClient testClient(config);

        logger->info("Attempting connection (timeout: {}ms)...", timeoutMs);

        bool connected = testClient.connect();

        if (connected) {
            logger->info("Successfully connected to cluster!");
            logger->info("Session ID: {}", testClient.get_session_id());
            logger->info("Leader Member: {}", testClient.get_leader_member_id());

            // Disconnect cleanly
            testClient.disconnect();
            return true;
        } else {
            logger->error("Failed to connect to cluster");
            return false;
        }

    } catch (const std::exception& e) {
        logger->error("Connection test failed with exception: {}", e.what());
        return false;
    }
}

int main(int argc, char* argv[]) {
    // Initialize logger
    logger = LoggerFactory::instance().getLogger("basic_client_example");
    
    // Parse command line arguments
    std::vector<std::string> clusterEndpoints = {"localhost:9002", "localhost:9102",
                                                 "localhost:9202"};
    std::string aeronDir = "/dev/shm/aeron";
    bool debugMode = false;
    bool checkOnly = false;
    std::string clientType = "publisher";  // Default to publisher
    int orderCount = 5;
    int orderInterval = 1000;       // milliseconds
    int connectionTimeout = 10000;  // milliseconds - FIXED: Add configurable timeout

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (arg == "--endpoints" && i + 1 < argc) {
            clusterEndpoints = parseEndpoints(argv[++i]);
        } else if (arg == "--aeron-dir" && i + 1 < argc) {
            aeronDir = argv[++i];
        } else if (arg == "--client-type" && i + 1 < argc) {
            clientType = argv[++i];
            if (clientType == "subscriber") {
                orderCount = 0;     // No orders for subscriber
                orderInterval = 0;  // No interval for subscriber
            } else if (clientType != "publisher") {
                logger->error("Invalid client type: {}. Use 'publisher' or 'subscriber'.", clientType);
                printUsage();
                return 1;
            }
        } else if (arg == "--debug") {
            debugMode = true;
        } else if (arg == "--check-only") {
            checkOnly = true;
        } else if (arg == "--orders" && i + 1 < argc) {
            orderCount = std::stoi(argv[++i]);
        } else if (arg == "--interval" && i + 1 < argc) {
            orderInterval = std::stoi(argv[++i]);
        } else if (arg == "--timeout" && i + 1 < argc) {
            connectionTimeout = std::stoi(argv[++i]);
        } else {
            logger->error("Unknown option: {}", arg);
            printUsage();
            return 1;
        }
    }

    // Setup signal handlers for graceful shutdown
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGHUP, signalHandler);  // Also handle SIGHUP for robustness

    logger->info("Starting Aeron Cluster C++ Client Example");
    logger->info("=============================================");
    logger->info("Configuration:");
    
    std::string endpointsStr;
    for (size_t i = 0; i < clusterEndpoints.size(); ++i) {
        endpointsStr += clusterEndpoints[i];
        if (i < clusterEndpoints.size() - 1)
            endpointsStr += ", ";
    }
    logger->info("Cluster endpoints: {}", endpointsStr);
    logger->info("Aeron directory: {}", aeronDir);
    logger->info("Debug mode: {}", debugMode ? "enabled" : "disabled");
    logger->info("Connection timeout: {}ms", connectionTimeout);

    if (clientType == "subscriber") {
        logger->info("Client type: Subscriber (no orders will be sent)");
    } else {
        logger->info("Client type: Publisher");
        if (!checkOnly) {
            logger->info("Orders to send: {}", orderCount);
            logger->info("Order interval: {}ms", orderInterval);
        } else {
            logger->info("Only checking cluster availability, no orders will be sent");
        }
    }

    try {
        // FIXED: Perform pre-flight checks first
        if (!performPreflightChecks(aeronDir, clusterEndpoints)) {
            return 1;
        }

        // Create client configuration with proper timeout
        auto config = ClusterClientConfigBuilder()
                          .with_cluster_endpoints(clusterEndpoints)
                          .with_aeron_dir(aeronDir)
                          .with_debug_logging(true)  // Enable debug logging to see what's happening
                          .with_response_timeout(std::chrono::milliseconds(connectionTimeout))
                          .with_max_retries(3)
                          .with_default_topic("order_request_topic")
                          .build();

        // Enable debug mode if requested
        config.debug_logging = debugMode;
        config.enable_console_info = true;
        config.enable_console_warnings = true;
        config.enable_console_errors = true;

        // Create the client
        ClusterClient client(config);

        // Set up message callback to handle responses
        int acknowledgedOrders = 0;
        int failedOrders = 0;

        client.set_message_callback([&](const aeron_cluster::ParseResult& result) {
            if (result.is_order_message()) {
                // Handle ORDER_MESSAGE specifically
                logger->info("ORDER_MESSAGE received: {}", result.payload.substr(0, 200));
                logger->debug("Message Type: {}", result.message_type);
                logger->debug("Message ID: {}", result.message_id);
            } else if (result.is_acknowledgment()) {
                // Handle acknowledgments
                logger->info("Acknowledgment received");
                // Parse the acknowledgment
                if (result.payload.find("\"success\":true") != std::string::npos ||
                    result.payload.find("\"status\":\"success\"") != std::string::npos ||
                    result.payload.find("SUCCESS") != std::string::npos) {
                    acknowledgedOrders++;
                    logger->info("Order acknowledged ({}/{})", acknowledgedOrders, orderCount);
                } else {
                    failedOrders++;
                    logger->warn("Order failed ({} total failures)", failedOrders);
                    if (debugMode) {
                        logger->debug("Failure details: {}", result.payload.substr(0, 200));
                    }
                }
            } else if (result.is_topic_message()) {
                // Handle other TopicMessages
                logger->info("TopicMessage received: {}", result.message_type);
                
                // Check for REPLAY_COMPLETE messages
                if (result.message_type == "REPLAY_COMPLETE" || 
                    result.payload.find("REPLAY_COMPLETE") != std::string::npos) {
                    logger->info("REPLAY_COMPLETE received: {}", result.payload);
                }
                // Check if this is actually an ORDER_MESSAGE by content
                else if (result.message_type == "CREATE_ORDER" || 
                    result.payload.find("order_details") != std::string::npos ||
                    result.payload.find("token_pair") != std::string::npos) {
                    logger->info("ORDER_MESSAGE detected by content: {}", result.payload.substr(0, 200));
                    logger->debug("Message Type: {}", result.message_type);
                    logger->debug("Message ID: {}", result.message_id);
                } else {
                    // For subscriber mode, show all messages
                    logger->info("Message details:");
                    logger->debug("Message Type: {}", result.message_type);
                    logger->debug("Message ID: {}", result.message_id);
                    logger->debug("Payload: {}", result.payload.substr(0, 200));
                    if (!result.headers.empty()) {
                        logger->debug("Headers: {}", result.headers);
                    }
                }
            } else if (debugMode) {
                // print all result
                logger->debug("Received message type: {}", result.message_type);
                logger->debug("Received message payload: {}", result.payload.substr(0, 200));
                logger->debug("Received message ID: {}...", result.message_id.substr(0, 12));
                logger->debug("Received message Timestamp: {}", result.timestamp);
                logger->debug("Correlation ID: {}", result.correlation_id);
                logger->debug("Received message length: {} bytes", result.payload.length());
            }
        });

        // Connect to the cluster (we know it works from connectivity test)
        logger->info("Connecting to Aeron Cluster for order processing...");

        if (!client.connect()) {
            logger->error("Failed to connect to cluster for order processing");
            return 1;
        }

        logger->info("Successfully connected to cluster!");
        logger->info("Session ID: {}", client.get_session_id());
        logger->info("Leader Member: {}", client.get_leader_member_id());

        if (clientType == "subscriber") {
            logger->info("Subscribing to order requests...");
            
            // Set up connection state callback to monitor connection changes
            client.set_connection_state_callback([&](aeron_cluster::ConnectionState old_state, aeron_cluster::ConnectionState new_state) {
                logger->info("Connection state changed: {} -> {}", static_cast<int>(old_state), static_cast<int>(new_state));
                if (new_state == aeron_cluster::ConnectionState::DISCONNECTED) {
                    logger->warn("Connection lost!");
                } else if (new_state == aeron_cluster::ConnectionState::CONNECTED) {
                    logger->info("Connection established!");
                }
            });
            
            logger->info("Waiting for orders (no orders will be sent)");
            
            // Start polling BEFORE sending subscription request to catch replay messages
            logger->info("Starting message polling...");
            int pollCount = 0;
            auto lastActivityTime = std::chrono::steady_clock::now();
            const auto connectionTimeout = std::chrono::seconds(30); // 30 second timeout
            bool subscriptionSent = false;
            bool isReconnecting = false;
            int reconnectAttempts = 0;
            const int maxReconnectAttempts = -1; // Infinite reconnection attempts
            const auto reconnectDelay = std::chrono::seconds(5); // Start with 5 second delay
            auto currentReconnectDelay = reconnectDelay;
            auto lastConnectionTime = std::chrono::steady_clock::now();
            const auto minConnectionDuration = std::chrono::seconds(10); // Minimum time to consider connection stable
            
            while (running) {
                // Check connection status first
                if (!client.is_connected()) {
                    auto now = std::chrono::steady_clock::now();
                    auto connectionDuration = now - lastConnectionTime;
                    
                    // Only start reconnection if we've been disconnected for a reasonable time
                    // or if we haven't been connected long enough to be stable
                    if (!isReconnecting && (connectionDuration > minConnectionDuration || reconnectAttempts == 0)) {
                        logger->warn("Connection lost! Starting reconnection process...");
                        isReconnecting = true;
                        subscriptionSent = false; // Reset subscription flag
                        reconnectAttempts = 0;
                        currentReconnectDelay = reconnectDelay; // Reset delay
                    }
                    
                    // Only attempt reconnection if we're in reconnection mode
                    if (!isReconnecting) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        continue;
                    }
                    
                    // Attempt reconnection
                    logger->info("Reconnection attempt {}...", reconnectAttempts + 1);
                    if (client.connect()) {
                        logger->info("Reconnected successfully!");
                        isReconnecting = false;
                        reconnectAttempts = 0;
                        currentReconnectDelay = reconnectDelay; // Reset delay for next time
                        lastActivityTime = std::chrono::steady_clock::now(); // Reset timeout
                        lastConnectionTime = std::chrono::steady_clock::now(); // Track connection time
                    } else {
                        reconnectAttempts++;
                        if (maxReconnectAttempts > 0 && reconnectAttempts >= maxReconnectAttempts) {
                            logger->error("Maximum reconnection attempts reached. Exiting...");
                            break;
                        }
                        
                        // Exponential backoff with jitter
                        auto backoffDelay = currentReconnectDelay + std::chrono::milliseconds(
                            std::rand() % 1000); // Add up to 1 second jitter
                        
                        logger->warn("Reconnection failed. Retrying in {} seconds... (attempt {})", 
                                  std::chrono::duration_cast<std::chrono::seconds>(backoffDelay).count(), 
                                  reconnectAttempts);
                        
                        std::this_thread::sleep_for(backoffDelay);
                        
                        // Increase delay for next attempt (capped at 30 seconds)
                        currentReconnectDelay = std::min(currentReconnectDelay * 2, std::chrono::seconds(30));
                        continue;
                    }
                }
                
                // Send subscription request on first connection or after reconnection
                if (!subscriptionSent && client.is_connected()) {
                    logger->info("Sending subscription request...");
                    try {
                        // Generate unique instance identifier for load balancing
                        std::string instance_id = "cpp_client_" + std::to_string(getpid()) + "_" + std::to_string(std::time(nullptr));
                        client.send_subscription_request("order_notification_topic", "ROHIT_AERON01_TX", "LAST_COMMIT", instance_id);
                        logger->info("Subscription request sent!");
                        subscriptionSent = true;
                    } catch (const std::exception& e) {
                        logger->error("Failed to send subscription request: {}", e.what());
                        subscriptionSent = false;
                        continue;
                    }
                }
                
                // Only poll messages if we're connected and subscribed
                if (client.is_connected() && subscriptionSent) {
                    int messagesReceived = client.poll_messages(10);
                    if (messagesReceived > 0) {
                        logger->debug("Polled {} messages", messagesReceived);
                        lastActivityTime = std::chrono::steady_clock::now(); // Reset timeout on activity
                    }
                } else if (client.is_connected() && !subscriptionSent) {
                    // We're connected but not subscribed yet, just wait a bit
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                
                pollCount++;
                if (pollCount % 50 == 0) {  // Show status every 5 seconds
                    if (isReconnecting) {
                        logger->info("Reconnecting... (attempt {})", reconnectAttempts);
                    } else if (client.is_connected() && subscriptionSent) {
                        logger->debug("Still waiting for messages... (polled {} times)", pollCount);
                    } else if (client.is_connected() && !subscriptionSent) {
                        logger->debug("Connected, waiting to subscribe...");
                    }
                }
                
                // Check for timeout - if no activity for 30 seconds, something might be wrong
                // But only if we're not already reconnecting and have been connected for a while
                auto now = std::chrono::steady_clock::now();
                auto connectionDuration = now - lastConnectionTime;
                if (!isReconnecting && connectionDuration > minConnectionDuration && 
                    now - lastActivityTime > connectionTimeout) {
                    logger->warn("No activity for 30 seconds, checking connection...");
                    if (!client.is_connected()) {
                        logger->warn("Connection lost during timeout check!");
                        isReconnecting = true; // Trigger reconnection logic
                        subscriptionSent = false; // Reset subscription flag
                        continue; // Will trigger reconnection logic above
                    }
                    lastActivityTime = now; // Reset timeout
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            return 0;  // Exit after subscriber logic
        } else {
            // Publish test orders
            logger->info("Publishing {} test orders...", orderCount);
            std::vector<std::string> publishedMessageIds;

            for (int i = 0; i < orderCount && running; ++i) {
                try {
                    // Create a sample order with varying parameters
                    std::string side = (i % 2 == 0) ? "BUY" : "SELL";
                    double quantity = 1.0 + (i * 0.5);
                    double price = 3500.0 + (i * 50.0);

                    Order order = ClusterClient::create_sample_limit_order("ETH", "USDC", side,
                                                                           quantity, price);

                    // Customize order for this example
                    order.account_id = 10000 + i;
                    order.customer_id = 50000 + i;

                    // Publish the order
                    std::string messageId = client.publish_order_to_topic(order, "order_request_topic");
                    publishedMessageIds.push_back(messageId);

                    logger->info("Order {}/{}: {} {} ETH @ {} USDC (ID: {}...)", 
                              i + 1, orderCount, side, quantity, price, messageId.substr(0, 12));

                    // Wait between orders
                    if (i < orderCount - 1 && running) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(orderInterval));
                    }

                    // Poll for responses periodically
                    client.poll_messages(5);

                } catch (const std::exception& e) {
                    logger->error("Failed to publish order {}: {}", i + 1, e.what());
                    failedOrders++;
                }
            }

            logger->info("Waiting for acknowledgments...");

            // Wait for acknowledgments with timeout
            auto waitStart = std::chrono::steady_clock::now();
            auto waitTimeout = std::chrono::seconds(30);

            while (running && (acknowledgedOrders + failedOrders) < orderCount) {
                // Poll for messages
                int messagesReceived = client.poll_messages(10);

                // Check timeout
                auto elapsed = std::chrono::steady_clock::now() - waitStart;
                if (elapsed > waitTimeout) {
                    logger->warn("Timeout waiting for acknowledgments");
                    break;
                }

                // Short sleep to avoid busy waiting
                if (messagesReceived == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
        }

        // Final statistics
        logger->info("Final Results:");
        logger->info("==============");

        ConnectionStats stats = client.get_connection_stats();

        logger->info("Orders published: {}", orderCount);
        logger->info("Orders acknowledged: {}", acknowledgedOrders);
        logger->info("Orders failed: {}", failedOrders);
        logger->info("Orders pending: {}", orderCount - acknowledgedOrders - failedOrders);

        logger->info("Connection Statistics:");
        logger->info("  Messages sent: {}", stats.messages_sent);
        logger->info("  Messages received: {}", stats.messages_received);
        logger->info("  Messages acknowledged: {}", stats.messages_acknowledged);
        logger->info("  Messages failed: {}", stats.messages_failed);
        logger->info("  Connection attempts: {}", stats.connection_attempts);
        logger->info("  Successful connections: {}", stats.successful_connections);
        logger->info("  Leader redirects: {}", stats.leader_redirects);

        if (stats.total_uptime.count() > 0) {
            logger->info("  Total uptime: {}ms", stats.total_uptime.count());
        }

        // Success rate calculation
        if (orderCount > 0) {
            double successRate = (double)acknowledgedOrders / orderCount * 100.0;
            logger->info("  Success rate: {:.1f}%", successRate);
        }

        // Continue polling for a bit longer to catch any late messages
        if (running && (acknowledgedOrders + failedOrders) < orderCount) {
            logger->info("Continuing to poll for late acknowledgments (10 seconds)...");

            auto extendedWaitStart = std::chrono::steady_clock::now();
            auto extendedTimeout = std::chrono::seconds(10);

            while (running && (acknowledgedOrders + failedOrders) < orderCount) {
                client.poll_messages(10);

                auto elapsed = std::chrono::steady_clock::now() - extendedWaitStart;
                if (elapsed > extendedTimeout) {
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            if ((acknowledgedOrders + failedOrders) >
                stats.messages_acknowledged + stats.messages_failed) {
                logger->info("Received additional acknowledgments!");
                logger->info("Final acknowledged: {}", acknowledgedOrders);
                logger->info("Final failed: {}", failedOrders);
            }
        }

        logger->info("Example completed successfully!");

        if (!running) {
            logger->info("Shutdown requested by user");
        }

        // Disconnect gracefully
        logger->info("Disconnecting from cluster...");
        client.disconnect();

        return 0;

    } catch (const std::exception& e) {
        logger->fatal("Fatal error: {}", e.what());
        logger->error("Common solutions:");
        logger->error("- Check that Aeron Media Driver is running");
        logger->error("- Verify cluster endpoints are correct and accessible");
        logger->error("- Ensure proper permissions for Aeron directory");
        logger->error("- Check firewall settings for UDP traffic");
        logger->error("- Verify JsonCpp library is properly installed");
        logger->error("- Try running with --check-only flag first");
        logger->error("- Use --debug flag for more detailed error information");
        return 1;
    }
}