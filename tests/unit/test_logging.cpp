#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <aeron_cluster/logging.hpp>
#include <sstream>

using namespace aeron_cluster;
using ::testing::_;
using ::testing::Return;
using ::testing::StrictMock;

class MockLogSink : public LogSink {
public:
    MOCK_METHOD(void, write, (const LogEntry& entry), (override));
    MOCK_METHOD(void, flush, (), (override));
};

class LoggingTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Reset logger factory for clean test environment
        LoggerFactory::instance().shutdown();
    }
    
    void TearDown() override {
        LoggerFactory::instance().shutdown();
    }
};

TEST_F(LoggingTest, LoggerCreation) {
    auto logger = LoggerFactory::instance().getLogger("test");
    EXPECT_NE(logger, nullptr);
    
    // Same name should return same instance
    auto logger2 = LoggerFactory::instance().getLogger("test");
    EXPECT_EQ(logger.get(), logger2.get());
}

TEST_F(LoggingTest, LogLevelFiltering) {
    auto mockSink = std::make_shared<StrictMock<MockLogSink>>();
    
    auto logger = LoggerFactory::instance().getLogger("test");
    logger->setLevel(LogLevel::WARN);
    logger->addSink(mockSink);
    
    // Should not call sink for levels below WARN
    logger->debug("Debug message");
    logger->info("Info message");
    
    // Should call sink for WARN and above
    EXPECT_CALL(*mockSink, write(_)).Times(2);
    logger->warn("Warning message");
    logger->error("Error message");
}

TEST_F(LoggingTest, LogEntryContainsCorrectInformation) {
    auto mockSink = std::make_shared<MockLogSink>();
    
    LogEntry capturedEntry(LogLevel::INFO, "", "");
    EXPECT_CALL(*mockSink, write(_))
        .WillOnce([&capturedEntry](const LogEntry& entry) {
            capturedEntry = entry;
        });
    
    auto logger = LoggerFactory::instance().getLogger("test-logger");
    logger->setLevel(LogLevel::DEBUG);
    logger->addSink(mockSink);
    
    logger->info("Test message with {} formatting", 42);
    
    EXPECT_EQ(capturedEntry.level, LogLevel::INFO);
    EXPECT_EQ(capturedEntry.logger_name, "test-logger");
    EXPECT_THAT(capturedEntry.message, ::testing::HasSubstr("Test message"));
    EXPECT_THAT(capturedEntry.message, ::testing::HasSubstr("42"));
    EXPECT_FALSE(capturedEntry.file_name.empty());
    EXPECT_FALSE(capturedEntry.function_name.empty());
    EXPECT_GT(capturedEntry.line_number, 0);
}

TEST_F(LoggingTest, ConsoleSinkFormatsCorrectly) {
    std::ostringstream oss;
    
    // Redirect cout for testing
    std::streambuf* original = std::cout.rdbuf();
    std::cout.rdbuf(oss.rdbuf());
    
    auto consoleSink = std::make_shared<ConsoleSink>(false); // No colors for testing
    auto logger = LoggerFactory::instance().getLogger("console-test");
    logger->addSink(consoleSink);
    
    logger->info("Test console output");
    
    // Restore cout
    std::cout.rdbuf(original);
    
    std::string output = oss.str();
    EXPECT_THAT(output, ::testing::HasSubstr("[INFO]"));
    EXPECT_THAT(output, ::testing::HasSubstr("[console-test]"));
    EXPECT_THAT(output, ::testing::HasSubstr("Test console output"));
}

TEST_F(LoggingTest, ResultTypeSuccessPath) {
    Result<int> result = Result<int>::success(42);
    
    EXPECT_TRUE(result.isSuccess());
    EXPECT_FALSE(result.isError());
    EXPECT_EQ(result.value(), 42);
    EXPECT_EQ(result.valueOr(0), 42);
}

TEST_F(LoggingTest, ResultTypeErrorPath) {
    Result<int> result = Result<int>::error(AeronClusterException("Test error"));
    
    EXPECT_FALSE(result.isSuccess());
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.valueOr(999), 999);
    EXPECT_THAT(result.error().what(), ::testing::HasSubstr("Test error"));
}

TEST_F(LoggingTest, ResultTypeMapping) {
    Result<int> intResult = Result<int>::success(21);
    
    auto stringResult = intResult.map([](int value) {
        return std::to_string(value * 2);
    });
    
    EXPECT_TRUE(stringResult.isSuccess());
    EXPECT_EQ(stringResult.value(), "42");
}