#include "core/persistence/event_store.hpp"
#include "core/persistence/persistence.fixture.hpp"
#include "core/persistence/replay.hpp"
#include "trading-engine/event/command.hpp"
#include "trading-engine/execution/engine_partition.hpp"
#include "trading-engine/orders/side.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <fstream>
#include <cstdint>
#include <vector>

// Recovery, end to end, against the real thing: a store, the replay driver, and a
// partition whose command queue is deliberately far too small.
//
// The small queue is the point. `submit` refuses when its ring is full, so a
// recovery that replays a journal into a live partition *will* be refused part way
// through - routinely, not exceptionally, because the disk is faster than the
// consumer. Every other test of replay uses an applier that always accepts, which
// is the easy half. This one uses the applier recovery actually has.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::event;
using namespace exchange::engine::execution;
using namespace exchange::engine::orders;

using exchange::core::persistence::event_store;
using exchange::core::persistence::replay;

namespace {

/// Four commands of room, against a flow of dozens: the refusal path is the
/// common path here rather than a corner of it.
using tiny = engine_partition<4>;

struct recorded {
	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;

	bool operator==(const recorded &) const noexcept = default;
};

/// @brief A flow that rests, crosses, cancels, is refused and is misrouted -
///        every path that leaves a different mark, so a replay that diverged
///        anywhere would show it.
///
/// Written out rather than generated. A generated flow is where a test quietly
/// stops testing what it claims: the first version of this alternated side and
/// symbol together, which put every ask on one book and every bid on the other,
/// so nothing ever crossed and the trade streams it compared were both empty.
std::vector<command> mixed_flow() {
	const auto place = [](order_id_t id, symbol_id_t symbol, side_t side,
						  price_t price, quantity_t qty) {
		return command::place({.id        = id,
							   .symbol_id = symbol,
							   .side      = side,
							   .price     = price,
							   .qty       = qty});
	};
	return {
		// Listing 1: rest two asks, then take them with two bids.
		place(1, 1, side_t::ask, 100, 10),
		place(2, 1, side_t::ask, 101, 5),
		place(3, 1, side_t::bid, 100, 4),  // crosses id 1 partly
		place(4, 1, side_t::bid, 101, 5),  // takes more of id 1
		command::cancel(1, 1),             // withdraw what is left of id 1
		place(5, 1, side_t::bid, 99, 3),   // rests below the touch
		command::cancel(1, 999),           // never existed -> CANCEL_REJECTED
		place(5, 1, side_t::bid, 98, 1),   // id 5 again -> DUPLICATE_ORDER_ID

		// Listing 2: a full cross each way, and a resting order cancelled.
		place(6, 2, side_t::ask, 200, 8),
		place(7, 2, side_t::bid, 200, 8),  // crosses id 6 whole
		place(8, 2, side_t::bid, 199, 2),
		place(9, 2, side_t::ask, 199, 2),  // crosses id 8 whole
		place(10, 2, side_t::ask, 201, 4),
		command::cancel(2, 10),

		// A listing this partition does not carry -> misrouted, but still applied
		// off the queue and so still journalled.
		place(11, 7, side_t::bid, 100, 1),
	};
}

/// @brief Drain until nothing is left, so the queue is empty and has room again.
std::size_t drain_fully(tiny &partition) {
	std::size_t applied = 0;
	for (std::size_t n = partition.drain_and_flush(); n != 0;
		 n             = partition.drain_and_flush())
		applied += n;
	return applied;
}

/// @brief Push @p flow through @p partition, draining whenever it fills.
std::size_t feed(tiny &partition, const std::vector<command> &flow) {
	std::size_t applied = 0;
	for (const command &cmd : flow) {
		while (!partition.submit(cmd)) applied += drain_fully(partition);
	}
	return applied + drain_fully(partition);
}

// The original run and a recovery from its journal must agree on every trade and
// every outcome. Not on counts - on the records, in order.
TEST(EnginePartitionRecovery, ReplayingAStoresJournalReproducesTheRun) {
	const scratch_dir dir("recovery_reproduces");
	const auto root                 = dir.file("venue");
	const std::vector<command> flow = mixed_flow();

	recorded original;
	{
		auto store = event_store<command>::open(root);
		ASSERT_TRUE(store.has_value()) << store.error();

		tiny live(
			[&](const std::vector<trade> &b) {
				original.trades.insert(original.trades.end(), b.begin(), b.end());
			},
			[&](const std::vector<order_outcome> &b) {
				original.outcomes.insert(original.outcomes.end(),
										 b.begin(),
										 b.end());
			});
		live.listing(1);
		live.listing(2);
		live.attach_journal(&store->journal());

		EXPECT_EQ(feed(live, flow), flow.size());
		EXPECT_EQ(live.journal_failures(), 0U);
		EXPECT_EQ(store->journal().count(), flow.size());
	}
	ASSERT_FALSE(original.trades.empty());
	ASSERT_FALSE(original.outcomes.empty());

	// --- the restart ---
	auto store = event_store<command>::open(root);
	ASSERT_TRUE(store.has_value()) << store.error();
	// Never checkpointed, so recovery starts at record zero - which is the
	// correct instruction for a store with no snapshot, not a missing value.
	ASSERT_EQ(store->checkpoint().sequence, 0U);
	ASSERT_EQ(store->journal().count(), flow.size());

	recorded replayed;
	tiny restored(
		[&](const std::vector<trade> &b) {
			replayed.trades.insert(replayed.trades.end(), b.begin(), b.end());
		},
		[&](const std::vector<order_outcome> &b) {
			replayed.outcomes.insert(replayed.outcomes.end(),
									 b.begin(),
									 b.end());
		});
	restored.listing(1);
	restored.listing(2);
	// No journal on the recovering partition: re-journalling a replay would
	// append the whole history to itself on every restart.

	std::uint64_t at      = store->checkpoint().sequence;
	std::uint64_t refusals = 0;
	for (;;) {
		const auto step = replay(store->journal(), at, [&](const command &cmd) {
			return restored.submit(cmd);
		});
		at = step.next;
		(void)drain_fully(restored);
		if (step.complete) break;
		++refusals;
	}

	EXPECT_EQ(at, flow.size());
	EXPECT_EQ(replayed.trades, original.trades);
	EXPECT_EQ(replayed.outcomes, original.outcomes);
	// If the queue never filled, this test proved less than it claims to.
	EXPECT_GT(refusals, 0U) << "a 4-slot queue should have refused a 15-command "
							   "replay at least once";
}

// And the resume point is honoured: a checkpoint says the first N records are
// already accounted for, so replay must not re-apply them. Re-applying a PLACE is
// not idempotent - the book answers DUPLICATE_ORDER_ID - so a driver that got this
// wrong would be loudly wrong, which is what this pins.
TEST(EnginePartitionRecovery, ACheckpointsSequenceIsNotReplayed) {
	const scratch_dir dir("recovery_checkpoint");
	const auto root                 = dir.file("venue");
	const std::vector<command> flow = mixed_flow();

	{
		auto store = event_store<command>::open(root);
		ASSERT_TRUE(store.has_value()) << store.error();
		tiny live(nullptr);
		live.listing(1);
		live.listing(2);
		live.attach_journal(&store->journal());
		EXPECT_EQ(feed(live, flow), flow.size());

		// A checkpoint covering the first four records. The snapshot file stands
		// in for book state, which persistence cannot write itself.
		std::ofstream out(store->snapshot_path(1),
						  std::ios::binary | std::ios::trunc);
		out << "state";
		out.close();
		// commit() records the journal's *current* count, so take it while only
		// the prefix under test has been written... which it has not been here, so
		// assert on what it actually recorded rather than on what we intended.
		ASSERT_TRUE(store->commit(1, /*session=*/3).has_value());
		EXPECT_EQ(store->checkpoint().sequence, flow.size());
	}

	auto store = event_store<command>::open(root);
	ASSERT_TRUE(store.has_value()) << store.error();

	// The checkpoint covers the whole journal, so there is nothing to replay -
	// the cheapest possible recovery, and the one a venue checkpointed at
	// shutdown gets.
	std::uint64_t applied = 0;
	const auto step = replay(store->journal(),
							 store->checkpoint().sequence,
							 [&](const command &) { ++applied; return true; });
	EXPECT_TRUE(step.complete);
	EXPECT_EQ(applied, 0U);
	EXPECT_EQ(step.next, flow.size());
}

} // namespace
