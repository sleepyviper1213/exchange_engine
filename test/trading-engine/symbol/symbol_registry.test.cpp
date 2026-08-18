#include "symbol_spec.fixture.hpp"

#include "trading-engine/symbol.hpp"

#include <gtest/gtest.h>

#include <cstdint>

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::orders;

// symbol_registry - the listings the engine will trade, keyed by id.

namespace {

TEST(symbol_registry, UnknownSymbolIsRejectedBeforeAnythingIsParsed) {
	symbol_registry registry;
	registry.add(equity());

	const auto err = registry
						 .validate({.id       = 1,
									.symbol   = 999,
									.side     = side_t::bid,
									.price    = "not even a number",
									.quantity = "also not"})
						 .error();
	EXPECT_EQ(err, reject_reason::UNKNOWN_SYMBOL);
}

TEST(symbol_registry, FindsRegisteredListingsAndReplacesOnReAdd) {
	symbol_registry registry;
	registry.add(equity());
	ASSERT_NE(registry.find(1), nullptr);
	EXPECT_EQ(registry.find(1)->symbol(), "ACME");
	EXPECT_EQ(registry.find(2), nullptr);

	// An operator correcting the tick size mid-session.
	registry.add(symbol_spec{1, "ACME", 2, 0, 5, 1, 5000, 2000});
	EXPECT_EQ(registry.find(1)->tick_scaled(), 5);
	EXPECT_EQ(registry.by_id.size(), 1U);
}

TEST(symbol_registry, ValidatesThroughTheRegisteredSpec) {
	symbol_registry registry;
	registry.add(equity());

	const auto order = registry.validate({.id       = 3,
										  .symbol   = 1,
										  .side     = side_t::ask,
										  .price    = "50.01",
										  .quantity = "25"});
	ASSERT_TRUE(order.has_value());
	EXPECT_EQ(order->price, 5001U);
	EXPECT_EQ(order->qty, 25);
}

} // namespace
