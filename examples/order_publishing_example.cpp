#include <aeron_cluster/cluster_client.hpp>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include <csignal>
#include <cstring>
#include <fstream>
#include <sstream>

using namespace aeron_cluster;

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

bool performPreflightChecks(const std::string& aeronDir, const std::vector<std::string>& endpoints) {
    std::cout << "🔍 Performing pre-flight checks..." << std::endl;
    
    // Check 1: Aeron directory and Media Driver
    std::string cncFile = aeronDir + "/cnc.dat";
    std::ifstream file(cncFile);
    bool aeronRunning = file.good();
    file.close();
    
    if (!aeronRunning) {
        std::cout << "❌ Aeron Media Driver not detected" << std::endl;
        return false;
    }
    std::cout << "✅ Aeron Media Driver detected at: " << aeronDir << std::endl;
    
    // Check 2: Warn about cluster requirements
    std::cout << "⚠️  Cluster Requirements:" << std::endl;
    std::cout << "   Make sure Aeron Cluster is running at these endpoints:" << std::endl;
    for (const auto& endpoint : endpoints) {
        std::cout << "   • " << endpoint << std::endl;
    }
    
    return true;
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
    std::cout << "Aeron Cluster Order Publishing Example" << std::endl;
    std::cout << "======================================" << std::endl;

    try {
        std::vector<std::string> clusterEndpoints = {"localhost:9002", "localhost:9102",
                                                     "localhost:9202"};
        std::string aeronDir = "/dev/shm/aeron";
        bool debugMode = false;
        bool checkOnly = false;
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

        if (!performPreflightChecks(aeronDir, clusterEndpoints)) {
            return 1;
        }
        
        // Create client configuration with proper timeout
        auto config = ClusterClientConfigBuilder()
            .with_cluster_endpoints(clusterEndpoints)
            .with_aeron_dir(aeronDir)
            .with_response_timeout(std::chrono::milliseconds(connectionTimeout))
            .with_max_retries(3)
            .build();
        
        // Enable debug mode if requested
        config.debug_logging = debugMode;
        config.enable_console_info = true;
        config.enable_console_warnings = true;
        config.enable_console_errors = true;

        ClusterClient client(config);

        std::cout << "🔌 Connecting to cluster..." << std::endl;

        if (!client.connect()) {
            std::cerr << "❌ Failed to connect to cluster" << std::endl;
            std::cerr << "Please ensure:" << std::endl;
            std::cerr << "  • Aeron Media Driver is running" << std::endl;
            std::cerr << "  • Cluster is accessible at the specified endpoints" << std::endl;
            return 1;
        }

        std::cout << "✅ Connected successfully!" << std::endl;
        std::cout << "   Session ID: " << client.get_session_id() << std::endl;
        std::cout << "   Leader Member: " << client.get_leader_member_id() << std::endl;
        std::cout << std::endl;

        // Publish some example orders
        std::cout << "📤 Publishing example orders..." << std::endl;

        for (int i = 1; i <= 3; ++i) {
            // Create a sample order
            std::string side = (i % 2 == 1) ? "BUY" : "SELL";
            double quantity = 1.0 + i * 0.5;
            double price = 3500.0 + i * 25.0;

            auto order =
                ClusterClient::create_sample_limit_order("ETH", "USDC", side, quantity, price);

            // Publish the order
            std::string messageId = client.publish_order(order);

            std::cout << "   Order " << i << ": " << side << " " << quantity << " ETH @ " << price
                      << " USDC" << std::endl;
            std::cout << "   Message ID: " << messageId.substr(0, 12) << "..." << std::endl;

            // Wait a bit between orders
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            // Poll for any responses
            int messages = client.poll_messages(5);
            if (messages > 0) {
                std::cout << "   📨 Received " << messages << " response(s)" << std::endl;
            }

            std::cout << std::endl;
        }

        std::cout << "⏳ Waiting for final responses..." << std::endl;

        // Poll for responses for a few more seconds
        for (int i = 0; i < 10; ++i) {
            int messages = client.poll_messages(10);
            if (messages > 0) {
                std::cout << "📨 Received " << messages << " additional response(s)" << std::endl;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        // Show final statistics
        auto stats = client.get_connection_stats();
        std::cout << std::endl;
        std::cout << "📊 Final Statistics:" << std::endl;
        std::cout << "   Messages sent: " << stats.messages_sent << std::endl;
        std::cout << "   Messages received: " << stats.messages_received << std::endl;
        std::cout << "   Connection attempts: " << stats.connection_attempts << std::endl;

        std::cout << std::endl;
        std::cout << "🏁 Order publishing example completed!" << std::endl;

        client.disconnect();

    } catch (const std::exception& e) {
        std::cerr << "💥 Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}