#include "session/ledger_view.hpp"

#include "risk_management/hooks/pre_trade/working_ledger.hpp"

#include <gtest/gtest.h>

// The live path's resting-order source. Four questions, and the reason each one
// matters is that the fill model asks it once per working order per frame and
// injects an aggressing order on the strength of the answer: a view that reports
// an order the gate has finished with makes the model fill against nothing, and
// one that hides an order that is still working makes a market-making run
// measure to zero.

using namespace exchange;
using namespace exchange::session;

using exchange::risk::hooks::pre_trade::working_ledger;

namespace {
constexpr std::uint32_t LEDGER_CAPACITY = 64;
} // namespace

TEST(AppLedgerView, AWorkingOrderIsReportedWithItsSidePriceAndLots) {
	working_ledger ledger{LEDGER_CAPACITY};
	ASSERT_TRUE(ledger.insert(7, side_t::bid, 100, 5));

	const ledger_view view{ledger};
	const auto resting = view.resting(7);

	ASSERT_TRUE(resting.has_value());
	EXPECT_EQ(resting->side, side_t::bid);
	EXPECT_EQ(resting->price, 100);
	EXPECT_EQ(resting->lots, 5) << "the three fields the fill model reads, and "
								   "the only three it is entitled to";
}

TEST(AppLedgerView, AnUnknownIdIsNotResting) {
	const working_ledger ledger{LEDGER_CAPACITY};
	const ledger_view view{ledger};

	EXPECT_FALSE(view.resting(7).has_value())
		<< "an order the gate never accepted is not one the venue can trade "
		   "against";
}

TEST(AppLedgerView, APartialTakeReducesTheLotsReported) {
	working_ledger ledger{LEDGER_CAPACITY};
	ASSERT_TRUE(ledger.insert(7, side_t::ask, 100, 5));
	ASSERT_TRUE(ledger.take(7, 2).has_value());

	const ledger_view view{ledger};
	const auto resting = view.resting(7);

	ASSERT_TRUE(resting.has_value());
	EXPECT_EQ(resting->lots, 3)
		<< "what is left is what can still fill; reporting the original size "
		   "would have the model inject against quantity that has already "
		   "traded";
}

TEST(AppLedgerView, AnOrderTheLedgerHasRetiredIsNotResting) {
	working_ledger ledger{LEDGER_CAPACITY};
	ASSERT_TRUE(ledger.insert(7, side_t::bid, 100, 5));
	ASSERT_TRUE(ledger.retire(7).has_value());

	const ledger_view view{ledger};

	EXPECT_FALSE(view.resting(7).has_value())
		<< "the gate retires on a terminal outcome, and that is exactly when "
		   "the model must stop inferring fills for the order";
}

TEST(AppLedgerView, AFullyTakenOrderIsNotResting) {
	working_ledger ledger{LEDGER_CAPACITY};
	ASSERT_TRUE(ledger.insert(7, side_t::bid, 100, 5));
	ASSERT_TRUE(ledger.take(7, 5).has_value());

	const ledger_view view{ledger};

	// Whether a fully-drawn entry is removed or left showing zero is the
	// ledger's business; either way this must not report it as fillable, which
	// is why the view tests the lots rather than trusting the lookup.
	EXPECT_FALSE(view.resting(7).has_value());
}
