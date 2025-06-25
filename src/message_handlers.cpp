#include "aeron_cluster/message_handlers.hpp"
#include <iostream>

namespace aeron_cluster {

class MessageHandler::Impl {
public:
    Impl() = default;
    
    void handleMessage(const ParseResult& result) {
        if (result.success) {
            std::cout << "[MessageHandler] Handled message: " << result.message_type << std::endl;
        } else {
            std::cout << "[MessageHandler] Failed to handle message: " << result.error_message << std::endl;
        }
    }
};

MessageHandler::MessageHandler() : pImpl(std::make_unique<Impl>()) {}
MessageHandler::~MessageHandler() = default;

void MessageHandler::handleMessage(const ParseResult& result) {
    pImpl->handleMessage(result);
}

} // namespace aeron_cluster
