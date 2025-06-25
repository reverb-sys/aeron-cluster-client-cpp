#include <gtest/gtest.h>
#include <aeron_cluster/order_types.hpp>

using namespace aeron_cluster;

class OrderTypesTest : public ::testing::Test {
protected:
    Order createValidOrder() {
        Order order;
        order.id = "test-order-123";
        order.baseToken = "ETH";
        order.quoteToken = "USDC";
        order.side = "BUY";
        order.quantity = 1.5;
        order.quantityToken = "ETH";
        order.limitPrice = 3500.0;
        order.limitPriceToken = "USDC";
        order.orderType = "LIMIT";
        order.tif = "GTC";
        order.status = "CREATED";
        order.accountID = 12345;
        order.initializeTimestamps();
        return order;
    }
};

TEST_F(OrderTypesTest, ValidOrderPassesValidation) {
    auto order = createValidOrder();
    EXPECT_TRUE(order.validate());
    
    auto errors = OrderUtils::validateOrder(order);
    EXPECT_TRUE(errors.empty());
}

TEST_F(OrderTypesTest, InvalidOrderFailsValidation) {
    auto order = createValidOrder();
    order.id.clear(); // Make invalid
    
    EXPECT_FALSE(order.validate());
    
    auto errors = OrderUtils::validateOrder(order);
    EXPECT_FALSE(errors.empty());
    EXPECT_THAT(errors, ::testing::Contains(::testing::HasSubstr("Order ID")));
}

TEST_F(OrderTypesTest, OrderFactoryCreatesValidOrders) {
    auto marketOrder = OrderFactory::createMarketBuyOrder("BTC", "USD", 0.1, 12345);
    EXPECT_TRUE(marketOrder.validate());
    EXPECT_EQ(marketOrder.orderType, "MARKET");
    EXPECT_EQ(marketOrder.side, "BUY");
    EXPECT_EQ(marketOrder.tif, "IOC");
    
    auto limitOrder = OrderFactory::createLimitSellOrder("ETH", "USDC", 2.0, 3400.0, 12345);
    EXPECT_TRUE(limitOrder.validate());
    EXPECT_EQ(limitOrder.orderType, "LIMIT");
    EXPECT_EQ(limitOrder.side, "SELL");
    EXPECT_EQ(limitOrder.limitPrice, 3400.0);
}

TEST_F(OrderTypesTest, JsonSerializationRoundTrip) {
    auto originalOrder = createValidOrder();
    originalOrder.generateClientOrderUUID();
    
    std::string json = originalOrder.toJsonString();
    EXPECT_FALSE(json.empty());
    
    auto deserializedOrder = Order::fromJsonString(json);
    
    EXPECT_EQ(originalOrder.id, deserializedOrder.id);
    EXPECT_EQ(originalOrder.baseToken, deserializedOrder.baseToken);
    EXPECT_EQ(originalOrder.quoteToken, deserializedOrder.quoteToken);
    EXPECT_EQ(originalOrder.side, deserializedOrder.side);
    EXPECT_DOUBLE_EQ(originalOrder.quantity, deserializedOrder.quantity);
    EXPECT_DOUBLE_EQ(originalOrder.limitPrice, deserializedOrder.limitPrice);
    EXPECT_EQ(originalOrder.clientOrderUUID, deserializedOrder.clientOrderUUID);
}

TEST_F(OrderTypesTest, NotionalValueCalculation) {
    auto order = createValidOrder();
    double expectedNotional = order.quantity * order.limitPrice;
    
    EXPECT_DOUBLE_EQ(OrderUtils::calculateNotionalValue(order), expectedNotional);
}

TEST_F(OrderTypesTest, OrderTypeCheckers) {
    auto limitOrder = createValidOrder();
    EXPECT_TRUE(OrderUtils::isLimitOrder(limitOrder));
    EXPECT_FALSE(OrderUtils::isMarketOrder(limitOrder));
    EXPECT_TRUE(OrderUtils::isBuyOrder(limitOrder));
    EXPECT_FALSE(OrderUtils::isSellOrder(limitOrder));
    
    auto marketOrder = OrderFactory::createMarketSellOrder("BTC", "USD", 0.5);
    EXPECT_TRUE(OrderUtils::isMarketOrder(marketOrder));
    EXPECT_FALSE(OrderUtils::isLimitOrder(marketOrder));
    EXPECT_FALSE(OrderUtils::isBuyOrder(marketOrder));
    EXPECT_TRUE(OrderUtils::isSellOrder(marketOrder));
}