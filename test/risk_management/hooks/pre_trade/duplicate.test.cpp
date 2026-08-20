// Reserving an order id, and the two ways it can fail.
//
// The rule that catches a strategy re-sending an order because it never
// processed the ack - each copy is individually reasonable, and the only
// evidence of the loop is that the id is already working. It is also the one
// pre-trade rule that mutates, so both failures have to leave the ledger
// untouched.

#include "risk_management/hooks/pre_trade/duplicate.hpp"

#include "../../gate/gate.fixture.hpp"
#include "risk_management/hooks/pre_trade/working_ledger.hpp"

#include <gtest/gtest.h>

namespace {

using namespace exchange;
using namespace exchange::risk;
using namespace exchange::risk::hooks;
using namespace exchange::risk::hooks::pre_trade;

TEST(RiskHooksDuplicate, AClaimReservesTheIdAndRefusesTheSecondCopy) {
	working_ledger ledger{4};

	EXPECT_EQ(claim(ledger, buy(1, 100, 10)), 0U);
	EXPECT_EQ(ledger.size(), 1U);
	EXPECT_TRUE(ledger.contains(1));

	EXPECT_TRUE(breach_set::from_bits(claim(ledger, buy(1, 100, 10)))
					.test(breach::DUPLICATE_ORDER));
	EXPECT_EQ(ledger.size(), 1U); // the refusal reserved nothing
}

TEST(RiskHooksDuplicate, DifferentIdsAtTheSamePriceAreNotDuplicates) {
	// The rule is about identity, not about the order looking familiar: a
	// quoter legitimately shows the same size at the same price all day.
	working_ledger ledger{4};
	EXPECT_EQ(claim(ledger, buy(1, 100, 10)), 0U);
	EXPECT_EQ(claim(ledger, buy(2, 100, 10)), 0U);
	EXPECT_EQ(ledger.size(), 2U);
}

TEST(RiskHooksDuplicate, AFullLedgerIsALimitAndNotAnAllocationFailure) {
	// The table is sized once from max_working_orders and never grows, because
	// a full one cannot be answered by allocating a bigger one on this path. So
	// it is reported as a limit an operator set.
	working_ledger ledger{2};
	ASSERT_EQ(claim(ledger, buy(1, 100, 1)), 0U);
	ASSERT_EQ(claim(ledger, buy(2, 100, 1)), 0U);

	EXPECT_TRUE(breach_set::from_bits(claim(ledger, buy(3, 100, 1)))
					.test(breach::WORKING_ORDERS));
	EXPECT_EQ(ledger.size(), 2U);
	EXPECT_FALSE(ledger.contains(3));
}

} // namespace
