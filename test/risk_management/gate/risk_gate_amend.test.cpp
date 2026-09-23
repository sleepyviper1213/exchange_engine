// What the gate does with a MODIFY, which is the one command that is neither
// new liquidity nor a withdrawal.
//
// The whole rule is netting: an amendment from 10 lots to 12 is two lots of new
// exposure, not twelve, and a gate that screened the whole quantity would
// refuse a strategy that is barely moving. The other half is the direction the
// gate is willing to be wrong in - an increase is counted at submission,
// because it is exposure the venue is about to have, and a reduction is not
// credited until the venue confirms it, because an order is working until
// somebody says otherwise. Both halves have a case here.

#include "event/command.hpp"
#include "gate.fixture.hpp"
#include "order_book/outcome.hpp"
#include "order_book/reject_reason.hpp"
#include "orders/amendment.hpp"
#include "orders/types.hpp"
#include "risk_management/hooks/breach.hpp"

#include <gtest/gtest.h>

#include <array>

namespace {

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::event;
using namespace exchange::engine::orders;
using namespace exchange::risk;
using namespace exchange::risk::hooks;
using namespace exchange::risk::hooks::system;

/// @brief A MODIFY on @c SYMBOL, for the batch cases that build one by hand.
[[nodiscard]] command amend_command(order_id_t id, price_t price,
									quantity_t qty) {
	return command::modify(
		SYMBOL,
		amendment{.id = id, .price = price, .quantity = qty});
}

// --------------------------------------------------------------------------
// Netting
// --------------------------------------------------------------------------

TEST(RiskGateAmend, AnIncreaseCountsOnlyTheDifferenceAsNewExposure) {
	harness h;
	ASSERT_TRUE(h.place(buy(1, 100, 10)));
	ASSERT_EQ(h.working(side_t::bid), 10);

	ASSERT_TRUE(h.amend(1, 100, 12));

	EXPECT_EQ(h.working(side_t::bid), 12)
		<< "two lots more, not a second order of twelve";
	EXPECT_EQ(h.gate().working_orders(), 1U) << "still one order";
	EXPECT_EQ(h.delivered().size(), 2U);
}

// An order is working until the venue says otherwise. Crediting the reduction
// here would leave the gate blind to lots still resting in a book, which is the
// same argument mass_cancel makes for not retiring what it cancels.
TEST(RiskGateAmend, AReductionIsNotCreditedUntilTheVenueConfirms) {
	harness h;
	ASSERT_TRUE(h.place(buy(1, 100, 10)));

	ASSERT_TRUE(h.amend(1, 100, 4));

	EXPECT_EQ(h.delivered().size(), 2U) << "it was still delivered";
	EXPECT_EQ(h.working(side_t::bid), 10)
		<< "the gate keeps counting what is resting";
}

// The ledger and the position book move together or the working count goes
// negative, which is the one arithmetic error a risk system must not have
// quietly. An amended order that is then withdrawn has to net to nothing.
TEST(RiskGateAmend, AnAmendedOrderNetsToNothingWhenItIsWithdrawn) {
	harness h;
	ASSERT_TRUE(h.place(buy(1, 100, 10)));
	ASSERT_TRUE(h.amend(1, 100, 12));
	ASSERT_EQ(h.working(side_t::bid), 12);

	h.outcome(order_outcome::cancelled(1, order_state{12}));

	EXPECT_EQ(h.working(side_t::bid), 0);
	EXPECT_EQ(h.gate().working_orders(), 0U);
}

TEST(RiskGateAmend, IntraBatchAmendmentsAccumulateAgainstEachOther) {
	risk_limits limits = permissive();
	limits.max_exposure_notional =
		2500; // tick-lots, so 25 lots at a mark of 100
	harness h{limits, /*reference=*/100};
	ASSERT_TRUE(h.place(buy(1, 100, 10)));

	const std::array<command, 2> batch{amend_command(1, 100, 20),
									   amend_command(1, 100, 30)};
	ASSERT_TRUE(h.submit(batch));

	// Each amendment adds ten, and the second is screened against a batch that
	// has already spent the first: twenty working plus ten pending plus its own
	// ten is thirty lots, or 3000 against a ceiling of 2500. A gate that
	// screened each against the exposure the batch started from would have made
	// that twenty and let both through.
	EXPECT_TRUE(h.saw(breach::EXPOSURE_LIMIT));
	EXPECT_EQ(h.delivered().size(), 2U) << "the place and the first amendment";
	EXPECT_EQ(h.working(side_t::bid), 20);
}

// --------------------------------------------------------------------------
// Which rule reads which quantity
// --------------------------------------------------------------------------

// A per-order size cap is a statement about how big an order may be, which is
// exactly what the amendment is asking to change - so it reads the whole
// quantity, not the increment.
TEST(RiskGateAmend, TheSizeCapReadsTheWholeAmendedQuantity) {
	risk_limits limits   = permissive();
	limits.max_order_qty = 10;
	harness h{limits};
	ASSERT_TRUE(h.place(buy(1, 100, 10)));

	ASSERT_TRUE(h.amend(1, 100, 12));

	EXPECT_EQ(h.delivered().size(), 1U) << "the amendment did not get through";
	EXPECT_EQ(h.sole_rejection().type, OutcomeType::MODIFY_REJECTED);
	EXPECT_EQ(h.sole_rejection().id, 1U);
	EXPECT_EQ(h.sole_rejection().reason, reject_reason::RISK_ORDER_QUANTITY);
	EXPECT_EQ(h.working(side_t::bid), 10) << "nothing was reserved";
}

// A position limit is a statement about how much more the account may take on,
// so it reads the increment. The amendment below asks for nineteen more lots
// against a limit of five, and is refused for the increment rather than for the
// twenty it names.
TEST(RiskGateAmend, ThePositionLimitReadsTheIncrement) {
	risk_limits limits       = permissive();
	limits.max_position_lots = 5;
	harness h{limits};
	ASSERT_TRUE(h.place(buy(1, 100, 1)));

	ASSERT_TRUE(h.amend(1, 100, 20));
	EXPECT_EQ(h.sole_rejection().reason, reject_reason::RISK_POSITION_LIMIT);

	// And an increment inside the limit is admitted, which is what says the
	// rule is reading the difference and not the total.
	ASSERT_TRUE(h.amend(1, 100, 4));
	EXPECT_EQ(h.delivered().size(), 2U);
	EXPECT_EQ(h.working(side_t::bid), 4);
}

TEST(RiskGateAmend, TheFatFingerBandReadsTheAmendedPrice) {
	risk_limits limits    = permissive();
	limits.price_band_bps = 100; // one percent either side of the mark
	harness h{limits, /*reference=*/100};
	ASSERT_TRUE(h.place(buy(1, 100, 10)));

	ASSERT_TRUE(h.amend(1, 400, 12));

	EXPECT_EQ(h.sole_rejection().reason, reject_reason::RISK_PRICE_BAND);
	EXPECT_EQ(h.delivered().size(), 1U);
}

// --------------------------------------------------------------------------
// The trading state
// --------------------------------------------------------------------------

// CANCEL_ONLY stops an account taking anything new on. An amendment that adds
// lots is taking something new on; one that gives lots up is the way out, and
// refusing it would leave live quotes in a book nobody is managing.
TEST(RiskGateAmend, ATrippedBreakerStopsAnIncreaseAndKeepsAReduction) {
	harness h;
	ASSERT_TRUE(h.place(buy(1, 100, 10)));
	ASSERT_TRUE(h.place(buy(2, 100, 10)));
	h.breaker().trip(trading_state::CANCEL_ONLY);

	ASSERT_TRUE(h.amend(1, 100, 12));
	EXPECT_EQ(h.sole_rejection().reason, reject_reason::RISK_HALTED);
	EXPECT_EQ(h.delivered().size(), 2U);

	ASSERT_TRUE(h.amend(2, 100, 4));
	EXPECT_EQ(h.delivered().size(), 3U) << "shrinking is still allowed";
}

TEST(RiskGateAmend, AHaltedBreakerStopsAmendmentsEitherWay) {
	harness h;
	ASSERT_TRUE(h.place(buy(1, 100, 10)));
	h.breaker().trip(trading_state::HALTED);

	ASSERT_TRUE(h.amend(1, 100, 4));

	EXPECT_EQ(h.delivered().size(), 1U);
	EXPECT_EQ(h.sole_rejection().type, OutcomeType::MODIFY_REJECTED);
	EXPECT_EQ(h.sole_rejection().reason, reject_reason::RISK_HALTED);
}

// --------------------------------------------------------------------------
// Orders the gate is not tracking, and back-pressure
// --------------------------------------------------------------------------

// The gate has no entry to amend, and no breach to report either: an order it
// does not know about is not a risk limit being broken. The book answers it
// with UNKNOWN_ORDER, which is the boundary that actually knows.
TEST(RiskGateAmend, AnAmendmentForAnUntrackedOrderIsLeftToTheBook) {
	harness h;

	ASSERT_TRUE(h.amend(77, 100, 10));

	EXPECT_EQ(h.delivered().size(), 1U);
	EXPECT_TRUE(h.gate().rejections().empty());
	EXPECT_EQ(h.working(side_t::bid), 0);
}

// The property the whole screen-deliver-commit ordering exists for, for the one
// command that amends an entry instead of inserting one. Retiring it would
// throw away an order the venue is still working, so what the amendment added
// is taken back off instead.
TEST(RiskGateAmend, ARefusedDeliveryLeavesTheLedgerAsItFoundIt) {
	harness h;
	ASSERT_TRUE(h.place(buy(1, 100, 10)));

	h.sink().refuse(true);
	ASSERT_FALSE(h.amend(1, 100, 12));

	EXPECT_EQ(h.gate().working_orders(), 1U) << "the order is still tracked";
	ASSERT_TRUE(h.gate().ledger().find(1).has_value());
	EXPECT_EQ(h.gate().ledger().find(1)->lots, 10) << "back to what it was";
	EXPECT_EQ(h.working(side_t::bid), 10);

	// The identical batch, retried, must behave as though the refusal never
	// happened - twelve lots working and not fourteen.
	h.sink().refuse(false);
	ASSERT_TRUE(h.amend(1, 100, 12));
	EXPECT_EQ(h.working(side_t::bid), 12);
}

// An amendment is a message the venue has to receive, parse and answer, so it
// crowds out new orders exactly as a cancel does.
TEST(RiskGateAmend, AnAmendmentIsChargedAgainstTheRateWindow) {
	risk_limits limits             = permissive();
	limits.max_messages_per_window = 2;
	limits.rate_window_log2_ns     = TEST_WINDOW_LOG2;
	harness h{limits};

	ASSERT_TRUE(h.place(buy(1, 100, 10)));
	ASSERT_TRUE(h.amend(1, 100, 12));

	ASSERT_TRUE(h.place(buy(2, 100, 1)));
	EXPECT_EQ(h.sole_rejection().reason, reject_reason::RISK_MESSAGE_RATE)
		<< "the amendment spent the window's second message";
}

} // namespace
