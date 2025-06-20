#pragma once

#include <string>
#include <chrono>
#include <optional>
#include <vector>
#include <unordered_map>

namespace aeron_cluster {

/**
 * @brief Trading order structure matching cluster service expectations
 * 
 * This structure represents a complete trading order with all fields
 * that might be required by a financial trading cluster service.
 */
struct Order {
    /**
     * @brief Unique order identifier
     * 
     * Should be unique across all orders for this client session.
     * Typically UUID or timestamp-based identifier.
     */
    std::string id;

    /**
     * @brief Base token symbol (e.g., "ETH", "BTC")
     */
    std::string baseToken;

    /**
     * @brief Quote token symbol (e.g., "USDC", "USD")
     */
    std::string quoteToken;

    /**
     * @brief Order side: "BUY" or "SELL"
     */
    std::string side;

    /**
     * @brief Order quantity amount
     */
    double quantity = 0.0;

    /**
     * @brief Token symbol for the quantity (usually same as baseToken)
     */
    std::string quantityToken;

    /**
     * @brief Limit price (for LIMIT orders)
     */
    double limitPrice = 0.0;

    /**
     * @brief Token symbol for the limit price (usually same as quoteToken)
     */
    std::string limitPriceToken;

    /**
     * @brief Customer identifier
     */
    int64_t customerID = 0;

    /**
     * @brief User identifier
     */
    int64_t userID = 0;

    /**
     * @brief Account identifier
     */
    int64_t accountID = 0;

    /**
     * @brief Order status: "CREATED", "PENDING", "FILLED", "CANCELLED", etc.
     */
    std::string status = "CREATED";

    /**
     * @brief Order type: "LIMIT", "MARKET", "STOP", "STOP_LIMIT"
     */
    std::string orderType = "LIMIT";

    /**
     * @brief Order creation timestamp (nanoseconds since epoch)
     */
    int64_t timestamp = 0;

    /**
     * @brief Last update timestamp (nanoseconds since epoch)
     */
    int64_t updatedAt = 0;

    /**
     * @brief Client-side order UUID for tracking
     */
    std::string clientOrderUUID;

    /**
     * @brief Time in force: "GTC", "IOC", "FOK", "DAY"
     */
    std::string tif = "GTC";

    /**
     * @brief Request source identifier (e.g., "API", "UI", "FIX")
     */
    std::string requestSource = "API";

    /**
     * @brief Request channel identifier
     */
    std::string requestChannel = "cpp_client";

    /**
     * @brief USD conversion rate for base token
     */
    double baseTokenUsdConversionRate = 0.0;

    /**
     * @brief USD conversion rate for quote token
     */
    double quoteTokenUsdConversionRate = 0.0;

    /**
     * @brief Stop price (for STOP and STOP_LIMIT orders)
     */
    std::optional<double> stopPrice;

    /**
     * @brief Expiry timestamp for GTD (Good Till Date) orders
     */
    std::optional<int64_t> expiryTimestamp;

    /**
     * @brief Additional order metadata as key-value pairs
     */
    std::unordered_map<std::string, std::string> metadata;

    /**
     * @brief Initialize timestamps to current time
     */
    void initializeTimestamps() {
        auto now = std::chrono::high_resolution_clock::now();
        int64_t nanos = now.time_since_epoch().count();
        timestamp = nanos;
        updatedAt = nanos;
    }

    /**
     * @brief Update the updatedAt timestamp to current time
     */
    void updateTimestamp() {
        auto now = std::chrono::high_resolution_clock::now();
        updatedAt = now.time_since_epoch().count();
    }

    /**
     * @brief Generate a client order UUID if not already set
     */
    void generateClientOrderUUID();

    /**
     * @brief Validate order fields for completeness and consistency
     * @return true if order is valid, false otherwise
     */
    bool validate() const;

    /**
     * @brief Get order as JSON string for debugging
     * @return JSON representation of the order
     */
    std::string toJsonString() const;

    /**
     * @brief Create order from JSON string
     * @param jsonStr JSON string representation
     * @return Order object parsed from JSON
     * @throws std::runtime_error on parsing errors
     */
    static Order fromJsonString(const std::string& jsonStr);
};

/**
 * @brief Order execution report from cluster
 */
struct OrderExecutionReport {
    /**
     * @brief Original order ID
     */
    std::string orderId;

    /**
     * @brief Client order UUID for correlation
     */
    std::string clientOrderUUID;

    /**
     * @brief Execution ID for this fill
     */
    std::string executionId;

    /**
     * @brief Execution type: "NEW", "PARTIAL_FILL", "FILL", "CANCELLED", "REJECTED"
     */
    std::string executionType;

    /**
     * @brief Current order status
     */
    std::string orderStatus;

    /**
     * @brief Execution price
     */
    double executionPrice = 0.0;

    /**
     * @brief Executed quantity
     */
    double executedQuantity = 0.0;

    /**
     * @brief Remaining quantity
     */
    double remainingQuantity = 0.0;

    /**
     * @brief Cumulative executed quantity
     */
    double cumulativeQuantity = 0.0;

    /**
     * @brief Average execution price
     */
    double averagePrice = 0.0;

    /**
     * @brief Execution timestamp
     */
    int64_t executionTimestamp = 0;

    /**
     * @brief Commission charged
     */
    double commission = 0.0;

    /**
     * @brief Commission currency
     */
    std::string commissionCurrency;

    /**
     * @brief Rejection reason (if applicable)
     */
    std::string rejectReason;

    /**
     * @brief Trading venue/exchange identifier
     */
    std::string venue;

    /**
     * @brief Counterparty information
     */
    std::string counterparty;

    /**
     * @brief Additional execution metadata
     */
    std::unordered_map<std::string, std::string> metadata;
};

/**
 * @brief Market data snapshot for a trading pair
 */
struct MarketDataSnapshot {
    /**
     * @brief Trading pair symbol (e.g., "ETH-USDC")
     */
    std::string symbol;

    /**
     * @brief Best bid price
     */
    double bidPrice = 0.0;

    /**
     * @brief Best ask price
     */
    double askPrice = 0.0;

    /**
     * @brief Best bid quantity
     */
    double bidQuantity = 0.0;

    /**
     * @brief Best ask quantity
     */
    double askQuantity = 0.0;

    /**
     * @brief Last trade price
     */
    double lastPrice = 0.0;

    /**
     * @brief Last trade quantity
     */
    double lastQuantity = 0.0;

    /**
     * @brief 24h volume
     */
    double volume24h = 0.0;

    /**
     * @brief 24h price change
     */
    double priceChange24h = 0.0;

    /**
     * @brief Timestamp of this snapshot
     */
    int64_t timestamp = 0;
};

/**
 * @brief Portfolio balance information
 */
struct Balance {
    /**
     * @brief Token symbol
     */
    std::string token;

    /**
     * @brief Available balance (not in orders)
     */
    double available = 0.0;

    /**
     * @brief Total balance (available + in orders)
     */
    double total = 0.0;

    /**
     * @brief Amount currently in orders
     */
    double inOrders = 0.0;

    /**
     * @brief USD value of this balance
     */
    double usdValue = 0.0;

    /**
     * @brief Last update timestamp
     */
    int64_t lastUpdated = 0;
};

/**
 * @brief Portfolio snapshot with all balances
 */
struct Portfolio {
    /**
     * @brief Account identifier
     */
    int64_t accountId = 0;

    /**
     * @brief All token balances
     */
    std::vector<Balance> balances;

    /**
     * @brief Total portfolio USD value
     */
    double totalUsdValue = 0.0;

    /**
     * @brief Portfolio snapshot timestamp
     */
    int64_t timestamp = 0;

    /**
     * @brief Get balance for specific token
     * @param token Token symbol to find
     * @return Pointer to balance, or nullptr if not found
     */
    const Balance* getBalance(const std::string& token) const;

    /**
     * @brief Update or add balance for a token
     * @param balance New balance information
     */
    void updateBalance(const Balance& balance);
};

/**
 * @brief Factory functions for creating common order types
 */
namespace OrderFactory {

/**
 * @brief Create a market buy order
 */
Order createMarketBuyOrder(const std::string& baseToken,
                          const std::string& quoteToken,
                          double quantity,
                          int64_t accountId = 0);

/**
 * @brief Create a market sell order
 */
Order createMarketSellOrder(const std::string& baseToken,
                           const std::string& quoteToken,
                           double quantity,
                           int64_t accountId = 0);

/**
 * @brief Create a limit buy order
 */
Order createLimitBuyOrder(const std::string& baseToken,
                         const std::string& quoteToken,
                         double quantity,
                         double limitPrice,
                         int64_t accountId = 0);

/**
 * @brief Create a limit sell order
 */
Order createLimitSellOrder(const std::string& baseToken,
                          const std::string& quoteToken,
                          double quantity,
                          double limitPrice,
                          int64_t accountId = 0);

/**
 * @brief Create a stop loss order
 */
Order createStopLossOrder(const std::string& baseToken,
                         const std::string& quoteToken,
                         double quantity,
                         double stopPrice,
                         int64_t accountId = 0);

/**
 * @brief Create a stop limit order
 */
Order createStopLimitOrder(const std::string& baseToken,
                          const std::string& quoteToken,
                          double quantity,
                          double stopPrice,
                          double limitPrice,
                          int64_t accountId = 0);

} // namespace OrderFactory

/**
 * @brief Utility functions for order validation and manipulation
 */
namespace OrderUtils {

/**
 * @brief Validate order fields for common errors
 * @param order Order to validate
 * @return Vector of validation error messages (empty if valid)
 */
std::vector<std::string> validateOrder(const Order& order);

/**
 * @brief Calculate order notional value (quantity * price)
 * @param order Order to calculate for
 * @return Notional value in quote token units
 */
double calculateNotionalValue(const Order& order);

/**
 * @brief Check if order is a buy order
 */
bool isBuyOrder(const Order& order);

/**
 * @brief Check if order is a sell order
 */
bool isSellOrder(const Order& order);

/**
 * @brief Check if order is a limit order
 */
bool isLimitOrder(const Order& order);

/**
 * @brief Check if order is a market order
 */
bool isMarketOrder(const Order& order);

/**
 * @brief Generate a unique order ID
 * @param prefix Optional prefix for the ID
 * @return Unique order identifier
 */
std::string generateOrderId(const std::string& prefix = "order");

/**
 * @brief Generate a unique message ID for tracking
 * @param prefix Optional prefix for the ID
 * @return Unique message identifier
 */
std::string generateMessageId(const std::string& prefix = "msg");

} // namespace OrderUtils

} // namespace aeron_cluster