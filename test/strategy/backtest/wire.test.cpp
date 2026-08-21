#include "strategy/backtest/wire.hpp"

#include "backtest.fixture.hpp"
#include "strategy/backtest/clock.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

// The modelled flight time between a strategy deciding to send a command and
// the engine having it. The cases divide into three claims: a zero-latency wire
// is indistinguishable from no wire at all, a non-zero one holds a command
// until market time reaches its due stamp, and nothing is ever lost or permuted
// on the way.

using namespace exchange;
using namespace exchange::strategy::backtest;

namespace {

/// @brief A wire onto a recording sink, with the clock that drives it.
///
/// Uses the real @c feed_clock rather than a stand-in: the wire's whole point
/// is that it runs on market time, and a clock that ran on anything else would
/// let the suite pass while the harness measured the machine. @see feed_clock
struct wire_under_test {
	feed_clock clock;
	recording_sink sink;
	wire<recording_sink, clock_view> flight;

	explicit wire_under_test(latency_model model = {})
		: flight(sink, clock_view{clock}, model) {}

	/// @brief Move market time to @p ns. Stamps are absolute and monotone, so a
	///        case walks forward through them the way a capture does.
	void at(std::uint64_t ns) { clock.advance_to(ns); }

	/// @brief A CANCEL naming @p id - the tersest command that carries an
	///        identity, which is all the ordering cases need.
	[[nodiscard]] static command tagged(order_id_t id) {
		return command::cancel(0, id);
	}

	[[nodiscard]] bool send(order_id_t id) { return flight.submit(tagged(id)); }

	/// @brief The ids the sink has been handed, in the order it got them.
	[[nodiscard]] std::vector<order_id_t> arrived() const {
		std::vector<order_id_t> ids;
		ids.reserve(sink.commands().size());
		for (const command &cmd : sink.commands())
			ids.push_back(cmd.as_cancel());
		return ids;
	}
};

constexpr std::uint64_t MICROSECOND = 1000;

} // namespace

TEST(BacktestWire, StartsWithNothingInFlight) {
	wire_under_test fixture;
	EXPECT_EQ(fixture.flight.in_flight(), 0U);
	EXPECT_EQ(fixture.flight.delivered(), 0U);
	EXPECT_FALSE(fixture.flight.is_stalled());
	EXPECT_EQ(fixture.flight.deliver(), 0U);
}

// The equivalence the default rests on: at zero latency a command is due the
// instant it is written, so the same settle round that wrote it delivers it and
// nothing downstream can tell a wire was there at all.
TEST(BacktestWire, DeliversImmediatelyAtZeroLatency) {
	wire_under_test fixture;
	fixture.at(5000);
	ASSERT_TRUE(fixture.send(7));

	EXPECT_EQ(fixture.flight.deliver(), 1U);
	EXPECT_EQ(fixture.arrived(), std::vector<order_id_t>{7});
	EXPECT_EQ(fixture.flight.in_flight(), 0U);
}

TEST(BacktestWire, HoldsACommandForItsFlightTime) {
	wire_under_test fixture{{.order_entry_ns = 50 * MICROSECOND}};
	fixture.at(1'000'000);
	ASSERT_TRUE(fixture.send(7));

	EXPECT_EQ(fixture.flight.deliver(), 0U) << "not due yet";
	EXPECT_EQ(fixture.flight.in_flight(), 1U);
	EXPECT_TRUE(fixture.sink.commands().empty());

	fixture.at(1'000'000 + 49 * MICROSECOND);
	EXPECT_EQ(fixture.flight.deliver(), 0U) << "still short of the due stamp";

	fixture.at(1'000'000 + 50 * MICROSECOND);
	EXPECT_EQ(fixture.flight.deliver(), 1U) << "due is inclusive";
	EXPECT_EQ(fixture.arrived(), std::vector<order_id_t>{7});
	EXPECT_EQ(fixture.flight.in_flight(), 0U);
	EXPECT_EQ(fixture.flight.delivered(), 1U);
}

// A command written later comes due later, so a wire is a pipeline rather than
// a gate that opens once.
TEST(BacktestWire, DeliversEachCommandOnItsOwnSchedule) {
	wire_under_test fixture{{.order_entry_ns = 10 * MICROSECOND}};
	fixture.at(100 * MICROSECOND);
	ASSERT_TRUE(fixture.send(1));
	fixture.at(105 * MICROSECOND);
	ASSERT_TRUE(fixture.send(2));

	fixture.at(110 * MICROSECOND);
	EXPECT_EQ(fixture.flight.deliver(), 1U);
	EXPECT_EQ(fixture.arrived(), std::vector<order_id_t>{1});

	fixture.at(115 * MICROSECOND);
	EXPECT_EQ(fixture.flight.deliver(), 1U);
	EXPECT_EQ(fixture.arrived(), (std::vector<order_id_t>{1, 2}));
}

// One message, one flight time: a batch is not permuted and does not straggle.
TEST(BacktestWire, DeliversABatchTogetherAndInOrder) {
	wire_under_test fixture{{.order_entry_ns = 10 * MICROSECOND}};
	const command batch[] = {wire_under_test::tagged(3),
							 wire_under_test::tagged(1),
							 wire_under_test::tagged(2)};
	fixture.at(MICROSECOND);
	ASSERT_TRUE(
		fixture.flight.submit_range(std::span<const command>{batch, 3}));

	fixture.at(11 * MICROSECOND);
	EXPECT_EQ(fixture.flight.deliver(), 3U);
	EXPECT_EQ(fixture.arrived(), (std::vector<order_id_t>{3, 1, 2}))
		<< "the order written, not the order of the ids";
	EXPECT_EQ(fixture.sink.batches(), 1U) << "one message arrives once";
}

TEST(BacktestWire, AcceptsAnEmptyBatchWithoutSchedulingAnything) {
	wire_under_test fixture;
	EXPECT_TRUE(fixture.flight.submit_range(std::span<const command>{}));
	EXPECT_EQ(fixture.flight.in_flight(), 0U);
}

TEST(BacktestWire, RefusesABatchThatWouldOverflowTheWire) {
	wire_under_test fixture{
		{.order_entry_ns = 10 * MICROSECOND, .max_in_flight = 2}};
	ASSERT_TRUE(fixture.send(1));
	ASSERT_TRUE(fixture.send(2));

	EXPECT_FALSE(fixture.send(3)) << "a full wire is back-pressure the gate "
									 "must be allowed to roll back against";
	EXPECT_EQ(fixture.flight.in_flight(), 2U);
	EXPECT_EQ(fixture.flight.refusals(), 1U);
}

TEST(BacktestWire, TakesMoreOnceDeliveryHasFreedRoom) {
	wire_under_test fixture{
		{.order_entry_ns = 10 * MICROSECOND, .max_in_flight = 1}};
	ASSERT_TRUE(fixture.send(1));
	ASSERT_FALSE(fixture.send(2));

	fixture.at(10 * MICROSECOND);
	ASSERT_EQ(fixture.flight.deliver(), 1U);
	EXPECT_TRUE(fixture.send(2));
}

// Back-pressure from the far side is retried, not dropped: a full command ring
// is a statement about room and evicting the commands would silently change
// what the run did.
TEST(BacktestWire, StagesADeliveryTheSinkHadNoRoomFor) {
	wire_under_test fixture;
	fixture.sink.refuse(true);
	ASSERT_TRUE(fixture.send(1));

	EXPECT_EQ(fixture.flight.deliver(), 0U);
	EXPECT_TRUE(fixture.flight.is_stalled());
	EXPECT_EQ(fixture.flight.in_flight(), 1U)
		<< "staged still counts as ours, not the engine's";
	EXPECT_EQ(fixture.flight.stalls(), 1U);

	fixture.sink.refuse(false);
	EXPECT_EQ(fixture.flight.deliver(), 1U);
	EXPECT_FALSE(fixture.flight.is_stalled());
	EXPECT_EQ(fixture.arrived(), std::vector<order_id_t>{1});
}

// And the staged batch keeps its place in front of whatever came due while it
// was stuck, so a stall cannot reorder the stream.
TEST(BacktestWire, KeepsAStagedDeliveryAheadOfNewlyDueCommands) {
	wire_under_test fixture;
	fixture.sink.refuse(true);
	ASSERT_TRUE(fixture.send(1));
	ASSERT_EQ(fixture.flight.deliver(), 0U);

	ASSERT_TRUE(fixture.send(2));
	fixture.sink.refuse(false);

	EXPECT_EQ(fixture.flight.deliver(), 2U);
	EXPECT_EQ(fixture.arrived(), (std::vector<order_id_t>{1, 2}));
}

TEST(BacktestWire, ReportsTheModelItWasBuiltWith) {
	wire_under_test fixture{{.order_entry_ns = 42}};
	EXPECT_EQ(fixture.flight.model().order_entry_ns, 42U);
}

namespace {

constexpr std::uint64_t JITTER = MICROSECOND;

/// @brief The flight times @p seed draws, read off the due stamp of the only
///        command on an otherwise-empty wire.
///
/// Submitting and then draining one at a time is what makes each draw visible
/// on its own; scheduling eight at once would only show the smallest.
std::vector<std::uint64_t> flight_times(std::uint64_t seed, int draws) {
	wire_under_test fixture{
		{.order_entry_ns = 0, .jitter_ns = JITTER, .seed = seed}};
	std::vector<std::uint64_t> times;
	std::uint64_t now = 1'000'000'000;
	for (int draw = 0; draw < draws; ++draw) {
		fixture.at(now);
		EXPECT_TRUE(fixture.flight.submit(wire_under_test::tagged(1)));
		times.push_back(fixture.flight.next_due_ns().value_or(now) - now);

		now += 1000 * MICROSECOND; // far past any draw, so the wire empties
		fixture.at(now);
		EXPECT_EQ(fixture.flight.deliver(), 1U);
	}
	return times;
}

} // namespace

// Jitter has to be reproducible or a run stops being a function of its input:
// two revisions of a strategy compared across two different draws measure the
// draw as much as the change.
TEST(BacktestWire, DrawsTheSameJitterFromTheSameSeed) {
	EXPECT_EQ(flight_times(12345, 8), flight_times(12345, 8));
}

TEST(BacktestWire, DrawsDifferentJitterFromADifferentSeed) {
	EXPECT_NE(flight_times(1, 8), flight_times(2, 8))
		<< "a seed that changed nothing would mean the jitter was not being "
		   "drawn from it";
}

TEST(BacktestWire, KeepsEveryDrawWithinTheConfiguredJitter) {
	for (const std::uint64_t drawn : flight_times(777, 32))
		EXPECT_LE(drawn, JITTER);
}

// The draws must actually vary, or "jitter" is a constant delay under another
// name and the two tests above would pass on a stub.
TEST(BacktestWire, DrawsMoreThanOneDistinctFlightTime) {
	const std::vector<std::uint64_t> times = flight_times(4242, 16);
	ASSERT_FALSE(times.empty());
	const auto differs = [&times](std::uint64_t t) { return t != times[0]; };
	EXPECT_TRUE(std::any_of(times.begin(), times.end(), differs));
}

// Jitter is drawn per message, so a batch cannot be split across two due times.
TEST(BacktestWire, DrawsJitterOncePerBatchRatherThanPerCommand) {
	wire_under_test fixture{
		{.order_entry_ns = 0, .jitter_ns = 1'000'000, .seed = 999}};
	const command batch[] = {wire_under_test::tagged(1),
							 wire_under_test::tagged(2),
							 wire_under_test::tagged(3)};
	ASSERT_TRUE(
		fixture.flight.submit_range(std::span<const command>{batch, 3}));

	fixture.at(1'000'000);
	EXPECT_EQ(fixture.flight.deliver(), 3U)
		<< "one draw for the message, so the whole of it is due at once";
	EXPECT_EQ(fixture.sink.batches(), 1U);
}

TEST(BacktestWire, AppliesNoJitterWhenNoneIsConfigured) {
	wire_under_test fixture{{.order_entry_ns = 10 * MICROSECOND}};
	ASSERT_TRUE(fixture.send(1));

	fixture.at(10 * MICROSECOND);
	EXPECT_EQ(fixture.flight.deliver(), 1U)
		<< "the flight time is exactly order_entry_ns, with nothing added";
}

// A batch the wire had no room for must leave the jitter sequence alone. If a
// refusal consumed a draw, the delays a run applied would depend on how full
// the wire happened to get - still reproducible, but for a reason nobody could
// reconstruct from the capture and the seed.
TEST(BacktestWire, DoesNotConsumeAJitterDrawOnARefusedBatch) {
	const auto draws = [](bool with_refusal) {
		wire_under_test fixture{{.order_entry_ns = 0,
								 .jitter_ns      = JITTER,
								 .seed           = 5150,
								 .max_in_flight  = 1}};
		std::vector<std::uint64_t> out;
		std::uint64_t now = 1'000'000'000;
		for (int i = 0; i < 4; ++i) {
			fixture.at(now);
			EXPECT_TRUE(fixture.send(1));
			out.push_back(fixture.flight.next_due_ns().value_or(now) - now);
			// The wire holds one command, so this one cannot be taken.
			if (with_refusal) EXPECT_FALSE(fixture.send(2));

			now += 1000 * MICROSECOND;
			fixture.at(now);
			EXPECT_EQ(fixture.flight.deliver(), 1U);
		}
		return out;
	};

	EXPECT_EQ(draws(false), draws(true));
}
