#include "core/persistence/persistence.fixture.hpp"
#include "core/persistence/record_log.hpp"
#include "engine_partition.fixture.hpp"
#include "event/command.hpp"
#include "event/journal_record.hpp"
#include "execution/engine_partition.hpp"
#include "orders/side.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <utility>
#include <vector>

// The claim TODO.md #6 exists to make good on: the engine is deterministic, so
// a log of what it was *asked* is enough to reproduce what it *did*. Nothing
// here records a trade - trades are re-derived by replaying commands into a
// fresh partition, and the test is that they come out identical.
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

using engine_partition_test::outcome_sink;
using engine_partition_test::recording;
using engine_partition_test::trade_sink;

namespace {

using journal_log = engine_partition<256>::journal;

/// @brief Every record in @p journal, decoded back into commands.
///
/// The journal holds the encoded form now, so a test that wants to replay what
/// was recorded has to come back through decode() - which is itself part of
/// what these suites check: a round trip that lost a field would show up here
/// as a replay that produced different trades.
std::vector<command> decoded(const std::vector<journal_record> &records) {
	std::vector<command> commands;
	commands.reserve(records.size());
	for (const journal_record &record : records) {
		const auto cmd = decode(record);
		EXPECT_TRUE(cmd.has_value()) << cmd.error();
		if (cmd) commands.push_back(*cmd);
	}
	return commands;
}

/// @brief Run @p commands through a fresh partition and report what it
/// published.
///
/// @param log Attached before the first drain when non-null, so a run either
///        journals everything or nothing - a partition that started recording
///        half way through would produce a log that replays to a different
///        book.
recording run(const std::vector<command> &commands, journal_log *log) {
	recording seen;
	engine_partition<256> partition(trade_sink(seen), outcome_sink(seen));
	partition.listing(1);
	partition.listing(2);
	partition.attach_journal(log);

	// Batched deliberately: the durability barrier is per flush, so driving
	// this in more than one batch is what exercises it more than once.
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
std::vector<command> journal_mixed_flow() {
	return {
		command::place({.id        = 1,
						.symbol_id = 1,
						.side      = side_t::ask,
						.price     = 100,
						.qty       = 10}),
		command::place({.id        = 2,
						.symbol_id = 1,
						.side      = side_t::ask,
						.price     = 101,
						.qty       = 5}),
		command::place({.id        = 3,
						.symbol_id = 2,
						.side      = side_t::bid,
						.price     = 90,
						.qty       = 7}),
		// Crosses id 1 partly.
		command::place({.id        = 4,
						.symbol_id = 1,
						.side      = side_t::bid,
						.price     = 100,
						.qty       = 4}),
		// Cancels what is left of id 1.
		command::cancel(1, 1),
		// A cancel for an order that never existed: CANCEL_REJECTED.
		command::cancel(1, 999),
		// A listing this partition does not carry: rejected, but still applied
		// off the queue and so still journalled.
		command::place({.id        = 5,
						.symbol_id = 9,
						.side      = side_t::bid,
						.price     = 50,
						.qty       = 1}),
		command::place({.id        = 6,
						.symbol_id = 2,
						.side      = side_t::ask,
						.price     = 90,
						.qty       = 7}),
	};
}

TEST(EnginePartitionJournal, WithNoJournalAttachedFlushStillSucceeds) {
	const recording seen = run(journal_mixed_flow(), nullptr);
	EXPECT_FALSE(seen.outcomes.empty());
}

TEST(EnginePartitionJournal, EveryAppliedCommandIsRecordedInOrder) {
	const scratch_dir dir("journal_records");
	const std::vector<command> flow = journal_mixed_flow();

	{
		auto log = journal_log::open_for_append(dir.file("journal.bin"));
		ASSERT_TRUE(log.has_value()) << log.error();
		(void)run(flow, &*log);
		EXPECT_EQ(log->count(), flow.size());
	}

	auto reader = journal_log::open_for_read(dir.file("journal.bin"));
	ASSERT_TRUE(reader.has_value()) << reader.error();
	const std::vector<command> recording_flow = decoded(reader->read_from(0));
	ASSERT_EQ(recording_flow.size(), flow.size());
	for (std::size_t i = 0; i < flow.size(); ++i) {
		EXPECT_EQ(recording_flow[i].type, flow[i].type) << "at " << i;
		EXPECT_EQ(recording_flow[i].symbol, flow[i].symbol) << "at " << i;
	}
}

// The one that matters. Replay the journal into a partition that has never seen
// any of it, and the trades and outcomes must match the original run exactly -
// not merely in count, but record for record.
TEST(EnginePartitionJournal, ReplayingTheJournalReproducesTheRunExactly) {
	const scratch_dir dir("journal_replay");
	const std::vector<command> flow = journal_mixed_flow();

	recording original;
	{
		auto log = journal_log::open_for_append(dir.file("journal.bin"));
		ASSERT_TRUE(log.has_value()) << log.error();
		original = run(flow, &*log);
	}
	ASSERT_FALSE(original.trades.empty());
	ASSERT_FALSE(original.outcomes.empty());

	auto reader = journal_log::open_for_read(dir.file("journal.bin"));
	ASSERT_TRUE(reader.has_value()) << reader.error();
	const recording replayed = run(decoded(reader->read_from(0)), nullptr);

	EXPECT_EQ(replayed.trades, original.trades);
	EXPECT_EQ(replayed.outcomes, original.outcomes);
}

// Recovery does not have to start at zero: replaying the tail of a journal on
// top of the state the head produced reaches the same place. This is the
// property a snapshot's resume point depends on, checked here without needing
// snapshots.
TEST(EnginePartitionJournal, ReplayingTheTailOnTopOfTheHeadMatchesTheWhole) {
	const scratch_dir dir("journal_split");
	const std::vector<command> flow = journal_mixed_flow();

	{
		auto log = journal_log::open_for_append(dir.file("journal.bin"));
		ASSERT_TRUE(log.has_value()) << log.error();
		(void)run(flow, &*log);
	}

	auto reader = journal_log::open_for_read(dir.file("journal.bin"));
	ASSERT_TRUE(reader.has_value()) << reader.error();
	const std::vector<command> whole = decoded(reader->read_from(0));
	ASSERT_EQ(whole.size(), flow.size());

	// The head into one partition, then the tail into that *same* partition -
	// which is the point, and the reason this cannot be written as two vectors
	// concatenated and run once. What is being checked is that the tail applies
	// correctly to state it did not build, because that is precisely what a
	// recovery does: load a snapshot, then replay from its sequence onto it. A
	// version of this that rejoined the halves and compared the result with the
	// whole was comparing a run against itself and would have passed no matter
	// what the resume point did.
	constexpr std::size_t SPLIT = 4;
	recording resumed;
	{
		engine_partition<256> partition(trade_sink(resumed),
										outcome_sink(resumed));
		partition.listing(1);
		partition.listing(2);

		// Two sittings, with a flush between them: the second batch starts
		// against books the first left behind, not against empty ones.
		const auto sitting = [&](std::size_t from, std::size_t to) {
			for (std::size_t at = from; at < to; ++at)
				ASSERT_TRUE(partition.submit(whole[at]));
			EXPECT_EQ(partition.drain_and_flush(), to - from);
		};
		sitting(0, SPLIT);
		sitting(SPLIT, whole.size());
	}

	const recording continuous = run(whole, nullptr);
	EXPECT_EQ(resumed.trades, continuous.trades);
	EXPECT_EQ(resumed.outcomes, continuous.outcomes);
	ASSERT_FALSE(continuous.trades.empty()) << "a flow with no trades in it "
											   "proves nothing about resuming";
}

// A journal that cannot be written is not a degraded mode. The partition stops:
// the command is not applied, nothing is published, and every later drain and
// flush refuses too.
//
// The poisoned log is a real one, and the poison is portable. An append to a
// handle opened for reading fails on every platform, and a failed append
// poisons the log, so its sync fails from then on as well - which is the other
// half of the barrier without depending on whether a given platform will fsync
// a read-only descriptor.
journal_log poisoned_log(const std::filesystem::path &path) {
	{
		auto seed = journal_log::open_for_append(path);
		EXPECT_TRUE(seed.has_value());
	}
	auto log = journal_log::open_for_read(path);
	EXPECT_TRUE(log.has_value());
	const command doomed = command::place(
		{.id = 1, .symbol_id = 1, .side = side_t::bid, .price = 1, .qty = 1});
	EXPECT_FALSE(log->append(encode(doomed)))
		<< "a read handle must refuse an append";
	EXPECT_FALSE(log->sync()) << "and a poisoned log must refuse a barrier";
	return std::move(*log);
}

TEST(EnginePartitionJournal, ACommandThatCannotBeJournalledIsNotApplied) {
	const scratch_dir dir("journal_poisoned");
	journal_log log = poisoned_log(dir.file("journal.bin"));

	bool published = false;
	engine_partition<256> partition(
		[&](const std::vector<trade> &) { published = true; },
		[&](const std::vector<order_outcome> &) { published = true; });
	partition.listing(1);
	partition.attach_journal(&log);

	ASSERT_TRUE(partition.submit(command::place({.id        = 1,
												 .symbol_id = 1,
												 .side      = side_t::bid,
												 .price     = 100,
												 .qty       = 5})));

	// Off the queue, refused by the log, and never handed to the engine. The
	// book is the assertion that matters: a command applied without being
	// recording puts the books somewhere no replay of this log can reach, which
	// is the one failure recovery cannot repair.
	EXPECT_EQ(partition.drain(), 0U);
	EXPECT_EQ(partition.orders().live(), 0U)
		<< "applied an unjournalled command";
	EXPECT_FALSE(partition.book(1)->best_bid().has_value());
	EXPECT_FALSE(partition.flush());
	EXPECT_FALSE(published)
		<< "published behind a journal that refused the write";
	EXPECT_EQ(partition.journal_failures(), 1U);
	EXPECT_TRUE(partition.is_journal_faulted());
}

// And the fault is terminal. A venue that kept matching after losing durability
// would be publishing trades it cannot prove it made, so the partition does not
// resume - it fills its queue and back-pressures the producer instead.
TEST(EnginePartitionJournal,
	 AFaultedPartitionAppliesAndPublishesNothingFurther) {
	const scratch_dir dir("journal_faulted");
	journal_log log = poisoned_log(dir.file("journal.bin"));

	bool published = false;
	engine_partition<4> partition(
		[&](const std::vector<trade> &) { published = true; },
		[&](const std::vector<order_outcome> &) { published = true; });
	partition.listing(1);
	partition.attach_journal(&log);

	const auto place = [](order_id_t id) {
		return command::place({.id        = id,
							   .symbol_id = 1,
							   .side      = side_t::bid,
							   .price     = 100,
							   .qty       = 1});
	};
	ASSERT_TRUE(partition.submit(place(1)));
	ASSERT_EQ(partition.drain(), 0U);
	ASSERT_TRUE(partition.is_journal_faulted());

	// Four slots, five submissions: without a drain that consumes them the
	// queue fills and the producer is told, which is the whole point of
	// stopping rather than dropping.
	std::size_t accepted = 0;
	for (order_id_t id = 2; id <= 6; ++id)
		if (partition.submit(place(id))) ++accepted;
	EXPECT_EQ(accepted, 4U) << "a stopped partition must not keep draining";

	EXPECT_EQ(partition.drain_and_flush(), 0U);
	EXPECT_EQ(partition.orders().live(), 0U);
	EXPECT_FALSE(partition.flush());
	EXPECT_FALSE(published);

	// Nor does swapping the log out clear it: the commands the old one lost are
	// not in the new one, so there is nothing a fresh log makes true again.
	partition.attach_journal(nullptr);
	EXPECT_TRUE(partition.is_journal_faulted());
	EXPECT_EQ(partition.drain(), 0U);
	EXPECT_FALSE(partition.flush());
	EXPECT_FALSE(published);
}

// A barrier is a device round trip, and an idle consumer polls this loop. So a
// flush with nothing appended must not issue one - checked with a log that
// would refuse any barrier it was asked for, which makes "was one attempted" a
// visible difference rather than a timing argument.
TEST(EnginePartitionJournal, AnEmptyBatchIssuesNoDurabilityBarrier) {
	const scratch_dir dir("journal_idle");
	journal_log log = poisoned_log(dir.file("journal.bin"));

	engine_partition<256> partition(nullptr);
	partition.listing(1);
	partition.attach_journal(&log);

	EXPECT_EQ(partition.drain(), 0U);
	EXPECT_TRUE(partition.flush()) << "synced a journal that owed nothing";
	EXPECT_EQ(partition.drain_and_flush(), 0U);
	EXPECT_FALSE(partition.is_journal_faulted());
	EXPECT_EQ(partition.journal_failures(), 0U);
}

} // namespace
