#include <aeron_cluster/cluster_client.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <string>
#include <sstream>
#include <random>
#include <regex>
#include <set>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <csignal>
#include <atomic>

using namespace aeron_cluster;

// Global flag for timeout handling
std::atomic<bool> running{true};

void signalHandler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    running = false;
    exit(0);
}

void printUsage() {
    std::cout << "Diagnostic Aeron Cluster Order Publishing Example" << std::endl;
    std::cout << "=================================================" << std::endl;
    std::cout << "\nUsage: ./order_publishing_example [options]" << std::endl;
    std::cout << "\nOptions:" << std::endl;
    std::cout << "  --help              Show this help message" << std::endl;
    std::cout << "  --endpoints LIST    Comma-separated cluster endpoints (default: localhost:9002,localhost:9102,localhost:9202)" << std::endl;
    std::cout << "  --aeron-dir PATH    Aeron media driver directory (default: /dev/shm/aeron)" << std::endl;
    std::cout << "  --debug             Enable debug logging" << std::endl;
    std::cout << "  --orders COUNT      Number of orders to send (default: 3)" << std::endl;
    std::cout << "  --timeout MS        Connection timeout in milliseconds (default: 10000)" << std::endl;
    std::cout << "  --local-response    Use localhost for response channel instead of cluster IP" << std::endl;
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

// Diagnostic function to test basic Aeron connectivity
bool testAeronConnectivity(const std::string& aeronDir) {
    std::cout << "🔍 Testing Aeron Media Driver connectivity..." << std::endl;
    
    try {
        aeron::Context context;
        context.aeronDir(aeronDir);
        
        std::cout << "   • Creating Aeron context with dir: " << aeronDir << std::endl;
        
        auto aeron = aeron::Aeron::connect(context);
        if (aeron) {
            std::cout << "   ✅ Aeron connection successful" << std::endl;
            return true;
        } else {
            std::cout << "   ❌ Aeron connection failed" << std::endl;
            return false;
        }
    } catch (const std::exception& e) {
        std::cout << "   ❌ Aeron connection exception: " << e.what() << std::endl;
        return false;
    }
}

int main(int argc, char* argv[]) {
    // Setup signal handlers
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    
    // Parse command line arguments
    std::vector<std::string> clusterEndpoints = {
        "localhost:9002", "localhost:9102", "localhost:9202"
    };
    std::string aeronDir = "/dev/shm/aeron";
    bool debugMode = false;
    bool useLocalhost = false;
    int orderCount = 3;
    int connectionTimeout = 10000;
    
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
        } else if (arg == "--local-response") {
            useLocalhost = true;
        } else if (arg == "--orders" && i + 1 < argc) {
            orderCount = std::stoi(argv[++i]);
        } else if (arg == "--timeout" && i + 1 < argc) {
            connectionTimeout = std::stoi(argv[++i]);
        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            printUsage();
            return 1;
        }
    }
    
    std::cout << "Diagnostic Aeron Cluster Order Publishing Example" << std::endl;
    std::cout << "=================================================" << std::endl;
    std::cout << "📋 Configuration:" << std::endl;
    std::cout << "   Cluster endpoints: ";
    for (size_t i = 0; i < clusterEndpoints.size(); ++i) {
        std::cout << clusterEndpoints[i];
        if (i < clusterEndpoints.size() - 1) std::cout << ", ";
    }
    std::cout << std::endl;
    std::cout << "   Aeron directory: " << aeronDir << std::endl;
    std::cout << "   Debug mode: " << (debugMode ? "enabled" : "disabled") << std::endl;
    std::cout << "   Use localhost for response: " << (useLocalhost ? "yes" : "no") << std::endl;
    std::cout << "   Orders to send: " << orderCount << std::endl;
    std::cout << "   Connection timeout: " << connectionTimeout << "ms" << std::endl;
    std::cout << std::endl;
    
    try {
        // Step 1: Test basic Aeron connectivity
        if (!testAeronConnectivity(aeronDir)) {
            std::cerr << "❌ Cannot proceed without Aeron Media Driver" << std::endl;
            return 1;
        }
        
        // Step 2: Configure response channel
        std::cout << "🔧 Configuring response channel..." << std::endl;
        std::string baseChannel = useLocalhost ? "aeron:udp?endpoint=localhost:0" : "aeron:udp?endpoint=localhost:0";
        // std::string responseChannel = resolveEgressEndpoint(baseChannel);
        // std::cout << "   Response channel: " << responseChannel << std::endl;
        std::cout << "   ℹ️  Using localhost for response channel (your machine must listen locally)" << std::endl;
        
        // Step 3: Create client configuration
        std::cout << "🏗️  Building client configuration..." << std::endl;
        auto config = ClusterClientConfigBuilder()
            .withClusterEndpoints(clusterEndpoints)
            .withAeronDir(aeronDir)
            .withResponseTimeout(std::chrono::milliseconds(connectionTimeout))
            .withMaxRetries(3)
            .build();
        
        // Configure logging
        config.enable_console_info = true;
        config.enable_console_warnings = true;
        config.enable_console_errors = true;
        config.debug_logging = debugMode;
        config.response_channel = config.resolve_egress_endpoint(baseChannel);

        std::cout << "   Response channel: " << config.response_channel << std::endl;

        std::cout << "   ✅ Configuration built successfully" << std::endl;
        
        // Step 4: Create client with timeout
        std::cout << "📡 Creating cluster client..." << std::endl;
        std::cout.flush(); // Force output
        
        std::unique_ptr<ClusterClient> client;
        std::atomic<bool> clientCreated{false};
        std::atomic<bool> clientCreateFailed{false};
        std::string createError;
        
        // Create client in separate thread with timeout
        std::thread clientThread([&]() {
            try {
                std::cout << "   • Initializing ClusterClient constructor..." << std::endl;
                client = std::make_unique<ClusterClient>(config);
                std::cout << "   ✅ ClusterClient created successfully" << std::endl;
                clientCreated = true;
            } catch (const std::exception& e) {
                createError = e.what();
                clientCreateFailed = true;
                std::cout << "   ❌ ClusterClient creation failed: " << e.what() << std::endl;
            }
        });
        
        // Wait for client creation with timeout
        auto createStart = std::chrono::steady_clock::now();
        auto createTimeout = std::chrono::seconds(5);
        
        while (!clientCreated && !clientCreateFailed && running) {
            if ((std::chrono::steady_clock::now() - createStart) > createTimeout) {
                std::cout << "   ⏰ Client creation timeout (5 seconds)" << std::endl;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        if (clientThread.joinable()) {
            if (!clientCreated && !clientCreateFailed) {
                std::cout << "   ⚠️  Detaching stuck client creation thread" << std::endl;
                clientThread.detach();
                std::cerr << "❌ Client creation timed out" << std::endl;
                return 1;
            } else {
                clientThread.join();
            }
        }
        
        if (clientCreateFailed) {
            std::cerr << "❌ Failed to create client: " << createError << std::endl;
            return 1;
        }
        
        if (!client) {
            std::cerr << "❌ Client creation failed for unknown reason" << std::endl;
            return 1;
        }
        
        // Step 5: Set up message callback
        std::cout << "📨 Setting up message callback..." << std::endl;
        int acknowledgedOrders = 0;
        int failedOrders = 0;
        
        client->setMessageCallback([&](const std::string& messageType, 
                                      const std::string& payload, 
                                      const std::string& headers) {
            std::cout << "📨 Received message:" << std::endl;
            std::cout << "   Type: " << messageType << std::endl;
            std::cout << "   Payload: " << payload.substr(0, 100) << (payload.length() > 100 ? "..." : "") << std::endl;
            
            if (messageType.find("ACK") != std::string::npos || 
                messageType.find("ACKNOWLEDGMENT") != std::string::npos) {
                
                if (payload.find("\"success\":true") != std::string::npos ||
                    payload.find("\"status\":\"success\"") != std::string::npos) {
                    acknowledgedOrders++;
                    std::cout << "✅ Order acknowledged (" << acknowledgedOrders << "/" << orderCount << ")" << std::endl;
                } else {
                    failedOrders++;
                    std::cout << "❌ Order failed (" << failedOrders << " total failures)" << std::endl;
                }
            }
            std::cout << std::endl;
        });
        
        // Step 6: Connect with timeout
        std::cout << "🔌 Connecting to cluster..." << std::endl;
        std::cout.flush();
        
        std::atomic<bool> connectionComplete{false};
        std::atomic<bool> connectionSuccess{false};
        
        std::thread connectThread([&]() {
            try {
                std::cout << "   • Starting connection process..." << std::endl;
                bool result = client->connect();
                connectionSuccess = result;
                connectionComplete = true;
                std::cout << "   • Connection process completed: " << (result ? "success" : "failed") << std::endl;
            } catch (const std::exception& e) {
                std::cout << "   ❌ Connection exception: " << e.what() << std::endl;
                connectionComplete = true;
            }
        });
        
        // Wait for connection with timeout
        auto connectStart = std::chrono::steady_clock::now();
        auto connectTimeout = std::chrono::milliseconds(connectionTimeout);
        
        while (!connectionComplete && running) {
            if ((std::chrono::steady_clock::now() - connectStart) > connectTimeout) {
                std::cout << "   ⏰ Connection timeout (" << connectionTimeout << "ms)" << std::endl;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        if (connectThread.joinable()) {
            if (!connectionComplete) {
                std::cout << "   ⚠️  Detaching stuck connection thread" << std::endl;
                connectThread.detach();
                std::cerr << "❌ Connection process timed out" << std::endl;
                return 1;
            } else {
                connectThread.join();
            }
        }
        
        if (!connectionSuccess) {
            std::cerr << "❌ Failed to connect to cluster" << std::endl;
            std::cerr << "Please ensure:" << std::endl;
            std::cerr << "  • Aeron Cluster is running at the specified endpoints" << std::endl;
            for (const auto& endpoint : clusterEndpoints) {
                std::cerr << "    - " << endpoint << std::endl;
            }
            std::cerr << "  • Network connectivity allows UDP traffic" << std::endl;
            std::cerr << "  • Try with --local-response flag if response channel is the issue" << std::endl;
            return 1;
        }
        
        std::cout << "✅ Connected successfully!" << std::endl;
        std::cout << "   Session ID: " << client->getSessionId() << std::endl;
        std::cout << "   Leader Member: " << client->getLeaderMemberId() << std::endl;
        std::cout << std::endl;
        
        // Rest of the order publishing logic...
        std::cout << "📤 Publishing " << orderCount << " example orders..." << std::endl;
        
        for (int i = 1; i <= orderCount && running; ++i) {
            std::string side = (i % 2 == 1) ? "BUY" : "SELL";
            double quantity = 1.0 + i * 0.5;
            double price = 3500.0 + i * 25.0;
            
            auto order = ClusterClient::createSampleLimitOrder(
                "ETH", "USDC", side, quantity, price);
            
            order.accountID = 10000 + i;
            order.customerID = 50000 + i;
            
            try {
                std::string messageId = client->publishOrder(order);
                
                std::cout << "   Order " << i << "/" << orderCount << ": " << side << " " << quantity 
                         << " ETH @ " << price << " USDC" << std::endl;
                std::cout << "   Message ID: " << messageId.substr(0, 12) << "..." << std::endl;
                
                // int messages = client->pollMessages(5);
                if (messages > 0) {
                    std::cout << "   📨 Received " << messages << " immediate response(s)" << std::endl;
                }
                std::cout << std::endl;
                
                if (i < orderCount) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
                
            } catch (const std::exception& e) {
                std::cerr << "❌ Failed to publish order " << i << ": " << e.what() << std::endl;
                failedOrders++;
            }
        }
        
        std::cout << "⏳ Waiting for responses..." << std::endl;
        
        // Poll for responses
        auto waitStart = std::chrono::steady_clock::now();
        auto waitTimeout = std::chrono::seconds(10);

        std::this_thread::sleep_for(std::chrono::milliseconds(10000));
        
        // Final statistics
        auto stats = client->getConnectionStats();
        std::cout << std::endl;
        std::cout << "📊 Final Statistics:" << std::endl;
        std::cout << "   Orders published: " << orderCount << std::endl;
        std::cout << "   Orders acknowledged: " << acknowledgedOrders << std::endl;
        std::cout << "   Orders failed: " << failedOrders << std::endl;
        std::cout << "   Messages sent: " << stats.messages_sent << std::endl;
        std::cout << "   Messages received: " << stats.messages_received << std::endl;
        
        std::cout << std::endl;
        std::cout << "🏁 Order publishing example completed!" << std::endl;
        
        client->disconnect();
        
    } catch (const std::exception& e) {
        std::cerr << "💥 Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}