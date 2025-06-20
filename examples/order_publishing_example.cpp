#include <aeron_cluster/cluster_client.hpp>
#include <iostream>
#include <thread>
#include <chrono>

using namespace aeron_cluster;

int main() {
    std::cout << "Aeron Cluster Order Publishing Example" << std::endl;
    std::cout << "======================================" << std::endl;
    
    try {
        // Configure the client
        auto config = ClusterClientConfigBuilder()
            .withClusterEndpoints({"localhost:9002", "localhost:9102", "localhost:9202"})
            .withAeronDir("/dev/shm/aeron")
            .build();
        
        // Enable logging
        config.enable_console_info = true;
        config.debug_logging = false;
        
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
        std::cout << "   Session ID: " << client.getSessionId() << std::endl;
        std::cout << "   Leader Member: " << client.getLeaderMemberId() << std::endl;
        std::cout << std::endl;
        
        // Publish some example orders
        std::cout << "📤 Publishing example orders..." << std::endl;
        
        for (int i = 1; i <= 3; ++i) {
            // Create a sample order
            std::string side = (i % 2 == 1) ? "BUY" : "SELL";
            double quantity = 1.0 + i * 0.5;
            double price = 3500.0 + i * 25.0;
            
            auto order = ClusterClient::createSampleLimitOrder(
                "ETH", "USDC", side, quantity, price);
            
            // Publish the order
            std::string messageId = client.publishOrder(order);
            
            std::cout << "   Order " << i << ": " << side << " " << quantity 
                     << " ETH @ " << price << " USDC" << std::endl;
            std::cout << "   Message ID: " << messageId.substr(0, 12) << "..." << std::endl;
            
            // Wait a bit between orders
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            
            // Poll for any responses
            int messages = client.pollMessages(5);
            if (messages > 0) {
                std::cout << "   📨 Received " << messages << " response(s)" << std::endl;
            }
            
            std::cout << std::endl;
        }
        
        std::cout << "⏳ Waiting for final responses..." << std::endl;
        
        // Poll for responses for a few more seconds
        for (int i = 0; i < 10; ++i) {
            int messages = client.pollMessages(10);
            if (messages > 0) {
                std::cout << "📨 Received " << messages << " additional response(s)" << std::endl;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        
        // Show final statistics
        auto stats = client.getConnectionStats();
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