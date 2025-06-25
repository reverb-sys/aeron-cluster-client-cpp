#pragma once

#include <string>
#include <chrono>
#include <optional>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>

namespace aeron_cluster {

/**
 * @brief Trading order structure for financial trading systems
 */
class Order {
public:
    // Core order identification
    std::string id;
    std::string client_order_uuid;
    
    // Trading pair
    std::string base_token;
    std::string quote_token;
    
    // Order details
    std::string side; // "BUY" or "SELL"
    double quantity = 0.0;
    std::string quantity_token;
    
    // Pricing
    double limit_price = 0.0;
    std::string limit_price_token;
    std::optional<double> stop_price;
    
    // Account information
    std::int64_t customer_id = 0;
    std::int64_t user_id = 0;
    std::int64_t account_id = 0;
    
    // Order state
    std::string status = "CREATED";
    std::string order_type = "LIMIT";
    std::string time_in_force = "GTC";
    
    // Timestamps (nanoseconds since epoch)
    std::int64_t timestamp = 0;
    std::int64_t updated_at = 0;
    std::optional<std::int64_t> expiry_timestamp;
    
    // Request tracking
    std::string request_source = "API";
    std::string request_channel = "cpp_client";
    
    // USD conversion rates
    double base_token_usd_rate = 0.0;
    double quote_token_usd_rate = 0.0;
    
    // Additional metadata
    std::unordered_map<std::string, std::string> metadata;

    /**
     * @brief Default constructor
     */
    Order() = default;

    /**
     * @brief Constructor with basic order parameters
     */
    Order(const std::string& base, const std::string& quote, 
          const std::string& side, double qty, const std::string& type = "LIMIT");

    /**
     * @brief Initialize timestamps to current time
     */
    void initialize_timestamps();

    /**
     * @brief Update the updated_at timestamp
     */
    void update_timestamp();

    /**
     * @brief Generate a client order UUID if not set
     */
    void generate_client_order_uuid();

    /**
     * @brief Validate order fields for completeness and consistency
     * @return Vector of validation errors (empty if valid)
     */
    std::vector<std::string> validate() const;

    /**
     * @brief Check if order is valid
     * @return true if order passes all validation checks
     */
    bool is_valid() const { return validate().empty(); }

    /**
     * @brief Serialize order to JSON string
     * @return JSON representation of the order
     */
    std::string to_json() const;

    /**
     * @brief Deserialize order from JSON string
     * @param json_str JSON string representation
     * @return Order object parsed from JSON
     * @throws std::runtime_error on parsing errors
     */
    static Order from_json(const std::string& json_str);

    /**
     * @brief Calculate notional value (quantity * price)
     * @return Notional value in quote token units
     */
    double calculate_notional_value() const;

    /**
     * @brief Check if this is a buy order
     */
    bool is_buy() const { return side == "BUY"; }

    /**
     * @brief Check if this is a sell order
     */
    bool is_sell() const { return side == "SELL"; }

    /**
     * @brief Check if this is a limit order
     */
    bool is_limit() const { return order_type == "LIMIT"; }

    /**
     * @brief Check if this is a market order
     */
    bool is_market() const { return order_type == "MARKET"; }

    /**
     * @brief Check if this is a stop order
     */
    bool is_stop() const { return order_type == "STOP" || order_type == "STOP_LIMIT"; }
};

/**
 * @brief Order execution report from cluster
 */
struct OrderExecutionReport {
    std::string order_id;
    std::string client_order_uuid;
    std::string execution_id;
    std::string execution_type; // "NEW", "PARTIAL_FILL", "FILL", "CANCELLED", "REJECTED"
    std::string order_status;
    
    double execution_price = 0.0;
    double executed_quantity = 0.0;
    double remaining_quantity = 0.0;
    double cumulative_quantity = 0.0;
    double average_price = 0.0;
    
    std::int64_t execution_timestamp = 0;
    
    double commission = 0.0;
    std::string commission_currency;
    std::string reject_reason;
    std::string venue;
    std::string counterparty;
    
    std::unordered_map<std::string, std::string> metadata;
};

/**
 * @brief Market data snapshot for a trading pair
 */
struct MarketDataSnapshot {
    std::string symbol;
    double bid_price = 0.0;
    double ask_price = 0.0;
    double bid_quantity = 0.0;
    double ask_quantity = 0.0;
    double last_price = 0.0;
    double last_quantity = 0.0;
    double volume_24h = 0.0;
    double price_change_24h = 0.0;
    std::int64_t timestamp = 0;

    /**
     * @brief Get mid price (average of bid and ask)
     */
    double get_mid_price() const {
        return (bid_price + ask_price) / 2.0;
    }

    /**
     * @brief Get spread
     */
    double get_spread() const {
        return ask_price - bid_price;
    }

    /**
     * @brief Get spread as percentage
     */
    double get_spread_percentage() const {
        double mid = get_mid_price();
        return mid > 0.0 ? (get_spread() / mid) * 100.0 : 0.0;
    }
};

/**
 * @brief Portfolio balance information
 */
struct Balance {
    std::string token;
    double available = 0.0;
    double total = 0.0;
    double in_orders = 0.0;
    double usd_value = 0.0;
    std::int64_t last_updated = 0;

    /**
     * @brief Check if balance is sufficient for order
     */
    bool is_sufficient(double required_amount) const {
        return available >= required_amount;
    }
};

/**
 * @brief Portfolio snapshot with all balances
 */
class Portfolio {
public:
    std::int64_t account_id = 0;
    std::vector<Balance> balances;
    double total_usd_value = 0.0;
    std::int64_t timestamp = 0;

    /**
     * @brief Get balance for specific token
     * @param token Token symbol to find
     * @return Pointer to balance, or nullptr if not found
     */
    const Balance* get_balance(const std::string& token) const;

    /**
     * @brief Update or add balance for a token
     * @param balance New balance information
     */
    void update_balance(const Balance& balance);

    /**
     * @brief Remove balance for a token
     * @param token Token to remove
     * @return true if balance was found and removed
     */
    bool remove_balance(const std::string& token);

    /**
     * @brief Get all token symbols
     */
    std::vector<std::string> get_tokens() const;

    /**
     * @brief Check if portfolio has sufficient balance for order
     */
    bool can_place_order(const Order& order) const;

private:
    void recalculate_total_value();
};

/**
 * @brief Factory functions for creating common order types
 */
namespace OrderFactory {

/**
 * @brief Create a market buy order
 */
std::unique_ptr<Order> create_market_buy(
    const std::string& base_token,
    const std::string& quote_token,
    double quantity,
    std::int64_t account_id = 0);

/**
 * @brief Create a market sell order
 */
std::unique_ptr<Order> create_market_sell(
    const std::string& base_token,
    const std::string& quote_token,
    double quantity,
    std::int64_t account_id = 0);

/**
 * @brief Create a limit buy order
 */
std::unique_ptr<Order> create_limit_buy(
    const std::string& base_token,
    const std::string& quote_token,
    double quantity,
    double limit_price,
    std::int64_t account_id = 0);

/**
 * @brief Create a limit sell order
 */
std::unique_ptr<Order> create_limit_sell(
    const std::string& base_token,
    const std::string& quote_token,
    double quantity,
    double limit_price,
    std::int64_t account_id = 0);

/**
 * @brief Create a stop loss order
 */
std::unique_ptr<Order> create_stop_loss(
    const std::string& base_token,
    const std::string& quote_token,
    double quantity,
    double stop_price,
    std::int64_t account_id = 0);

/**
 * @brief Create a stop limit order
 */
std::unique_ptr<Order> create_stop_limit(
    const std::string& base_token,
    const std::string& quote_token,
    double quantity,
    double stop_price,
    double limit_price,
    std::int64_t account_id = 0);

} // namespace OrderFactory

/**
 * @brief Utility functions for order validation and manipulation
 */
namespace OrderUtils {

/**
 * @brief Generate a unique order ID
 * @param prefix Optional prefix for the ID
 * @return Unique order identifier
 */
std::string generate_order_id(const std::string& prefix = "order");

/**
 * @brief Generate a unique message ID for tracking
 * @param prefix Optional prefix for the ID
 * @return Unique message identifier
 */
std::string generate_message_id(const std::string& prefix = "msg");

/**
 * @brief Generate a UUID string
 * @return UUID string
 */
std::string generate_uuid();

/**
 * @brief Validate order side
 * @param side Order side to validate
 * @return true if valid ("BUY" or "SELL")
 */
bool is_valid_side(const std::string& side);

/**
 * @brief Validate order type
 * @param order_type Order type to validate
 * @return true if valid
 */
bool is_valid_order_type(const std::string& order_type);

/**
 * @brief Validate time in force
 * @param tif Time in force to validate
 * @return true if valid
 */
bool is_valid_time_in_force(const std::string& tif);

/**
 * @brief Get current timestamp in nanoseconds
 * @return Current timestamp
 */
std::int64_t get_current_timestamp_nanos();

} // namespace OrderUtils

} // namespace aeron_cluster