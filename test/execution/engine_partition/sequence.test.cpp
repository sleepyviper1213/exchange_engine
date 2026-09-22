#include "engine_partition.fixture.hpp"
#include "execution/engine_partition.hpp"

#include <gtest/gtest.h>

#include <vector>

// The engine sequence: the number a partition gives each command as it applies
// it, stamped on every record that command produced.
//
// Two properties carry the whole feature, and each test here pins one of them.
// It is *dense*, so a consumer can tell a lost message from a reordered one.
// And it is *derived from position in the applied stream* rather than assigned
// by a producer, which is what makes it equal to the command's index in the
// journal - and therefore what makes a replay comparable to the run it is
// reproducing. @see TODO.md #7, engine_sequence_t

using namespace exchange::engine;
using namespace exchange::engine::orders;
using namespace exchange;
using namespace exchange::engine::event;
using namespace exchange::engine::execution;

using engine_partition_test::Engine;

namespace {

/// @brief The sequence numbers on @p records, in order, so a test asserts
///        against the whole stamped stream rather than one record of it.
template <typename Record>
std::vector<engine_sequence_t>
partition_sequences_of(const std::vector<Record> &records) {
	std::vector<engine_sequence_t> numbers;
	numbers.reserve(records.size());
	for (const Record &record : records) numbers.push_back(record.sequence);
	return numbers;
}

} // namespace

TEST(EnginePartitionSequence, NumberingStartsAtOneAndCountsCommands) {
	Engine engine(nullptr);
	EXPECT_EQ(engine.sequence(), 0U) << "nothing applied yet";

	ASSERT_TRUE(engine.submit(command::place(
		{.id = 1, .side = side_t::ask, .price = 100, .qty = 10})));
	ASSERT_TRUE(engine.submit(command::cancel(0, 1)));
	ASSERT_EQ(engine.drain(), 2U);

	EXPECT_EQ(engine.sequence(), 2U);
	// The ACCEPTED came off command 1 and the CANCELLED off command 2, which is
	// the correlation a client needs to tie an answer to what it sent.
	EXPECT_EQ(partition_sequences_of(engine.outcomes()),
			  (std::vector<engine_sequence_t>{1U, 2U}));
}

TEST(EnginePartitionSequence, EveryRecordOneCommandProducedSharesItsNumber) {
	Engine engine(nullptr);

	ASSERT_TRUE(engine.submit(command::place(
		{.id = 1, .side = side_t::ask, .price = 100, .qty = 10})));
	ASSERT_TRUE(engine.submit(command::place(
		{.id = 2, .side = side_t::bid, .price = 100, .qty = 4})));
	ASSERT_EQ(engine.drain(), 2U);

	// The crossing is one command: an ACCEPTED, two FILLs and the trade between
	// them all belong to it. A per-record counter would have spread them over
	// four numbers and made the group unrecoverable.
	ASSERT_EQ(engine.trades().size(), 1U);
	EXPECT_EQ(engine.trades()[0].sequence, 2U);
	EXPECT_EQ(partition_sequences_of(engine.outcomes()),
			  (std::vector<engine_sequence_t>{1U, 2U, 2U, 2U}));
}

TEST(EnginePartitionSequence, NumberingIsContinuousAcrossDrains) {
	Engine engine(nullptr);

	ASSERT_TRUE(engine.submit(command::place(
		{.id = 1, .side = side_t::ask, .price = 100, .qty = 10})));
	ASSERT_EQ(engine.drain(), 1U);
	ASSERT_TRUE(engine.flush());

	ASSERT_TRUE(engine.submit(command::cancel(0, 1)));
	ASSERT_EQ(engine.drain(), 1U);

	// A batch boundary is a publication boundary and nothing else. Restarting
	// the count at each drain would make the number mean "position within a
	// batch", which no consumer can join anything on.
	EXPECT_EQ(engine.sequence(), 2U);
	ASSERT_EQ(engine.outcomes().size(), 1U);
	EXPECT_EQ(engine.outcomes()[0].sequence, 2U);
}

TEST(EnginePartitionSequence, AMisroutedCommandTakesANumberLikeAnyOther) {
	Engine engine(nullptr); // carries symbol 0 only

	ASSERT_TRUE(engine.submit(command::place({.id        = 7,
											  .symbol_id = 9,
											  .side      = side_t::bid,
											  .price     = 100,
											  .qty       = 10})));
	ASSERT_TRUE(engine.submit(command::place(
		{.id = 8, .side = side_t::bid, .price = 100, .qty = 10})));
	ASSERT_EQ(engine.drain(), 2U);

	// It came off the queue, it went into the journal, and it was answered - so
	// skipping it would leave a hole where a consumer reads a lost message, and
	// reusing its number would put the following command at the wrong index in
	// the journal.
	EXPECT_EQ(engine.misrouted(), 1U);
	EXPECT_EQ(partition_sequences_of(engine.outcomes()),
			  (std::vector<engine_sequence_t>{1U, 2U}));
	EXPECT_EQ(engine.outcomes()[0].reason, reject_reason::UNKNOWN_SYMBOL);
}

TEST(EnginePartitionSequence,
	 ADepthCommandTakesANumberEvenThoughItAnswersNone) {
	Engine engine(nullptr);

	// ADD carries no identity, so there is nobody to report to and no record
	// comes out of it. The number is still spent, because the journal holds the
	// command and recovery replays it.
	ASSERT_TRUE(engine.submit(command::add(0, side_t::bid, 100, 10)));
	ASSERT_TRUE(engine.submit(command::place(
		{.id = 1, .side = side_t::bid, .price = 99, .qty = 10})));
	ASSERT_EQ(engine.drain(), 2U);

	EXPECT_EQ(engine.sequence(), 2U);
	ASSERT_EQ(engine.outcomes().size(), 1U);
	EXPECT_EQ(engine.outcomes()[0].sequence, 2U);
}

TEST(EnginePartitionSequence, ResumingContinuesAfterTheNumberGivenBack) {
	Engine engine(nullptr);

	// What a recovery does when it loads a snapshot instead of replaying from
	// record zero: the commands behind that state were never applied here, so
	// counting from zero would re-issue numbers the last session published.
	engine.resume_sequence(1000);
	EXPECT_EQ(engine.sequence(), 1000U);

	ASSERT_TRUE(engine.submit(command::place(
		{.id = 1, .side = side_t::bid, .price = 100, .qty = 10})));
	ASSERT_EQ(engine.drain(), 1U);

	ASSERT_EQ(engine.outcomes().size(), 1U);
	EXPECT_EQ(engine.outcomes()[0].sequence, 1001U);
}

TEST(EnginePartitionSequence,
	 ReplayingTheSameCommandsReproducesTheSameNumbers) {
	const std::vector<command> stream{
		command::place({.id = 1, .side = side_t::ask, .price = 100, .qty = 10}),
		command::place({.id = 2, .side = side_t::bid, .price = 100, .qty = 4}),
		command::cancel(0, 1),
		command::place({.id = 3, .side = side_t::bid, .price = 99, .qty = 7}),
	};

	engine_partition_test::recording live;
	engine_partition_test::recording replayed;
	for (engine_partition_test::recording *into : {&live, &replayed}) {
		Engine engine(engine_partition_test::trade_sink(*into),
					  engine_partition_test::outcome_sink(*into));
		ASSERT_TRUE(engine.submit_range(stream));
		ASSERT_EQ(engine.drain(), stream.size());
		ASSERT_TRUE(engine.flush());
	}

	// The comparison reads every field, sequence and trade id included, so this
	// fails the moment either is derived from anything but position in the
	// applied stream - a clock read, an address, a counter carried across runs.
	// That is the property recovery rests on, stated as a test rather than as a
	// comment. @see EnginePartitionRecovery for the same claim over a journal.
	EXPECT_EQ(live, replayed);
	EXPECT_FALSE(live.trades.empty());
}
