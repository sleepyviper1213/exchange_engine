// The return path into risk, end to end: a partition publishes, the dispatcher
// routes, and each listing's gate learns what became of its own orders.
//
// The gap this closes is worth stating, because it is invisible until it has
// been running for an hour: a gate that is never fed keeps every order it has
// ever sent in its working ledger, so its exposure only ever ratchets up and it
// eventually refuses everything. So the assertions here are about the loop
// actually closing - working quantity coming back down - and about routing being
// exact, since a print delivered to the wrong listing's gate moves a position
// that never traded.

#include "hooks.fixture.hpp"
#include "event/event_dispatcher.hpp"

// The event side already has a source that hands out a scripted batch, which is
// what makes a pump's boundaries predictable. Reaching for it beats copying it.
#include "event/event_dispatcher/event_dispatcher.fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <span>


namespace {

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::event;
using namespace exchange::risk;
using namespace exchange::risk::hooks;

// The two claims that make this type usable at all: it is what a dispatcher
// accepts, and a gate is what it accepts. Asserted here rather than in the
// module, which would have to include the whole dispatcher for one line - the
// same reason the command_sink conformance lives in a test. @see feedback.hpp
static_assert(feedback_gate<test_gate>);
static_assert(event_handler<feedback_router<test_gate>>);

/// @brief A gate double missing the one thing routing needs.
struct symbolless_gate {
	void on_trades(std::span<const trade>) noexcept {}
	void on_outcomes(std::span<const order_outcome>) noexcept {}
};

static_assert(!feedback_gate<symbolless_gate>,
			  "routing is by the gate's own listing; a gate that cannot name "
			  "one cannot be attached");

TEST(RiskHooksFeedback, AttachRoutesByTheGatesOwnListing) {
	feedback_desk desk;
	desk.attach_all();

	EXPECT_EQ(desk.router().gate_for(SYMBOL), &desk.first());
	EXPECT_EQ(desk.router().gate_for(OTHER_SYMBOL), &desk.second());
	EXPECT_EQ(desk.router().gate_for(UNSCREENED_SYMBOL), nullptr);
	EXPECT_EQ(desk.router().listings(), 2U);

	// In range is not the same question as attached, and both are worth asking:
	// a symbol past the table can never be routed however it is wired.
	EXPECT_TRUE(desk.router().carries(UNSCREENED_SYMBOL));
	EXPECT_FALSE(desk.router().carries(feedback_desk::LISTINGS));
}

TEST(RiskHooksFeedback, AFillReachesOnlyTheGateThatScreenedIt) {
	feedback_desk desk;
	desk.attach_all();
	ASSERT_TRUE(desk.place(SYMBOL, 1, 100, 10));
	ASSERT_TRUE(desk.place(OTHER_SYMBOL, 2, 100, 10));
	ASSERT_EQ(desk.working(SYMBOL, side_t::bid), 10);
	ASSERT_EQ(desk.working(OTHER_SYMBOL, side_t::bid), 10);

	const std::array fills{filled_at(1, 999, 100, 10)};
	EXPECT_EQ(desk.router().on_trades(SYMBOL, fills), 1U);

	// The listing that traded moved; the one that did not is untouched, and that
	// is the whole point of routing by symbol rather than broadcasting.
	EXPECT_EQ(desk.net(SYMBOL), 10);
	EXPECT_EQ(desk.working(SYMBOL, side_t::bid), 0);
	EXPECT_EQ(desk.net(OTHER_SYMBOL), 0);
	EXPECT_EQ(desk.working(OTHER_SYMBOL, side_t::bid), 10);
	EXPECT_EQ(desk.first().working_orders(), 0U);
	EXPECT_EQ(desk.second().working_orders(), 1U);
	EXPECT_EQ(desk.router().applied_trades(), 1U);
}

TEST(RiskHooksFeedback, ATerminalOutcomeGivesTheWorkingQuantityBack) {
	// The half a fill-only loop would miss: a cancelled order never prints, so
	// nothing but the outcome can tell the gate its exposure is gone.
	feedback_desk desk;
	desk.attach_all();
	ASSERT_TRUE(desk.place(SYMBOL, 1, 100, 10));
	ASSERT_EQ(desk.first().working_orders(), 1U);

	const std::array records{withdrawn(1, 10)};
	EXPECT_EQ(desk.router().on_outcomes(SYMBOL, records), 1U);

	EXPECT_EQ(desk.working(SYMBOL, side_t::bid), 0);
	EXPECT_EQ(desk.first().working_orders(), 0U);
	EXPECT_EQ(desk.net(SYMBOL), 0); // withdrawn, not filled
	EXPECT_EQ(desk.router().applied_outcomes(), 1U);
}

TEST(RiskHooksFeedback, AnUnscreenedListingIsConsumedAndCounted) {
	// The anti-hang property. A short return means "re-deliver this", so a
	// handler that dropped an unroutable event by refusing it would wedge the
	// dispatcher on that run forever. @see feedback.hpp
	feedback_desk desk;
	desk.attach_all();
	const std::array fills{filled_at(7, 8, 100, 3), filled_at(9, 8, 101, 4)};

	EXPECT_EQ(desk.router().on_trades(UNSCREENED_SYMBOL, fills), 2U);
	// Past the end of the table, which must be a miss and not a read.
	EXPECT_EQ(desk.router().on_trades(symbol_id_t{9'999}, fills), 2U);
	const std::array records{withdrawn(7, 3)};
	EXPECT_EQ(desk.router().on_outcomes(UNSCREENED_SYMBOL, records), 1U);

	EXPECT_EQ(desk.router().unrouted(), 5U);
	EXPECT_EQ(desk.router().applied_trades(), 0U);
	EXPECT_EQ(desk.router().applied_outcomes(), 0U);
	EXPECT_EQ(desk.net(SYMBOL), 0);
	EXPECT_EQ(desk.net(OTHER_SYMBOL), 0);
}

TEST(RiskHooksFeedback, AnEmptySpanIsConsumedAndChangesNothing) {
	feedback_desk desk;
	desk.attach_all();

	EXPECT_EQ(desk.router().on_trades(SYMBOL, {}), 0U);
	EXPECT_EQ(desk.router().on_outcomes(SYMBOL, {}), 0U);
	EXPECT_EQ(desk.router().applied_trades(), 0U);
	EXPECT_EQ(desk.router().unrouted(), 0U);
}

TEST(RiskHooksFeedback, ADispatcherDrainsAMixedStreamWithoutEverStalling) {
	// The wiring as an app would have it: one dispatcher, one handler, several
	// listings interleaved - including one nobody screens, which is what a
	// partition carrying more symbols than a strategy trades actually looks
	// like.
	feedback_desk desk;
	desk.attach_all();
	ASSERT_TRUE(desk.place(SYMBOL, 1, 100, 10));
	ASSERT_TRUE(desk.place(OTHER_SYMBOL, 2, 100, 10));

	scripted_source source({
		engine_event::of(SYMBOL, filled_at(1, 999, 100, 4)),
		engine_event::of(UNSCREENED_SYMBOL, filled_at(50, 51, 100, 1)),
		engine_event::of(OTHER_SYMBOL, withdrawn(2, 10)),
		engine_event::of(SYMBOL, filled_at(1, 999, 100, 6)),
	});
	event_dispatcher route(source, desk.router());

	EXPECT_EQ(route.pump_all(), 4U);
	EXPECT_FALSE(route.is_stalled());
	EXPECT_EQ(route.backlog(), 0U);
	EXPECT_EQ(route.stalls(), 0U);

	// Both fills landed on the first listing and its order is done; the second
	// listing's order was withdrawn instead; the unscreened print went nowhere.
	EXPECT_EQ(desk.net(SYMBOL), 10);
	EXPECT_EQ(desk.first().working_orders(), 0U);
	EXPECT_EQ(desk.working(OTHER_SYMBOL, side_t::bid), 0);
	EXPECT_EQ(desk.second().working_orders(), 0U);
	EXPECT_EQ(desk.router().unrouted(), 1U);
	EXPECT_EQ(desk.router().applied_trades(), 2U);
	EXPECT_EQ(desk.router().applied_outcomes(), 1U);
}

TEST(RiskHooksFeedback, TheMarkTravelsWithTheListingItPrintedOn) {
	// A gate marks its fat-finger collar at whatever last printed, so a
	// misrouted print would band one listing around another's price.
	feedback_desk desk;
	desk.attach_all();
	const std::array fills{filled_at(900, 901, 500, 1)};

	ASSERT_EQ(desk.router().on_trades(OTHER_SYMBOL, fills), 1U);

	EXPECT_EQ(desk.second().reference_price(), 500U);
	EXPECT_EQ(desk.first().reference_price(), 0U);
}

TEST(RiskHooksFeedback, ARouterWithNoGatesAttachedRoutesNothingAndSaysSo) {
	// The failure this makes visible: a deployment that forgot to attach. Every
	// event is consumed, so nothing hangs, and unrouted() is the number that
	// says the loop is not closing.
	feedback_router<test_gate> router{4};
	const std::array fills{filled_at(1, 2, 100, 1)};

	EXPECT_EQ(router.listings(), 0U);
	EXPECT_EQ(router.on_trades(SYMBOL, fills), 1U);
	EXPECT_EQ(router.unrouted(), 1U);
	EXPECT_EQ(router.capacity(), 4U);
}

} // namespace
