// The size rules, against literals.
//
// `gate/screening.test.cpp` drives these through a gate, where the interesting
// interactions are. What is only visible here is that they are pure functions of
// two numbers and a policy - so the whole rule is checkable at compile time,
// which the arithmetic buried in a member function could never demonstrate.

#include "pre_trade.fixture.hpp"
#include "risk_management/hooks/pre_trade/order_size_check.hpp"

#include <gtest/gtest.h>


namespace {

using namespace exchange;
using namespace exchange::risk;
using namespace exchange::risk::hooks;
using namespace exchange::risk::hooks::pre_trade;

TEST(RiskHooksOrderSize, TheRulesReportEveryUnitTheyMeasure) {
	const risk_limits limits = sized(/*max_qty=*/5, /*max_notional=*/100);

	EXPECT_EQ(size_breaches(10, 5, limits), 0U); // 5 lots, 50 notional
	EXPECT_TRUE(breach_set::from_bits(size_breaches(10, 6, limits))
					.test(breach::ORDER_QUANTITY));
	// Inside the lot limit and over the notional - the case a quantity-only
	// check would pass.
	EXPECT_TRUE(breach_set::from_bits(size_breaches(100, 5, limits))
					.test(breach::ORDER_NOTIONAL));

	// Both at once, because the mask is the complete answer rather than the
	// first rule that objected.
	const breach_set both =
		breach_set::from_bits(size_breaches(100, 6, limits));
	EXPECT_TRUE(both.test(breach::ORDER_QUANTITY));
	EXPECT_TRUE(both.test(breach::ORDER_NOTIONAL));
	EXPECT_EQ(both.count(), 2U);
}

TEST(RiskHooksOrderSize, ExactlyTheLimitIsAdmitted) {
	const risk_limits limits = sized(5, 100);
	EXPECT_EQ(size_breaches(20, 5, limits), 0U); // 5 lots, 100 notional
	EXPECT_NE(size_breaches(21, 5, limits), 0U); // 105 notional
}

TEST(RiskHooksOrderSize, ANonPositiveQuantityIsARuleAndNotAnAssertion) {
	// It arrives from outside the process, so it is refused rather than asserted
	// on - the book would refuse it too, and a client should not be able to tell
	// which boundary answered.
	const risk_limits limits = sized(5, 100);
	EXPECT_TRUE(breach_set::from_bits(size_breaches(10, 0, limits))
					.test(breach::NON_POSITIVE_QUANTITY));
	EXPECT_TRUE(breach_set::from_bits(size_breaches(10, -1, limits))
					.test(breach::NON_POSITIVE_QUANTITY));
}

} // namespace
