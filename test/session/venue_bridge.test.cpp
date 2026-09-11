#include "session/venue_bridge.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

// Carrying an order across the seam, and carrying the venue's answer back.
//
// The mappings that matter are the ones where a naive reading loses money: a
// cancel of a partly-filled order must not read as terminal-and-unfilled, and a
// PENDING_CANCEL must not read as cancelled while the order can still trade.

using exchange::order_id_t;
using exchange::side_t;
using exchange::engine::OrderStatus;
using exchange::engine::OutcomeType;
using exchange::engine::reject_reason;
using exchange::engine::symbol_spec;
using exchange::session::bridge_error;
using exchange::session::reconcile;
using exchange::session::reconciliation;
using exchange::session::to_engine_status;
using exchange::session::to_outbound_cancel;
using exchange::session::to_outcome;
using exchange::session::to_outcome_type;
using exchange::venue::execution_kind;
using exchange::venue::execution_report;
using exchange::venue::execution_status;

namespace {

/// SOLUSDT's real grid: tick 0.01 at scale 2, lot 0.001 at scale 3.
[[nodiscard]] symbol_spec bridge_listing() {
	return symbol_spec{/*id=*/7,
					   /*symbol=*/"SOLUSDT",
					   /*price_scale=*/2,
					   /*qty_scale=*/3,
					   /*tick_scaled=*/1,
					   /*lot_scaled=*/1,
					   /*reference_scaled=*/15345,
					   /*collar_bps=*/symbol_spec::NO_COLLAR};
}

/// A report about order 42 with the fields every case here shares.
[[nodiscard]] execution_report bridge_report() {
	execution_report report;
	report.symbol              = "SOLUSDT";
	report.client_order_id     = "42";
	report.order_qty_scaled    = 1500; // 1.500
	report.order_price_scaled  = 15345;
	report.event_time_ms       = 1;
	report.transaction_time_ms = 1;
	return report;
}

} // namespace

TEST(VenueBridge, AnAcknowledgementBecomesAnAcceptedOutcome) {
	execution_report report = bridge_report();
	report.status           = execution_status::accepted;
	report.kind             = execution_kind::acknowledgement;

	const auto outcome = to_outcome(report, bridge_listing());
	ASSERT_TRUE(outcome.has_value()) << message(outcome.error());

	EXPECT_EQ(outcome->id, order_id_t{42});
	EXPECT_EQ(outcome->type, OutcomeType::ACCEPTED);
	EXPECT_EQ(outcome->status, OrderStatus::LIVE);
	EXPECT_EQ(outcome->traded, 0);
	EXPECT_EQ(outcome->remaining, 1500);
	EXPECT_EQ(outcome->reason, reject_reason::NONE);
}

TEST(VenueBridge, AFillCarriesTheCumulativeQuantityAndTheRemainder) {
	execution_report report      = bridge_report();
	report.status                = execution_status::partially_filled;
	report.kind                  = execution_kind::trade;
	report.last_qty_scaled       = 400;
	report.cumulative_qty_scaled = 400;
	report.last_price_scaled     = 15344;

	const auto outcome = to_outcome(report, bridge_listing());
	ASSERT_TRUE(outcome.has_value()) << message(outcome.error());

	EXPECT_EQ(outcome->type, OutcomeType::FILL);
	EXPECT_EQ(outcome->status, OrderStatus::PARTIALLY_FILLED);
	// Cumulative, not the per-message quantity: a report delivered twice must
	// not book the fill twice.
	EXPECT_EQ(outcome->traded, 400);
	EXPECT_EQ(outcome->remaining, 1100);
}

TEST(VenueBridge, ACancelAfterAPartialFillKeepsWhatWasAlreadyTraded) {
	// The venue reports the cancel with the *cumulative* fill still on it and
	// the per-message quantity zero. An outcome that reported traded=0 here
	// would tell the engine the order went away having done nothing, and the
	// position would be wrong by the amount that actually traded.
	execution_report report         = bridge_report();
	report.status                   = execution_status::cancelled;
	report.kind                     = execution_kind::cancellation;
	report.last_qty_scaled          = 0;
	report.cumulative_qty_scaled    = 400;
	report.client_order_id          = "ex-42-c";
	report.original_client_order_id = "42";

	const auto outcome = to_outcome(report, bridge_listing());
	ASSERT_TRUE(outcome.has_value()) << message(outcome.error());

	// Named for the order that went away, not for the cancel request.
	EXPECT_EQ(outcome->id, order_id_t{42});
	EXPECT_EQ(outcome->type, OutcomeType::CANCELLED);
	EXPECT_EQ(outcome->status, OrderStatus::CANCELLED);
	EXPECT_EQ(outcome->traded, 400);
	EXPECT_EQ(outcome->remaining, 1100);
}

TEST(VenueBridge, ARejectionNamesTheVenueAsTheReason) {
	execution_report report = bridge_report();
	report.status           = execution_status::rejected;
	report.kind             = execution_kind::rejection;
	report.reject_reason    = "INSUFFICIENT_BALANCE";

	const auto outcome = to_outcome(report, bridge_listing());
	ASSERT_TRUE(outcome.has_value()) << message(outcome.error());

	EXPECT_EQ(outcome->type, OutcomeType::REJECTED);
	EXPECT_EQ(outcome->status, OrderStatus::REJECTED);
	// Not one of the engine's own reasons: nothing here decided to refuse it,
	// and reporting e.g. RISK_HALTED would blame a gate that passed it.
	EXPECT_EQ(outcome->reason, reject_reason::VENUE_REJECTED);
}

TEST(VenueBridge, APendingCancelIsStillWorking) {
	// The cancel-after-fill race from the other side. An order with a cancel in
	// flight can still trade, and treating it as terminal would have the engine
	// retire an order that then fills.
	EXPECT_EQ(to_engine_status(execution_status::pending_cancel, false),
			  OrderStatus::LIVE);
	EXPECT_EQ(to_engine_status(execution_status::pending_cancel, true),
			  OrderStatus::PARTIALLY_FILLED);
}

TEST(VenueBridge, AnUnknownVenueStatusIsAssumedStillWorking) {
	// The safe direction: believing an order works when it does not costs a
	// redundant cancel, where believing it gone when it works leaves an
	// unmanaged position on the venue.
	EXPECT_EQ(to_engine_status(execution_status::unknown, false),
			  OrderStatus::LIVE);
	EXPECT_EQ(to_outcome_type(execution_kind::other), OutcomeType::ACCEPTED);
}

TEST(VenueBridge, AnExpiryIsReportedAsACancellation) {
	// The engine has no EXPIRED status. Cancelled is the truthful mapping -
	// the order left the book with quantity unexecuted - and the venue's own
	// wording survives on the report for anyone reading the log.
	EXPECT_EQ(to_engine_status(execution_status::expired, false),
			  OrderStatus::CANCELLED);
	EXPECT_EQ(to_outcome_type(execution_kind::expiry), OutcomeType::CANCELLED);
}

TEST(VenueBridge, AReportForAnOrderWeDidNotPlaceIsRefused) {
	execution_report report = bridge_report();
	report.client_order_id  = "web_placed_by_hand";
	report.status           = execution_status::filled;
	report.kind             = execution_kind::trade;

	// Not an engine order, so there is no id to attach the outcome to. Silently
	// dropping it would be worse: this is how a shared account shows up.
	EXPECT_EQ(to_outcome(report, bridge_listing()).error(),
			  bridge_error::unknown_order);
}

TEST(VenueBridge, ACancelNamesTheOrderAndNotTheRequest) {
	const auto cancel = to_outbound_cancel(42, "SOLUSDT");

	EXPECT_EQ(cancel.symbol, "SOLUSDT");
	EXPECT_EQ(cancel.client_order_id, "42");
}

// --- reconciliation --------------------------------------------------------

TEST(VenueBridge, ReconcilingAgreementReportsBothSidesAgreeing) {
	const std::vector<order_id_t> ours{42, 43};
	const std::vector<std::string> open{"42", "43"};

	const auto found = reconcile(ours, open);
	ASSERT_EQ(found.size(), 2U);
	for (const auto &entry : found)
		EXPECT_EQ(entry.finding, reconciliation::agreed)
			<< entry.client_order_id;
}

TEST(VenueBridge, AnOrderTheVenueHasAndWeDoNotIsAdopted) {
	// The unanswered placement: `may_resend` refused to send it again precisely
	// so this could establish that it did in fact arrive.
	const std::vector<order_id_t> ours{};
	const std::vector<std::string> open{"99"};

	const auto found = reconcile(ours, open);
	ASSERT_EQ(found.size(), 1U);
	EXPECT_EQ(found[0].finding, reconciliation::adopt);
	EXPECT_EQ(found[0].id, order_id_t{99});
}

TEST(VenueBridge, AnOrderWeHaveAndTheVenueDoesNotIsPresumedGone) {
	const std::vector<order_id_t> ours{42};
	const std::vector<std::string> open{};

	const auto found = reconcile(ours, open);
	ASSERT_EQ(found.size(), 1U);
	// A finding, not a conclusion: it may have filled, been cancelled or
	// expired, and an open-orders list cannot say which.
	EXPECT_EQ(found[0].finding, reconciliation::presumed_gone);
	EXPECT_EQ(found[0].id, order_id_t{42});
}

TEST(VenueBridge, AnOrderPlacedOutsideThisProcessIsReportedAndNotAdopted) {
	const std::vector<order_id_t> ours{};
	const std::vector<std::string> open{"web_abc123"};

	const auto found = reconcile(ours, open);
	ASSERT_EQ(found.size(), 1U);
	// The account may be shared with a human. Cancelling an order it did not
	// recognise is the most destructive thing a reconciliation could do.
	EXPECT_EQ(found[0].finding, reconciliation::foreign);
	EXPECT_EQ(found[0].client_order_id, "web_abc123");
	EXPECT_EQ(found[0].id, order_id_t{0});
}

TEST(VenueBridge, EveryOrderOnEitherSideIsAccountedFor) {
	const std::vector<order_id_t> ours{1, 2};
	const std::vector<std::string> open{"2", "3", "web_abc123"};

	const auto found = reconcile(ours, open);
	// 1 gone, 2 agreed, 3 adopted, one foreign - nothing silently dropped, so a
	// caller can act on the whole picture rather than on the overlap.
	ASSERT_EQ(found.size(), 4U);

	int agreed = 0, adopt = 0, gone = 0, foreign = 0;
	for (const auto &entry : found) switch (entry.finding) {
		case reconciliation::agreed: ++agreed; break;
		case reconciliation::adopt: ++adopt; break;
		case reconciliation::presumed_gone: ++gone; break;
		case reconciliation::foreign: ++foreign; break;
		}
	EXPECT_EQ(agreed, 1);
	EXPECT_EQ(adopt, 1);
	EXPECT_EQ(gone, 1);
	EXPECT_EQ(foreign, 1);
}
