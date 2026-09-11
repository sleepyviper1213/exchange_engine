#include "session/client_order_id.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <string>

// The only key a venue gives back for an order we placed.
//
// Every property here is one reconciliation depends on: that our ids
// round-trip, that a near-miss does not, and that an order somebody placed by
// hand is recognisably not ours.

using exchange::order_id_t;
using exchange::session::cancel_order_id;
using exchange::session::client_order_id;
using exchange::session::CLIENT_ORDER_ID_MAX;
using exchange::session::CLIENT_ORDER_PREFIX;
using exchange::session::engine_order_id;
using exchange::session::is_cancel_id;
using exchange::session::is_ours;

TEST(ClientOrderId, RoundTripsAnyEngineOrderId) {
	for (const order_id_t id : {order_id_t{1},
								order_id_t{42},
								order_id_t{1'000'000},
								std::numeric_limits<order_id_t>::max()}) {
		const std::string text = client_order_id(id);
		EXPECT_EQ(engine_order_id(text), id) << text;
	}
}

TEST(ClientOrderId, TheWidestIdStillFitsTheVenuesLimit) {
	// A uint64 is 20 digits and Binance allows 36 characters. Worth pinning:
	// a longer prefix that overflowed the limit would be refused only at the
	// venue, on the widest ids, long after it was introduced.
	const std::string widest =
		cancel_order_id(std::numeric_limits<order_id_t>::max());
	EXPECT_LE(widest.size(), CLIENT_ORDER_ID_MAX) << widest;
}

TEST(ClientOrderId, AnIdFromAnotherProcessIsNotOurs) {
	// What an order placed by hand in the venue's web UI looks like. It must
	// not parse - reconciliation cancels what it does not recognise as working,
	// and mistaking a human's order for a stale one of ours would cancel it.
	EXPECT_FALSE(is_ours("web_abc123"));
	EXPECT_FALSE(is_ours("and_this_one_too"));
	EXPECT_FALSE(is_ours(""));
	EXPECT_FALSE(engine_order_id("web_abc123").has_value());
}

TEST(ClientOrderId, ANearMissIsRefusedRatherThanTruncated) {
	// Each of these shares our prefix and would be read as a valid id by a
	// parser that stopped at the first bad character - attaching a venue order
	// to an unrelated engine order.
	EXPECT_FALSE(engine_order_id("ex-12x").has_value());
	EXPECT_FALSE(engine_order_id("ex-12 ").has_value());
	EXPECT_FALSE(engine_order_id("ex-").has_value());
	EXPECT_FALSE(engine_order_id("ex-1.5").has_value());
	// A sign is not part of an unsigned id, and "ex--1" must not become 1.
	EXPECT_FALSE(engine_order_id("ex--1").has_value());
	EXPECT_FALSE(engine_order_id("ex-+1").has_value());
}

TEST(ClientOrderId, ThePrefixIsRequiredAndNotJustLeading) {
	// A bare number is not ours: another process using plain ids on the same
	// account would otherwise be adopted wholesale.
	EXPECT_FALSE(is_ours("42"));
	EXPECT_FALSE(is_ours("xex-42"));
	EXPECT_TRUE(is_ours(std::string(CLIENT_ORDER_PREFIX) + "42"));
}

TEST(ClientOrderId, ACancelIsDistinguishableFromThePlacement) {
	const std::string placed = client_order_id(42);
	const std::string cancel = cancel_order_id(42);

	EXPECT_NE(placed, cancel)
		<< "a cancel sharing its order's id makes the two reports ambiguous";
	EXPECT_TRUE(is_cancel_id(cancel));
	EXPECT_FALSE(is_cancel_id(placed));
	// And still recognisably ours, so reconciliation does not treat a cancel
	// we sent as somebody else's order.
	EXPECT_TRUE(is_ours(cancel));
}
