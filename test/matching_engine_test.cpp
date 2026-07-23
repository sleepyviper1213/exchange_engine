#include "execution/matching_engine.hpp"

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <vector>

using namespace order_book;
using namespace event;
using namespace execution;

namespace {

// The default lockfree capacity (1<<16) makes the lockfree's inline ring ~3 MB,
// which overflows the stack when the engine is a local. A long-lived production
// engine lives on the heap/static; the tests just need a small ring.
using Engine = MatchingEngine<256>;

TEST(MatchingEngine, DrainCrossesAndReportsTradeBatch) {
	std::vector<Trade> seen;
	Engine engine([&](const std::vector<Trade> &batch) {
		seen.insert(seen.end(), batch.begin(), batch.end());
	});

	// Producer hands off: rest a sell, then a buy that crosses part of it.
	ASSERT_TRUE(engine.submit(Command::place(
		{.id = 1, .side = Side::ASK, .price = 100, .volume = 10})));
	ASSERT_TRUE(engine.submit(Command::place(
		{.id = 2, .side = Side::BID, .price = 100, .volume = 4})));

	// Consumer applies both and fires the sink once with the batch's trades.
	EXPECT_EQ(engine.drain(), 2U);

	ASSERT_EQ(seen.size(), 1U);
	EXPECT_EQ(seen[0].aggressor, 2U);
	EXPECT_EQ(seen[0].resting, 1U);
	EXPECT_EQ(seen[0].price, 100U);
	EXPECT_EQ(seen[0].volume, 4);

	// 6 of the sell remain resting; the buy was fully filled.
	ASSERT_TRUE(engine.book().best_ask().has_value());
	EXPECT_EQ(engine.book().best_ask().value(), 100U);
	EXPECT_EQ(engine.book().best_bid(), std::nullopt);
}

TEST(MatchingEngine, CancelRemovesRestingOrder) {
	Engine engine(nullptr); // trades ignored
	ASSERT_TRUE(engine.submit(Command::place(
		{.id = 1, .side = Side::BID, .price = 99, .volume = 5})));
	ASSERT_TRUE(engine.submit(Command::cancel(1)));
	EXPECT_EQ(engine.drain(), 2U);
	EXPECT_EQ(engine.book().best_bid(), std::nullopt);
}

TEST(MatchingEngine, AnonymousLevelCommands) {
	Engine engine(nullptr);
	ASSERT_TRUE(engine.submit(Command::set_level(Side::BID, 50, 20)));
	ASSERT_TRUE(engine.submit(Command::add(Side::ASK, 60, 7)));
	ASSERT_TRUE(engine.submit(Command::reduce(Side::ASK, 60, 3)));
	EXPECT_EQ(engine.drain(), 3U);

	EXPECT_EQ(engine.book().best_bid().value(), 50U);
	EXPECT_EQ(engine.book().volume_at_price(50, Side::BID), 20);
	EXPECT_EQ(engine.book().volume_at_price(60, Side::ASK), 4);
}

TEST(MatchingEngine, SubmitRangeBatchesInOneShot) {
	std::vector<Trade> seen;
	Engine engine([&](const std::vector<Trade> &batch) {
		seen.insert(seen.end(), batch.begin(), batch.end());
	});

	const std::array batch{
		Command::place({.id = 1, .side = Side::ASK, .price = 100, .volume = 5}),
		Command::place({.id = 2, .side = Side::ASK, .price = 101, .volume = 5}),
		Command::place({.id = 3, .side = Side::BID, .price = 101, .volume = 8}),
	};
	ASSERT_TRUE(engine.submit_range(batch));
	EXPECT_EQ(engine.drain(), 3U);

	// The buy sweeps 5@100 then 3@101 → two fills.
	ASSERT_EQ(seen.size(), 2U);
	EXPECT_EQ(seen[0].price, 100U);
	EXPECT_EQ(seen[0].volume, 5);
	EXPECT_EQ(seen[1].price, 101U);
	EXPECT_EQ(seen[1].volume, 3);
	EXPECT_EQ(engine.book().volume_at_price(101, Side::ASK), 2);
}

} // namespace
