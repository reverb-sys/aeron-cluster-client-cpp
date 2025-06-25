#include <gtest/gtest.h>
#include <aeron_cluster/cluster_client.hpp>
#include <thread>
#include <chrono>

using namespace aeron_cluster;

class EndToEndTest : public ::testing::Test {
protected:
    void SetUp() override {
        // This test requires a running Aeron cluster
        // In a real environment, you'd start a test cluster here
        config_ = ClusterClientConfig::builder()
            .withClusterEndpoints({"localhost:9002"})
            .withResponseTimeout(std::chrono::seconds(5))
            .withLogLevel(LogLevel::DEBUG)
            .build();
    }
    
    ClusterClientConfig config_;
};

TEST_F(EndToEndTest, DISABLED_ConnectionAndOrderPublishing) {
    // This test is disabled by default as it requires infrastructure
    // Enable when you have a test cluster running
    
    ClusterClient client(config_);
    
    bool connected = false;
    ASSERT_NO_THROW({
        connected = client.connect();
    });
    
    if (!connected) {
        GTEST_SKIP() << "Could not connect to test cluster";
    }
    
    EXPECT_TRUE(client.isConnected());
    EXPECT_TRUE(client.getSessionId().has_value());
    
    // Test order publishing
    auto order = ClusterClient::createSampleLimitOrder("ETH", "USDC", "BUY", 1.0, 3500.0);
    
    std::string messageId;
    ASSERT_NO_THROW({
        messageId = client.publishOrder(order);
    });
    
    EXPECT_FALSE(messageId.empty());
    
    // Wait for acknowledgment
    auto result = client.waitForAcknowledgment(messageId, std::chrono::seconds(5));
    EXPECT_EQ(result, MessageResult::SUCCESS);
    
    client.disconnect();
    EXPECT_FALSE(client.isConnected());
}

TEST_F(EndToEndTest, DISABLED_ReconnectionHandling) {
    ClusterClient client(config_);
    client.setAutoReconnect(true, 3, 1.5);
    
    // Enable connection state monitoring
    ConnectionState lastState = ConnectionState::DISCONNECTED;
    client.onConnectionStateChange([&lastState](ConnectionState old, ConnectionState new_state) {
        lastState = new_state;
    });
    
    bool connected = client.connect();
    if (!connected) {
        GTEST_SKIP() << "Could not connect to test cluster";
    }
    
    EXPECT_EQ(lastState, ConnectionState::CONNECTED);
    
    // Simulate network interruption by forcing disconnect
    client.forceDisconnect();
    
    // Wait for reconnection attempt
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // Should automatically reconnect
    EXPECT_TRUE(client.isConnected());
    EXPECT_EQ(lastState, ConnectionState::CONNECTED);
}