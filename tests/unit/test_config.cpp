#include <gtest/gtest.h>
#include <aeron_cluster/config.hpp>

using namespace aeron_cluster;

class ConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup test environment
    }
    
    void TearDown() override {
        // Cleanup
    }
};

TEST_F(ConfigTest, DefaultConfigurationIsValid) {
    auto config = ClusterClientConfig::defaultConfig();
    
    EXPECT_FALSE(config.clusterEndpoints().empty());
    EXPECT_GT(config.responseTimeout().count(), 0);
    EXPECT_GE(config.maxRetries(), 0);
    EXPECT_FALSE(config.aeronDir().empty());
}

TEST_F(ConfigTest, BuilderPatternWorks) {
    auto config = ClusterClientConfig::builder()
        .withClusterEndpoints({"localhost:9001", "localhost:9002"})
        .withResponseTimeout(std::chrono::seconds(30))
        .withMaxRetries(5)
        .withLogLevel(LogLevel::DEBUG)
        .build();
    
    EXPECT_EQ(config.clusterEndpoints().size(), 2);
    EXPECT_EQ(config.responseTimeout(), std::chrono::seconds(30));
    EXPECT_EQ(config.maxRetries(), 5);
    EXPECT_EQ(config.logLevel(), LogLevel::DEBUG);
}

TEST_F(ConfigTest, ValidationCatchesInvalidEndpoints) {
    EXPECT_THROW({
        ClusterClientConfig::builder()
            .withClusterEndpoints({"invalid-endpoint"})
            .build();
    }, ConfigurationException);
}

TEST_F(ConfigTest, ValidationCatchesInvalidTimeout) {
    EXPECT_THROW({
        ClusterClientConfig::builder()
            .withResponseTimeout(std::chrono::milliseconds(-1))
            .build();
    }, ConfigurationException);
}

TEST_F(ConfigTest, ValidationCatchesEmptyEndpoints) {
    EXPECT_THROW({
        ClusterClientConfig::builder()
            .withClusterEndpoints({})
            .build();
    }, ConfigurationException);
}

TEST_F(ConfigTest, ImmutabilityAfterConstruction) {
    auto config = ClusterClientConfig::builder()
        .withClusterEndpoints({"localhost:9001"})
        .build();
    
    // Config should be immutable - no setters available
    // This is a compile-time test
    auto endpoints = config.clusterEndpoints();
    EXPECT_EQ(endpoints.size(), 1);
}