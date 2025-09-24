#include "aeron_cluster/cluster_client.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <chrono>

using namespace aeron_cluster;

class CommitResumeTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto config = ClusterClientConfigBuilder()
            .with_cluster_endpoints({"localhost:9000"})
            .with_response_channel("aeron:udp?endpoint=localhost:9001")
            .with_aeron_dir("/tmp/aeron_test")
            .with_client_id("test_client")
            .build();
        
        client_ = std::make_unique<ClusterClient>(config);
    }
    
    void TearDown() override {
        if (client_) {
            client_->disconnect();
        }
    }
    
    std::unique_ptr<ClusterClient> client_;
};

TEST_F(CommitResumeTest, TestCommitMessage) {
    // Test manual commit
    std::string topic = "test_topic";
    std::string message_id = "test_msg_123";
    std::uint64_t timestamp = 1234567890;
    std::uint64_t sequence = 1;
    
    client_->commit_message(topic, message_id, timestamp, sequence);
    
    // Verify commit was stored
    auto last_commit = client_->get_last_commit(topic);
    ASSERT_NE(last_commit, nullptr);
    EXPECT_EQ(last_commit->topic, topic);
    EXPECT_EQ(last_commit->message_id, message_id);
    EXPECT_EQ(last_commit->timestamp_nanos, timestamp);
    EXPECT_EQ(last_commit->sequence_number, sequence);
}

TEST_F(CommitResumeTest, TestNoCommitInitially) {
    // Test that no commit exists initially
    auto last_commit = client_->get_last_commit("nonexistent_topic");
    EXPECT_EQ(last_commit, nullptr);
}

TEST_F(CommitResumeTest, TestMultipleCommits) {
    std::string topic = "test_topic";
    
    // Commit first message
    client_->commit_message(topic, "msg1", 1000, 1);
    
    // Commit second message
    client_->commit_message(topic, "msg2", 2000, 2);
    
    // Verify last commit is the second message
    auto last_commit = client_->get_last_commit(topic);
    ASSERT_NE(last_commit, nullptr);
    EXPECT_EQ(last_commit->message_id, "msg2");
    EXPECT_EQ(last_commit->timestamp_nanos, 2000);
    EXPECT_EQ(last_commit->sequence_number, 2);
}

TEST_F(CommitResumeTest, TestDifferentTopics) {
    // Test commits for different topics
    client_->commit_message("topic1", "msg1", 1000, 1);
    client_->commit_message("topic2", "msg2", 2000, 2);
    
    auto commit1 = client_->get_last_commit("topic1");
    auto commit2 = client_->get_last_commit("topic2");
    
    ASSERT_NE(commit1, nullptr);
    ASSERT_NE(commit2, nullptr);
    
    EXPECT_EQ(commit1->message_id, "msg1");
    EXPECT_EQ(commit2->message_id, "msg2");
    EXPECT_NE(commit1->message_id, commit2->message_id);
}

// Note: These tests require a running Aeron cluster
// They are marked as disabled by default
TEST_F(CommitResumeTest, DISABLED_TestUnsubscribe) {
    // This test requires a running cluster
    if (!client_->connect()) {
        GTEST_SKIP() << "Cannot connect to cluster";
    }
    
    std::string topic = "test_topic";
    
    // Subscribe first
    EXPECT_TRUE(client_->subscribe_topic(topic));
    
    // Unsubscribe
    EXPECT_TRUE(client_->unsubscribe_topic(topic));
}

TEST_F(CommitResumeTest, DISABLED_TestResumeFromCommit) {
    // This test requires a running cluster
    if (!client_->connect()) {
        GTEST_SKIP() << "Cannot connect to cluster";
    }
    
    std::string topic = "test_topic";
    
    // Commit a message first
    client_->commit_message(topic, "resume_msg", 3000, 3);
    
    // Resume from last commit
    EXPECT_TRUE(client_->resume_from_last_commit(topic));
}

TEST_F(CommitResumeTest, DISABLED_TestResumeWithoutCommit) {
    // This test requires a running cluster
    if (!client_->connect()) {
        GTEST_SKIP() << "Cannot connect to cluster";
    }
    
    std::string topic = "new_topic";
    
    // Resume without previous commit should fallback to LATEST
    EXPECT_TRUE(client_->resume_from_last_commit(topic));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
