#include <aeron_cluster/cluster_client.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
// Helper function implementations for string parsing
#include <sstream>
#include <iomanip>
#include <fstream>

#include <random>
#include <regex>
#include <stdexcept>
#include <string>
#include <set>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

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
    std::cout << "  --timeout MS        Connection timeout in milliseconds (default: 10000)" << std::endl;
    std::cout << "  --check-only        Only check cluster availability, don't send orders" << std::endl;
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

int getAvailablePortInRange(int minPort = 40000, int maxPort = 59000) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dist(minPort, maxPort);
        std::set<int> tried;

        while (tried.size() < (maxPort - minPort + 1)) {
            int port = dist(gen);
            if (tried.count(port)) continue;
            tried.insert(port);

            int sock = socket(AF_INET, SOCK_DGRAM, 0);
            if (sock < 0) continue;

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(port);

            if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                close(sock);
                return port;  // port is available
            }

            close(sock);
        }

        throw std::runtime_error("No available UDP port found in the range");
    }

 std::string resolveEgressEndpoint(const std::string& channel) {
        // Match hostnames (e.g., localhost), IPv4, and port numbers
        std::regex pattern(R"(endpoint=([a-zA-Z0-9\.\-]+):(\d+))");
        std::smatch match;

        if (std::regex_search(channel, match, pattern)) {
            std::string host = match[1].str();
            std::string portStr = match[2].str();
            int port = std::stoi(portStr);

            if (port == 0) {
                int newPort = getAvailablePortInRange();
                std::string newEndpoint = "endpoint=" + host + ":" + std::to_string(newPort);
                return std::regex_replace(channel, pattern, newEndpoint);
            } else {
                return channel;
            }
        } else {
            throw std::invalid_argument("Channel endpoint is not in the correct format: expected 'endpoint=HOST:PORT'");
        }
    }

// FIXED: Add pre-flight checks
bool performPreflightChecks(const std::string& aeronDir, const std::vector<std::string>& endpoints) {
    std::cout << "🔍 Performing pre-flight checks..." << std::endl;
    
    // Check 1: Aeron directory and Media Driver
    std::string cncFile = aeronDir + "/cnc.dat";
    std::ifstream file(cncFile);
    bool aeronRunning = file.good();
    file.close();
    
    if (!aeronRunning) {
        std::cout << "❌ Aeron Media Driver not detected" << std::endl;
        std::cout << "   Expected CnC file: " << cncFile << std::endl;
        std::cout << "💡 To start Aeron Media Driver:" << std::endl;
        std::cout << "   1. Download Aeron: https://github.com/real-logic/aeron/releases" << std::endl;
        std::cout << "   2. Start Media Driver:" << std::endl;
        std::cout << "      java -cp aeron-all-X.X.X.jar io.aeron.driver.MediaDriver" << std::endl;
        std::cout << "   3. Or specify different directory with --aeron-dir" << std::endl;
        return false;
    }
    std::cout << "✅ Aeron Media Driver detected at: " << aeronDir << std::endl;
    
    // Check 2: Warn about cluster requirements
    std::cout << "⚠️  Cluster Requirements:" << std::endl;
    std::cout << "   Make sure Aeron Cluster is running at these endpoints:" << std::endl;
    for (const auto& endpoint : endpoints) {
        std::cout << "   • " << endpoint << std::endl;
    }
    std::cout << std::endl;
    std::cout << "💡 To start a test cluster:" << std::endl;
    std::cout << "   java -cp aeron-all-X.X.X.jar io.aeron.samples.cluster.tutorial.BasicAuctionClusteredService" << std::endl;
    std::cout << std::endl;
    
    return true;
}

// FIXED: Add connection test with timeout
bool testClusterConnectivity(const std::vector<std::string>& endpoints, 
                            const std::string& aeronDir, 
                            int timeoutMs, 
                            bool debugMode) {
    std::cout << "🔌 Testing cluster connectivity..." << std::endl;
    
    try {
        // Create minimal configuration for connectivity test
        auto config = ClusterClientConfigBuilder()
            .withClusterEndpoints(endpoints)
            .withAeronDir(aeronDir)
            .withResponseTimeout(std::chrono::milliseconds(timeoutMs))
            .withMaxRetries(1) // Only one retry for quick test
            .build();
        
        config.debug_logging = debugMode;
        config.enable_console_info = true;
        config.enable_console_warnings = true;
        config.enable_console_errors = true;
        
        // Create client and attempt connection
        ClusterClient testClient(config);
        
        std::cout << "⏱️  Attempting connection (timeout: " << timeoutMs << "ms)..." << std::endl;
        
        bool connected = testClient.connect();
        
        if (connected) {
            std::cout << "✅ Successfully connected to cluster!" << std::endl;
            std::cout << "   Session ID: " << testClient.getSessionId() << std::endl;
            std::cout << "   Leader Member: " << testClient.getLeaderMemberId() << std::endl;
            
            // Disconnect cleanly
            testClient.disconnect();
            return true;
        } else {
            std::cout << "❌ Failed to connect to cluster" << std::endl;
            return false;
        }
        
    } catch (const std::exception& e) {
        std::cout << "❌ Connection test failed with exception: " << e.what() << std::endl;
        return false;
    }
}

int main(int argc, char* argv[]) {
    // Parse command line arguments
    std::vector<std::string> clusterEndpoints = {
        "localhost:9002", "localhost:9102", "localhost:9202"
    };
    std::string aeronDir = "/dev/shm/aeron";
    bool debugMode = false;
    bool checkOnly = false;
    int orderCount = 5;
    int orderInterval = 1000; // milliseconds
    int connectionTimeout = 10000; // milliseconds - FIXED: Add configurable timeout
    
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
        } else if (arg == "--check-only") {
            checkOnly = true;
        } else if (arg == "--orders" && i + 1 < argc) {
            orderCount = std::stoi(argv[++i]);
        } else if (arg == "--interval" && i + 1 < argc) {
            orderInterval = std::stoi(argv[++i]);
        } else if (arg == "--timeout" && i + 1 < argc) {
            connectionTimeout = std::stoi(argv[++i]);
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
    std::cout << "📋 Configuration:" << std::endl;
    std::cout << "   Cluster endpoints: ";
    for (size_t i = 0; i < clusterEndpoints.size(); ++i) {
        std::cout << clusterEndpoints[i];
        if (i < clusterEndpoints.size() - 1) std::cout << ", ";
    }
    std::cout << std::endl;
    std::cout << "   Aeron directory: " << aeronDir << std::endl;
    std::cout << "   Debug mode: " << (debugMode ? "enabled" : "disabled") << std::endl;
    std::cout << "   Connection timeout: " << connectionTimeout << "ms" << std::endl;
    if (!checkOnly) {
        std::cout << "   Orders to send: " << orderCount << std::endl;
        std::cout << "   Order interval: " << orderInterval << "ms" << std::endl;
    }
    std::cout << std::endl;
    
    try {
        // FIXED: Perform pre-flight checks first
        if (!performPreflightChecks(aeronDir, clusterEndpoints)) {
            return 1;
        }
        
        // FIXED: Test connectivity first
        if (!testClusterConnectivity(clusterEndpoints, aeronDir, connectionTimeout, debugMode)) {
            std::cout << std::endl;
            std::cout << "🔧 Troubleshooting Steps:" << std::endl;
            std::cout << "1. Verify Aeron Cluster is running:" << std::endl;
            std::cout << "   java -cp aeron-all-X.X.X.jar io.aeron.samples.cluster.tutorial.BasicAuctionClusteredService" << std::endl;
            std::cout << "2. Check network connectivity:" << std::endl;
            std::cout << "   netstat -tlnp | grep -E \"(9002|9102|9202)\"" << std::endl;
            std::cout << "3. Verify firewall allows UDP traffic on cluster ports" << std::endl;
            std::cout << "4. Check Aeron Media Driver is running and accessible" << std::endl;
            std::cout << "5. Try with --debug flag for more detailed information" << std::endl;
            return 1;
        }
        
        // If check-only mode, exit successfully after connectivity test
        if (checkOnly) {
            std::cout << "🎉 Cluster connectivity check completed successfully!" << std::endl;
            return 0;
        }
        
        // Create client configuration with proper timeout
        auto config = ClusterClientConfigBuilder()
            .withClusterEndpoints(clusterEndpoints)
            .withAeronDir(aeronDir)
            .withResponseTimeout(std::chrono::milliseconds(connectionTimeout))
            .withMaxRetries(3)
            .build();
        
        // Enable debug mode if requested
        config.debug_logging = debugMode;
        config.enable_console_info = true;
        config.enable_console_warnings = true;
        config.enable_console_errors = true;
        // config.response_channel = "aeron:udp?endpoint=10.37.47.181:0"; // Default response channel
        config.response_channel = resolveEgressEndpoint("aeron:udp?endpoint=10.37.47.181:0");
        
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
        
        // Connect to the cluster (we know it works from connectivity test)
        std::cout << "🔌 Connecting to Aeron Cluster for order processing..." << std::endl;
        
        if (!client.connect()) {
            std::cerr << "❌ Failed to connect to cluster for order processing" << std::endl;
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
                failedOrders++;
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
        std::cerr << "• Try running with --check-only flag first" << std::endl;
        std::cerr << "• Use --debug flag for more detailed error information" << std::endl;
        return 1;
    }
}