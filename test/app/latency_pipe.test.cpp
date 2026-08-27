// The order path's two shapes: a pass-through, and a wire.
//
// Driven against a stub sink rather than through a session, because the cases
// worth pinning here are the ones a session cannot reach. A full wire is
// relieved only by time passing, and `live_session::quote` waits for that by
// retrying - so a hand-driven clock would hang rather than fail the assertion.
// Here the sink records instead of matching, refuses when told to, and the
// clock moves when a case says so.

#include "session/latency_pipe.hpp"

// manual_clock: the same one the risk and live-session suites drive.
#include "../risk_management/risk.fixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using exchange::engine::event::command;
using exchange::session::latency_pipe;
using latency_model = exchange::strategy::backtest::latency_model;

/// @brief A sink that records what reached it and can be told to refuse.
///
/// Named for what it is a stub *of*, per testing.md: `recording_sink` is
/// already taken by strategy.fixture.hpp, and two different classes under one
/// name at global scope is an ODR violation the linker resolves by picking one.
struct recording_pipe_sink {
	std::vector<command> accepted;
	bool has_room          = true;
	std::uint64_t refusals = 0;

	bool submit_range(std::span<const command> batch) {
		if (!has_room) {
			++refusals;
			return false;
		}
		accepted.insert(accepted.end(), batch.begin(), batch.end());
		return true;
	}
};

using test_pipe = latency_pipe<recording_pipe_sink, manual_clock>;

constexpr std::uint64_t PIPE_FLIGHT_NS = 1000;

/// @brief One order of ours - what the wire is for.
command our_place(exchange::order_id_t id) {
	return command::place(
		{.id = id, .side = exchange::side_t::bid, .price = 100, .qty = 1});
}

/// @brief The venue's depth being mirrored - what the wire must not delay.
command add_depth(exchange::price_t price) {
	return command::add(0, exchange::side_t::bid, price, 5);
}

latency_model flight(std::size_t max_in_flight = 16) {
	return latency_model{.order_entry_ns = PIPE_FLIGHT_NS,
						 .max_in_flight  = max_in_flight};
}

// --- no latency: the chain as it is without any of this -------------------

TEST(AppLatencyPipe, WithoutLatencyEverythingGoesStraightThrough) {
	recording_pipe_sink sink;
	manual_clock clock;
	test_pipe pipe(sink, clock);

	EXPECT_FALSE(pipe.is_modelled());
	const command batch[] = {our_place(1), our_place(2)};
	EXPECT_TRUE(pipe.submit_range(batch));
	EXPECT_EQ(sink.accepted.size(), 2U) << "the sink has it already";
	EXPECT_EQ(pipe.in_flight(), 0U);
	EXPECT_EQ(pipe.deliver_due(), 0U) << "nothing was held, so nothing is due";
	EXPECT_FALSE(pipe.next_due_ns().has_value());
}

TEST(AppLatencyPipe, WithoutLatencyARefusalIsTheSinksRefusal) {
	recording_pipe_sink sink;
	manual_clock clock;
	test_pipe pipe(sink, clock);

	sink.has_room         = false;
	const command batch[] = {our_place(1)};
	EXPECT_FALSE(pipe.submit_range(batch))
		<< "passed through, including the refusal - the gate rolls back";
	EXPECT_EQ(sink.refusals, 1U);
	EXPECT_EQ(pipe.refusals(), 0U) << "no wire refused it; the sink did";
}

// --- a wire: ours waits, the venue's does not ------------------------------

TEST(AppLatencyPipe, OurOrdersAreHeldForTheFlightTime) {
	recording_pipe_sink sink;
	manual_clock clock;
	test_pipe pipe(sink, clock, flight());

	ASSERT_TRUE(pipe.is_modelled());
	const command batch[] = {our_place(1), our_place(2)};
	ASSERT_TRUE(pipe.submit_range(batch));

	EXPECT_TRUE(sink.accepted.empty());
	EXPECT_EQ(pipe.in_flight(), 2U);
	ASSERT_TRUE(pipe.next_due_ns().has_value());
	EXPECT_EQ(*pipe.next_due_ns(), PIPE_FLIGHT_NS);

	EXPECT_EQ(pipe.deliver_due(), 0U) << "not due yet";
	clock.advance(PIPE_FLIGHT_NS);
	EXPECT_EQ(pipe.deliver_due(), 2U);
	EXPECT_EQ(sink.accepted.size(), 2U);
	EXPECT_EQ(pipe.in_flight(), 0U);
	EXPECT_EQ(pipe.delivered(), 2U);
}

TEST(AppLatencyPipe, MirroredDepthIsNotOursToDelay) {
	recording_pipe_sink sink;
	manual_clock clock;
	test_pipe pipe(sink, clock, flight());

	const command depth[] = {add_depth(100), add_depth(99)};
	ASSERT_TRUE(pipe.submit_range(depth));

	EXPECT_EQ(sink.accepted.size(), 2U)
		<< "the venue's liquidity already exists there; copying it is not a "
		   "message we sent";
	EXPECT_EQ(pipe.in_flight(), 0U);
	EXPECT_FALSE(pipe.next_due_ns().has_value());
}

TEST(AppLatencyPipe, AMixedBatchIsDelayedWholeRatherThanSplit) {
	// A batch is all-or-nothing, so it cannot be half delivered and half held -
	// that would hand the sink one order while telling the gate to roll back
	// both. Delaying the whole batch is the conservative resolution.
	recording_pipe_sink sink;
	manual_clock clock;
	test_pipe pipe(sink, clock, flight());

	const command mixed[] = {add_depth(100), our_place(1)};
	ASSERT_TRUE(pipe.submit_range(mixed));

	EXPECT_TRUE(sink.accepted.empty());
	EXPECT_EQ(pipe.in_flight(), 2U);
	clock.advance(PIPE_FLIGHT_NS);
	EXPECT_EQ(pipe.deliver_due(), 2U);
}

TEST(AppLatencyPipe, AnEmptyBatchIsAcceptedAndHoldsNothing) {
	recording_pipe_sink sink;
	manual_clock clock;
	test_pipe pipe(sink, clock, flight());

	EXPECT_TRUE(pipe.submit_range({}));
	EXPECT_EQ(pipe.in_flight(), 0U);
	EXPECT_FALSE(pipe.next_due_ns().has_value());
}

// --- the wire's own back-pressure -----------------------------------------

TEST(AppLatencyPipe, AFullWireRefusesTheBatchWholeAndCountsIt) {
	recording_pipe_sink sink;
	manual_clock clock;
	test_pipe pipe(sink, clock, flight(2));

	const command pair[] = {our_place(1), our_place(2)};
	ASSERT_TRUE(pipe.submit_range(pair));
	ASSERT_EQ(pipe.in_flight(), 2U);

	const command more[] = {our_place(3), our_place(4)};
	EXPECT_FALSE(pipe.submit_range(more)) << "the schedule is full";
	EXPECT_EQ(pipe.in_flight(), 2U) << "and it kept none of the refused batch";
	EXPECT_EQ(pipe.refusals(), 1U);

	// Relieved by time, which is what makes the retry in live_session::quote
	// terminate rather than deadlock.
	clock.advance(PIPE_FLIGHT_NS);
	ASSERT_EQ(pipe.deliver_due(), 2U);
	EXPECT_TRUE(pipe.submit_range(more))
		<< "room again once they were handed on";
}

TEST(AppLatencyPipe, ASinkWithNoRoomStagesTheDeliveryAndRetriesIt) {
	// The far side refusing is a statement about room, not about the commands,
	// so what was released stays released and is offered again. Dropping it
	// would silently change what the run did.
	recording_pipe_sink sink;
	manual_clock clock;
	test_pipe pipe(sink, clock, flight());

	const command batch[] = {our_place(1)};
	ASSERT_TRUE(pipe.submit_range(batch));
	clock.advance(PIPE_FLIGHT_NS);

	sink.has_room = false;
	EXPECT_EQ(pipe.deliver_due(), 0U);
	EXPECT_TRUE(pipe.is_stalled());
	EXPECT_EQ(pipe.stalls(), 1U);
	EXPECT_EQ(pipe.in_flight(), 1U) << "still ours until the sink takes it";

	sink.has_room = true;
	EXPECT_EQ(pipe.deliver_due(), 1U);
	EXPECT_FALSE(pipe.is_stalled());
	EXPECT_EQ(sink.accepted.size(), 1U);
}

} // namespace
