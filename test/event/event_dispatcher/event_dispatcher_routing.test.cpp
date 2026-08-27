#include "event/event_dispatcher.hpp"
#include "event_dispatcher.fixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

// Who gets which events, in what order, and in how many calls. The dispatcher's
// entire job is cutting one interleaved stream into maximal same-listing,
// same-kind runs - so the suites here are about where the cuts fall, and about
// the total order surviving all of them, because a strategy that sees a fill
// before the ack that preceded it is being lied to about its own order.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::event;

namespace {

// Four events per pump, so a suite can force a batch boundary without building
// a thousand-event script.
using Dispatcher = event_dispatcher<scripted_source, recording_handler, 4>;

TEST(EventDispatcherRouting, AnEmptySourceDeliversNothing) {
	scripted_source source({});
	recording_handler handler;
	Dispatcher route(source, handler);

	EXPECT_EQ(route.pump(), 0U);
	EXPECT_TRUE(handler.seen().empty());
	EXPECT_EQ(route.delivered(), 0U);
	EXPECT_EQ(route.pumps(), 0U); // an empty dequeue is not a batch
}

// The amortisation the span interface exists for: three consecutive trades on
// one listing reach the handler as one span of three, not three spans of one.
TEST(EventDispatcherRouting, ARunOfOneKindOnOneListingIsOneCall) {
	scripted_source source({engine_event::of(7, dispatcher_print(1)),
							engine_event::of(7, dispatcher_print(2)),
							engine_event::of(7, dispatcher_print(3))});
	recording_handler handler;
	Dispatcher route(source, handler);

	EXPECT_EQ(route.pump(), 3U);
	EXPECT_EQ(handler.trade_spans(), std::vector<std::size_t>{3U});
	EXPECT_TRUE(handler.outcome_spans().empty());
	EXPECT_EQ(
		handler.seen(),
		(std::vector<delivery>{{.symbol = 7, .is_trade = true, .id = 1},
							   {.symbol = 7, .is_trade = true, .id = 2},
							   {.symbol = 7, .is_trade = true, .id = 3}}));
}

// A change of kind ends a run: the two streams have different payload types and
// different entry points, so they cannot share a span.
TEST(EventDispatcherRouting, AChangeOfKindCutsTheRun) {
	scripted_source source(
		{engine_event::of(7, dispatcher_print(1)),
		 engine_event::of(7, dispatcher_print(2)),
		 engine_event::of(7, order_outcome::accepted(1, 4))});
	recording_handler handler;
	Dispatcher route(source, handler);

	EXPECT_EQ(route.pump(), 3U);
	EXPECT_EQ(handler.trade_spans(), std::vector<std::size_t>{2U});
	EXPECT_EQ(handler.outcome_spans(), std::vector<std::size_t>{1U});
	// Trades first, as the channel staged them - the fill, then the state it
	// left.
	ASSERT_EQ(handler.seen().size(), 3U);
	EXPECT_TRUE(handler.seen()[1].is_trade);
	EXPECT_FALSE(handler.seen()[2].is_trade);
}

// A change of listing ends a run too, and this is the cut that matters: the
// symbol is the argument the handler routes on, so two listings can never share
// one call.
TEST(EventDispatcherRouting, AChangeOfListingCutsTheRun) {
	scripted_source source({engine_event::of(7, dispatcher_print(1)),
							engine_event::of(8, dispatcher_print(2)),
							engine_event::of(8, dispatcher_print(3))});
	recording_handler handler;
	Dispatcher route(source, handler);

	EXPECT_EQ(route.pump(), 3U);
	EXPECT_EQ(handler.trade_spans(), (std::vector<std::size_t>{1U, 2U}));
	EXPECT_EQ(
		handler.seen(),
		(std::vector<delivery>{{.symbol = 7, .is_trade = true, .id = 1},
							   {.symbol = 8, .is_trade = true, .id = 2},
							   {.symbol = 8, .is_trade = true, .id = 3}}));
}

// Returning to a listing opens a new span rather than joining the earlier one:
// coalescing out of order would hand a strategy its events in the wrong
// sequence, which is the one thing the single ring was chosen to prevent.
TEST(EventDispatcherRouting, ReturningToAListingOpensANewCall) {
	scripted_source source({engine_event::of(7, dispatcher_print(1)),
							engine_event::of(8, dispatcher_print(2)),
							engine_event::of(7, dispatcher_print(3))});
	recording_handler handler;
	Dispatcher route(source, handler);

	EXPECT_EQ(route.pump(), 3U);
	EXPECT_EQ(handler.trade_spans(), (std::vector<std::size_t>{1U, 1U, 1U}));
	EXPECT_EQ(
		handler.seen(),
		(std::vector<delivery>{{.symbol = 7, .is_trade = true, .id = 1},
							   {.symbol = 8, .is_trade = true, .id = 2},
							   {.symbol = 7, .is_trade = true, .id = 3}}));
}

// Worst case for the run-cutter: every event cuts. It still delivers all of
// them, in order, one call each - degraded, not wrong.
TEST(EventDispatcherRouting, AFullyInterleavedStreamKeepsItsOrder) {
	std::vector<engine_event> script;
	for (order_id_t i = 1; i <= 4; ++i) {
		script.push_back(
			engine_event::of(static_cast<symbol_id_t>(i), dispatcher_print(i)));
		script.push_back(engine_event::of(static_cast<symbol_id_t>(i),
										  order_outcome::accepted(i, 4)));
	}
	scripted_source source(script);
	recording_handler handler;
	// One pump per two events at this batch size, so this also exercises the
	// buffer boundary landing mid-listing.
	Dispatcher route(source, handler);

	EXPECT_EQ(route.pump_all(), 8U);
	ASSERT_EQ(handler.seen().size(), 8U);
	for (std::size_t i = 0; i < 8U; ++i) {
		const auto expected_id = static_cast<order_id_t>(i / 2 + 1);
		EXPECT_EQ(handler.seen()[i].symbol,
				  static_cast<symbol_id_t>(expected_id));
		EXPECT_EQ(handler.seen()[i].id, expected_id);
		EXPECT_EQ(handler.seen()[i].is_trade, i % 2 == 0);
	}
}

// A pump takes one dequeue's worth and no more - BatchSize is a bound on the
// buffers, so exceeding it is not an option, and pump_all is how a caller says
// "as much as is there".
TEST(EventDispatcherRouting, APumpTakesAtMostOneBatch) {
	std::vector<engine_event> script;
	for (order_id_t i = 1; i <= 10; ++i)
		script.push_back(engine_event::of(7, dispatcher_print(i)));
	scripted_source source(script);
	recording_handler handler;
	Dispatcher route(source, handler);

	EXPECT_EQ(route.pump(), Dispatcher::BATCH_SIZE);
	EXPECT_EQ(route.pump(), Dispatcher::BATCH_SIZE);
	EXPECT_EQ(route.pump(), 2U); // whatever is left
	EXPECT_EQ(route.pump(), 0U);

	EXPECT_EQ(route.delivered(), 10U);
	EXPECT_EQ(route.pumps(), 3U);
	EXPECT_FALSE(route.is_stalled());
}

// pump_all stops when the source runs dry rather than spinning, so it
// terminates against a producer that is still publishing.
TEST(EventDispatcherRouting, PumpAllDrainsWhatIsThereAndStops) {
	std::vector<engine_event> script;
	for (order_id_t i = 1; i <= 10; ++i)
		script.push_back(engine_event::of(7, dispatcher_print(i)));
	scripted_source source(script);
	recording_handler handler;
	Dispatcher route(source, handler);

	EXPECT_EQ(route.pump_all(), 10U);
	EXPECT_EQ(source.remaining(), 0U);
	EXPECT_EQ(route.pump_all(), 0U);
}

// A run split across a batch boundary is two calls, and the split is invisible
// in the event order. Nothing about the boundary changes what the handler
// concludes.
TEST(EventDispatcherRouting, ARunSplitByABatchBoundaryStillArrivesInOrder) {
	std::vector<engine_event> script;
	for (order_id_t i = 1; i <= 6; ++i)
		script.push_back(engine_event::of(7, dispatcher_print(i)));
	scripted_source source(script);
	recording_handler handler;
	Dispatcher route(source, handler);

	EXPECT_EQ(route.pump_all(), 6U);
	EXPECT_EQ(handler.trade_spans(), (std::vector<std::size_t>{4U, 2U}));
	ASSERT_EQ(handler.seen().size(), 6U);
	for (std::size_t i = 0; i < 6U; ++i)
		EXPECT_EQ(handler.seen()[i].id, static_cast<order_id_t>(i + 1));
}

} // namespace
