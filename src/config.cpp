#include "aeron_cluster/config.hpp"
#include <regex>
#include <random>
#include <set>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <iphlpapi.h>
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "iphlpapi.lib")
#else
    #include <ifaddrs.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <net/if.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

namespace aeron_cluster {

void ClusterClientConfig::validate() {
    if (cluster_endpoints.empty()) {
        throw std::invalid_argument("At least one cluster endpoint must be specified");
    }
    
    if (response_channel.empty()) {
        throw std::invalid_argument("Response channel cannot be empty");
    }
    
    // Resolve response channel if it contains port 0
    if (needs_port_resolution(response_channel)) {
        response_channel = resolve_egress_endpoint(response_channel);
    }
    
    if (aeron_dir.empty()) {
        throw std::invalid_argument("Aeron directory cannot be empty");
    }
    
    if (response_timeout.count() <= 0) {
        throw std::invalid_argument("Response timeout must be positive");
    }
    
    if (max_retries < 0) {
        throw std::invalid_argument("Max retries cannot be negative");
    }
    
    if (keepalive_interval.count() <= 0) {
        throw std::invalid_argument("Keepalive interval must be positive");
    }
}

bool ClusterClientConfig::needs_port_resolution(const std::string& channel) const {
    return channel.find("endpoint=localhost:0") != std::string::npos ||
           channel.find("endpoint=0.0.0.0:0") != std::string::npos ||
           channel.find(":0") != std::string::npos;
}

std::chrono::milliseconds ConnectionStats::get_current_uptime() const {
    if (!is_connected) {
        return total_uptime;
    }
    
    auto now = std::chrono::steady_clock::now();
    auto current_session_uptime = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - connection_established_time);
    
    return total_uptime + current_session_uptime;
}

double ConnectionStats::get_connection_success_rate() const {
    if (connection_attempts == 0) {
        return 0.0;
    }
    
    return static_cast<double>(successful_connections) / static_cast<double>(connection_attempts);
}

double ConnectionStats::get_message_success_rate() const {
    std::uint64_t total_messages = messages_sent;
    if (total_messages == 0) {
        return 0.0;
    }
    
    std::uint64_t successful_messages = messages_acknowledged;
    return static_cast<double>(successful_messages) / static_cast<double>(total_messages);
}

std::string ClusterClientConfig::get_local_ip_address() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return "";
    }
    
    ULONG bufferSize = 0;
    GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, 
                        nullptr, nullptr, &bufferSize);
    
    std::vector<char> buffer(bufferSize);
    PIP_ADAPTER_ADDRESSES adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
    
    DWORD result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                       nullptr, adapters, &bufferSize);
    
    if (result == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
            if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK || 
                adapter->OperStatus != IfOperStatusUp) {
                continue;
            }
            
            for (PIP_ADAPTER_UNICAST_ADDRESS addr = adapter->FirstUnicastAddress; 
                 addr != nullptr; addr = addr->Next) {
                
                if (addr->Address.lpSockaddr->sa_family == AF_INET) {
                    struct sockaddr_in* sockaddr_ipv4 = 
                        reinterpret_cast<struct sockaddr_in*>(addr->Address.lpSockaddr);
                    
                    char ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &(sockaddr_ipv4->sin_addr), ip_str, INET_ADDRSTRLEN);
                    
                    std::uint32_t ip = ntohl(sockaddr_ipv4->sin_addr.s_addr);
                    if (ip != 0x7F000001) { // not 127.0.0.1
                        WSACleanup();
                        return std::string(ip_str);
                    }
                }
            }
        }
    }
    
    WSACleanup();
    return "";
    
#else
    struct ifaddrs* ifaddrs_ptr = nullptr;
    if (getifaddrs(&ifaddrs_ptr) == -1) {
        return "";
    }
    
    for (struct ifaddrs* ifa = ifaddrs_ptr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        
        if (!(ifa->ifa_flags & IFF_UP) || 
            !(ifa->ifa_flags & IFF_RUNNING) ||
            (ifa->ifa_flags & IFF_LOOPBACK)) {
            continue;
        }
        
        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in* addr = (struct sockaddr_in*)ifa->ifa_addr;
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(addr->sin_addr), ip_str, INET_ADDRSTRLEN);
            
            std::uint32_t ip = ntohl(addr->sin_addr.s_addr);
            if (ip != 0x7F000001) { // not 127.0.0.1
                freeifaddrs(ifaddrs_ptr);
                return std::string(ip_str);
            }
        }
    }
    
    freeifaddrs(ifaddrs_ptr);
    return "";
#endif
}

int ClusterClientConfig::find_available_port(int min_port, int max_port) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(min_port, max_port);
    std::set<int> tried;

    while (tried.size() < static_cast<std::size_t>(max_port - min_port + 1)) {
        int port = dist(gen);
        if (tried.count(port)) continue;
        tried.insert(port);

        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) continue;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(static_cast<std::uint16_t>(port));

        if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
#ifdef _WIN32
            closesocket(sock);
#else
            close(sock);
#endif
            return port;
        }

#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
    }

    throw std::runtime_error("No available UDP port found in the range " + 
                           std::to_string(min_port) + "-" + std::to_string(max_port));
}

std::string ClusterClientConfig::resolve_egress_endpoint(const std::string& base_channel) const {
    std::regex pattern(R"(endpoint=([a-zA-Z0-9\.\-]+):(\d+))");
    std::smatch match;

    if (std::regex_search(base_channel, match, pattern)) {
        std::string host = match[1].str();
        std::string port_str = match[2].str();
        int port = std::stoi(port_str);

        if (port == 0) {
            int new_port = find_available_port();
            std::string machine_ip = get_local_ip_address();
            
            std::string resolved_host = machine_ip.empty() ? host : machine_ip;
            std::string new_endpoint = "endpoint=" + resolved_host + ":" + std::to_string(new_port);
            return std::regex_replace(base_channel, pattern, new_endpoint);
        } else {
            return base_channel;
        }
    } else {
        throw std::invalid_argument("Channel endpoint is not in the correct format: expected 'endpoint=HOST:PORT'");
    }
}

} // namespace aeron_cluster