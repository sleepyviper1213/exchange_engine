#include "event/engine_event.hpp"
#include "order_book/outcome.hpp"
#include "order_book/reject_reason.hpp"
#include "order_book/trade.hpp"

#include <gtest/gtest.h>

#include <type_traits>

// The return path's record: a listing, a tag, and the payload the tag names.
// Everything here is about the tag and the payload agreeing - a union whose tag
// lies is undefined behaviour on the next read, not a wrong value, so the
// factories are the only way to build one and this is what pins them.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::event;

namespace {

TEST(EngineEvent, StaysTriviallyCopyableForTheQueuesMemcpyPath) {
	// The reason the payload is a union and not a std::variant. If this ever
	// fails, spsc_queue silently drops to per-element construction.
	static_assert(std::is_trivially_copyable_v<engine_event>);
	static_assert(std::is_trivially_copyable_v<symbol_run>);
	// A receive buffer is an array of these, which is why - unlike command -
	// a default constructor exists at all.
	static_assert(std::is_default_constructible_v<engine_event>);
	SUCCEED();
}

TEST(EngineEvent, ADefaultEventsTagMatchesItsPayload) {
	const engine_event event;
	EXPECT_EQ(event.symbol(), 0U);
	EXPECT_EQ(event.kind(), event_kind::TRADE);
	// Reading the arm the tag names must be defined, which is the whole point of
	// zeroing rather than leaving the union uninitialised.
	EXPECT_EQ(event.as_trade(), trade{});
}

TEST(EngineEvent, ATradeIsStampedWithItsListingAndKeepsItsPayload) {
	const trade print{.aggressor = 7, .resting = 3, .price = 100, .volume = 4};
	const engine_event event = engine_event::of(42, print);

	EXPECT_EQ(event.symbol(), 42U);
	EXPECT_EQ(event.kind(), event_kind::TRADE);
	EXPECT_EQ(event.as_trade(), print);
}

TEST(EngineEvent, AnOutcomeIsStampedWithItsListingAndKeepsItsPayload) {
	const order_outcome record =
		order_outcome::rejected(9, reject_reason::DUPLICATE_ORDER_ID, 10);
	const engine_event event = engine_event::of(42, record);

	EXPECT_EQ(event.symbol(), 42U);
	EXPECT_EQ(event.kind(), event_kind::OUTCOME);
	EXPECT_EQ(event.as_outcome(), record);
}

// Two events of different kinds are never equal, even when the bytes of the
// wider arm happen to coincide: equality reads the arm the tag names, because
// a memberwise default would compare the union's padding.
TEST(EngineEvent, EqualityComparesTheArmTheTagNames) {
	const trade print{.aggressor = 1, .resting = 2, .price = 3, .volume = 4};
	const order_outcome record = order_outcome::accepted(1, 10);

	EXPECT_EQ(engine_event::of(1, print), engine_event::of(1, print));
	EXPECT_EQ(engine_event::of(1, record), engine_event::of(1, record));
	EXPECT_NE(engine_event::of(1, print), engine_event::of(1, record));

	// The listing is part of the identity: the same print on two listings is two
	// different events, which is exactly what routing depends on.
	EXPECT_NE(engine_event::of(1, print), engine_event::of(2, print));

	const trade bigger{.aggressor = 1, .resting = 2, .price = 3, .volume = 5};
	EXPECT_NE(engine_event::of(1, print), engine_event::of(1, bigger));
}

TEST(EngineEvent, TheKindPrintsAsItsName) {
	EXPECT_EQ(to_string(event_kind::TRADE), "TRADE");
	EXPECT_EQ(to_string(event_kind::OUTCOME), "OUTCOME");
}

} // namespace
