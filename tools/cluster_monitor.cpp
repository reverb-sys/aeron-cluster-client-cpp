#include <aeron_cluster/cluster_client.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include <iomanip>
#include <vector>
#include <map>

using namespace aeron_cluster;

/**
 * @brief Cluster Monitor Tool
 * 
 * This tool monitors Aeron Cluster health by:
 * - Testing connectivity to cluster members
 * - Monitoring leader election status
 * - Tracking message throughput
 * - Displaying real-time cluster statistics
 */

std::atomic<bool> running{true};

void signalHandler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    running = false;
    exit(0);
}

struct MemberStats {
    std::string endpoint;
    bool reachable = false;
    bool isLeader = false;
    int connectionAttempts = 0;
    int successfulConnections = 0;
    std::chrono::milliseconds lastResponseTime{0};
    std::chrono::steady_clock::time_point lastSeen;
    std::string lastError;
};

struct ClusterStats {
    std::map<int, MemberStats> members;
    int currentLeader = -1;
    int totalMessages = 0;
    int successfulMessages = 0;
    int failedMessages = 0;
    std::chrono::steady_clock::time_point startTime;
    std::chrono::milliseconds totalUptime{0};
};

void printUsage(const char* programName) {
    std::cout << "Aeron Cluster Monitor" << std::endl;
    std::cout << "====================" << std::endl;
    std::cout << std::endl;
    std::cout << "Usage: " << programName << " [options]" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --endpoints LIST    Comma-separated cluster endpoints" << std::endl;
    std::cout << "                      (default: localhost:9002,localhost:9102,localhost:9202)" << std::endl;
    std::cout << "  --interval SEC      Monitoring interval in seconds (default: 5)" << std::endl;
    std::cout << "  --aeron-dir PATH    Aeron media driver directory (default: /dev/shm/aeron)" << std::endl;
    std::cout << "  --timeout SEC       Connection timeout in seconds (default: 10)" << std::endl;
    std::cout << "  --continuous        Run continuously until stopped" << std::endl;
    std::cout << "  --once              Run once and exit" << std::endl;
    std::cout << "  --verbose           Enable verbose output" << std::endl;
    std::cout << "  --help              Show this help message" << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  " << programName << " --continuous" << std::endl;
    std::cout << "  " << programName << " --endpoints localhost:9002,remote:9002 --interval 10" << std::endl;
    std::cout << "  " << programName << " --once --verbose" << std::endl;
    std::cout << std::endl;
}

std::vector<std::string> parseEndpoints(const std::string& endpointList) {
    std::vector<std::string> endpoints;
    std::stringstream ss(endpointList);
    std::string endpoint;
    
    while (std::getline(ss, endpoint, ',')) {
        // Trim whitespace
        endpoint.erase(0, endpoint.find_first_not_of(" \t"));
        endpoint.erase(endpoint.find_last_not_of(" \t") + 1);
        if (!endpoint.empty()) {
            endpoints.push_back(endpoint);
        }
    }
    
    return endpoints;
}

bool testMemberConnectivity(const std::string& endpoint, 
                           const std::string& aeronDir,
                           std::chrono::seconds timeout,
                           MemberStats& stats,
                           bool verbose) {
    stats.connectionAttempts++;
    auto startTime = std::chrono::steady_clock::now();
    
    try {
        if (verbose) {
            std::cout << "  Testing connection to " << endpoint << "..." << std::endl;
        }
        
        // Create a minimal configuration for testing
        auto config = ClusterClientConfigBuilder()
            .withClusterEndpoints({endpoint})
            .withAeronDir(aeronDir)
            .withResponseTimeout(std::chrono::duration_cast<std::chrono::milliseconds>(timeout))
            .withMaxRetries(1)
            .build();
        
        // Disable logging for clean output
        config.debug_logging = false;
        config.enable_console_info = false;
        config.enable_console_warnings = false;
        config.enable_console_errors = false;
        
        ClusterClient client(config);
        
        // Attempt connection
        bool connected = client.connect();
        
        auto endTime = std::chrono::steady_clock::now();
        stats.lastResponseTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        
        if (connected) {
            stats.successfulConnections++;
            stats.reachable = true;
            stats.lastSeen = endTime;
            stats.lastError.clear();
            
            // Check if this member is the leader
            int64_t sessionId = client.getSessionId();
            stats.isLeader = (sessionId > 0);
            
            if (verbose) {
                std::cout << "    ✅ Connected successfully (session: " << sessionId 
                         << ", time: " << stats.lastResponseTime.count() << "ms)" << std::endl;
            }
            
            client.disconnect();
            return true;
        } else {
            stats.reachable = false;
            stats.isLeader = false;
            stats.lastError = "Connection failed";
            
            if (verbose) {
                std::cout << "    ❌ Connection failed (time: " << stats.lastResponseTime.count() << "ms)" << std::endl;
            }
            
            return false;
        }
        
    } catch (const std::exception& e) {
        auto endTime = std::chrono::steady_clock::now();
        stats.lastResponseTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        stats.reachable = false;
        stats.isLeader = false;
        stats.lastError = e.what();
        
        if (verbose) {
            std::cout << "    ❌ Error: " << e.what() << std::endl;
        }
        
        return false;
    }
}

void displayClusterStatus(const ClusterStats& stats, bool verbose) {
    auto now = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - stats.startTime);
    
    // Clear screen and move to top (ANSI escape codes)
    std::cout << "\033[2J\033[H";
    
    std::cout << "🔍 Aeron Cluster Monitor" << std::endl;
    std::cout << "========================" << std::endl;
    std::cout << "Uptime: " << uptime.count() << "s | ";
    std::cout << "Leader: ";
    
    if (stats.currentLeader >= 0) {
        std::cout << "Member " << stats.currentLeader;
    } else {
        std::cout << "Unknown";
    }
    
    std::cout << " | Cluster Members: " << stats.members.size() << std::endl;
    std::cout << std::endl;
    
    // Member status table
    std::cout << "📊 Member Status:" << std::endl;
    std::cout << "┌─────────┬─────────────────────┬──────────┬─────────┬──────────┬──────────┐" << std::endl;
    std::cout << "│ Member  │ Endpoint            │ Status   │ Leader  │ Response │ Success  │" << std::endl;
    std::cout << "├─────────┼─────────────────────┼──────────┼─────────┼──────────┼──────────┤" << std::endl;
    
    for (const auto& pair : stats.members) {
        int memberId = pair.first;
        const MemberStats& member = pair.second;
        
        std::cout << "│ " << std::setw(7) << memberId << " │ ";
        std::cout << std::setw(19) << std::left << member.endpoint.substr(0, 19) << std::right << " │ ";
        
        if (member.reachable) {
            std::cout << "\033[32m" << std::setw(8) << "Online" << "\033[0m" << " │ ";
        } else {
            std::cout << "\033[31m" << std::setw(8) << "Offline" << "\033[0m" << " │ ";
        }
        
        if (member.isLeader) {
            std::cout << "\033[33m" << std::setw(7) << "Yes" << "\033[0m" << " │ ";
        } else {
            std::cout << std::setw(7) << "No" << " │ ";
        }
        
        std::cout << std::setw(8) << member.lastResponseTime.count() << "ms │ ";
        
        if (member.connectionAttempts > 0) {
            double successRate = (double)member.successfulConnections / member.connectionAttempts * 100.0;
            std::cout << std::setw(6) << std::fixed << std::setprecision(1) << successRate << "% │";
        } else {
            std::cout << std::setw(7) << "N/A │";
        }
        
        std::cout << std::endl;
        
        // Show error details if verbose and there's an error
        if (verbose && !member.lastError.empty() && !member.reachable) {
            std::cout << "│         │ Error: " << std::setw(60) << std::left 
                     << member.lastError.substr(0, 60) << std::right << " │" << std::endl;
        }
    }
    
    std::cout << "└─────────┴─────────────────────┴──────────┴─────────┴──────────┴──────────┘" << std::endl;
    std::cout << std::endl;
    
    // Cluster statistics
    std::cout << "📈 Cluster Statistics:" << std::endl;
    std::cout << "  Total connection attempts: " << stats.totalMessages << std::endl;
    std::cout << "  Successful connections: " << stats.successfulMessages << std::endl;
    std::cout << "  Failed connections: " << stats.failedMessages << std::endl;
    
    if (stats.totalMessages > 0) {
        double successRate = (double)stats.successfulMessages / stats.totalMessages * 100.0;
        std::cout << "  Overall success rate: " << std::fixed << std::setprecision(1) << successRate << "%" << std::endl;
    }
    
    // Count online members
    int onlineMembers = 0;
    for (const auto& pair : stats.members) {
        if (pair.second.reachable) {
            onlineMembers++;
        }
    }
    
    std::cout << "  Online members: " << onlineMembers << "/" << stats.members.size() << std::endl;
    
    // Cluster health assessment
    std::cout << std::endl;
    std::cout << "🏥 Cluster Health: ";
    
    if (onlineMembers == 0) {
        std::cout << "\033[31mCRITICAL - No members reachable\033[0m" << std::endl;
    } else if (onlineMembers < (int)stats.members.size() / 2 + 1) {
        std::cout << "\033[33mWARNING - No quorum available\033[0m" << std::endl;
    } else if (stats.currentLeader < 0) {
        std::cout << "\033[33mWARNING - No clear leader\033[0m" << std::endl;
    } else {
        std::cout << "\033[32mHEALTHY - Cluster operational\033[0m" << std::endl;
    }
    
    std::cout << std::endl;
    std::cout << "Press Ctrl+C to exit" << std::endl;
}

int main(int argc, char* argv[]) {
    // Default configuration
    std::vector<std::string> endpoints = {
        "localhost:9002", "localhost:9102", "localhost:9202"
    };
    std::string aeronDir = "/dev/shm/aeron";
    std::chrono::seconds interval{5};
    std::chrono::seconds timeout{10};
    bool continuous = true;
    bool verbose = false;
    
    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--endpoints" && i + 1 < argc) {
            endpoints = parseEndpoints(argv[++i]);
        } else if (arg == "--interval" && i + 1 < argc) {
            interval = std::chrono::seconds(std::stoi(argv[++i]));
        } else if (arg == "--aeron-dir" && i + 1 < argc) {
            aeronDir = argv[++i];
        } else if (arg == "--timeout" && i + 1 < argc) {
            timeout = std::chrono::seconds(std::stoi(argv[++i]));
        } else if (arg == "--continuous") {
            continuous = true;
        } else if (arg == "--once") {
            continuous = false;
        } else if (arg == "--verbose") {
            verbose = true;
        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }
    
    // Setup signal handlers
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    
    std::cout << "🚀 Starting Aeron Cluster Monitor" << std::endl;
    std::cout << "=================================" << std::endl;
    std::cout << "Monitoring " << endpoints.size() << " cluster members" << std::endl;
    std::cout << "Interval: " << interval.count() << "s | Timeout: " << timeout.count() << "s" << std::endl;
    std::cout << "Aeron Directory: " << aeronDir << std::endl;
    std::cout << std::endl;
    
    if (verbose) {
        std::cout << "Endpoints:" << std::endl;
        for (size_t i = 0; i < endpoints.size(); ++i) {
            std::cout << "  Member " << i << ": " << endpoints[i] << std::endl;
        }
        std::cout << std::endl;
    }
    
    // Initialize cluster statistics
    ClusterStats clusterStats;
    clusterStats.startTime = std::chrono::steady_clock::now();
    
    // Initialize member statistics
    for (size_t i = 0; i < endpoints.size(); ++i) {
        MemberStats memberStats;
        memberStats.endpoint = endpoints[i];
        clusterStats.members[i] = memberStats;
    }
    
    // Main monitoring loop
    int iteration = 0;
    do {
        iteration++;
        
        if (verbose) {
            std::cout << "🔄 Monitoring iteration " << iteration << std::endl;
        }
        
        // Test each member
        int currentLeader = -1;
        for (auto& pair : clusterStats.members) {
            int memberId = pair.first;
            MemberStats& memberStats = pair.second;
            
            clusterStats.totalMessages++;
            
            bool connected = testMemberConnectivity(
                memberStats.endpoint, aeronDir, timeout, memberStats, verbose);
            
            if (connected) {
                clusterStats.successfulMessages++;
                if (memberStats.isLeader) {
                    currentLeader = memberId;
                }
            } else {
                clusterStats.failedMessages++;
            }
            
            // Small delay between member tests to avoid overwhelming
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        clusterStats.currentLeader = currentLeader;
        
        // Display results
        if (continuous) {
            displayClusterStatus(clusterStats, verbose);
        } else {
            // Single run output
            std::cout << "📊 Cluster Status Report" << std::endl;
            std::cout << "========================" << std::endl;
            
            for (const auto& pair : clusterStats.members) {
                int memberId = pair.first;
                const MemberStats& member = pair.second;
                
                std::cout << "Member " << memberId << " (" << member.endpoint << "): ";
                
                if (member.reachable) {
                    std::cout << "✅ Online";
                    if (member.isLeader) {
                        std::cout << " (Leader)";
                    }
                    std::cout << " - " << member.lastResponseTime.count() << "ms";
                } else {
                    std::cout << "❌ Offline";
                    if (!member.lastError.empty()) {
                        std::cout << " - " << member.lastError;
                    }
                }
                std::cout << std::endl;
            }
            
            std::cout << std::endl;
            std::cout << "Current Leader: ";
            if (currentLeader >= 0) {
                std::cout << "Member " << currentLeader << std::endl;
            } else {
                std::cout << "None detected" << std::endl;
            }
        }
        
        // Wait for next iteration
        if (continuous && running) {
            auto sleepStart = std::chrono::steady_clock::now();
            while (running && 
                   (std::chrono::steady_clock::now() - sleepStart) < interval) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        
    } while (continuous && running);
    
    if (!continuous) {
        // Final summary for single run
        std::cout << std::endl;
        std::cout << "📈 Summary:" << std::endl;
        
        int totalAttempts = 0;
        int totalSuccesses = 0;
        int onlineMembers = 0;
        int currentLeader = clusterStats.currentLeader;
        
        for (const auto& pair : clusterStats.members) {
            const MemberStats& member = pair.second;
            totalAttempts += member.connectionAttempts;
            totalSuccesses += member.successfulConnections;
            if (member.reachable) {
                onlineMembers++;
            }
        }
        
        std::cout << "  Online members: " << onlineMembers << "/" << clusterStats.members.size() << std::endl;
        std::cout << "  Connection success rate: ";
        if (totalAttempts > 0) {
            double successRate = (double)totalSuccesses / totalAttempts * 100.0;
            std::cout << std::fixed << std::setprecision(1) << successRate << "%" << std::endl;
        } else {
            std::cout << "N/A" << std::endl;
        }
        
        // Health assessment
        std::cout << "  Cluster health: ";
        if (onlineMembers == 0) {
            std::cout << "CRITICAL - No members reachable" << std::endl;
        } else if (onlineMembers < (int)clusterStats.members.size() / 2 + 1) {
            std::cout << "WARNING - No quorum available" << std::endl;
        } else if (currentLeader < 0) {
            std::cout << "WARNING - No clear leader" << std::endl;
        } else {
            std::cout << "HEALTHY - Cluster operational" << std::endl;
        }
    }
    
    std::cout << std::endl;
    std::cout << "👋 Monitor stopped" << std::endl;
    
    return 0;
}