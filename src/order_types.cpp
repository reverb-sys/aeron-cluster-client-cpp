#include "aeron_cluster/order_types.hpp"
#include <json/json.h>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace aeron_cluster {

// Order implementation

void Order::generateClientOrderUUID() {
    if (!clientOrderUUID.empty()) {
        return; // Already has UUID
    }
    
    // Generate a simple UUID-like string
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis;
    
    std::stringstream ss;
    ss << std::hex 
       << dis(gen) << "-" 
       << (dis(gen) & 0xFFFF) << "-" 
       << (dis(gen) & 0xFFFF) << "-" 
       << (dis(gen) & 0xFFFF) << "-" 
       << dis(gen) << dis(gen);
    
    clientOrderUUID = ss.str();
}

bool Order::validate() const {
    // Basic validation checks
    if (id.empty()) return false;
    if (baseToken.empty() || quoteToken.empty()) return false;
    if (side != "BUY" && side != "SELL") return false;
    if (quantity <= 0.0) return false;
    
    // Order type specific validation
    if (orderType == "LIMIT" && limitPrice <= 0.0) return false;
    if (orderType == "STOP" && !stopPrice.has_value()) return false;
    if (orderType == "STOP_LIMIT" && (!stopPrice.has_value() || limitPrice <= 0.0)) return false;
    
    // Time in force validation
    if (tif != "GTC" && tif != "IOC" && tif != "FOK" && tif != "DAY" && tif != "GTD") return false;
    if (tif == "GTD" && !expiryTimestamp.has_value()) return false;
    
    return true;
}

std::string Order::toJsonString() const {
    Json::Value json;
    
    json["id"] = id;
    json["base_token"] = baseToken;
    json["quote_token"] = quoteToken;
    json["side"] = side;
    json["quantity"] = quantity;
    json["quantity_token"] = quantityToken;
    json["limit_price"] = limitPrice;
    json["limit_price_token"] = limitPriceToken;
    json["customer_id"] = customerID;
    json["user_id"] = userID;
    json["account_id"] = accountID;
    json["status"] = status;
    json["order_type"] = orderType;
    json["timestamp"] = timestamp;
    json["updated_at"] = updatedAt;
    json["client_order_uuid"] = clientOrderUUID;
    json["time_in_force"] = tif;
    json["request_source"] = requestSource;
    json["request_channel"] = requestChannel;
    json["base_token_usd_conversion_rate"] = baseTokenUsdConversionRate;
    json["quote_token_usd_conversion_rate"] = quoteTokenUsdConversionRate;
    
    if (stopPrice.has_value()) {
        json["stop_price"] = stopPrice.value();
    }
    
    if (expiryTimestamp.has_value()) {
        json["expiry_timestamp"] = expiryTimestamp.value();
    }
    
    // Add metadata
    if (!metadata.empty()) {
        Json::Value metadataJson;
        for (const auto& pair : metadata) {
            metadataJson[pair.first] = pair.second;
        }
        json["metadata"] = metadataJson;
    }
    
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, json);
}

Order Order::fromJsonString(const std::string& jsonStr) {
    Json::Reader reader;
    Json::Value root;
    
    if (!reader.parse(jsonStr, root)) {
        throw std::runtime_error("Failed to parse JSON: " + reader.getFormattedErrorMessages());
    }
    
    Order order;
    
    // Required fields
    order.id = root.get("id", "").asString();
    order.baseToken = root.get("base_token", "").asString();
    order.quoteToken = root.get("quote_token", "").asString();
    order.side = root.get("side", "").asString();
    order.quantity = root.get("quantity", 0.0).asDouble();
    order.orderType = root.get("order_type", "LIMIT").asString();
    
    // Optional fields with defaults
    order.quantityToken = root.get("quantity_token", order.baseToken).asString();
    order.limitPrice = root.get("limit_price", 0.0).asDouble();
    order.limitPriceToken = root.get("limit_price_token", order.quoteToken).asString();
    order.customerID = root.get("customer_id", 0).asInt64();
    order.userID = root.get("user_id", 0).asInt64();
    order.accountID = root.get("account_id", 0).asInt64();
    order.status = root.get("status", "CREATED").asString();
    order.timestamp = root.get("timestamp", 0).asInt64();
    order.updatedAt = root.get("updated_at", 0).asInt64();
    order.clientOrderUUID = root.get("client_order_uuid", "").asString();
    order.tif = root.get("time_in_force", "GTC").asString();
    order.requestSource = root.get("request_source", "API").asString();
    order.requestChannel = root.get("request_channel", "cpp_client").asString();
    order.baseTokenUsdConversionRate = root.get("base_token_usd_conversion_rate", 0.0).asDouble();
    order.quoteTokenUsdConversionRate = root.get("quote_token_usd_conversion_rate", 1.0).asDouble();
    
    // Optional fields
    if (root.isMember("stop_price") && !root["stop_price"].isNull()) {
        order.stopPrice = root["stop_price"].asDouble();
    }
    
    if (root.isMember("expiry_timestamp") && !root["expiry_timestamp"].isNull()) {
        order.expiryTimestamp = root["expiry_timestamp"].asInt64();
    }
    
    // Metadata
    if (root.isMember("metadata") && root["metadata"].isObject()) {
        const Json::Value& metadataJson = root["metadata"];
        for (const auto& key : metadataJson.getMemberNames()) {
            order.metadata[key] = metadataJson[key].asString();
        }
    }
    
    return order;
}

// Portfolio implementation

const Balance* Portfolio::getBalance(const std::string& token) const {
    auto it = std::find_if(balances.begin(), balances.end(),
        [&token](const Balance& b) { return b.token == token; });
    
    return (it != balances.end()) ? &(*it) : nullptr;
}

void Portfolio::updateBalance(const Balance& balance) {
    auto it = std::find_if(balances.begin(), balances.end(),
        [&balance](const Balance& b) { return b.token == balance.token; });
    
    if (it != balances.end()) {
        *it = balance;
    } else {
        balances.push_back(balance);
    }
    
    // Recalculate total USD value
    totalUsdValue = 0.0;
    for (const auto& bal : balances) {
        totalUsdValue += bal.usdValue;
    }
    
    timestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
}

// OrderFactory implementation

namespace OrderFactory {

Order createMarketBuyOrder(const std::string& baseToken,
                          const std::string& quoteToken,
                          double quantity,
                          int64_t accountId) {
    Order order;
    order.id = OrderUtils::generateOrderId("market_buy");
    order.baseToken = baseToken;
    order.quoteToken = quoteToken;
    order.side = "BUY";
    order.quantity = quantity;
    order.quantityToken = baseToken;
    order.orderType = "MARKET";
    order.accountID = accountId;
    order.status = "CREATED";
    order.tif = "IOC"; // Market orders are typically IOC
    order.initializeTimestamps();
    order.generateClientOrderUUID();
    
    return order;
}

Order createMarketSellOrder(const std::string& baseToken,
                           const std::string& quoteToken,
                           double quantity,
                           int64_t accountId) {
    Order order;
    order.id = OrderUtils::generateOrderId("market_sell");
    order.baseToken = baseToken;
    order.quoteToken = quoteToken;
    order.side = "SELL";
    order.quantity = quantity;
    order.quantityToken = baseToken;
    order.orderType = "MARKET";
    order.accountID = accountId;
    order.status = "CREATED";
    order.tif = "IOC"; // Market orders are typically IOC
    order.initializeTimestamps();
    order.generateClientOrderUUID();
    
    return order;
}

Order createLimitBuyOrder(const std::string& baseToken,
                         const std::string& quoteToken,
                         double quantity,
                         double limitPrice,
                         int64_t accountId) {
    Order order;
    order.id = OrderUtils::generateOrderId("limit_buy");
    order.baseToken = baseToken;
    order.quoteToken = quoteToken;
    order.side = "BUY";
    order.quantity = quantity;
    order.quantityToken = baseToken;
    order.limitPrice = limitPrice;
    order.limitPriceToken = quoteToken;
    order.orderType = "LIMIT";
    order.accountID = accountId;
    order.status = "CREATED";
    order.tif = "GTC";
    order.initializeTimestamps();
    order.generateClientOrderUUID();
    
    return order;
}

Order createLimitSellOrder(const std::string& baseToken,
                          const std::string& quoteToken,
                          double quantity,
                          double limitPrice,
                          int64_t accountId) {
    Order order;
    order.id = OrderUtils::generateOrderId("limit_sell");
    order.baseToken = baseToken;
    order.quoteToken = quoteToken;
    order.side = "SELL";
    order.quantity = quantity;
    order.quantityToken = baseToken;
    order.limitPrice = limitPrice;
    order.limitPriceToken = quoteToken;
    order.orderType = "LIMIT";
    order.accountID = accountId;
    order.status = "CREATED";
    order.tif = "GTC";
    order.initializeTimestamps();
    order.generateClientOrderUUID();
    
    return order;
}

Order createStopLossOrder(const std::string& baseToken,
                         const std::string& quoteToken,
                         double quantity,
                         double stopPrice,
                         int64_t accountId) {
    Order order;
    order.id = OrderUtils::generateOrderId("stop_loss");
    order.baseToken = baseToken;
    order.quoteToken = quoteToken;
    order.side = "SELL"; // Stop loss is typically a sell order
    order.quantity = quantity;
    order.quantityToken = baseToken;
    order.stopPrice = stopPrice;
    order.orderType = "STOP";
    order.accountID = accountId;
    order.status = "CREATED";
    order.tif = "GTC";
    order.initializeTimestamps();
    order.generateClientOrderUUID();
    
    return order;
}

Order createStopLimitOrder(const std::string& baseToken,
                          const std::string& quoteToken,
                          double quantity,
                          double stopPrice,
                          double limitPrice,
                          int64_t accountId) {
    Order order;
    order.id = OrderUtils::generateOrderId("stop_limit");
    order.baseToken = baseToken;
    order.quoteToken = quoteToken;
    order.side = "SELL"; // Stop limit is typically a sell order
    order.quantity = quantity;
    order.quantityToken = baseToken;
    order.stopPrice = stopPrice;
    order.limitPrice = limitPrice;
    order.limitPriceToken = quoteToken;
    order.orderType = "STOP_LIMIT";
    order.accountID = accountId;
    order.status = "CREATED";
    order.tif = "GTC";
    order.initializeTimestamps();
    order.generateClientOrderUUID();
    
    return order;
}

} // namespace OrderFactory

// OrderUtils implementation

namespace OrderUtils {

std::vector<std::string> validateOrder(const Order& order) {
    std::vector<std::string> errors;
    
    if (order.id.empty()) {
        errors.push_back("Order ID is required");
    }
    
    if (order.baseToken.empty()) {
        errors.push_back("Base token is required");
    }
    
    if (order.quoteToken.empty()) {
        errors.push_back("Quote token is required");
    }
    
    if (order.side != "BUY" && order.side != "SELL") {
        errors.push_back("Side must be 'BUY' or 'SELL'");
    }
    
    if (order.quantity <= 0.0) {
        errors.push_back("Quantity must be positive");
    }
    
    // Order type specific validation
    if (order.orderType == "LIMIT") {
        if (order.limitPrice <= 0.0) {
            errors.push_back("Limit price must be positive for LIMIT orders");
        }
    } else if (order.orderType == "STOP") {
        if (!order.stopPrice.has_value() || order.stopPrice.value() <= 0.0) {
            errors.push_back("Stop price must be positive for STOP orders");
        }
    } else if (order.orderType == "STOP_LIMIT") {
        if (!order.stopPrice.has_value() || order.stopPrice.value() <= 0.0) {
            errors.push_back("Stop price must be positive for STOP_LIMIT orders");
        }
        if (order.limitPrice <= 0.0) {
            errors.push_back("Limit price must be positive for STOP_LIMIT orders");
        }
    } else if (order.orderType != "MARKET") {
        errors.push_back("Unknown order type: " + order.orderType);
    }
    
    // Time in force validation
    if (order.tif != "GTC" && order.tif != "IOC" && order.tif != "FOK" && order.tif != "DAY" && order.tif != "GTD") {
        errors.push_back("Invalid time in force: " + order.tif);
    }
    
    if (order.tif == "GTD" && !order.expiryTimestamp.has_value()) {
        errors.push_back("Expiry timestamp required for GTD orders");
    }
    
    // Price relationship validation for stop orders
    if (order.orderType == "STOP_LIMIT" && order.stopPrice.has_value()) {
        if (order.side == "BUY" && order.stopPrice.value() <= order.limitPrice) {
            errors.push_back("For BUY stop-limit orders, stop price should be above limit price");
        } else if (order.side == "SELL" && order.stopPrice.value() >= order.limitPrice) {
            errors.push_back("For SELL stop-limit orders, stop price should be below limit price");
        }
    }
    
    return errors;
}

double calculateNotionalValue(const Order& order) {
    if (order.orderType == "MARKET") {
        // For market orders, we can't calculate exact notional without market price
        return 0.0;
    } else if (order.orderType == "LIMIT" || order.orderType == "STOP_LIMIT") {
        return order.quantity * order.limitPrice;
    } else if (order.orderType == "STOP" && order.stopPrice.has_value()) {
        return order.quantity * order.stopPrice.value();
    }
    
    return 0.0;
}

bool isBuyOrder(const Order& order) {
    return order.side == "BUY";
}

bool isSellOrder(const Order& order) {
    return order.side == "SELL";
}

bool isLimitOrder(const Order& order) {
    return order.orderType == "LIMIT";
}

bool isMarketOrder(const Order& order) {
    return order.orderType == "MARKET";
}

std::string generateOrderId(const std::string& prefix) {
    auto now = std::chrono::high_resolution_clock::now();
    int64_t timestamp = now.time_since_epoch().count();
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis(1000, 9999);
    
    return prefix + "_" + std::to_string(timestamp) + "_" + std::to_string(dis(gen));
}

std::string generateMessageId(const std::string& prefix) {
    auto now = std::chrono::high_resolution_clock::now();
    int64_t timestamp = now.time_since_epoch().count();
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis(10000, 99999);
    
    return prefix + "_" + std::to_string(timestamp) + "_" + std::to_string(dis(gen));
}

} // namespace OrderUtils

} // namespace aeron_cluster