#include "live_session.fixture.hpp"
#include "session/client_order_id.hpp"
#include "session/venue_gateway.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// A live session with a venue on the other end of its order path.
//
// The wiring this pins is the whole reason `--send-orders` exists: what the
// quoter writes reaches an outbox, and what the venue reports reaches the same
// risk gate that screened it. Nothing here opens a socket - the session is
// driven with scripted depth and hand-built execution reports, which is what
// makes it possible to assert that a *particular* report moved a *particular*
// number.

using exchange::session::gateway_limits;
using exchange::session::replace_answer;
using exchange::session::request_kind;
using exchange::session::venue_gateway;
using exchange::venue::credentials;
using exchange::venue::environment;
using exchange::venue::execution_kind;
using exchange::venue::execution_report;
using exchange::venue::execution_status;

namespace {

/// The listing as the venue spells it. `unit_listing` calls itself something
/// else, and the two namespaces are deliberately separate. @see venue_bridge
constexpr auto VENUE_DESK_SYMBOL = "SOLUSDT";

[[nodiscard]] live_session_options venue_desk_options() {
	live_session_options options;
	// Passive, and it has to be. `--take` would cross the venue's depth
	// mirrored into this engine's own book and fill there as well as at the
	// venue, which is the double count `cmd_serve` refuses outright.
	options.quoting.take_liquidity = false;
	options.venue_symbol           = VENUE_DESK_SYMBOL;
	return options;
}

/**
 * @brief A live desk with the order path switched on.
 *
 * Declaration order is construction order and it matters here: the gateway asks
 * the *session's* circuit breaker whether an order may go, so the session has
 * to exist first. That is the same ordering `cmd_serve` has, and the reason
 * @c attach_gateway exists rather than a constructor argument.
 */
class venue_desk {
public:
	explicit venue_desk(live_session_options options = venue_desk_options())
		: desk_(std::move(options)),
		  gateway_(credentials{.key = "test-key", .secret = "test-secret"},
				   environment::testnet, gateway_limits{},
				   &desk_.session().breaker()) {
		desk_.session().attach_gateway(gateway_);
	}

	/// @brief A report about one of our orders, with nothing traded on it.
	[[nodiscard]] static execution_report
	report_for(order_id_t id, execution_kind kind, execution_status status) {
		execution_report report{};
		report.symbol            = VENUE_DESK_SYMBOL;
		// The engine's own encoding rather than a literal, so a case cannot
		// pass against a spelling the venue would never send back.
		report.client_order_id   = exchange::session::client_order_id(id);
		report.kind              = kind;
		report.status            = status;
		report.order_qty_scaled  = 1;
		report.event_time_ms     = 1;
		return report;
	}

	/// @brief A report saying @p lots of order @p id traded at @p price.
	/// @note @p price is an engine price, put on the venue's grid through the
	///       listing the way the venue would state it.
	[[nodiscard]] static execution_report fill_for(order_id_t id,
												   exchange::price_t price,
												   std::int64_t lots) {
		execution_report report =
			report_for(id, execution_kind::trade,
					   execution_status::partially_filled);
		report.last_qty_scaled = lots;
		report.last_price_scaled =
			exchange::scaled_of(unit_listing().price_to_scaled(price));
		report.cumulative_qty_scaled = lots;
		report.order_qty_scaled      = lots;
		// Ours was the resting order, which is what a quote inside the touch is
		// and what the venue reports for one.
		report.is_maker = true;
		return report;
	}

	[[nodiscard]] live_desk &desk() noexcept { return desk_; }
	[[nodiscard]] test_live_session &session() noexcept {
		return desk_.session();
	}

	/// @brief The first order the quoter has working, or zero if it has none.
	[[nodiscard]] order_id_t working_id() noexcept {
		const auto bid = desk_.session().quoter().live_order(side_t::bid);
		return bid != 0 ? bid
						: desk_.session().quoter().live_order(side_t::ask);
	}

private:
	live_desk desk_;
	venue_gateway gateway_;
};

/// A touch wide enough for the quoter to move its bid twice without running
/// into its own offer.
constexpr price_t VENUE_WIDE_ASK = at_tick(110);

/// The first request in @p queued of @p kind, or null.
[[nodiscard]] const exchange::session::outbound_request *
venue_first_of(const std::vector<exchange::session::outbound_request> &queued,
			   request_kind kind) {
	for (const auto &request : queued)
		if (request.kind == kind) return &request;
	return nullptr;
}

/// A report that @p traded of order @p id's leg @p leg has executed, @p last of
/// it on this message, out of @p ordered on that leg.
[[nodiscard]] execution_report
venue_leg_fill(order_id_t id, exchange::session::venue_leg_t leg,
			   std::int64_t last, std::int64_t traded, std::int64_t ordered) {
	execution_report report{};
	report.symbol          = VENUE_DESK_SYMBOL;
	report.client_order_id = exchange::session::client_order_id(id, leg);
	report.kind            = execution_kind::trade;
	report.status          = execution_status::partially_filled;
	report.last_qty_scaled = last;
	report.last_price_scaled =
		exchange::scaled_of(unit_listing().price_to_scaled(LIVE_TOUCH_BID));
	report.cumulative_qty_scaled = traded;
	report.order_qty_scaled      = ordered;
	report.event_time_ms         = 1;
	report.is_maker              = true;
	return report;
}

} // namespace

TEST(LiveSessionVenue, AQuotersOrderIsQueuedForTheVenue) {
	venue_desk venue;
	ASSERT_TRUE(venue.desk().seed_touch());

	// The property `--send-orders` is for: an order the strategy decided on and
	// the gate passed is now something to send, rather than only a row in a
	// book this process keeps to itself.
	ASSERT_TRUE(venue.session().has_outbound());
	const auto queued = venue.session().take_outbound();
	ASSERT_FALSE(queued.empty());
	EXPECT_TRUE(queued.front().request.target.contains("symbol=SOLUSDT"));
	EXPECT_TRUE(queued.front().request.target.contains("signature="));
	// Signed with the listing's own name at the venue, not the engine's symbol
	// table entry - they are separate namespaces.
	EXPECT_EQ(queued.front().host, "testnet.binance.vision");
}

TEST(LiveSessionVenue, AVenueFillMovesThePositionTheGateScreensAgainst) {
	venue_desk venue;
	ASSERT_TRUE(venue.desk().seed_touch());
	const order_id_t id = venue.working_id();
	ASSERT_NE(id, 0U) << "the quoter placed nothing to be filled";
	ASSERT_EQ(venue.desk().net(), 0 * exchange::units::lot);

	venue.session().on_report(venue_desk::fill_for(
		id, LIVE_TOUCH_BID + 1U * exchange::units::tick, 1));

	// The whole point of the return leg. Nothing in this session can produce a
	// fill on its own - depth mirrored from the venue is rested without
	// matching - so if this number moved, it moved because the venue said so.
	EXPECT_EQ(venue.desk().net(), 1 * exchange::units::lot);
	EXPECT_EQ(venue.session().report().venue_filled_lots,
			  1 * exchange::units::lot);
	EXPECT_EQ(venue.session().report().venue_booked, 1U);
	// And the post-trade monitor counted it, which is what makes the burst and
	// order-to-trade rules mean anything on a run that sends orders.
	EXPECT_EQ(venue.desk().fills(), 1U);
}

TEST(LiveSessionVenue, AnAcknowledgementConfirmsRatherThanBooksASecondTime) {
	venue_desk venue;
	ASSERT_TRUE(venue.desk().seed_touch());
	const order_id_t id = venue.working_id();
	ASSERT_NE(id, 0U);

	venue.session().on_report(venue_desk::report_for(
		id, execution_kind::acknowledgement, execution_status::accepted));

	// The partition already applied this PLACE and published ACCEPTED for it.
	// Routing the venue's agreement as well would double every message the
	// order-to-trade rule counts, turning a statement about *our* quoting churn
	// into one about how chatty the venue is.
	EXPECT_EQ(venue.session().report().venue_reports, 1U);
	EXPECT_EQ(venue.session().report().venue_confirmations, 1U);
	EXPECT_EQ(venue.session().report().venue_booked, 0U);
}

TEST(LiveSessionVenue, ARejectionRetiresTheOrderTheEngineHadAccepted) {
	venue_desk venue;
	ASSERT_TRUE(venue.desk().seed_touch());
	const order_id_t id = venue.working_id();
	ASSERT_NE(id, 0U);
	const std::uint32_t before = venue.session().gate().working_orders();
	ASSERT_GT(before, 0U);

	venue.session().on_report(venue_desk::report_for(
		id, execution_kind::rejection, execution_status::rejected));

	// New information, and the sharpest kind: the engine accepted this order
	// and the venue did not. Left unrouted, the gate would go on screening
	// every later order against exposure that does not exist.
	EXPECT_EQ(venue.session().report().venue_booked, 1U);
	EXPECT_LT(venue.session().gate().working_orders(), before);
}

TEST(LiveSessionVenue, AReportNamingSomebodyElsesOrderIsCountedNotApplied) {
	venue_desk venue;
	ASSERT_TRUE(venue.desk().seed_touch());

	execution_report foreign =
		venue_desk::fill_for(1, LIVE_TOUCH_BID, 1);
	// An id from the venue's own web UI: no `ex-` prefix, so it does not parse.
	foreign.client_order_id = "web_d8949e94bb624f04b1c23c749a4ef5cf";

	const volume_t before = venue.desk().net();
	venue.session().on_report(foreign);

	EXPECT_EQ(venue.session().report().venue_unusable, 1U);
	EXPECT_EQ(venue.session().report().venue_booked, 0U);
	// The account may be shared with a human, and booking their fill into this
	// process's position is the most destructive reading available.
	EXPECT_EQ(venue.desk().net(), before);
}

TEST(LiveSessionVenue, AStreamGapStopsNewOrdersWhileOursAreWorking) {
	venue_desk venue;
	ASSERT_TRUE(venue.desk().seed_touch());
	ASSERT_GT(venue.session().gate().working_orders(), 0U);
	ASSERT_TRUE(venue.session().breaker().passes_new_orders());

	venue.session().on_gap("the account stream was rebuilt");

	// A gap in market data loses public information the next snapshot restores.
	// A gap here loses reports about our own orders and nothing replays them -
	// so the position every later size is screened against is a number this
	// process can no longer justify.
	EXPECT_EQ(venue.session().report().venue_gaps, 1U);
	EXPECT_FALSE(venue.session().breaker().passes_new_orders());
	// Cancel-only rather than halted: the right thing to do with a position you
	// cannot account for is to be able to reduce it.
	EXPECT_TRUE(venue.session().breaker().passes_cancels());
	EXPECT_EQ(venue.session().breaker().cause(),
			  exchange::risk::hooks::system::trip_cause::STALE_WORKING);
}

TEST(LiveSessionVenue, AStreamGapWithNothingWorkingIsOnlyCounted) {
	venue_desk venue;

	venue.session().on_gap("the account stream was rebuilt");

	// A stream that drops while this session is flat has lost reports about no
	// orders. Halting a run over an ordinary reconnect would make the reconnect
	// the outage.
	EXPECT_EQ(venue.session().report().venue_gaps, 1U);
	EXPECT_TRUE(venue.session().breaker().passes_new_orders());
}

TEST(LiveSessionVenue, ARefusedPlacementIsWithdrawnFromTheEnginesLedger) {
	venue_desk venue;
	ASSERT_TRUE(venue.desk().seed_touch());
	const order_id_t id = venue.working_id();
	ASSERT_NE(id, 0U);
	const std::uint32_t before = venue.session().gate().working_orders();
	ASSERT_GT(before, 0U);

	venue.session().on_send_refused(id);

	// The case the account stream can never cover: an order the venue declined
	// at the door never existed, so it has no lifecycle to report and the HTTP
	// response is the only notification there will ever be. Seen live as
	// "working 2" against a venue that had refused both with -1013.
	EXPECT_EQ(venue.session().report().venue_refused, 1U);
	EXPECT_LT(venue.session().gate().working_orders(), before);
	// Nothing traded, so the position must not have moved.
	EXPECT_EQ(venue.desk().net(), 0 * exchange::units::lot);
}

TEST(LiveSessionVenue, WithdrawingAtExitCancelsWhatTheQuoterHasLive) {
	venue_desk venue;
	ASSERT_TRUE(venue.desk().seed_touch());
	ASSERT_NE(venue.working_id(), 0U);
	// The placements, taken out of the way so what follows is only the exit.
	(void)venue.session().take_outbound();

	const std::size_t withdrawn = venue.session().withdraw_all();

	// A GTC quote does not expire with the process that priced it. Left
	// resting, the next thing to happen to it is a fill nobody is watching for.
	ASSERT_GT(withdrawn, 0U);
	EXPECT_EQ(venue.session().report().venue_withdrawn, withdrawn);

	const auto queued = venue.session().take_outbound();
	ASSERT_EQ(queued.size(), withdrawn);
	for (const auto &request : queued) {
		// Cancels, by our own client id - which works before a placement's ack
		// has come back with the venue's.
		EXPECT_TRUE(request.request.target.contains("origClientOrderId="))
			<< request.request.target;
		EXPECT_FALSE(request.is_placement());
	}
}

TEST(LiveSessionVenue, WithdrawingWithNothingLiveWritesNothing) {
	venue_desk venue;

	// No frame has arrived, so the quoter has never quoted. A run that exits
	// flat must not send a cancel naming an order id it never used.
	EXPECT_EQ(venue.session().withdraw_all(), 0U);
	EXPECT_FALSE(venue.session().has_outbound());
	EXPECT_EQ(venue.session().report().venue_withdrawn, 0U);
}

TEST(LiveSessionVenue, ASessionWithNoGatewayWithdrawsNothing) {
	// The default posture: nothing was ever sent, so there is nothing at a
	// venue to withdraw and the exit path must stay silent rather than
	// manufacturing cancels for a venue this run never spoke to.
	live_desk plain{venue_desk_options()};
	ASSERT_TRUE(plain.seed_touch());
	EXPECT_EQ(plain.session().withdraw_all(), 0U);
}

// --- amendments, which reach Binance spot as a cancel-replace ---------------

TEST(LiveSessionVenue, ARequoteReachesTheVenueAsACancelReplace) {
	venue_desk venue;
	ASSERT_TRUE(venue.desk().seed_touch(LIVE_TOUCH_BID, VENUE_WIDE_ASK));
	const order_id_t bid = venue.session().quoter().live_order(side_t::bid);
	ASSERT_NE(bid, 0U);
	(void)venue.session().take_outbound();

	venue.desk().move_touch(2, at_tick(102), VENUE_WIDE_ASK);

	// The quoter amends in place; until this was found on testnet the router
	// sent only placements and cancels, so the venue's quote stayed at the
	// price the engine had moved away from.
	const auto queued   = venue.session().take_outbound();
	const auto *replace = venue_first_of(queued, request_kind::replacement);
	ASSERT_NE(replace, nullptr) << "the reprice never left the process";
	EXPECT_TRUE(replace->request.target.contains(
		"cancelOrigClientOrderId=" + exchange::session::client_order_id(bid) +
		"&"));
	EXPECT_TRUE(replace->request.target.contains(
		"newClientOrderId=" + exchange::session::client_order_id(bid, 1)));
}

TEST(LiveSessionVenue, FillsOnEveryLegCountTowardsTheOrdersTraded) {
	live_session_options options = venue_desk_options();
	options.quoting.lots         = 3 * exchange::units::lot;
	venue_desk venue{options};
	ASSERT_TRUE(venue.desk().seed_touch(LIVE_TOUCH_BID, VENUE_WIDE_ASK));
	const order_id_t bid = venue.session().quoter().live_order(side_t::bid);
	ASSERT_NE(bid, 0U);
	(void)venue.session().take_outbound();

	// One lot on the original leg, then a reprice onto leg 1.
	venue.session().on_report(venue_leg_fill(bid, 0, 1, 1, 3));
	venue.desk().move_touch(2, at_tick(102), VENUE_WIDE_ASK);
	(void)venue.session().take_outbound();
	venue.session().on_replace_answered(bid, replace_answer::replaced);

	// One more on leg 1, whose own count starts again from zero.
	venue.session().on_report(venue_leg_fill(bid, 1, 1, 1, 3));
	venue.desk().move_touch(3, at_tick(104), VENUE_WIDE_ASK);

	const auto queued   = venue.session().take_outbound();
	const auto *replace = venue_first_of(queued, request_kind::replacement);
	ASSERT_NE(replace, nullptr);
	// Two traded across the legs, three to show. Had the engine been told the
	// leg's count - one - the quoter would have asked for 1 + 3 in all, and
	// the router, which knows two traded, would have rested only two.
	EXPECT_TRUE(replace->request.target.contains("quantity=3&"))
		<< replace->request.target;
	EXPECT_TRUE(replace->request.target.contains(
		"newClientOrderId=" + exchange::session::client_order_id(bid, 2)));
	EXPECT_EQ(venue.session().report().venue_filled_lots,
			  2 * exchange::units::lot);
}

TEST(LiveSessionVenue, ARepriceTheVenueDeclinedIsWithdrawnNotForgotten) {
	venue_desk venue;
	ASSERT_TRUE(venue.desk().seed_touch(LIVE_TOUCH_BID, VENUE_WIDE_ASK));
	const order_id_t bid = venue.session().quoter().live_order(side_t::bid);
	venue.desk().move_touch(2, at_tick(102), VENUE_WIDE_ASK);
	(void)venue.session().take_outbound();

	venue.session().on_replace_answered(bid, replace_answer::unchanged);
	EXPECT_EQ(venue.session().report().venue_replace_failures, 1U);
	EXPECT_EQ(venue.session().quoter().live_order(side_t::bid), 0U);

	// The next frame carries the withdrawal out. It names leg zero - the one
	// the venue still has - and that cancel is exactly what the testnet run
	// that exited "CLEAN" with two quotes resting never sent.
	venue.desk().move_touch(3, at_tick(102), VENUE_WIDE_ASK);
	const auto queued  = venue.session().take_outbound();
	const auto *cancel = venue_first_of(queued, request_kind::cancel);
	ASSERT_NE(cancel, nullptr) << "the declined quote was left resting";
	EXPECT_TRUE(cancel->request.target.contains(
		"origClientOrderId=" + exchange::session::client_order_id(bid) + "&"));
}

TEST(LiveSessionVenue, APlacementTheGatewayRefusedIsRetiredFromTheLedger) {
	// One lot of the unit listing at ~100 is far below this floor, which is the
	// shape of the testnet run that sent nothing at --lots 1.
	live_session_options options = venue_desk_options();
	live_desk desk{options};
	venue_gateway gateway{credentials{.key = "k", .secret = "s"},
						  environment::testnet,
						  gateway_limits{.min_notional_scaled = 1'000'000},
						  &desk.session().breaker()};
	desk.session().attach_gateway(gateway);
	ASSERT_TRUE(desk.seed_touch(LIVE_TOUCH_BID, VENUE_WIDE_ASK));

	// Told to the engine, so the gate does not carry exposure that is not
	// there and the quoter does not go on believing it has a quote.
	EXPECT_GT(desk.session().report().venue_feedback, 0U);
	EXPECT_EQ(desk.session().gate().working_orders(), 0U);
	EXPECT_EQ(gateway.stats().refused_by.at(static_cast<std::size_t>(
				  exchange::session::gateway_refusal::below_min_notional)),
			  gateway.stats().refused);
}

// Found on testnet: our ask filled at the venue, the next frame repriced our
// bid through the price the ask had rested at, and the engine - whose own book
// still held the ask, since venue fills are never applied to it - matched our
// two orders against each other. It reported the bid filled, the quoter forgot
// it, and the bid's venue leg rested until the exit sweep cancelled it.
TEST(LiveSessionVenue, AnOrderTheVenueFilledLeavesTheEnginesBookToo) {
	venue_desk venue;
	ASSERT_TRUE(venue.desk().seed_touch(LIVE_TOUCH_BID, VENUE_WIDE_ASK));
	const order_id_t bid = venue.session().quoter().live_order(side_t::bid);
	const order_id_t ask = venue.session().quoter().live_order(side_t::ask);
	ASSERT_NE(bid, 0U);
	ASSERT_NE(ask, 0U);

	execution_report filled = venue_leg_fill(ask, 0, 1, 1, 1);
	filled.status           = execution_status::filled;
	venue.session().on_report(filled);

	// The touch moves up past where our ask rested (109): the bid reprices to
	// 110, through it.
	venue.desk().frame(
		diff(2,
			 0,
			 std::to_array<book_level>({live_level(at_tick(109), 5)}),
			 std::to_array<book_level>(
				 {live_level(at_tick(110), 0), live_level(at_tick(113), 5)})));

	EXPECT_EQ(venue.session().report().engine_withdrawals, 1U);
	EXPECT_EQ(venue.session().quoter().live_order(side_t::bid), bid)
		<< "the engine matched our bid against our own filled ask";
}

// Two testnet runs ended holding -50 lots with a P&L of exactly zero. The gate
// re-marks on every print, and the only prints a sending run sees are its own
// fills - so the position was valued at the price it was opened at, forever.
TEST(LiveSessionVenue, AHeldPositionIsMarkedToTheVenuesMarket) {
	venue_desk venue;
	ASSERT_TRUE(venue.desk().seed_touch(LIVE_TOUCH_BID, VENUE_WIDE_ASK));
	const order_id_t ask = venue.session().quoter().live_order(side_t::ask);
	ASSERT_NE(ask, 0U);

	// Short one lot at 100.
	execution_report filled = venue_leg_fill(ask, 0, 1, 1, 1);
	filled.status           = execution_status::filled;
	venue.session().on_report(filled);
	ASSERT_EQ(venue.desk().net(), -1 * exchange::units::lot);

	// The market moves up against the short: bid 120, ask 130, mid 125.
	venue.desk().frame(
		diff(2,
			 0,
			 std::to_array<book_level>({live_level(at_tick(120), 5)}),
			 std::to_array<book_level>({live_level(VENUE_WIDE_ASK, 0),
										live_level(at_tick(130), 5)})));

	EXPECT_EQ(venue.session().gate().pnl(),
			  -25 * (exchange::units::tick * exchange::units::lot))
		<< "sold at 100, marked at the 125 mid";
}

TEST(LiveSessionVenue, AMarketMovingAgainstAHeldPositionTripsTheLossFloor) {
	live_session_options options = venue_desk_options();
	options.limits.max_loss =
		10 * (exchange::units::tick * exchange::units::lot);
	venue_desk venue{options};
	ASSERT_TRUE(venue.desk().seed_touch(LIVE_TOUCH_BID, VENUE_WIDE_ASK));
	const order_id_t ask = venue.session().quoter().live_order(side_t::ask);
	ASSERT_NE(ask, 0U);

	execution_report filled = venue_leg_fill(ask, 0, 1, 1, 1);
	filled.status           = execution_status::filled;
	venue.session().on_report(filled);
	ASSERT_TRUE(venue.session().breaker().passes_new_orders())
		<< "no loss yet at the fill's own price";

	// No fill of ours prints here. Before, nothing but a print could re-check
	// the floor, so this loss was invisible to it.
	venue.desk().frame(
		diff(2,
			 0,
			 std::to_array<book_level>({live_level(at_tick(120), 5)}),
			 std::to_array<book_level>({live_level(VENUE_WIDE_ASK, 0),
										live_level(at_tick(130), 5)})));

	EXPECT_FALSE(venue.session().breaker().passes_new_orders());
	EXPECT_EQ(venue.session().breaker().cause(),
			  exchange::risk::hooks::system::trip_cause::LOSS_LIMIT);
}

TEST(LiveSessionVenue, AStreamGapAsksTheVenueWhatIsWorking) {
	venue_desk venue;
	ASSERT_TRUE(venue.desk().seed_touch());
	(void)venue.session().take_outbound();

	venue.session().on_gap("the account stream was rebuilt");

	// Reports lost in the gap are lost for good; the read is the only way to
	// learn what they said.
	const auto queued = venue.session().take_outbound();
	EXPECT_NE(venue_first_of(queued, request_kind::open_orders), nullptr);
}

TEST(LiveSessionVenue, AnOrderTheVenueNoLongerHasIsReplacedByTheQuoter) {
	venue_desk venue;
	ASSERT_TRUE(venue.desk().seed_touch(LIVE_TOUCH_BID, VENUE_WIDE_ASK));
	const order_id_t bid = venue.session().quoter().live_order(side_t::bid);
	ASSERT_NE(bid, 0U);
	ASSERT_TRUE(venue.session().request_reconciliation());
	(void)venue.session().take_outbound();

	// The venue has nothing: both quotes are gone without a report.
	venue.session().on_open_orders(std::vector<std::string>{});
	EXPECT_EQ(venue.session().quoter().live_order(side_t::bid), 0U);

	// And the engine's book let go of them, so the next frame quotes afresh.
	venue.desk().move_touch(2, LIVE_TOUCH_BID, VENUE_WIDE_ASK);
	EXPECT_NE(venue.session().quoter().live_order(side_t::bid), 0U);
	EXPECT_NE(venue.session().quoter().live_order(side_t::bid), bid);
}
