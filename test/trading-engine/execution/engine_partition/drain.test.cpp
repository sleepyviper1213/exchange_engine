#include "engine_partition.fixture.hpp"
#include "trading-engine/execution/engine_partition.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <optional>
#include <thread>
#include <vector>

// The producer/consumer contract: what submit hands over, what drain applies,
// and what flush publishes. Nothing here is about which listing a command
// belongs to (listings.test.cpp) or about what the venue remembers afterwards
// (records.test.cpp) — this is the queue and the batch, and the properties that
// have to hold whichever thread is on which end of them.

using namespace exchange::engine;
using namespace exchange::engine::orders;
using namespace exchange;
using namespace exchange::engine::event;
using namespace exchange::engine::execution;

using engine_partition_test::Engine;

namespace {

TEST(EnginePartitionDrain, DrainCrossesAndReportsTradeBatch) {
	std::vector<trade> seen;
	Engine engine(
		[&](const std::vector<trade> &batch) { seen.append_range(batch); });

	// Producer hands off: rest a sell, then a buy that crosses part of it.
	ASSERT_TRUE(engine.submit(command::place(
		{.id = 1, .side = side_t::ask, .price = 100, .qty = 10})));
	ASSERT_TRUE(engine.submit(command::place(
		{.id = 2, .side = side_t::bid, .price = 100, .qty = 4})));

	// Consumer applies both and fires the sink once with the batch's trades.
	EXPECT_EQ(engine.drain_and_flush(), 2U);

	ASSERT_EQ(seen.size(), 1U);
	EXPECT_EQ(seen[0].aggressor, 2U);
	EXPECT_EQ(seen[0].resting, 1U);
	EXPECT_EQ(seen[0].price, 100U);
	EXPECT_EQ(seen[0].volume, 4);

	// 6 of the sell remain resting; the buy was fully filled. The optional is
	// held in a local because each best_ask() call returns a fresh temporary —
	// asserting on one and dereferencing another guards nothing.
	const std::optional<price_t> best_ask = (*engine.book(0)).best_ask();
	ASSERT_TRUE(best_ask.has_value());
	EXPECT_EQ(*best_ask, 100U);
	EXPECT_FALSE((*engine.book(0)).best_bid().has_value());
}

TEST(EnginePartitionDrain, CancelRemovesRestingOrder) {
	Engine engine(nullptr); // trades ignored
	ASSERT_TRUE(engine.submit(
		command::place({.id = 1, .side = side_t::bid, .price = 99, .qty = 5})));
	ASSERT_TRUE(engine.submit(command::cancel(0, 1)));
	EXPECT_EQ(engine.drain_and_flush(), 2U);
	EXPECT_FALSE((*engine.book(0)).best_bid().has_value());
}

// --------------------------------------------------------------------------
// Outcome stream — the only channel that carries a command's fate back past
// the queue. @see verification/order-lifecycle/OrderLifecycle.tla
// --------------------------------------------------------------------------

TEST(EnginePartitionDrain, DrainReportsOutcomesForTheWholeBatch) {
	std::vector<order_outcome> seen;
	Engine engine(nullptr, [&](const std::vector<order_outcome> &batch) {
		seen.append_range(batch);
	});

	ASSERT_TRUE(engine.submit(command::place(
		{.id = 1, .side = side_t::ask, .price = 100, .qty = 5})));
	ASSERT_TRUE(engine.submit(command::place(
		{.id = 2, .side = side_t::bid, .price = 100, .qty = 5})));
	EXPECT_EQ(engine.drain_and_flush(), 2U);

	// ACCEPTED(1), ACCEPTED(2), FILL(1), FILL(2) — both orders fully filled.
	ASSERT_EQ(seen.size(), 4U);
	EXPECT_EQ(seen[0].type, OutcomeType::ACCEPTED);
	EXPECT_EQ(seen[1].type, OutcomeType::ACCEPTED);
	EXPECT_EQ(seen[2].type, OutcomeType::FILL);
	EXPECT_EQ(seen[2].status, OrderStatus::FILLED);
	EXPECT_EQ(seen[3].type, OutcomeType::FILL);
	EXPECT_EQ(seen[3].status, OrderStatus::FILLED);
}

// The race the queue makes real: a cancel submitted while the order is still
// live arrives at a book where a later-submitted-but-same-drain place has
// already filled it. The producer cannot know that; the outcome stream is how
// it finds out.
TEST(EnginePartitionDrain, CancelLosingToAFillIsDeclined) {
	std::vector<order_outcome> seen;
	Engine engine(nullptr, [&](const std::vector<order_outcome> &batch) {
		seen.append_range(batch);
	});

	ASSERT_TRUE(engine.submit(command::place(
		{.id = 1, .side = side_t::ask, .price = 100, .qty = 5})));
	ASSERT_TRUE(engine.submit(command::place(
		{.id = 2, .side = side_t::bid, .price = 100, .qty = 5})));
	ASSERT_TRUE(engine.submit(command::cancel(0, 1)));
	EXPECT_EQ(engine.drain_and_flush(), 3U);

	ASSERT_FALSE(seen.empty());
	const order_outcome &last = seen.back();
	EXPECT_EQ(last.id, 1U);
	EXPECT_EQ(last.type, OutcomeType::CANCEL_REJECTED);
	// The book's own answer here is UNKNOWN_ORDER — its index holds resting
	// orders only, so a filled order and one that never existed leave the same
	// empty probe. The partition's record store kept order 1, so the reason the
	// client receives says which of the two it was. All three commands landed in
	// one drain, so this is the race itself and not a lookup after the fact.
	EXPECT_EQ(last.reason, reject_reason::ORDER_ALREADY_FILLED);
}

// drain() without flush(): the records stay in the partition's buffers, which
// is what a consumer that installed no sinks reads. flush() is what empties
// them, so it is deliberately not called here.
TEST(EnginePartitionDrain, OutcomesAreReadableWithoutASink) {
	Engine engine(nullptr); 
	ASSERT_TRUE(engine.submit(
		command::place({.id = 1, .side = side_t::bid, .price = 99, .qty = 5})));
	EXPECT_EQ(engine.drain(), 1U);

	ASSERT_EQ(engine.outcomes().size(), 1U);
	EXPECT_EQ(engine.outcomes()[0].type, OutcomeType::ACCEPTED);

	// The buffer is reused, so the next drain replaces it rather than
	// appending.
	ASSERT_TRUE(engine.submit(command::cancel(0, 1)));
	EXPECT_EQ(engine.drain(), 1U);
	ASSERT_EQ(engine.outcomes().size(), 1U);
	EXPECT_EQ(engine.outcomes()[0].type, OutcomeType::CANCELLED);
}

// ...and flush() empties them, so a consumer cannot read the same batch twice.
TEST(EnginePartitionDrain, FlushEmptiesTheBuffers) {
	Engine engine(nullptr);
	ASSERT_TRUE(engine.submit(
		command::place({.id = 1, .side = side_t::bid, .price = 99, .qty = 5})));
	EXPECT_EQ(engine.drain(), 1U);
	ASSERT_FALSE(engine.outcomes().empty());

	engine.flush();
	EXPECT_TRUE(engine.outcomes().empty());
	EXPECT_TRUE(engine.trades().empty());
}

TEST(EnginePartitionDrain, AnonymousLevelCommands) {
	Engine engine(nullptr);
	ASSERT_TRUE(engine.submit(command::add(0, side_t::bid, 50, 20)));
	ASSERT_TRUE(engine.submit(command::add(0, side_t::ask, 60, 7)));
	ASSERT_TRUE(engine.submit(command::reduce(0, side_t::ask, 60, 3)));
	EXPECT_EQ(engine.drain_and_flush(), 3U);

	const std::optional<price_t> best_bid = (*engine.book(0)).best_bid();
	ASSERT_TRUE(best_bid.has_value());
	EXPECT_EQ(*best_bid, 50U);
	EXPECT_EQ((*engine.book(0)).volume_at_price(50, side_t::bid), 20);
	EXPECT_EQ((*engine.book(0)).volume_at_price(60, side_t::ask), 4);
}

TEST(EnginePartitionDrain, SubmitRangeBatchesInOneShot) {
	std::vector<trade> seen;
	Engine engine(
		[&](const std::vector<trade> &batch) { seen.append_range(batch); });

	const std::array batch{
		command::place({.id = 1, .side = side_t::ask, .price = 100, .qty = 5}),
		command::place({.id = 2, .side = side_t::ask, .price = 101, .qty = 5}),
		command::place({.id = 3, .side = side_t::bid, .price = 101, .qty = 8}),
	};
	ASSERT_TRUE(engine.submit_range(batch));
	EXPECT_EQ(engine.drain_and_flush(), 3U);

	// The buy sweeps 5@100 then 3@101 → two fills.
	ASSERT_EQ(seen.size(), 2U);
	EXPECT_EQ(seen[0].price, 100U);
	EXPECT_EQ(seen[0].volume, 5);
	EXPECT_EQ(seen[1].price, 101U);
	EXPECT_EQ(seen[1].volume, 3);
	EXPECT_EQ((*engine.book(0)).volume_at_price(101, side_t::ask), 2);
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

TEST(EnginePartitionDrain, ConcurrentSubmitAndDrainConservesTrades) {
	constexpr std::size_t PAIRS         = 5000;
	constexpr std::size_t COMMAND_COUNT = PAIRS * 2;
	constexpr quantity_t LOT_SIZE       = 3;
	constexpr price_t PRICE             = 100;

	// Touched only by the consumer thread — the sink runs inside drain() — and
	// read on the main thread after join, so the join is the synchronisation.
	std::size_t trade_count   = 0;
	quantity_t matched_volume = 0;
	Engine engine([&](const std::vector<trade> &batch) {
		trade_count += batch.size();
		for (const trade &trade : batch) matched_volume += trade.volume;
	});

	std::thread consumer([&] {
		for (std::size_t applied = 0; applied < COMMAND_COUNT;) {
			const std::size_t n = engine.drain_and_flush();
			if (n == 0) std::this_thread::yield();
			else applied += n;
		}
	});

	// Producer is this thread. Each pair rests a sell then crosses it exactly,
	// so at most one order rests at any moment and the book must end empty --
	// whatever batch boundaries the consumer happened to choose.
	for (std::size_t i = 0; i < PAIRS; ++i) {
		const auto ask_id = static_cast<order_id_t>(2U * i);
		while (!engine.submit(command::place({.id    = ask_id,
											  .side  = side_t::ask,
											  .price = PRICE,
											  .qty   = LOT_SIZE})))
			std::this_thread::yield();
		while (!engine.submit(command::place({.id    = ask_id + 1U,
											  .side  = side_t::bid,
											  .price = PRICE,
											  .qty   = LOT_SIZE})))
			std::this_thread::yield();
	}
	consumer.join();

	EXPECT_EQ(trade_count, PAIRS)
		<< "a command was lost, duplicated, or read torn";
	EXPECT_EQ(matched_volume, static_cast<quantity_t>(PAIRS) * LOT_SIZE);
	EXPECT_FALSE((*engine.book(0)).best_bid().has_value())
		<< "every bid should have been fully filled";
	EXPECT_FALSE((*engine.book(0)).best_ask().has_value())
		<< "every ask should have been fully filled";
}

// A ring far smaller than the batch forces submit() to fail repeatedly, so the
// producer's retry path — the "lossless back-pressure" the API documents — is
// genuinely taken. The rejection count is asserted non-zero on purpose: without
// it this test would quietly decay into the one above the moment the consumer
// became fast enough to keep up, and the branch would go back to being
// uncovered without anyone noticing.
TEST(EnginePartitionDrain, SubmitAppliesBackPressureWithoutLosingCommands) {
	using TinyEngine = engine_partition<8>;

	constexpr std::size_t COMMAND_COUNT = 2000;
	constexpr quantity_t LOT_SIZE       = 1;
	constexpr price_t BASE_PRICE        = 50;

	std::size_t applied_total = 0;
	TinyEngine engine(nullptr); // trades ignored; this is about the queue
	engine.listing(0);          // registered before the producer starts

	std::thread consumer([&] {
		while (applied_total < COMMAND_COUNT) {
			const std::size_t n = engine.drain_and_flush();
			if (n == 0) std::this_thread::yield();
			else applied_total += n;
		}
	});

	std::size_t rejections = 0;
	for (std::size_t i = 0; i < COMMAND_COUNT; ++i) {
		const command cmd =
			command::add(0, side_t::bid, BASE_PRICE + i, LOT_SIZE);
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
	quantity_t resting = 0;
	for (std::size_t i = 0; i < COMMAND_COUNT; ++i)
		resting +=
			(*engine.book(0)).volume_at_price(BASE_PRICE + i, side_t::bid);
	EXPECT_EQ(resting, static_cast<quantity_t>(COMMAND_COUNT) * LOT_SIZE);
}

} // namespace
