#include "event_dispatcher.fixture.hpp"
#include "trading-engine/event/event_dispatcher.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

// What happens when the far end cannot take everything it is offered. A
// strategy host turns events back into commands and can fill its outbound queue
// mid-batch, so a short return is normal traffic and not an error - and the
// only wrong answers are dropping the remainder or delivering it out of order.
// These suites exist to fail if either ever becomes possible.
//
// A stalled pump must also not dequeue: taking more events while events are
// already undelivered would put newer ones behind older ones in the buffer,
// which is the reordering the single ring was chosen to prevent.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::event;

namespace {

using Dispatcher = event_dispatcher<scripted_source, recording_handler, 4>;

std::vector<engine_event> trades_on(symbol_id_t symbol, order_id_t count) {
	std::vector<engine_event> script;
	for (order_id_t i = 1; i <= count; ++i)
		script.push_back(engine_event::of(symbol, print(i)));
	return script;
}

// A handler taking one event per call still makes progress, one per pump, and
// the dispatcher keeps the rest rather than dropping it.
TEST(EventDispatcherBackpressure, APartlyAcceptedRunIsHeldAndResumed) {
	scripted_source source(trades_on(7, 3));
	recording_handler handler(1);
	Dispatcher route(source, handler);

	EXPECT_EQ(route.pump(), 1U);
	EXPECT_TRUE(route.is_stalled());
	EXPECT_EQ(route.backlog(), 2U);
	EXPECT_EQ(route.stalls(), 1U);

	EXPECT_EQ(route.pump(), 1U);
	EXPECT_EQ(route.backlog(), 1U);

	EXPECT_EQ(route.pump(), 1U);
	EXPECT_FALSE(route.is_stalled());
	EXPECT_EQ(route.backlog(), 0U);

	// Three events, once each, in the order they were published.
	ASSERT_EQ(handler.seen().size(), 3U);
	for (std::size_t i = 0; i < 3U; ++i)
		EXPECT_EQ(handler.seen()[i].id, static_cast<order_id_t>(i + 1));
}

// The invariant that keeps a stall from reordering the stream: while a batch is
// part-delivered, the dispatcher does not ask the source for more.
TEST(EventDispatcherBackpressure, AStalledPumpDoesNotDequeue) {
	scripted_source source(trades_on(7, 3), /*chunk=*/3);
	recording_handler handler(1);
	Dispatcher route(source, handler);

	EXPECT_EQ(route.pump(), 1U);
	EXPECT_EQ(source.calls(), 1U); // the one that filled the buffer
	ASSERT_TRUE(route.is_stalled());

	EXPECT_EQ(route.pump(), 1U);
	EXPECT_EQ(source.calls(), 1U) << "a stalled pump must not take more events";
	EXPECT_EQ(source.remaining(), 0U);
}

// A handler that refuses everything makes a pump report zero - the same number
// an empty channel reports, which is why is_stalled() exists to tell them
// apart. They call for opposite responses: wait for the engine, or go drain
// whatever the handler is blocked on.
TEST(EventDispatcherBackpressure,
	 ATotalRefusalIsDistinguishableFromAnEmptySource) {
	scripted_source source(trades_on(7, 3));
	recording_handler handler(0);
	Dispatcher route(source, handler);

	EXPECT_EQ(route.pump(), 0U);
	EXPECT_TRUE(route.is_stalled());
	EXPECT_EQ(route.backlog(), 3U);
	EXPECT_TRUE(handler.seen().empty());

	// And it does not spin: pump_all gives up on a pump that delivered nothing.
	EXPECT_EQ(route.pump_all(), 0U);
	EXPECT_TRUE(route.is_stalled());

	// Once the handler can take them, everything held is still there and still
	// in order.
	handler.unblock();
	EXPECT_EQ(route.pump_all(), 3U);
	EXPECT_FALSE(route.is_stalled());
	ASSERT_EQ(handler.seen().size(), 3U);
	EXPECT_EQ(handler.seen()[0].id, 1U);
	EXPECT_EQ(handler.seen()[2].id, 3U);
}

// The case a partial delivery has to get right: the remainder of the stalled
// batch goes out *before* anything newer, so the handler never sees event 4
// ahead of event 2.
TEST(EventDispatcherBackpressure, TheRemainderIsDeliveredBeforeAnythingNewer) {
	scripted_source source(trades_on(7, 6), /*chunk=*/3);
	recording_handler handler(1);
	Dispatcher route(source, handler);

	EXPECT_EQ(route.pump(), 1U); // 1 of the first three
	EXPECT_EQ(route.pump(), 1U); // 2 of the first three
	ASSERT_TRUE(route.is_stalled());
	handler.unblock();

	// One pump, and it does both halves in the required order: the third event
	// of the stalled batch first, and only then the dequeue that brings 4, 5
	// and 6.
	EXPECT_EQ(route.pump(), 4U);
	EXPECT_FALSE(route.is_stalled());
	EXPECT_EQ(route.pump_all(), 0U) << "nothing should be left to fetch";

	ASSERT_EQ(handler.seen().size(), 6U);
	for (std::size_t i = 0; i < 6U; ++i)
		EXPECT_EQ(handler.seen()[i].id, static_cast<order_id_t>(i + 1))
			<< "at " << i;
}

// A stall in the middle of a multi-run batch resumes inside the run it stopped
// in, not at the start of it - an event delivered twice is as wrong as one
// lost.
TEST(EventDispatcherBackpressure, AResumeDoesNotRedeliverWhatWasAccepted) {
	scripted_source source({engine_event::of(7, print(1)),
							engine_event::of(7, print(2)),
							engine_event::of(8, print(3))});
	recording_handler handler(1);
	Dispatcher route(source, handler);

	// The run of two on listing 7 stalls after one. The second pump resumes
	// into the *middle* of it - one event, not two - and then reaches listing
	// 7's neighbour, which the cap lets through as a run of its own. Two
	// events, and neither of them is event 1 again.
	EXPECT_EQ(route.pump(), 1U);
	EXPECT_EQ(route.pump(), 2U);
	EXPECT_FALSE(route.is_stalled());
	handler.unblock();
	EXPECT_EQ(route.pump(), 0U);

	EXPECT_EQ(
		handler.seen(),
		(std::vector<delivery>{{.symbol = 7, .is_trade = true, .id = 1},
							   {.symbol = 7, .is_trade = true, .id = 2},
							   {.symbol = 8, .is_trade = true, .id = 3}}));
	EXPECT_EQ(route.delivered(), 3U);
}

} // namespace
