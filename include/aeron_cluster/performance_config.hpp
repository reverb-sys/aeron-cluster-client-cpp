#pragma once

#include <chrono>
#include <cstdint>
#include <vector>
#include <string>
#include <iostream>
#include <cstdlib>

namespace aeron_cluster {

/**
 * @brief Performance-optimized configuration for production use
 */
struct PerformanceConfig {
    // Polling configuration
    static constexpr int DEFAULT_POLL_BATCH_SIZE = 100;           // Process up to 100 messages per poll
    static constexpr int MAX_POLL_BATCH_SIZE = 1000;             // Maximum batch size
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1}; // 1ms polling interval
    
    // Memory management
    static constexpr size_t DEFAULT_BUFFER_SIZE = 64 * 1024;     // 64KB default buffer
    static constexpr size_t MAX_BUFFER_SIZE = 1024 * 1024;      // 1MB maximum buffer
    static constexpr size_t FRAGMENT_REASSEMBLY_BUFFER = 256 * 1024; // 256KB for reassembly
    
    // Connection settings
    static constexpr std::chrono::milliseconds CONNECTION_TIMEOUT{5000};  // 5 second timeout
    static constexpr std::chrono::milliseconds RECONNECT_DELAY{1000};     // 1 second reconnect delay
    static constexpr int MAX_RECONNECT_ATTEMPTS = 10;
    
    // Threading
    static constexpr bool USE_DEDICATED_POLLING_THREAD = true;
    static constexpr int POLLING_THREAD_PRIORITY = 0;  // Normal priority
    
    // Performance optimizations
    static constexpr bool ENABLE_MESSAGE_BATCHING = true;
    static constexpr bool ENABLE_ZERO_COPY = true;
    static constexpr bool ENABLE_FAST_PATH = true;
    
    // Debug settings (only enabled via environment variable)
    static bool is_debug_enabled() {
        static bool debug_enabled = []() {
            const char* debug_env = std::getenv("AERON_CLUSTER_DEBUG");
            return debug_env && std::string(debug_env) == "1";
        }();
        return debug_enabled;
    }
};

/**
 * @brief Optimized message batch processor
 */
class MessageBatchProcessor {
public:
    static constexpr int BATCH_SIZE = PerformanceConfig::DEFAULT_POLL_BATCH_SIZE;
    
    template<typename Subscription, typename Callback>
    static int process_batch(Subscription& subscription, Callback&& callback) {
        int fragments = 0;
        try {
            fragments = subscription.poll(
                [&](auto& buffer, auto offset, auto length, auto& header) {
                    callback(buffer, offset, length, header);
                },
                BATCH_SIZE);
        } catch (const std::exception& e) {
            // Use a simple approach for debug logging without including debug_utils
            if (std::getenv("AERON_CLUSTER_DEBUG") && std::string(std::getenv("AERON_CLUSTER_DEBUG")) == "1") {
                std::cerr << "[DEBUG] Error in MessageBatchProcessor::process_batch: " << e.what() << std::endl;
            }
        }
        return fragments;
    }
};

/**
 * @brief Memory pool for efficient buffer management
 */
class BufferPool {
public:
    static constexpr size_t POOL_SIZE = 16;
    static constexpr size_t BUFFER_SIZE = PerformanceConfig::DEFAULT_BUFFER_SIZE;
    
    static std::vector<std::uint8_t> get_buffer() {
        thread_local std::vector<std::vector<std::uint8_t>> pool;
        thread_local size_t current_index = 0;
        
        if (pool.empty()) {
            pool.resize(POOL_SIZE);
            for (auto& buffer : pool) {
                buffer.reserve(BUFFER_SIZE);
            }
        }
        
        auto& buffer = pool[current_index];
        current_index = (current_index + 1) % POOL_SIZE;
        
        buffer.clear();
        return std::move(buffer);
    }
};

} // namespace aeron_cluster
