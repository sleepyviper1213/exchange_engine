#include "event/command.hpp"
#include "event/engine_event.hpp"
#include "execution/engine_partition.hpp"
#include "orders/side.hpp"

#include <gtest/gtest.h>

#include <cstddef>

// The cut list: which listing produced which slice of a drain's trades and
// outcomes. Neither trade nor order_outcome names a listing, and a partition
// carries many, so without this a published batch is unroutable - every suite
// here is about the offsets being exactly the boundaries of what each command
// appended, because a slice attributed to the wrong listing sends a fill to the
// wrong strategy.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::event;
using namespace exchange::engine::execution;
using namespace exchange::engine::orders;

namespace {

// A three-listing partition; symbol 0 stays out of it so nothing here can pass
// by accident on the "unspecified" id.
class Partition : public engine_partition<256> {
public:
	Partition() : engine_partition<256>(nullptr) {
		listing(1);
		listing(2);
		listing(3);
	}
};

void place(Partition &partition, symbol_id_t symbol, order_id_t id, side_t side,
		   price_t price, quantity_t qty) {
	ASSERT_TRUE(partition.submit(command::place({.id        = id,
												 .symbol_id = symbol,
												 .side      = side,
												 .price     = price,
												 .qty       = qty})));
}

TEST(EnginePartitionRuns, AnEmptyDrainCutsNothing) {
	Partition partition;
	EXPECT_EQ(partition.drain(), 0U);
	EXPECT_TRUE(partition.runs().empty());
}

// The first run starts at zero by convention, so a single-listing drain is one
// run whose ends are simply the buffer sizes.
TEST(EnginePartitionRuns, OneListingsBatchIsOneRunCoveringTheWholeBuffer) {
	Partition partition;
	place(partition, 1, 1, side_t::ask, 100, 10);
	place(partition, 1, 2, side_t::bid, 100, 4);
	EXPECT_EQ(partition.drain(), 2U);

	ASSERT_EQ(partition.runs().size(), 1U);
	EXPECT_EQ(partition.runs()[0].symbol, 1U);
	EXPECT_EQ(partition.runs()[0].trade_end, partition.trades().size());
	EXPECT_EQ(partition.runs()[0].outcome_end, partition.outcomes().size());
}

// Consecutive commands for the same listing extend the run at the back rather
// than appending a second entry - the common case in a partition whose flow is
// concentrated in a few names, and what keeps the list short.
TEST(EnginePartitionRuns, ConsecutiveCommandsForOneListingCoalesce) {
	Partition partition;
	for (order_id_t id = 1; id <= 8; ++id)
		place(partition, 2, id, side_t::bid, 100, 5);
	EXPECT_EQ(partition.drain(), 8U);

	EXPECT_EQ(partition.runs().size(), 1U);
	EXPECT_EQ(partition.outcomes().size(), 8U); // one ACCEPTED each
}

// Two listings, so two runs, and the second must begin exactly where the first
// ended. A begin is never stored - it is the previous end - so this is what
// proves the slices tile the buffer with no gap and no overlap.
TEST(EnginePartitionRuns, TwoListingsCutTheBufferIntoAdjacentSlices) {
	Partition partition;
	place(partition, 1, 1, side_t::bid, 100, 5);
	place(partition, 2, 2, side_t::bid, 100, 5);
	place(partition, 3, 3, side_t::bid, 100, 5);
	EXPECT_EQ(partition.drain(), 3U);

	ASSERT_EQ(partition.runs().size(), 3U);
	EXPECT_EQ(partition.runs()[0].symbol, 1U);
	EXPECT_EQ(partition.runs()[1].symbol, 2U);
	EXPECT_EQ(partition.runs()[2].symbol, 3U);

	// One ACCEPTED per placement, one per slice, in submission order.
	EXPECT_EQ(partition.runs()[0].outcome_end, 1U);
	EXPECT_EQ(partition.runs()[1].outcome_end, 2U);
	EXPECT_EQ(partition.runs()[2].outcome_end, 3U);
	EXPECT_EQ(partition.outcomes().size(), 3U);

	// Nothing crossed, so every trade slice is empty - and empty is the right
	// answer, not a missing run.
	EXPECT_EQ(partition.runs()[2].trade_end, 0U);
	EXPECT_TRUE(partition.trades().empty());
}

// Returning to a listing after leaving it opens a *third* run rather than
// reopening the first. Coalescing is only ever with the back entry, because a
// run means "from the previous run's end to here" and that reading breaks the
// moment a run is not contiguous.
TEST(EnginePartitionRuns, ReturningToAListingOpensANewRun) {
	Partition partition;
	place(partition, 1, 1, side_t::bid, 100, 5);
	place(partition, 2, 2, side_t::bid, 100, 5);
	place(partition, 1, 3, side_t::bid, 101, 5);
	EXPECT_EQ(partition.drain(), 3U);

	ASSERT_EQ(partition.runs().size(), 3U);
	EXPECT_EQ(partition.runs()[0].symbol, 1U);
	EXPECT_EQ(partition.runs()[1].symbol, 2U);
	EXPECT_EQ(partition.runs()[2].symbol, 1U);
	EXPECT_EQ(partition.runs()[2].outcome_end, 3U);
}

// A trade and the outcomes it caused are attributed to the same listing, and
// the crossing listing's slice holds both.
TEST(EnginePartitionRuns, ACrossingListingsSliceHoldsItsTradesAndItsOutcomes) {
	Partition partition;
	place(partition, 3, 1, side_t::ask, 100, 10);
	place(partition, 1, 2, side_t::bid, 90, 5);  // rests elsewhere, no cross
	place(partition, 3, 3, side_t::bid, 100, 4); // crosses on listing 3
	EXPECT_EQ(partition.drain(), 3U);

	ASSERT_EQ(partition.runs().size(), 3U);
	EXPECT_EQ(partition.runs()[0].symbol, 3U);
	EXPECT_EQ(partition.runs()[1].symbol, 1U);
	EXPECT_EQ(partition.runs()[2].symbol, 3U);

	// The only print belongs to listing 3's second slice: it appeared after the
	// first two commands had published none.
	ASSERT_EQ(partition.trades().size(), 1U);
	EXPECT_EQ(partition.runs()[0].trade_end, 0U);
	EXPECT_EQ(partition.runs()[1].trade_end, 0U);
	EXPECT_EQ(partition.runs()[2].trade_end, 1U);
}

// A misrouted command still publishes - a REJECTED naming the symbol it asked
// for - so it gets a slice like any other. Dropping it here would lose the one
// record the client is waiting for.
TEST(EnginePartitionRuns, AMisroutedCommandStillGetsASlice) {
	Partition partition;
	place(partition, 9, 1, side_t::bid, 100, 5); // listing 9 is not carried
	EXPECT_EQ(partition.drain(), 1U);

	EXPECT_EQ(partition.misrouted(), 1U);
	ASSERT_EQ(partition.runs().size(), 1U);
	EXPECT_EQ(partition.runs()[0].symbol, 9U);
	EXPECT_EQ(partition.runs()[0].outcome_end, 1U);
}

// flush() empties the batch, and the cut list is part of the batch: offsets
// that outlived the buffers they index would be a stale view of a cleared
// vector.
TEST(EnginePartitionRuns, FlushEmptiesTheCutListWithTheBuffers) {
	Partition partition;
	place(partition, 1, 1, side_t::bid, 100, 5);
	EXPECT_EQ(partition.drain(), 1U);
	ASSERT_FALSE(partition.runs().empty());

	partition.flush();
	EXPECT_TRUE(partition.runs().empty());
	EXPECT_TRUE(partition.trades().empty());
	EXPECT_TRUE(partition.outcomes().empty());
}

// A second drain describes only its own batch, never the previous one's.
TEST(EnginePartitionRuns, ASecondDrainStartsTheCutListOver) {
	Partition partition;
	place(partition, 1, 1, side_t::bid, 100, 5);
	EXPECT_EQ(partition.drain(), 1U);
	place(partition, 2, 2, side_t::bid, 100, 5);
	EXPECT_EQ(partition.drain(), 1U);

	ASSERT_EQ(partition.runs().size(), 1U);
	EXPECT_EQ(partition.runs()[0].symbol, 2U);
	EXPECT_EQ(partition.runs()[0].outcome_end, 1U);
}

} // namespace
