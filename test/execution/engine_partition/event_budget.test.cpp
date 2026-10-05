#include "core/persistence/persistence.fixture.hpp"
#include "core/persistence/record_log.hpp"
#include "event/command.hpp"
#include "execution/engine_partition.hpp"
#include "orders/side.hpp"

#include <gtest/gtest.h>

#include <cstddef>

// A drain is bounded in events, not only in commands. A command is bounded but
// its fan-out is not, so a cap on commands alone let one sweep through a deep
// level grow the batch buffers on the matching thread. Every suite here drives
// an eight-slot partition, whose budget is small enough to reach by hand:
// EVENT_BUDGET is 64, and a drain stops taking commands once 32 are staged.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::event;
using namespace exchange::engine::execution;
using namespace exchange::engine::orders;

namespace {

using budget_partition = engine_partition<8>;

static_assert(budget_partition::EVENT_BUDGET == 64,
			  "the arithmetic in these suites assumes an eight-slot queue");

constexpr price_t BUDGET_PRICE = at_tick(100);

void budget_place(budget_partition &partition, order_id_t id, side_t side,
				  quantity_t qty) {
	ASSERT_TRUE(partition.submit(command::place({.id        = id,
												 .symbol_id = 0,
												 .side      = side,
												 .price     = BUDGET_PRICE,
												 .qty       = qty})));
}

/// @brief Rest @p count one-lot asks, ids from 1, a queue's worth at a time.
void budget_rest_asks(budget_partition &partition, std::size_t count) {
	for (std::size_t i = 0; i < count; ++i) {
		budget_place(partition,
					 static_cast<order_id_t>(i + 1),
					 side_t::ask,
					 1 * units::lot);
		if (i % 8 == 7 || i + 1 == count)
			while (partition.drain_and_flush() != 0) {}
	}
}

// Without the event cap all eight commands come off in one drain. Each bid
// takes two resting asks - two trades, an ACCEPTED and four FILLs, seven events
// - so the staged count before the k-th is 7(k-1), and the sixth would start
// at 35, past the 32 the cap allows.
TEST(EnginePartitionEventBudget,
	 ADrainStopsTakingCommandsOnceHalfItsBudgetIsStaged) {
	budget_partition partition(nullptr);
	partition.listing(0);
	budget_rest_asks(partition, 16);

	for (order_id_t id = 101; id <= 108; ++id)
		budget_place(partition, id, side_t::bid, 2 * units::lot);

	EXPECT_EQ(partition.drain(), 5U);
	EXPECT_EQ(partition.trades().size(), 10U);
	EXPECT_EQ(partition.outcomes().size(), 25U);
	ASSERT_TRUE(partition.flush());

	EXPECT_EQ(partition.drain(), 3U) << "the tail stays queued, not dropped";
	EXPECT_EQ(partition.buffer_growths(), 0U);
}

// The residual case the budget cannot rule out, made visible rather than
// silent: one aggressor through 40 resting orders is 81 outcomes against a
// reservation of 64.
TEST(EnginePartitionEventBudget, ASweepPastTheHeadroomIsCountedAsAGrowth) {
	budget_partition partition(nullptr);
	partition.listing(0);
	budget_rest_asks(partition, 40);
	EXPECT_EQ(partition.buffer_growths(), 0U);

	budget_place(partition, 500, side_t::bid, 40 * units::lot);
	EXPECT_EQ(partition.drain(), 1U);
	EXPECT_EQ(partition.trades().size(), 40U);
	EXPECT_EQ(partition.buffer_growths(), 1U);
}

// A journalled batch is recorded whole before any of it is applied, so a drain
// the budget cuts short leaves journalled commands unapplied. They must be
// applied next, ahead of anything newer, or the books diverge from the log.
TEST(EnginePartitionEventBudget, AJournalledBatchCutShortIsFinishedFirst) {
	const scratch_dir dir("event_budget_journal");
	auto log =
		budget_partition::journal::open_for_append(dir.file("journal.bin"));
	ASSERT_TRUE(log.has_value()) << log.error();

	budget_partition partition(nullptr);
	partition.listing(0);
	partition.attach_journal(&*log);
	budget_rest_asks(partition, 16);

	for (order_id_t id = 101; id <= 108; ++id)
		budget_place(partition, id, side_t::bid, 2 * units::lot);
	EXPECT_EQ(partition.drain(), 5U);
	EXPECT_EQ(log->count(), 24U) << "all eight were journalled up front";
	ASSERT_TRUE(partition.flush());

	// Rests rather than crosses: every ask is gone by the time it applies.
	budget_place(partition, 200, side_t::bid, 1 * units::lot);
	EXPECT_EQ(partition.drain(), 4U);
	ASSERT_FALSE(partition.outcomes().empty());
	EXPECT_EQ(partition.outcomes().back().id, 200U)
		<< "the new command must come after the carried ones";
	EXPECT_EQ(partition.sequence(), 25U);
	EXPECT_EQ(log->count(), 25U);
	EXPECT_EQ(partition.journal_failures(), 0U);
}

} // namespace
