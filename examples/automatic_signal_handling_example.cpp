/**
 * @file automatic_signal_handling_example.cpp
 * @brief Example demonstrating automatic signal handling in Aeron Cluster C++ client
 * 
 * This example shows how the library automatically handles SIGINT and SIGTERM
 * signals to ensure graceful disconnection when the application is terminated
 * abruptly (e.g., Ctrl+C).
 * 
 * Key features demonstrated:
 * - Automatic signal handler registration on connect()
 * - Graceful disconnection on SIGINT/SIGTERM
 * - No manual signal handling code required
 * - Cluster detects disconnection as CLIENT_ACTION instead of TIMEOUT
 */

 #include <iostream>
 #include <memory>
 #include <thread>
 #include <chrono>
 
 #include "aeron_cluster/cluster_client.hpp"
 #include "aeron_cluster/config.hpp"
 #include "aeron_cluster/logging.hpp"
 
 int main() {
     // Setup logging
     auto logger = aeron_cluster::LoggerFactory::instance().getLogger("AutoSignalExample");
     logger->setLevel(aeron_cluster::LogLevel::INFO);
 
     std::cout << "=== Aeron Cluster C++ Automatic Signal Handling Example ===" << std::endl;
     std::cout << "This example demonstrates automatic graceful disconnection on signals." << std::endl;
     std::cout << "No manual signal handling code is required!" << std::endl;
     std::cout << std::endl;
 
     // Configure client
     auto config = aeron_cluster::ClusterClientConfigBuilder()
         .with_cluster_endpoints({"localhost:9002", "localhost:9102", "localhost:9202"})
         .with_aeron_dir("/dev/shm/aeron-ubuntu/")
         .with_debug_logging(true)
         .with_keepalive_enabled(true)
         .with_keepalive_interval(std::chrono::milliseconds(1000))
         .with_application_name("auto_signal_example")
         .build();
     
     // Create client - signal handling will be automatically enabled on connect()
     auto client = std::make_shared<aeron_cluster::ClusterClient>(config);
     
     std::cout << "Connecting to cluster..." << std::endl;
     if (client->connect()) {
         std::cout << "✅ Connected successfully!" << std::endl;
         std::cout << "   Session ID: " << client->get_session_id() << std::endl;
         std::cout << "   Leader Member: " << client->get_leader_member_id() << std::endl;
         std::cout << std::endl;
         
         std::cout << "🔧 Automatic signal handling is now active!" << std::endl;
         std::cout << "   - SIGINT (Ctrl+C) and SIGTERM signals are handled automatically" << std::endl;
         std::cout << "   - SessionCloseRequest will be sent to cluster on termination" << std::endl;
         std::cout << "   - Cluster will detect disconnection as CLIENT_ACTION" << std::endl;
         std::cout << std::endl;
         
         std::cout << "Press Ctrl+C to test automatic graceful disconnect..." << std::endl;
         std::cout << "The cluster will see this as CLIENT_ACTION, not TIMEOUT!" << std::endl;
         std::cout << std::endl;
         
         // Keep the client alive - keepalives are handled automatically
         int counter = 0;
         while (true) {
             std::this_thread::sleep_for(std::chrono::milliseconds(1000));
             counter++;
             
             if (counter % 10 == 0) {
                 std::cout << "⏰ Client running for " << counter << " seconds..." << std::endl;
             }
         }
     } else {
         std::cerr << "❌ Failed to connect to cluster" << std::endl;
         return 1;
     }
     
     return 0;
 }
 