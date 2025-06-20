#include <aeron_cluster/cluster_client.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>

using namespace aeron_cluster;

class AdvancedClientDemo {
private:
    std::atomic<int> messagesReceived_{0};
    std::atomic<int> acknowledgementsReceived_{0};
    
public:
    void run() {
        std::cout << "Aeron Cluster Advanced Features Example" << std::endl;
        std::cout << "=======================================" << std::endl;
        
        try {
            // Configure the client with advanced settings
            auto config = ClusterClientConfigBuilder()
                .withClusterEndpoints({"localhost:9002", "localhost:9102", "localhost:9202"})
                .withAeronDir("/dev/shm/aeron")
                .withResponseTimeout(std::chrono::seconds(15))
                .withMaxRetries(5)
                .withRetryDelay(std::chrono::milliseconds(1000))
                .build();
            
            // Enable detailed logging
            config.enable_console_info = true;
            config.enable_console_warnings = true;
            config.debug_logging = true;
            
            ClusterClient client(config);
            
            // Set up message callback for handling responses
            client.setMessageCallback([this](const std::string& messageType, 
                                            const std::string& payload, 
                                            const std::string& headers) {
                messagesReceived_++;
                
                std::cout << "📨 Received message:" << std::endl;
                std::cout << "   Type: " << messageType << std::endl;
                std::cout << "   Payload: " << payload.substr(0, 100);
                if (payload.length() > 100) std::cout << "...";
                std::cout << std::endl;
                
                if (messageType.find("ACK") != std::string::npos || 
                    payload.find("success") != std::string::npos) {
                    acknowledgementsReceived_++;
                }
            });
            
            std::cout << "🔌 Connecting with advanced configuration..." << std::endl;
            
            if (!client.connect()) {
                std::cerr << "❌ Failed to connect to cluster" << std::endl;
                return;
            }
            
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
                client.pollMessages(10);
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
        auto limitOrder = ClusterClient::createSampleLimitOrder("BTC", "USDC", "BUY", 0.1, 50000.0);
        std::string limitOrderId = client.publishOrder(limitOrder);
        std::cout << "   📝 Limit order published: " << limitOrderId.substr(0, 12) << "..." << std::endl;
        
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        client.pollMessages(5);
        
        // Different order with custom metadata
        auto sellOrder = ClusterClient::createSampleLimitOrder("ETH", "USDC", "SELL", 2.0, 3600.0);
        sellOrder.metadata["strategy"] = "momentum";
        sellOrder.metadata["risk_level"] = "medium";
        std::string sellOrderId = client.publishOrder(sellOrder);
        std::cout << "   📝 Sell order with metadata: " << sellOrderId.substr(0, 12) << "..." << std::endl;
        
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        client.pollMessages(5);
        
        std::cout << std::endl;
    }
    
    void demonstrateCustomMessages(ClusterClient& client) {
        std::cout << "🔧 Demonstrating custom messages..." << std::endl;
        
        // Portfolio query
        std::string portfolioQuery = R"({"type":"portfolio_query","account_id":12345})";
        std::string queryId = client.publishMessage("PORTFOLIO_QUERY", portfolioQuery, "{}");
        std::cout << "   💼 Portfolio query sent: " << queryId.substr(0, 12) << "..." << std::endl;
        
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        client.pollMessages(5);
        
        // Market data subscription
        std::string marketDataSub = R"({"symbols":["BTC-USDC","ETH-USDC"],"depth":5})";
        std::string subId = client.publishMessage("MARKET_DATA_SUBSCRIBE", marketDataSub, "{}");
        std::cout << "   📊 Market data subscription: " << subId.substr(0, 12) << "..." << std::endl;
        
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        client.pollMessages(5);
        
        std::cout << std::endl;
    }
    
    void demonstrateStatisticsMonitoring(ClusterClient& client) {
        std::cout << "📈 Demonstrating statistics monitoring..." << std::endl;
        
        auto stats = client.getConnectionStats();
        
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
        
        auto stats = client.getConnectionStats();
        
        std::cout << "📈 Connection Statistics:" << std::endl;
        std::cout << "   Total messages sent: " << stats.messages_sent << std::endl;
        std::cout << "   Total messages received: " << stats.messages_received << std::endl;
        std::cout << "   Messages acknowledged: " << stats.messages_acknowledged << std::endl;
        std::cout << "   Messages failed: " << stats.messages_failed << std::endl;
        
        std::cout << std::endl;
        std::cout << "🎯 Callback Statistics:" << std::endl;
        std::cout << "   Messages received via callback: " << messagesReceived_.load() << std::endl;
        std::cout << "   Acknowledgements received: " << acknowledgementsReceived_.load() << std::endl;
        
        if (stats.messages_sent > 0) {
            double successRate = (double)stats.messages_acknowledged / stats.messages_sent * 100.0;
            std::cout << "   Success rate: " << std::fixed << std::setprecision(1) << successRate << "%" << std::endl;
        }
        
        std::cout << std::endl;
        std::cout << "🏁 Advanced features demonstration completed!" << std::endl;
    }
};

int main() {
    AdvancedClientDemo demo;
    demo.run();
    return 0;
}