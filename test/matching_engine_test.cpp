#include "trading-engine/execution/matching_engine.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

using namespace exchange::engine;
using namespace exchange;
using namespace exchange::engine::event;
using namespace exchange::engine::execution;

namespace {

// The default queue capacity (1<<14, see execution/fwd.hpp) makes the queue's
// inline ring large enough to overflow the stack when the engine is a local. A
// long-lived production engine lives on the heap/static; the tests just need a
// small ring.
using Engine = MatchingEngine<256>;

TEST(MatchingEngine, DrainCrossesAndReportsTradeBatch) {
	std::vector<Trade> seen;
	Engine engine([&](const std::vector<Trade> &batch) {
		seen.insert(seen.end(), batch.begin(), batch.end());
	});

	// Producer hands off: rest a sell, then a buy that crosses part of it.
	ASSERT_TRUE(engine.submit(Command::place(
		{.id = 1, .side = side::ask, .price = 100, .qty = 10})));
	ASSERT_TRUE(engine.submit(Command::place(
		{.id = 2, .side = side::bid, .price = 100, .qty = 4})));

	// Consumer applies both and fires the sink once with the batch's trades.
	EXPECT_EQ(engine.drain(), 2U);

	ASSERT_EQ(seen.size(), 1U);
	EXPECT_EQ(seen[0].aggressor, 2U);
	EXPECT_EQ(seen[0].resting, 1U);
	EXPECT_EQ(seen[0].price, 100U);
	EXPECT_EQ(seen[0].volume, 4);

	// 6 of the sell remain resting; the buy was fully filled. The optional is
	// held in a local because each best_ask() call returns a fresh temporary —
	// asserting on one and dereferencing another guards nothing.
	const std::optional<price> best_ask = engine.book().best_ask();
	ASSERT_TRUE(best_ask.has_value());
	EXPECT_EQ(*best_ask, 100U);
	EXPECT_FALSE(engine.book().best_bid().has_value());
}

TEST(MatchingEngine, CancelRemovesRestingOrder) {
	Engine engine(nullptr); // trades ignored
	ASSERT_TRUE(engine.submit(Command::place(
		{.id = 1, .side = side::bid, .price = 99, .qty = 5})));
	ASSERT_TRUE(engine.submit(Command::cancel(1)));
	EXPECT_EQ(engine.drain(), 2U);
	EXPECT_FALSE(engine.book().best_bid().has_value());
}

TEST(MatchingEngine, AnonymousLevelCommands) {
	Engine engine(nullptr);
	ASSERT_TRUE(engine.submit(Command::set_level(side::bid, 50, 20)));
	ASSERT_TRUE(engine.submit(Command::add(side::ask, 60, 7)));
	ASSERT_TRUE(engine.submit(Command::reduce(side::ask, 60, 3)));
	EXPECT_EQ(engine.drain(), 3U);

	const std::optional<price> best_bid = engine.book().best_bid();
	ASSERT_TRUE(best_bid.has_value());
	EXPECT_EQ(*best_bid, 50U);
	EXPECT_EQ(engine.book().volume_at_price(50, side::bid), 20);
	EXPECT_EQ(engine.book().volume_at_price(60, side::ask), 4);
}

TEST(MatchingEngine, SubmitRangeBatchesInOneShot) {
	std::vector<Trade> seen;
	Engine engine([&](const std::vector<Trade> &batch) {
		seen.insert(seen.end(), batch.begin(), batch.end());
	});

	const std::array batch{
		Command::place({.id = 1, .side = side::ask, .price = 100, .qty = 5}),
		Command::place({.id = 2, .side = side::ask, .price = 101, .qty = 5}),
		Command::place({.id = 3, .side = side::bid, .price = 101, .qty = 8}),
	};
	ASSERT_TRUE(engine.submit_range(batch));
	EXPECT_EQ(engine.drain(), 3U);

	// The buy sweeps 5@100 then 3@101 → two fills.
	ASSERT_EQ(seen.size(), 2U);
	EXPECT_EQ(seen[0].price, 100U);
	EXPECT_EQ(seen[0].volume, 5);
	EXPECT_EQ(seen[1].price, 101U);
	EXPECT_EQ(seen[1].volume, 3);
	EXPECT_EQ(engine.book().volume_at_price(101, side::ask), 2);
}

// --------------------------------------------------------------------------
// Concurrency. The engine exists for the cross-thread hand-off its "Threading
// contract" documents — one producer calling submit(), one consumer calling
// drain(). Every test above drives both from a single thread, which covers the
// matching logic but never the queue's memory ordering, a Command union
// crossing a cache line, or the batching non-determinism.
//
// The invariant these assert is that the consumer's choice of how many commands
// to pull per drain() must not change the outcome. So they check conserved
// quantities — trade count, matched qty, an empty book — never how the work
// was split. gtest's EXPECT/ASSERT macros are not thread-safe, so worker
// threads touch only their own state and the main thread asserts after joining.
// These are the tests meant to run under the ThreadSanitizer preset.
// --------------------------------------------------------------------------

TEST(MatchingEngine, ConcurrentSubmitAndDrainConservesTrades) {
	constexpr std::size_t PAIRS         = 5000;
	constexpr std::size_t COMMAND_COUNT = PAIRS * 2;
	constexpr quantity LOT_SIZE           = 3;
	constexpr price PRICE               = 100;

	// Touched only by the consumer thread — the sink runs inside drain() — and
	// read on the main thread after join, so the join is the synchronisation.
	std::size_t trade_count = 0;
	quantity matched_volume   = 0;
	Engine engine([&](const std::vector<Trade> &batch) {
		trade_count += batch.size();
		for (const Trade &trade : batch) matched_volume += trade.volume;
	});

	std::thread consumer([&] {
		for (std::size_t applied = 0; applied < COMMAND_COUNT;) {
			const std::size_t n = engine.drain();
			if (n == 0) std::this_thread::yield();
			else applied += n;
		}
	});

	// Producer is this thread. Each pair rests a sell then crosses it exactly,
	// so at most one order rests at any moment and the book must end empty --
	// whatever batch boundaries the consumer happened to choose.
	for (std::size_t i = 0; i < PAIRS; ++i) {
		const auto ask_id = static_cast<order_id>(2U * i);
		while (!engine.submit(Command::place({.id     = ask_id,
											  .side   = side::ask,
											  .price  = PRICE,
											  .qty = LOT_SIZE})))
			std::this_thread::yield();
		while (!engine.submit(Command::place({.id     = ask_id + 1U,
											  .side   = side::bid,
											  .price  = PRICE,
											  .qty = LOT_SIZE})))
			std::this_thread::yield();
	}
	consumer.join();

	EXPECT_EQ(trade_count, PAIRS)
		<< "a command was lost, duplicated, or read torn";
	EXPECT_EQ(matched_volume, static_cast<quantity>(PAIRS) * LOT_SIZE);
	EXPECT_FALSE(engine.book().best_bid().has_value())
		<< "every bid should have been fully filled";
	EXPECT_FALSE(engine.book().best_ask().has_value())
		<< "every ask should have been fully filled";
}

// A ring far smaller than the batch forces submit() to fail repeatedly, so the
// producer's retry path — the "lossless back-pressure" the API documents — is
// genuinely taken. The rejection count is asserted non-zero on purpose: without
// it this test would quietly decay into the one above the moment the consumer
// became fast enough to keep up, and the branch would go back to being
// uncovered without anyone noticing.
TEST(MatchingEngine, SubmitAppliesBackPressureWithoutLosingCommands) {
	using TinyEngine = MatchingEngine<8>;

	constexpr std::size_t COMMAND_COUNT = 2000;
	constexpr quantity LOT_SIZE           = 1;
	constexpr price BASE_PRICE          = 50;

	std::size_t applied_total = 0;
	TinyEngine engine(nullptr); // trades ignored; this is about the queue

	std::thread consumer([&] {
		while (applied_total < COMMAND_COUNT) {
			const std::size_t n = engine.drain();
			if (n == 0) std::this_thread::yield();
			else applied_total += n;
		}
	});

	std::size_t rejections = 0;
	for (std::size_t i = 0; i < COMMAND_COUNT; ++i) {
		const Command cmd = Command::add(side::bid, BASE_PRICE + i, LOT_SIZE);
		while (!engine.submit(cmd)) {
			++rejections;
			std::this_thread::yield();
		}
	}
	consumer.join();

	EXPECT_GT(rejections, 0U)
		<< "the ring never filled, so the back-pressure path was not exercised";
	EXPECT_EQ(applied_total, COMMAND_COUNT);

	// Stronger than the applied count: each command must have landed at its own
	// price. A torn Command would be applied, but at the wrong level.
	quantity resting = 0;
	for (std::size_t i = 0; i < COMMAND_COUNT; ++i)
		resting += engine.book().volume_at_price(BASE_PRICE + i, side::bid);
	EXPECT_EQ(resting, static_cast<quantity>(COMMAND_COUNT) * LOT_SIZE);
}

} // namespace
