#pragma once

#include <iostream>
#include <string>

namespace aeron_cluster {

/**
 * @brief Debug utility for conditional logging
 */
class DebugLogger {
public:
    static bool is_debug_enabled() {
        static bool debug_enabled = []() {
            const char* debug_env = std::getenv("AERON_CLUSTER_DEBUG");
            return debug_env && std::string(debug_env) == "1";
        }();
        return debug_enabled;
    }

    template<typename... Args>
    static void debug(const std::string& prefix, Args&&... args) {
        if (is_debug_enabled()) {
            std::cout << prefix;
            (std::cout << ... << args);
            std::cout << std::endl;
        }
    }

    template<typename... Args>
    static void debug_hex(const std::string& prefix, const uint8_t* data, size_t length, Args&&... args) {
        if (is_debug_enabled()) {
            std::cout << prefix;
            (std::cout << ... << args);
            std::cout << " (hex dump):" << std::endl;
            print_hex_dump(data, length, "  ");
        }
    }

private:
    static void print_hex_dump(const uint8_t* data, size_t length, const std::string& prefix) {
        if (!is_debug_enabled()) return;
        
        for (size_t i = 0; i < length; i += 16) {
            std::cout << prefix;
            // Print offset
            std::printf("%04zx: ", i);
            
            // Print hex bytes
            for (size_t j = 0; j < 16; ++j) {
                if (i + j < length) {
                    std::printf("%02x ", data[i + j]);
                } else {
                    std::cout << "   ";
                }
                if (j == 7) std::cout << " ";
            }
            
            // Print ASCII representation
            std::cout << " |";
            for (size_t j = 0; j < 16 && i + j < length; ++j) {
                char c = data[i + j];
                std::cout << (std::isprint(c) ? c : '.');
            }
            std::cout << "|" << std::endl;
        }
    }
};

// Convenience macros
#define DEBUG_LOG(...) DebugLogger::debug("[DEBUG] ", __VA_ARGS__)
#define DEBUG_HEX(prefix, data, length, ...) DebugLogger::debug_hex(prefix, data, length, __VA_ARGS__)

} // namespace aeron_cluster
