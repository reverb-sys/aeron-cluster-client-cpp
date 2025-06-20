#include <aeron_cluster/cluster_client.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
// Helper function implementations for string parsing
#include <sstream>
#include <iomanip>

using namespace aeron_cluster;

// Global flag for graceful shutdown
std::atomic<bool> running{true};

void signalHandler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down gracefully..." << std::endl;
    running = false;
}

void printUsage() {
    std::cout << "Aeron Cluster C++ Client - Basic Example" << std::endl;
    std::cout << "=========================================" << std::endl;
    std::cout << "\nThis example demonstrates:" << std::endl;
    std::cout << "• Connecting to Aeron Cluster" << std::endl;
    std::cout << "• Publishing sample trading orders" << std::endl;
    std::cout << "• Receiving and handling acknowledgments" << std::endl;
    std::cout << "• Connection statistics and monitoring" << std::endl;
    std::cout << "\nUsage: ./basic_client_example [options]" << std::endl;
    std::cout << "\nOptions:" << std::endl;
    std::cout << "  --help              Show this help message" << std::endl;
    std::cout << "  --endpoints LIST    Comma-separated cluster endpoints (default: localhost:9002,localhost:9102,localhost:9202)" << std::endl;
    std::cout << "  --aeron-dir PATH    Aeron media driver directory (default: /dev/shm/aeron)" << std::endl;
    std::cout << "  --debug             Enable debug logging" << std::endl;
    std::cout << "  --orders COUNT      Number of test orders to send (default: 5)" << std::endl;
    std::cout << "  --interval MS       Interval between orders in milliseconds (default: 1000)" << std::endl;
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

int main(int argc, char* argv[]) {
    // Parse command line arguments
    std::vector<std::string> clusterEndpoints = {
        "localhost:9002", "localhost:9102", "localhost:9202"
    };
    std::string aeronDir = "/dev/shm/aeron";
    bool debugMode = false;
    int orderCount = 5;
    int orderInterval = 1000; // milliseconds
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (arg == "--endpoints" && i + 1 < argc) {
            clusterEndpoints = parseEndpoints(argv[++i]);
        } else if (arg == "--aeron-dir" && i + 1 < argc) {
            aeronDir = argv[++i];
        } else if (arg == "--debug") {
            debugMode = true;
        } else if (arg == "--orders" && i + 1 < argc) {
            orderCount = std::stoi(argv[++i]);
        } else if (arg == "--interval" && i + 1 < argc) {
            orderInterval = std::stoi(argv[++i]);
        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            printUsage();
            return 1;
        }
    }
    
    // Setup signal handlers for graceful shutdown
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    
    std::cout << "🚀 Starting Aeron Cluster C++ Client Example" << std::endl;
    std::cout << "=============================================" << std::endl;
    
    try {
        // Create client configuration
        auto config = ClusterClientConfigBuilder()
            .withClusterEndpoints(clusterEndpoints)
            .withAeronDir(aeronDir)
            .withResponseTimeout(std::chrono::seconds(10))
            .withMaxRetries(3)
            .build();
        
        // Enable debug mode if requested
        config.debug_logging = debugMode;
        config.enable_console_info = true;
        
        std::cout << "📋 Configuration:" << std::endl;
        std::cout << "   Cluster endpoints: ";
        for (size_t i = 0; i < clusterEndpoints.size(); ++i) {
            std::cout << clusterEndpoints[i];
            if (i < clusterEndpoints.size() - 1) std::cout << ", ";
        }
        std::cout << std::endl;
        std::cout << "   Aeron directory: " << aeronDir << std::endl;
        std::cout << "   Debug mode: " << (debugMode ? "enabled" : "disabled") << std::endl;
        std::cout << "   Orders to send: " << orderCount << std::endl;
        std::cout << "   Order interval: " << orderInterval << "ms" << std::endl;
        std::cout << std::endl;
        
        // Create the client
        ClusterClient client(config);
        
        // Set up message callback to handle responses
        int acknowledgedOrders = 0;
        int failedOrders = 0;
        
        client.setMessageCallback([&](const std::string& messageType, 
                                     const std::string& payload, 
                                     const std::string& headers) {
            if (messageType.find("ACK") != std::string::npos || 
                messageType.find("ACKNOWLEDGMENT") != std::string::npos) {
                
                // Parse the acknowledgment
                if (payload.find("\"success\":true") != std::string::npos ||
                    payload.find("\"status\":\"success\"") != std::string::npos) {
                    acknowledgedOrders++;
                    std::cout << "✅ Order acknowledged (" << acknowledgedOrders << "/" << orderCount << ")" << std::endl;
                } else {
                    failedOrders++;
                    std::cout << "❌ Order failed (" << failedOrders << " total failures)" << std::endl;
                    if (debugMode) {
                        std::cout << "   Failure details: " << payload.substr(0, 200) << std::endl;
                    }
                }
            } else if (debugMode) {
                std::cout << "📨 Received message type: " << messageType << std::endl;
            }
        });
        
        // Connect to the cluster
        std::cout << "🔌 Connecting to Aeron Cluster..." << std::endl;
        
        if (!client.connect()) {
            std::cerr << "❌ Failed to connect to cluster. Please ensure:" << std::endl;
            std::cerr << "   • Aeron Media Driver is running" << std::endl;
            std::cerr << "   • Cluster nodes are accessible at specified endpoints" << std::endl;
            std::cerr << "   • Aeron directory (" << aeronDir << ") exists and is writable" << std::endl;
            return 1;
        }
        
        std::cout << "🎉 Successfully connected to cluster!" << std::endl;
        std::cout << "   Session ID: " << client.getSessionId() << std::endl;
        std::cout << "   Leader Member: " << client.getLeaderMemberId() << std::endl;
        std::cout << std::endl;
        
        // Publish test orders
        std::cout << "📤 Publishing " << orderCount << " test orders..." << std::endl;
        std::vector<std::string> publishedMessageIds;
        
        for (int i = 0; i < orderCount && running; ++i) {
            try {
                // Create a sample order with varying parameters
                std::string side = (i % 2 == 0) ? "BUY" : "SELL";
                double quantity = 1.0 + (i * 0.5);
                double price = 3500.0 + (i * 50.0);
                
                Order order = ClusterClient::createSampleLimitOrder(
                    "ETH", "USDC", side, quantity, price);
                
                // Customize order for this example
                order.accountID = 10000 + i;
                order.customerID = 50000 + i;
                
                // Publish the order
                std::string messageId = client.publishOrder(order);
                publishedMessageIds.push_back(messageId);
                
                std::cout << "   Order " << (i + 1) << "/" << orderCount 
                         << ": " << side << " " << quantity << " ETH @ " << price 
                         << " USDC (ID: " << messageId.substr(0, 12) << "...)" << std::endl;
                
                // Wait between orders
                if (i < orderCount - 1 && running) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(orderInterval));
                }
                
                // Poll for responses periodically
                client.pollMessages(5);
                
            } catch (const std::exception& e) {
                std::cerr << "❌ Failed to publish order " << (i + 1) << ": " << e.what() << std::endl;
            }
        }
        
        std::cout << std::endl;
        std::cout << "⏳ Waiting for acknowledgments..." << std::endl;
        
        // Wait for acknowledgments with timeout
        auto waitStart = std::chrono::steady_clock::now();
        auto waitTimeout = std::chrono::seconds(30);
        
        while (running && (acknowledgedOrders + failedOrders) < orderCount) {
            // Poll for messages
            int messagesReceived = client.pollMessages(10);
            
            // Check timeout
            auto elapsed = std::chrono::steady_clock::now() - waitStart;
            if (elapsed > waitTimeout) {
                std::cout << "⏰ Timeout waiting for acknowledgments" << std::endl;
                break;
            }
            
            // Short sleep to avoid busy waiting
            if (messagesReceived == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        
        // Final statistics
        std::cout << std::endl;
        std::cout << "📊 Final Results:" << std::endl;
        std::cout << "=================" << std::endl;
        
        ConnectionStats stats = client.getConnectionStats();
        
        std::cout << "📤 Orders published: " << orderCount << std::endl;
        std::cout << "✅ Orders acknowledged: " << acknowledgedOrders << std::endl;
        std::cout << "❌ Orders failed: " << failedOrders << std::endl;
        std::cout << "⏸️  Orders pending: " << (orderCount - acknowledgedOrders - failedOrders) << std::endl;
        std::cout << std::endl;
        
        std::cout << "🔗 Connection Statistics:" << std::endl;
        std::cout << "   Messages sent: " << stats.messages_sent << std::endl;
        std::cout << "   Messages received: " << stats.messages_received << std::endl;
        std::cout << "   Messages acknowledged: " << stats.messages_acknowledged << std::endl;
        std::cout << "   Messages failed: " << stats.messages_failed << std::endl;
        std::cout << "   Connection attempts: " << stats.connection_attempts << std::endl;
        std::cout << "   Successful connections: " << stats.successful_connections << std::endl;
        std::cout << "   Leader redirects: " << stats.leader_redirects << std::endl;
        
        if (stats.total_uptime.count() > 0) {
            std::cout << "   Total uptime: " << stats.total_uptime.count() << "ms" << std::endl;
        }
        
        // Success rate calculation
        if (orderCount > 0) {
            double successRate = (double)acknowledgedOrders / orderCount * 100.0;
            std::cout << "   Success rate: " << std::fixed << std::setprecision(1) 
                     << successRate << "%" << std::endl;
        }
        
        // Continue polling for a bit longer to catch any late messages
        if (running && (acknowledgedOrders + failedOrders) < orderCount) {
            std::cout << std::endl;
            std::cout << "🔄 Continuing to poll for late acknowledgments (10 seconds)..." << std::endl;
            
            auto extendedWaitStart = std::chrono::steady_clock::now();
            auto extendedTimeout = std::chrono::seconds(10);
            
            while (running && (acknowledgedOrders + failedOrders) < orderCount) {
                client.pollMessages(10);
                
                auto elapsed = std::chrono::steady_clock::now() - extendedWaitStart;
                if (elapsed > extendedTimeout) {
                    break;
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
            if ((acknowledgedOrders + failedOrders) > stats.messages_acknowledged + stats.messages_failed) {
                std::cout << "📨 Received additional acknowledgments!" << std::endl;
                std::cout << "✅ Final acknowledged: " << acknowledgedOrders << std::endl;
                std::cout << "❌ Final failed: " << failedOrders << std::endl;
            }
        }
        
        std::cout << std::endl;
        std::cout << "🏁 Example completed successfully!" << std::endl;
        
        if (!running) {
            std::cout << "ℹ️  Shutdown requested by user" << std::endl;
        }
        
        // Disconnect gracefully
        std::cout << "👋 Disconnecting from cluster..." << std::endl;
        client.disconnect();
        
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "💥 Fatal error: " << e.what() << std::endl;
        std::cerr << std::endl;
        std::cerr << "Common solutions:" << std::endl;
        std::cerr << "• Check that Aeron Media Driver is running" << std::endl;
        std::cerr << "• Verify cluster endpoints are correct and accessible" << std::endl;
        std::cerr << "• Ensure proper permissions for Aeron directory" << std::endl;
        std::cerr << "• Check firewall settings for UDP traffic" << std::endl;
        std::cerr << "• Verify JsonCpp library is properly installed" << std::endl;
        return 1;
    }
}
