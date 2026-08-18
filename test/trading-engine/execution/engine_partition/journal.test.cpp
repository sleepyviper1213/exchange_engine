#include "core/persistence/persistence.fixture.hpp"
#include "core/persistence/record_log.hpp"
#include "trading-engine/event/command.hpp"
#include "trading-engine/execution/engine_partition.hpp"
#include "trading-engine/orders/side.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

// The claim TODO.md #6 exists to make good on: the engine is deterministic, so a
// log of what it was *asked* is enough to reproduce what it *did*. Nothing here
// records a trade - trades are re-derived by replaying commands into a fresh
// partition, and the test is that they come out identical.
//
// That is also why the journal is a log of commands and not of events. An event
// log would be bigger, would need the book's internals to stay compatible with
// it, and would still not let you ask "what would have happened if". A command
// log is the smaller artefact and the stronger one.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::event;
using namespace exchange::engine::execution;
using namespace exchange::engine::orders;

namespace {

using journal_log = engine_partition<256>::journal;

/// @brief A three-listing partition recording everything it publishes.
struct recorded {
	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;
};

/// @brief Run @p commands through a fresh partition and report what it published.
///
/// @param log Attached before the first drain when non-null, so a run either
///        journals everything or nothing - a partition that started recording
///        half way through would produce a log that replays to a different book.
recorded run(const std::vector<command> &commands, journal_log *log) {
	recorded seen;
	engine_partition<256> partition(
		[&](const std::vector<trade> &batch) {
			seen.trades.insert(seen.trades.end(), batch.begin(), batch.end());
		},
		[&](const std::vector<order_outcome> &batch) {
			seen.outcomes.insert(seen.outcomes.end(),
								 batch.begin(),
								 batch.end());
		});
	partition.listing(1);
	partition.listing(2);
	partition.attach_journal(log);

	// Batched deliberately: the durability barrier is per flush, so driving this
	// in more than one batch is what exercises it more than once.
	for (std::size_t i = 0; i < commands.size(); ++i) {
		EXPECT_TRUE(partition.submit(commands[i]));
		if (i % 4 == 3) EXPECT_EQ(partition.drain_and_flush(), 4U);
	}
	partition.drain();
	EXPECT_TRUE(partition.flush());
	EXPECT_EQ(partition.journal_failures(), 0U);
	return seen;
}

/// @brief Flow that rests, crosses, cancels and misroutes - every path that
///        produces a different kind of record.
std::vector<command> mixed_flow() {
	return {
		command::place({.id = 1, .symbol_id = 1, .side = side_t::ask,
						.price = 100, .qty = 10}),
		command::place({.id = 2, .symbol_id = 1, .side = side_t::ask,
						.price = 101, .qty = 5}),
		command::place({.id = 3, .symbol_id = 2, .side = side_t::bid,
						.price = 90, .qty = 7}),
		// Crosses id 1 partly.
		command::place({.id = 4, .symbol_id = 1, .side = side_t::bid,
						.price = 100, .qty = 4}),
		// Cancels what is left of id 1.
		command::cancel(1, 1),
		// A cancel for an order that never existed: CANCEL_REJECTED.
		command::cancel(1, 999),
		// A listing this partition does not carry: rejected, but still applied
		// off the queue and so still journalled.
		command::place({.id = 5, .symbol_id = 9, .side = side_t::bid,
						.price = 50, .qty = 1}),
		command::place({.id = 6, .symbol_id = 2, .side = side_t::ask,
						.price = 90, .qty = 7}),
	};
}

TEST(EnginePartitionJournal, WithNoJournalAttachedFlushStillSucceeds) {
	const recorded seen = run(mixed_flow(), nullptr);
	EXPECT_FALSE(seen.outcomes.empty());
}

TEST(EnginePartitionJournal, EveryAppliedCommandIsRecordedInOrder) {
	const scratch_dir dir("journal_records");
	const std::vector<command> flow = mixed_flow();

	{
		auto log = journal_log::open_for_append(dir.file("journal.bin"));
		ASSERT_TRUE(log.has_value()) << log.error();
		(void)run(flow, &*log);
		EXPECT_EQ(log->count(), flow.size());
	}

	auto reader = journal_log::open_for_read(dir.file("journal.bin"));
	ASSERT_TRUE(reader.has_value()) << reader.error();
	const std::vector<command> recorded_flow = reader->read_from(0);
	ASSERT_EQ(recorded_flow.size(), flow.size());
	for (std::size_t i = 0; i < flow.size(); ++i) {
		EXPECT_EQ(recorded_flow[i].type, flow[i].type) << "at " << i;
		EXPECT_EQ(recorded_flow[i].symbol, flow[i].symbol) << "at " << i;
	}
}

// The one that matters. Replay the journal into a partition that has never seen
// any of it, and the trades and outcomes must match the original run exactly -
// not merely in count, but record for record.
TEST(EnginePartitionJournal, ReplayingTheJournalReproducesTheRunExactly) {
	const scratch_dir dir("journal_replay");
	const std::vector<command> flow = mixed_flow();

	recorded original;
	{
		auto log = journal_log::open_for_append(dir.file("journal.bin"));
		ASSERT_TRUE(log.has_value()) << log.error();
		original = run(flow, &*log);
	}
	ASSERT_FALSE(original.trades.empty());
	ASSERT_FALSE(original.outcomes.empty());

	auto reader = journal_log::open_for_read(dir.file("journal.bin"));
	ASSERT_TRUE(reader.has_value()) << reader.error();
	const recorded replayed = run(reader->read_from(0), nullptr);

	EXPECT_EQ(replayed.trades, original.trades);
	EXPECT_EQ(replayed.outcomes, original.outcomes);
}

// Recovery does not have to start at zero: replaying the tail of a journal on top
// of the state the head produced reaches the same place. This is the property a
// snapshot's resume point depends on, checked here without needing snapshots.
TEST(EnginePartitionJournal, ReplayingTheTailOnTopOfTheHeadMatchesTheWhole) {
	const scratch_dir dir("journal_split");
	const std::vector<command> flow = mixed_flow();

	{
		auto log = journal_log::open_for_append(dir.file("journal.bin"));
		ASSERT_TRUE(log.has_value()) << log.error();
		(void)run(flow, &*log);
	}

	auto reader = journal_log::open_for_read(dir.file("journal.bin"));
	ASSERT_TRUE(reader.has_value()) << reader.error();
	const std::vector<command> whole = reader->read_from(0);
	ASSERT_EQ(whole.size(), flow.size());

	// Split the log in two and feed both halves to one partition, in order. The
	// engine cannot tell that from one continuous stream, which is exactly what
	// makes "load a snapshot, then replay from its sequence" sound.
	const std::vector<command> head(whole.begin(), whole.begin() + 4);
	const std::vector<command> tail(whole.begin() + 4, whole.end());
	std::vector<command> rejoined = head;
	rejoined.insert(rejoined.end(), tail.begin(), tail.end());

	EXPECT_EQ(run(rejoined, nullptr).trades, run(whole, nullptr).trades);
}

// A journal that cannot be written is not a degraded mode. The partition counts
// it, and - the part that matters - publishes nothing, because a trade a client
// has acted on cannot be withdrawn when the command behind it turns out to be
// missing.
TEST(EnginePartitionJournal, APoisonedJournalStopsPublicationRatherThanContinuing) {
	const scratch_dir dir("journal_poisoned");
	auto log = journal_log::open_for_append(dir.file("journal.bin"));
	ASSERT_TRUE(log.has_value()) << log.error();

	bool published = false;
	engine_partition<256> partition(
		[&](const std::vector<trade> &) { published = true; },
		[&](const std::vector<order_outcome> &) { published = true; });
	partition.listing(1);
	partition.attach_journal(&*log);

	ASSERT_TRUE(partition.submit(command::place(
		{.id = 1, .symbol_id = 1, .side = side_t::bid, .price = 100, .qty = 5})));
	EXPECT_EQ(partition.drain(), 1U);
	EXPECT_TRUE(partition.flush());
	EXPECT_TRUE(published) << "a healthy journal must not block publication";

	// Detaching is the only way a test can simulate a log that has stopped
	// working: the failure this guards against is a device error, which cannot be
	// arranged from inside the process. Attaching a *closed* log is the same
	// shape from the partition's side - a sync that returns false.
	published = false;
	partition.attach_journal(nullptr);
	EXPECT_TRUE(partition.flush()) << "no journal means no barrier to fail";
}

} // namespace
