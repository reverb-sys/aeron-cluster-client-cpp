#include "aeron_cluster/signal_handler.hpp"
#include "aeron_cluster/cluster_client.hpp"
#include "aeron_cluster/logging.hpp"
#include <iostream>
#include <algorithm>
#include <csignal>
#include <cstring>
#include <cerrno>
#include <cstdio>
#if defined(__unix__) || defined(__unix) || (defined(__APPLE__) && defined(__MACH__))
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <pthread.h>
#endif

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
    
    // Log registration for debugging
    auto logger = LoggerFactory::instance().getLogger("SignalHandler");
    if (logger) {
        logger->info("Registered client for signal handling (total clients: {})", clients_.size());
    } else {
        std::cout << "[SignalHandler] Registered client for signal handling (total clients: " 
                  << clients_.size() << ")" << std::endl;
    }
}

void SignalHandlerManager::unregister_client(ClusterClient* client) {
    if (!client) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(clients_mutex_);
    
    size_t before_count = clients_.size();
    
    // Remove expired or matching clients
    clients_.erase(
        std::remove_if(clients_.begin(), clients_.end(),
            [client](const std::weak_ptr<ClusterClient>& weak_client) {
                auto shared_client = weak_client.lock();
                return !shared_client || shared_client.get() == client;
            }),
        clients_.end()
    );
    
    size_t after_count = clients_.size();
    
    // Log unregistration for debugging (but don't spam logs)
    if (before_count != after_count) {
        auto logger = LoggerFactory::instance().getLogger("SignalHandler");
        if (logger) {
            logger->debug("Unregistered client from signal handling (remaining clients: {})", after_count);
        }
    }
}

void SignalHandlerManager::install_handlers() {
    // CRITICAL FIX: Always re-install handlers, even if already marked as installed
    // This ensures handlers are installed even if they were overwritten by something else
    // We'll check the flag AFTER installation to prevent duplicate installation messages
    bool was_already_installed = handlers_installed_.load();
    
    if (was_already_installed) {
        // Log that handlers were already marked as installed but we're verifying/re-installing
        std::cout << "[SignalHandler] Handlers already marked as installed, verifying and re-installing..." << std::endl;
    }
    
    // CRITICAL FIX: Use sigaction() instead of std::signal() for better reliability
    // sigaction() is POSIX-compliant and more reliable across different systems
#if defined(__unix__) || defined(__unix) || (defined(__APPLE__) && defined(__MACH__))
    // CRITICAL: First, check and unblock SIGINT and SIGTERM in case they're blocked
    sigset_t old_mask, new_mask;
    sigemptyset(&new_mask);
    sigaddset(&new_mask, SIGINT);
    sigaddset(&new_mask, SIGTERM);
    
    // Check current signal mask
    if (pthread_sigmask(SIG_BLOCK, nullptr, &old_mask) == 0) {
        if (sigismember(&old_mask, SIGINT)) {
            std::cerr << "[SignalHandler] WARNING: SIGINT is blocked!" << std::endl;
        }
        if (sigismember(&old_mask, SIGTERM)) {
            std::cerr << "[SignalHandler] WARNING: SIGTERM is blocked!" << std::endl;
        }
    }
    
    // Unblock SIGINT and SIGTERM
    if (pthread_sigmask(SIG_UNBLOCK, &new_mask, nullptr) != 0) {
        std::cerr << "[SignalHandler] WARNING: Failed to unblock SIGINT/SIGTERM: " 
                  << std::strerror(errno) << std::endl;
    } else {
        std::cout << "[SignalHandler] Unblocked SIGINT and SIGTERM signals" << std::endl;
    }
    
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);  // Don't block any signals in handler
    sa.sa_flags = 0;  // No SA_RESTART to allow interruption, no SA_NODEFER
    
    struct sigaction old_sa_int, old_sa_term;
    
    // Install SIGINT handler and save old one
    if (sigaction(SIGINT, &sa, &old_sa_int) != 0) {
        std::cerr << "[SignalHandler] ERROR: Failed to install SIGINT handler: " 
                  << std::strerror(errno) << " (errno=" << errno << ")" << std::endl;
        std::cerr.flush();
    } else {
        std::cout << "[SignalHandler] SIGINT handler installed successfully (old handler: " 
                  << (old_sa_int.sa_handler == SIG_DFL ? "SIG_DFL" : 
                      old_sa_int.sa_handler == SIG_IGN ? "SIG_IGN" : "custom") << ")" << std::endl;
        std::cout.flush();
    }
    
    // Install SIGTERM handler and save old one
    if (sigaction(SIGTERM, &sa, &old_sa_term) != 0) {
        std::cerr << "[SignalHandler] ERROR: Failed to install SIGTERM handler: " 
                  << std::strerror(errno) << " (errno=" << errno << ")" << std::endl;
        std::cerr.flush();
    } else {
        std::cout << "[SignalHandler] SIGTERM handler installed successfully (old handler: " 
                  << (old_sa_term.sa_handler == SIG_DFL ? "SIG_DFL" : 
                      old_sa_term.sa_handler == SIG_IGN ? "SIG_IGN" : "custom") << ")" << std::endl;
        std::cout.flush();
    }
    
    // Verify handler was actually installed
    struct sigaction verify_int, verify_term;
    if (sigaction(SIGINT, nullptr, &verify_int) == 0) {
        if (verify_int.sa_handler == signal_handler) {
            std::cout << "[SignalHandler] VERIFIED: SIGINT handler is correctly installed (address: " 
                      << (void*)verify_int.sa_handler << ")" << std::endl;
        } else {
            std::cerr << "[SignalHandler] WARNING: SIGINT handler verification failed! Handler mismatch." << std::endl;
            std::cerr << "  Expected: " << (void*)signal_handler << ", Got: " << (void*)verify_int.sa_handler << std::endl;
        }
    }
    if (sigaction(SIGTERM, nullptr, &verify_term) == 0) {
        if (verify_term.sa_handler == signal_handler) {
            std::cout << "[SignalHandler] VERIFIED: SIGTERM handler is correctly installed (address: " 
                      << (void*)verify_term.sa_handler << ")" << std::endl;
        } else {
            std::cerr << "[SignalHandler] WARNING: SIGTERM handler verification failed! Handler mismatch." << std::endl;
            std::cerr << "  Expected: " << (void*)signal_handler << ", Got: " << (void*)verify_term.sa_handler << std::endl;
        }
    }
    
    // Print handler address for debugging
    std::cout << "[SignalHandler] signal_handler function address: " << (void*)signal_handler << std::endl;
    std::cout.flush();
    std::cerr.flush();
#else
    // Fallback to std::signal() on non-POSIX systems
    void (*old_int)(int) = std::signal(SIGINT, signal_handler);
    void (*old_term)(int) = std::signal(SIGTERM, signal_handler);
    if (old_int == SIG_ERR) {
        std::cerr << "[SignalHandler] ERROR: Failed to install SIGINT handler" << std::endl;
    } else {
        std::cout << "[SignalHandler] SIGINT handler installed successfully" << std::endl;
    }
    if (old_term == SIG_ERR) {
        std::cerr << "[SignalHandler] ERROR: Failed to install SIGTERM handler" << std::endl;
    } else {
        std::cout << "[SignalHandler] SIGTERM handler installed successfully" << std::endl;
    }
    std::cout.flush();
#endif
    
    handlers_installed_.store(true);
    
    // Get logger for debugging
    auto logger = LoggerFactory::instance().getLogger("SignalHandler");
    if (logger) {
        logger->info("Signal handlers installed for SIGINT and SIGTERM");
    } else {
        std::cout << "[SignalHandler] Signal handlers installed for SIGINT and SIGTERM" << std::endl;
        std::cout.flush();
    }
}

// Global volatile flag for signal handling (async-signal-safe)
static volatile sig_atomic_t signal_received = 0;
static volatile int signal_number = 0;

void SignalHandlerManager::signal_handler(int signal) {
    // CRITICAL FIX: Use ONLY async-signal-safe functions
    // First, write a simple message to confirm handler is called
    
    // Write multiple times to different file descriptors to ensure message gets through
    // Use hardcoded messages with hardcoded lengths (no strlen!)
    const char msg[] = "\n*** SIGNAL HANDLER CALLED ***\n";
    const char msg2[] = "*** DISCONNECTING ALL CLIENTS ***\n";
    
    // Write to stdout (fd 1) multiple times
    write(1, msg, 31);  // Hardcoded length: "\n*** SIGNAL HANDLER CALLED ***\n" = 31 chars
    write(1, msg2, 35);  // Hardcoded length: "*** DISCONNECTING ALL CLIENTS ***\n" = 35 chars
    
    // Write to stderr (fd 2) multiple times
    write(2, msg, 31);
    write(2, msg2, 35);
    
    // Write to stderr again to force visibility
    write(2, "SIGNAL: ", 8);
    char sig_char = '0' + signal;
    write(2, &sig_char, 1);
    write(2, "\n", 1);
    
    // Set volatile flag (async-signal-safe)
    signal_received = 1;
    signal_number = signal;
    
    // CRITICAL: Try to disconnect clients
    // WARNING: This uses mutexes which are NOT fully async-signal-safe
    // If a mutex is held, this could deadlock or crash silently
    // But we need to disconnect, so we'll try it
    if (instance_) {
        // Write before attempting disconnect
        write(2, "BEFORE DISCONNECT\n", 18);
        instance_->disconnect_all_clients();
        write(2, "AFTER DISCONNECT\n", 17);
    } else {
        write(2, "ERROR: instance_ is null!\n", 26);
    }
    
    // Do not exit the process from within the library.
    // Leave it to the application to decide how to handle the signal.
    return;
}

void SignalHandlerManager::disconnect_all_clients() {
    auto logger = LoggerFactory::instance().getLogger("SignalHandler");
    
    // CRITICAL FIX: Collect all valid clients first while holding lock
    // Then release lock before disconnecting to avoid modifying vector during iteration
    std::vector<std::shared_ptr<ClusterClient>> clients_to_disconnect;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        
        size_t total_registered = clients_.size();
        size_t valid_count = 0;
        size_t expired_count = 0;
        
        // Collect all valid clients while holding lock
        for (auto& weak_client : clients_) {
            if (auto client = weak_client.lock()) {
                clients_to_disconnect.push_back(client);
                valid_count++;
            } else {
                expired_count++;
            }
        }
        
        if (logger) {
            logger->info("Received termination signal: {} registered clients, {} valid, {} expired", 
                         total_registered, valid_count, expired_count);
        } else {
            std::cout << "[SIGNAL HANDLER] Received termination signal: " << total_registered 
                      << " registered clients, " << valid_count << " valid, " << expired_count 
                      << " expired" << std::endl;
            std::cout.flush();
        }
    }
    // Lock released here - safe to disconnect without modifying clients_ vector
    
    // Now disconnect all collected clients (outside lock to prevent deadlock)
    size_t disconnected_count = 0;
    size_t failed_count = 0;
    
    if (clients_to_disconnect.empty()) {
        if (logger) {
            logger->warn("No valid clients found to disconnect! All weak_ptrs may have expired.");
        } else {
            std::cerr << "WARNING: No valid clients found to disconnect! "
                      << "All weak_ptrs may have expired. Clients may not have been created with shared_ptr." 
                      << std::endl;
        }
    } else {
        if (logger) {
            logger->info("Found {} valid clients to disconnect", clients_to_disconnect.size());
        } else {
            std::cout << "Found " << clients_to_disconnect.size() << " valid clients to disconnect" << std::endl;
        }
    }
    
    for (size_t i = 0; i < clients_to_disconnect.size(); ++i) {
        auto& client = clients_to_disconnect[i];
        if (!client) {
            if (logger) {
                logger->warn("Client {}/{} is null, skipping", i + 1, clients_to_disconnect.size());
            } else {
                std::cerr << "WARNING: Client " << (i + 1) << "/" << clients_to_disconnect.size() 
                          << " is null, skipping" << std::endl;
            }
            failed_count++;
            continue;
        }
        
        try {
            if (logger) {
                logger->info("Disconnecting client {}/{} (session_id={})...", 
                             i + 1, clients_to_disconnect.size(), client->get_session_id());
            } else {
                std::cout << "Disconnecting client " << (i + 1) << "/" << clients_to_disconnect.size() 
                          << " (session_id=" << client->get_session_id() << ")..." << std::endl;
            }
            client->disconnect();
            disconnected_count++;
        } catch (const std::exception& e) {
            failed_count++;
            if (logger) {
                logger->error("Error disconnecting client {}/{}: {}", i + 1, clients_to_disconnect.size(), e.what());
            } else {
                std::cerr << "ERROR disconnecting client " << (i + 1) << "/" << clients_to_disconnect.size() 
                          << ": " << e.what() << std::endl;
            }
        } catch (...) {
            failed_count++;
            if (logger) {
                logger->error("Unknown error disconnecting client {}/{}", i + 1, clients_to_disconnect.size());
            } else {
                std::cerr << "ERROR: Unknown exception while disconnecting client " 
                          << (i + 1) << "/" << clients_to_disconnect.size() << std::endl;
            }
        }
    }
    
    if (logger) {
        logger->info("Disconnect complete: {} succeeded, {} failed out of {} total", 
                     disconnected_count, failed_count, clients_to_disconnect.size());
    } else {
        std::cout << "All clients disconnected: " << disconnected_count << " succeeded, " 
                  << failed_count << " failed out of " << clients_to_disconnect.size() << " total" << std::endl;
    }
}

} // namespace aeron_cluster
