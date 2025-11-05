#pragma once

#include <memory>
#include <mutex>
#include <vector>
#include <csignal>
#include <atomic>

namespace aeron_cluster {

/**
 * @brief Forward declaration of ClusterClient
 */
class ClusterClient;

/**
 * @brief Global signal handler manager for graceful disconnection
 * 
 * This class automatically handles SIGINT and SIGTERM signals to ensure
 * all registered clients are properly disconnected when the application
 * is terminated abruptly (e.g., Ctrl+C).
 */
class SignalHandlerManager {
public:
    /**
     * @brief Get the singleton instance
     */
    static SignalHandlerManager& instance();
    
    /**
     * @brief Register a client for signal handling
     * @param client Shared pointer to the client
     */
    void register_client(std::shared_ptr<ClusterClient> client);
    
    /**
     * @brief Unregister a client from signal handling
     * @param client Raw pointer to the client (for comparison)
     */
    void unregister_client(ClusterClient* client);
    
    /**
     * @brief Check if signal handlers are already installed
     */
    bool are_handlers_installed() const { return handlers_installed_.load(); }

private:
    SignalHandlerManager() = default;
    ~SignalHandlerManager() = default;
    
    // Disable copy and move
    SignalHandlerManager(const SignalHandlerManager&) = delete;
    SignalHandlerManager& operator=(const SignalHandlerManager&) = delete;
    SignalHandlerManager(SignalHandlerManager&&) = delete;
    SignalHandlerManager& operator=(SignalHandlerManager&&) = delete;
    
    /**
     * @brief Install signal handlers if not already installed
     */
    void install_handlers();
    
    /**
     * @brief Signal handler function
     */
    static void signal_handler(int signal);
    
    /**
     * @brief Disconnect all registered clients
     * 
     * Internal method called by signal handler.
     */
    void disconnect_all_clients();
    
    // Registered clients
    std::vector<std::weak_ptr<ClusterClient>> clients_;
    mutable std::mutex clients_mutex_;
    
    // Signal handler installation flag
    std::atomic<bool> handlers_installed_{false};
    
    // Static instance for signal handler access
    static SignalHandlerManager* instance_;
};

} // namespace aeron_cluster
