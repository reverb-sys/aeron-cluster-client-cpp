#pragma once

#include "aeron_cluster/sbe_messages.hpp"

namespace aeron_cluster {

class ClusterClient;

class MessageHandler {
public:
    explicit MessageHandler(ClusterClient::Impl& client);
    ~MessageHandler();
    
    void handleMessage(const ParseResult& result);
    
private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace aeron_cluster
