#include <aeron_cluster/cluster_client.hpp>
#include <atomic>
#include <chrono>
#include <iomanip>
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

class AdvancedClientDemo {
   private:
    std::atomic<int> messagesReceived_{0};
    std::atomic<int> acknowledgementsReceived_{0};

   public:
    void run(int argc, char* argv[]) {
        std::cout << "Aeron Cluster Advanced Features Example" << std::endl;
        std::cout << "=======================================" << std::endl;

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
                    return ;
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
                    return;
                }
            }

            if (!performPreflightChecks(aeronDir, clusterEndpoints)) {
                return;
            }

            // Create client configuration with proper timeout
            auto config = ClusterClientConfigBuilder()
                              .with_cluster_endpoints(clusterEndpoints)
                              .with_aeron_dir(aeronDir)
                              .with_response_timeout(std::chrono::milliseconds(connectionTimeout))
                              .with_max_retries(3)
                              .with_retry_delay(std::chrono::milliseconds(1000))
                              .build();

            // Enable debug mode if requested
            config.debug_logging = debugMode;
            config.enable_console_info = true;
            config.enable_console_warnings = true;
            config.enable_console_errors = true;

            ClusterClient client(config);

            // Set up message callback for handling responses
            client.set_message_callback([this](const aeron_cluster::ParseResult& result) {
                messagesReceived_++;

                std::cout << "📨 Received message:" << std::endl;
                std::cout << "   Type: " << result.message_type << std::endl;
                std::cout << "   Payload: " << result.payload.substr(0, 100);
                if (result.payload.length() > 100)
                    std::cout << "...";
                std::cout << std::endl;

                if (result.message_type.find("ACK") != std::string::npos ||
                    result.payload.find("success") != std::string::npos) {
                    acknowledgementsReceived_++;
                }
            });

            std::cout << "🔌 Connecting with advanced configuration..." << std::endl;

            if (!client.connect()) {
                std::cerr << "❌ Failed to connect to cluster" << std::endl;
                return;
            }
            client.stop_polling();

            std::cout << "✅ Connected with advanced features!" << std::endl;
            std::cout << std::endl;

            // Demonstrate different order types
            demonstrateOrderTypes(client);

            // Demonstrate custom messages
            demonstrateCustomMessages(client);

            // Demonstrate statistics monitoring
            demonstrateStatisticsMonitoring(client);

            // Wait for final responses
            std::cout << "⏳ Waiting for final responses..." << std::endl;

            for (int i = 0; i < 20; ++i) {
                client.poll_messages(10);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }

            // Final report
            printFinalReport(client);

            client.disconnect();

        } catch (const std::exception& e) {
            std::cerr << "💥 Error: " << e.what() << std::endl;
        }
    }

   private:
    void demonstrateOrderTypes(ClusterClient& client) {
        std::cout << "🎯 Demonstrating different order types..." << std::endl;

        // Limit order
        auto limitOrder =
            ClusterClient::create_sample_limit_order("BTC", "USDC", "BUY", 0.1, 50000.0);
        std::string limitOrderId = client.publish_order(limitOrder);
        std::cout << "   📝 Limit order published: " << limitOrderId.substr(0, 12) << "..."
                  << std::endl;

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        client.poll_messages(5);

        // Different order with custom metadata
        auto sellOrder =
            ClusterClient::create_sample_limit_order("ETH", "USDC", "SELL", 2.0, 3600.0);
        sellOrder.metadata["strategy"] = "momentum";
        sellOrder.metadata["risk_level"] = "medium";
        std::string sellOrderId = client.publish_order(sellOrder);
        std::cout << "   📝 Sell order with metadata: " << sellOrderId.substr(0, 12) << "..."
                  << std::endl;

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        client.poll_messages(5);

        std::cout << std::endl;
    }

    void demonstrateCustomMessages(ClusterClient& client) {
        std::cout << "🔧 Demonstrating custom messages..." << std::endl;

        // Portfolio query
        std::string portfolioQuery = R"({"type":"portfolio_query","account_id":12345})";
        std::string queryId = client.publish_message("PORTFOLIO_QUERY", portfolioQuery, "{}");
        std::cout << "   💼 Portfolio query sent: " << queryId.substr(0, 12) << "..." << std::endl;

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        client.poll_messages(5);

        // Market data subscription
        std::string marketDataSub = R"({"symbols":["BTC-USDC","ETH-USDC"],"depth":5})";
        std::string subId = client.publish_message("MARKET_DATA_SUBSCRIBE", marketDataSub, "{}");
        std::cout << "   📊 Market data subscription: " << subId.substr(0, 12) << "..."
                  << std::endl;

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        client.poll_messages(5);

        std::cout << std::endl;
    }

    void demonstrateStatisticsMonitoring(ClusterClient& client) {
        std::cout << "📈 Demonstrating statistics monitoring..." << std::endl;

        auto stats = client.get_connection_stats();

        std::cout << "   Current connection statistics:" << std::endl;
        std::cout << "     Messages sent: " << stats.messages_sent << std::endl;
        std::cout << "     Messages received: " << stats.messages_received << std::endl;
        std::cout << "     Current session ID: " << stats.current_session_id << std::endl;
        std::cout << "     Current leader: " << stats.current_leader_id << std::endl;
        std::cout << "     Connection attempts: " << stats.connection_attempts << std::endl;
        std::cout << "     Successful connections: " << stats.successful_connections << std::endl;
        std::cout << "     Leader redirects: " << stats.leader_redirects << std::endl;

        std::cout << std::endl;
    }

    void printFinalReport(ClusterClient& client) {
        std::cout << "📊 Final Report:" << std::endl;
        std::cout << "================" << std::endl;

        auto stats = client.get_connection_stats();

        std::cout << "📈 Connection Statistics:" << std::endl;
        std::cout << "   Total messages sent: " << stats.messages_sent << std::endl;
        std::cout << "   Total messages received: " << stats.messages_received << std::endl;
        std::cout << "   Messages acknowledged: " << stats.messages_acknowledged << std::endl;
        std::cout << "   Messages failed: " << stats.messages_failed << std::endl;

        std::cout << std::endl;
        std::cout << "🎯 Callback Statistics:" << std::endl;
        std::cout << "   Messages received via callback: " << messagesReceived_.load() << std::endl;
        std::cout << "   Acknowledgements received: " << acknowledgementsReceived_.load()
                  << std::endl;

        if (stats.messages_sent > 0) {
            double successRate = (double)stats.messages_acknowledged / stats.messages_sent * 100.0;
            std::cout << "   Success rate: " << std::fixed << std::setprecision(1) << successRate
                      << "%" << std::endl;
        }

        std::cout << std::endl;
        std::cout << "🏁 Advanced features demonstration completed!" << std::endl;
    }
};

int main(int argc, char* argv[]) {
    AdvancedClientDemo demo;
    demo.run(argc, argv);
    return 0;
}