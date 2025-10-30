#include "aeron_cluster/signal_handler.hpp"
#include "aeron_cluster/cluster_client.hpp"
#include "aeron_cluster/logging.hpp"
#include <iostream>
#include <algorithm>

namespace aeron_cluster {

// Static member initialization
SignalHandlerManager* SignalHandlerManager::instance_ = nullptr;

SignalHandlerManager& SignalHandlerManager::instance() {
    static SignalHandlerManager instance;
    instance_ = &instance;
    return instance;
}

void SignalHandlerManager::register_client(std::shared_ptr<ClusterClient> client) {
    if (!client) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(clients_mutex_);
    
    // Install signal handlers on first client registration
    if (!handlers_installed_.load()) {
        install_handlers();
    }
    
    // Add client to the list
    clients_.emplace_back(client);
}

void SignalHandlerManager::unregister_client(ClusterClient* client) {
    if (!client) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(clients_mutex_);
    
    // Remove expired or matching clients
    clients_.erase(
        std::remove_if(clients_.begin(), clients_.end(),
            [client](const std::weak_ptr<ClusterClient>& weak_client) {
                auto shared_client = weak_client.lock();
                return !shared_client || shared_client.get() == client;
            }),
        clients_.end()
    );
}

void SignalHandlerManager::install_handlers() {
    if (handlers_installed_.load()) {
        return;
    }
    
    // Install signal handlers for graceful shutdown
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    handlers_installed_.store(true);
    
    // Get logger for debugging
    auto logger = LoggerFactory::instance().getLogger("SignalHandler");
    if (logger) {
        logger->info("Signal handlers installed for SIGINT and SIGTERM");
    }
}

void SignalHandlerManager::signal_handler(int signal) {
    if (instance_) {
        instance_->disconnect_all_clients();
    }
    
    // Exit with the signal number
    std::exit(signal);
}

void SignalHandlerManager::disconnect_all_clients() {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    
    auto logger = LoggerFactory::instance().getLogger("SignalHandler");
    if (logger) {
        logger->info("Received termination signal, disconnecting {} clients...", clients_.size());
    } else {
        std::cout << "Received termination signal, disconnecting clients..." << std::endl;
    }
    
    // Disconnect all valid clients
    for (auto& weak_client : clients_) {
        if (auto client = weak_client.lock()) {
            try {
                if (logger) {
                    logger->info("Disconnecting client...");
                }
                client->disconnect();
            } catch (const std::exception& e) {
                if (logger) {
                    logger->error("Error disconnecting client: {}", e.what());
                } else {
                    std::cerr << "Error disconnecting client: " << e.what() << std::endl;
                }
            }
        }
    }
    
    if (logger) {
        logger->info("All clients disconnected");
    } else {
        std::cout << "All clients disconnected" << std::endl;
    }
}

} // namespace aeron_cluster
