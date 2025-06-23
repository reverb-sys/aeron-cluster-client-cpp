#pragma once

#include "aeron_cluster/config.hpp"
#include "aeron_cluster/sbe_messages.hpp"
#include <Aeron.h>
#include <memory>

namespace aeron_cluster {

class SessionManager {
public:
    explicit SessionManager(const ClusterClientConfig& config);
    ~SessionManager();
    
    bool connect(std::shared_ptr<aeron::Aeron> aeron);
    void disconnect();
    bool isConnected() const;
    
    int64_t getSessionId() const;
    int32_t getLeaderMemberId() const;
    
    bool publishMessage(const std::string& topic,
                       const std::string& messageType,
                       const std::string& messageId,
                       const std::string& payload,
                       const std::string& headers);
    
    void handleSessionEvent(const ParseResult& result);
    std::string resolveEgressEndpoint(const std::string& channel);
    int64_t getLeadershipTermId();
    bool sendRawMessage(const std::vector<uint8_t>& encodedMessage);
private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace aeron_cluster
