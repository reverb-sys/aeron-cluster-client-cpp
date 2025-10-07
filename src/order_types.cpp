#include "aeron_cluster/order_types.hpp"
#include <json/json.h>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <stdexcept>

namespace aeron_cluster {

// Order implementation
Order::Order(const std::string& base, const std::string& quote, 
             const std::string& side, double qty, const std::string& type)
    : base_token(base)
    , quote_token(quote)
    , side(side)
    , quantity(qty)
    , order_type(type)
{
    quantity_token = base_token;
    limit_price_token = quote_token;
    initialize_timestamps();
    generate_client_order_uuid();
}

void Order::initialize_timestamps() {
    auto now = std::chrono::high_resolution_clock::now();
    std::int64_t nanos = now.time_since_epoch().count();
    timestamp = nanos;
    updated_at = nanos;
}

void Order::update_timestamp() {
    auto now = std::chrono::high_resolution_clock::now();
    updated_at = now.time_since_epoch().count();
}

void Order::generate_client_order_uuid() {
    if (!client_order_uuid.empty()) {
        return; // Already has UUID
    }
    
    // Generate a simple UUID-like string
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::uint32_t> dis;
    
    std::stringstream ss;
    ss << std::hex 
       << dis(gen) << "-" 
       << (dis(gen) & 0xFFFF) << "-" 
       << (dis(gen) & 0xFFFF) << "-" 
       << (dis(gen) & 0xFFFF) << "-" 
       << dis(gen) << dis(gen);
    
    client_order_uuid = ss.str();
}

std::vector<std::string> Order::validate() const {
    std::vector<std::string> errors;
    
    if (id.empty()) {
        errors.push_back("Order ID is required");
    }
    
    if (base_token.empty()) {
        errors.push_back("Base token is required");
    }
    
    if (quote_token.empty()) {
        errors.push_back("Quote token is required");
    }
    
    if (!OrderUtils::is_valid_side(side)) {
        errors.push_back("Side must be 'BUY' or 'SELL'");
    }
    
    if (quantity <= 0.0) {
        errors.push_back("Quantity must be positive");
    }
    
    if (!OrderUtils::is_valid_order_type(order_type)) {
        errors.push_back("Invalid order type: " + order_type);
    }
    
    // Order type specific validation
    if (order_type == "LIMIT" && limit_price <= 0.0) {
        errors.push_back("Limit price must be positive for LIMIT orders");
    }
    
    if (order_type == "STOP" && (!stop_price.has_value() || stop_price.value() <= 0.0)) {
        errors.push_back("Stop price must be positive for STOP orders");
    }
    
    if (order_type == "STOP_LIMIT") {
        if (!stop_price.has_value() || stop_price.value() <= 0.0) {
            errors.push_back("Stop price must be positive for STOP_LIMIT orders");
        }
        if (limit_price <= 0.0) {
            errors.push_back("Limit price must be positive for STOP_LIMIT orders");
        }
    }
    
    if (!OrderUtils::is_valid_time_in_force(time_in_force)) {
        errors.push_back("Invalid time in force: " + time_in_force);
    }
    
    if (time_in_force == "GTD" && !expiry_timestamp.has_value()) {
        errors.push_back("Expiry timestamp required for GTD orders");
    }
    
    return errors;
}

std::string Order::to_json() const {
    // Build the nested message structure with headers and message objects
    Json::Value root;
    
    // Top level structure
    root["uuid"] = client_order_uuid;
    root["msg_type"] = "D";
    
    // Nested message structure
    Json::Value nested_message;
    
    // Headers
    Json::Value headers;
    headers["origin"] = "fix";
    headers["origin_name"] = "FIX_GATEWAY";
    headers["origin_id"] = "SEKAR_AERON01_TX";
    headers["connection_uuid"] = "130032";
    headers["customer_id"] = std::to_string(customer_id);
    headers["ip_address"] = "10.37.62.251";
    headers["create_ts"] = std::to_string(timestamp / 1000000); // Convert to milliseconds
    headers["auth_token"] = "Bearer xxx";
    
    // Message content
    Json::Value message_content;
    message_content["action"] = "CREATE";
    
    // Order details
    Json::Value order_details;
    
    // Token pair
    Json::Value token_pair;
    token_pair["base_token"] = base_token;
    token_pair["quote_token"] = quote_token;
    order_details["token_pair"] = token_pair;
    
    // Quantity
    Json::Value quantity_obj;
    quantity_obj["token"] = base_token;
    quantity_obj["value"] = quantity;
    order_details["quantity"] = quantity_obj;
    
    // Other order details
    order_details["side"] = side;
    order_details["order_type"] = "market";
    order_details["quantity_value_str"] = std::to_string(quantity);
    order_details["client_order_id"] = client_order_uuid;
    
    message_content["order_details"] = order_details;
    
    // Assemble the nested message structure
    nested_message["headers"] = headers;
    nested_message["message"] = message_content;
    
    // Final message structure
    root["message"] = nested_message;
    
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, root);
}

Order Order::from_json(const std::string& json_str) {
    Json::Reader reader;
    Json::Value root;
    
    if (!reader.parse(json_str, root)) {
        throw std::runtime_error("Failed to parse JSON: " + reader.getFormattedErrorMessages());
    }
    
    Order order;
    
    // Required fields
    order.id = root.get("id", "").asString();
    order.base_token = root.get("base_token", "").asString();
    order.quote_token = root.get("quote_token", "").asString();
    order.side = root.get("side", "").asString();
    order.quantity = root.get("quantity", 0.0).asDouble();
    order.order_type = root.get("order_type", "LIMIT").asString();
    
    // Optional fields with defaults
    order.quantity_token = root.get("quantity_token", order.base_token).asString();
    order.limit_price = root.get("limit_price", 0.0).asDouble();
    order.limit_price_token = root.get("limit_price_token", order.quote_token).asString();
    order.customer_id = root.get("customer_id", 0).asInt64();
    order.user_id = root.get("user_id", 0).asInt64();
    order.account_id = root.get("account_id", 0).asInt64();
    order.status = root.get("status", "CREATED").asString();
    order.timestamp = root.get("timestamp", 0).asInt64();
    order.updated_at = root.get("updated_at", 0).asInt64();
    order.client_order_uuid = root.get("client_order_uuid", "").asString();
    order.time_in_force = root.get("time_in_force", "GTC").asString();
    order.request_source = root.get("request_source", "API").asString();
    order.request_channel = root.get("request_channel", "cpp_client").asString();
    order.base_token_usd_rate = root.get("base_token_usd_rate", 0.0).asDouble();
    order.quote_token_usd_rate = root.get("quote_token_usd_rate", 1.0).asDouble();
    
    // Optional fields
    if (root.isMember("stop_price") && !root["stop_price"].isNull()) {
        order.stop_price = root["stop_price"].asDouble();
    }
    
    if (root.isMember("expiry_timestamp") && !root["expiry_timestamp"].isNull()) {
        order.expiry_timestamp = root["expiry_timestamp"].asInt64();
    }
    
    // Metadata
    if (root.isMember("metadata") && root["metadata"].isObject()) {
        const Json::Value& metadata_json = root["metadata"];
        for (const auto& key : metadata_json.getMemberNames()) {
            order.metadata[key] = metadata_json[key].asString();
        }
    }
    
    return order;
}

double Order::calculate_notional_value() const {
    if (order_type == "MARKET") {
        // For market orders, we can't calculate exact notional without market price
        return 0.0;
    } else if (order_type == "LIMIT" || order_type == "STOP_LIMIT") {
        return quantity * limit_price;
    } else if (order_type == "STOP" && stop_price.has_value()) {
        return quantity * stop_price.value();
    }
    
    return 0.0;
}

// Portfolio implementation
const Balance* Portfolio::get_balance(const std::string& token) const {
    auto it = std::find_if(balances.begin(), balances.end(),
        [&token](const Balance& b) { return b.token == token; });
    
    return (it != balances.end()) ? &(*it) : nullptr;
}

void Portfolio::update_balance(const Balance& balance) {
    auto it = std::find_if(balances.begin(), balances.end(),
        [&balance](const Balance& b) { return b.token == balance.token; });
    
    if (it != balances.end()) {
        *it = balance;
    } else {
        balances.push_back(balance);
    }
    
    recalculate_total_value();
    timestamp = OrderUtils::get_current_timestamp_nanos();
}

bool Portfolio::remove_balance(const std::string& token) {
    auto it = std::find_if(balances.begin(), balances.end(),
        [&token](const Balance& b) { return b.token == token; });
    
    if (it != balances.end()) {
        balances.erase(it);
        recalculate_total_value();
        return true;
    }
    
    return false;
}

std::vector<std::string> Portfolio::get_tokens() const {
    std::vector<std::string> tokens;
    tokens.reserve(balances.size());
    
    for (const auto& balance : balances) {
        tokens.push_back(balance.token);
    }
    
    return tokens;
}

bool Portfolio::can_place_order(const Order& order) const {
    if (order.is_buy()) {
        // For buy orders, need quote token balance
        const Balance* balance = get_balance(order.quote_token);
        if (!balance) return false;
        
        double required = order.calculate_notional_value();
        return balance->is_sufficient(required);
    } else {
        // For sell orders, need base token balance
        const Balance* balance = get_balance(order.base_token);
        if (!balance) return false;
        
        return balance->is_sufficient(order.quantity);
    }
}

void Portfolio::recalculate_total_value() {
    total_usd_value = 0.0;
    for (const auto& balance : balances) {
        total_usd_value += balance.usd_value;
    }
}

// OrderFactory implementation
namespace OrderFactory {

std::unique_ptr<Order> create_market_buy(
    const std::string& base_token,
    const std::string& quote_token,
    double quantity,
    std::int64_t account_id) {
    
    auto order = std::make_unique<Order>(base_token, quote_token, "BUY", quantity, "MARKET");
    order->id = OrderUtils::generate_order_id("market_buy");
    order->account_id = account_id;
    order->time_in_force = "IOC"; // Market orders are typically IOC
    return order;
}

std::unique_ptr<Order> create_market_sell(
    const std::string& base_token,
    const std::string& quote_token,
    double quantity,
    std::int64_t account_id) {
    
    auto order = std::make_unique<Order>(base_token, quote_token, "SELL", quantity, "MARKET");
    order->id = OrderUtils::generate_order_id("market_sell");
    order->account_id = account_id;
    order->time_in_force = "IOC"; // Market orders are typically IOC
    return order;
}

std::unique_ptr<Order> create_limit_buy(
    const std::string& base_token,
    const std::string& quote_token,
    double quantity,
    double limit_price,
    std::int64_t account_id) {
    
    auto order = std::make_unique<Order>(base_token, quote_token, "BUY", quantity, "LIMIT");
    order->id = OrderUtils::generate_order_id("limit_buy");
    order->limit_price = limit_price;
    order->account_id = account_id;
    order->time_in_force = "GTC";
    return order;
}

std::unique_ptr<Order> create_limit_sell(
    const std::string& base_token,
    const std::string& quote_token,
    double quantity,
    double limit_price,
    std::int64_t account_id) {
    
    auto order = std::make_unique<Order>(base_token, quote_token, "SELL", quantity, "LIMIT");
    order->id = OrderUtils::generate_order_id("limit_sell");
    order->limit_price = limit_price;
    order->account_id = account_id;
    order->time_in_force = "GTC";
    return order;
}

std::unique_ptr<Order> create_stop_loss(
    const std::string& base_token,
    const std::string& quote_token,
    double quantity,
    double stop_price,
    std::int64_t account_id) {
    
    auto order = std::make_unique<Order>(base_token, quote_token, "SELL", quantity, "STOP");
    order->id = OrderUtils::generate_order_id("stop_loss");
    order->stop_price = stop_price;
    order->account_id = account_id;
    order->time_in_force = "GTC";
    return order;
}

std::unique_ptr<Order> create_stop_limit(
    const std::string& base_token,
    const std::string& quote_token,
    double quantity,
    double stop_price,
    double limit_price,
    std::int64_t account_id) {
    
    auto order = std::make_unique<Order>(base_token, quote_token, "SELL", quantity, "STOP_LIMIT");
    order->id = OrderUtils::generate_order_id("stop_limit");
    order->stop_price = stop_price;
    order->limit_price = limit_price;
    order->account_id = account_id;
    order->time_in_force = "GTC";
    return order;
}

} // namespace OrderFactory

// OrderUtils implementation
namespace OrderUtils {

std::string generate_order_id(const std::string& prefix) {
    auto now = std::chrono::high_resolution_clock::now();
    std::int64_t timestamp = now.time_since_epoch().count();
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::uint32_t> dis(1000, 9999);
    
    return prefix + "_" + std::to_string(timestamp) + "_" + std::to_string(dis(gen));
}

std::string generate_message_id(const std::string& prefix) {
    auto now = std::chrono::high_resolution_clock::now();
    std::int64_t timestamp = now.time_since_epoch().count();
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::uint32_t> dis(10000, 99999);
    
    return prefix + "_" + std::to_string(timestamp) + "_" + std::to_string(dis(gen));
}

std::string generate_uuid() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::uint32_t> dis;
    
    std::stringstream ss;
    ss << std::hex 
       << dis(gen) << "-" 
       << (dis(gen) & 0xFFFF) << "-" 
       << (4000 | (dis(gen) & 0x0FFF)) << "-" // Version 4 UUID
       << (0x8000 | (dis(gen) & 0x3FFF)) << "-" // Variant bits
       << dis(gen) << (dis(gen) & 0xFFFF);
    
    return ss.str();
}

bool is_valid_side(const std::string& side) {
    return side == "BUY" || side == "SELL";
}

bool is_valid_order_type(const std::string& order_type) {
    return order_type == "MARKET" || order_type == "LIMIT" || 
           order_type == "STOP" || order_type == "STOP_LIMIT";
}

bool is_valid_time_in_force(const std::string& tif) {
    return tif == "GTC" || tif == "IOC" || tif == "FOK" || 
           tif == "DAY" || tif == "GTD";
}

std::int64_t get_current_timestamp_nanos() {
    auto now = std::chrono::high_resolution_clock::now();
    return now.time_since_epoch().count();
}

} // namespace OrderUtils

} // namespace aeron_cluster