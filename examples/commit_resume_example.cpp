#include "aeron_cluster/cluster_client.hpp"
#include "aeron_cluster/commit_manager.hpp"
#include <iostream>
#include <thread>
#include <chrono>

using namespace aeron_cluster;

int main() {
    std::cout << "Aeron Cluster Client - Commit/Resume Example" << std::endl;
    std::cout << "=============================================" << std::endl;

    // Create client configuration
    auto config = ClusterClientConfigBuilder()
        .with_cluster_endpoints({"localhost:9000"})
        .with_response_channel("aeron:udp?endpoint=localhost:9001")
        .with_aeron_dir("/tmp/aeron")
        .with_debug_logging(true)
        .with_client_id("commit_example_client")
        .build();

    // Create client
    ClusterClient client(config);

    // Set up message callback
    client.set_message_callback([](const ParseResult& result) {
        std::cout << "Received message: " << result.message_type 
                  << " (ID: " << result.message_id 
                  << ", Seq: " << result.sequence_number << ")" << std::endl;
        
        // The message is automatically committed by the client with the sequence number
        std::cout << "Message automatically committed with sequence: " << result.sequence_number << std::endl;
    });

    // Connect to cluster
    std::cout << "Connecting to cluster..." << std::endl;
    if (!client.connect()) {
        std::cerr << "Failed to connect to cluster" << std::endl;
        return 1;
    }
    std::cout << "Connected successfully!" << std::endl;
    client.stop_polling();

    // Subscribe to a topic
    std::string topic = "orders";
    std::cout << "Subscribing to topic: " << topic << std::endl;
    if (!client.subscribe_topic(topic, "LATEST")) {
        std::cerr << "Failed to subscribe to topic" << std::endl;
        return 1;
    }

    // Simulate receiving some messages
    std::cout << "Polling for messages for 10 seconds..." << std::endl;
    for (int i = 0; i < 10; ++i) {
        int processed = client.poll_messages(10);
        if (processed > 0) {
            std::cout << "Processed " << processed << " messages" << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Demonstrate commit functionality
    std::cout << "\n--- Commit Functionality Demo ---" << std::endl;
    
    // Manually commit a message with message identifier
    std::string message_id = "test_msg_123";
    std::string message_identifier = "sub_123";
    std::uint64_t timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::uint64_t sequence = timestamp; // Use timestamp as sequence number
    
    client.commit_message(topic, message_identifier, message_id, timestamp, sequence);
    std::cout << "Manually committed message: " << message_id 
              << " with identifier: " << message_identifier
              << " and sequence: " << sequence << std::endl;

    // Get last commit for specific message identifier
    auto last_commit = client.get_last_commit(topic, message_identifier);
    if (last_commit) {
        std::cout << "Last commit for topic '" << topic << "' with identifier '" << message_identifier << "':" << std::endl;
        std::cout << "  Message ID: " << last_commit->message_id << std::endl;
        std::cout << "  Message Identifier: " << last_commit->message_identifier << std::endl;
        std::cout << "  Timestamp: " << last_commit->timestamp_nanos << std::endl;
        std::cout << "  Sequence: " << last_commit->sequence_number << std::endl;
    }

    // Demonstrate unsubscribe
    std::cout << "\n--- Unsubscribe Demo ---" << std::endl;
    std::cout << "Unsubscribing from topic: " << topic << std::endl;
    if (client.unsubscribe_topic(topic)) {
        std::cout << "Successfully unsubscribed from topic" << std::endl;
    } else {
        std::cerr << "Failed to unsubscribe from topic" << std::endl;
    }

    // Demonstrate resume functionality
    std::cout << "\n--- Resume Demo ---" << std::endl;
    std::cout << "Resuming subscription from last commit for topic: " << topic 
              << " with identifier: " << message_identifier << std::endl;
    if (client.resume_from_last_commit(topic, message_identifier)) {
        std::cout << "Successfully resumed from last commit" << std::endl;
    } else {
        std::cout << "No previous commit found, subscribing from latest" << std::endl;
    }

    // Send commit request
    std::cout << "Sending commit request for topic: " << topic << std::endl;
    if (client.send_commit_request(topic)) {
        std::cout << "Commit request sent successfully" << std::endl;
    } else {
        std::cerr << "Failed to send commit request" << std::endl;
    }

    // Clean up
    std::cout << "\nDisconnecting..." << std::endl;
    client.disconnect();
    std::cout << "Disconnected successfully" << std::endl;

    return 0;
}
