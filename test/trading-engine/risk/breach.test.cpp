// The bit vocabulary: that the bits are distinct, that the severity order is
// what the reported reason follows, and that no rule is missing a string.

#include "trading-engine/risk/breach.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>

namespace {

using exchange::engine::reject_reason;
using exchange::engine::risk::breach;
using exchange::engine::risk::breach_set;
using exchange::engine::risk::BREACH_ALL_BITS;
using exchange::engine::risk::BREACH_BIT_COUNT;
using exchange::engine::risk::describe;
using exchange::engine::risk::first_reason;
using exchange::engine::risk::reason_for;
using exchange::engine::risk::REASON_BY_BIT;
using exchange::engine::risk::to_string;

TEST(RiskBreach, EveryRuleOwnsOneDistinctBit) {
	EXPECT_EQ(std::popcount(BREACH_ALL_BITS),
			  static_cast<int>(BREACH_BIT_COUNT));
	// Contiguous from bit zero, which is what lets countr_zero index the table.
	EXPECT_EQ(BREACH_ALL_BITS, (std::uint32_t{1} << BREACH_BIT_COUNT) - 1);
}

TEST(RiskBreach, NoneIsTheEmptySetAndReportsNothing) {
	EXPECT_EQ(static_cast<std::uint32_t>(breach::NONE), 0U);
	EXPECT_EQ(first_reason(breach_set{}), reject_reason::NONE);
	EXPECT_EQ(reason_for(breach::NONE), reject_reason::NONE);
}

TEST(RiskBreach, ASingleBitReportsItsOwnReason) {
	EXPECT_EQ(first_reason(breach::HALTED), reject_reason::RISK_HALTED);
	EXPECT_EQ(first_reason(breach::PRICE_BAND), reject_reason::RISK_PRICE_BAND);
	EXPECT_EQ(first_reason(breach::MESSAGE_RATE),
			  reject_reason::RISK_MESSAGE_RATE);
}

TEST(RiskBreach, TwoBreachesReportTheMoreSevereOne) {
	// HALTED is bit 0 and outranks everything: "we are not trading" is a truer
	// answer than "that order was large".
	EXPECT_EQ(first_reason(breach::HALTED | breach::ORDER_QUANTITY),
			  reject_reason::RISK_HALTED);
	// And severity is bit order, not argument order.
	EXPECT_EQ(first_reason(breach::MESSAGE_RATE | breach::PRICE_BAND),
			  reject_reason::RISK_PRICE_BAND);
}

TEST(RiskBreach, TwoBreachesReusingEngineReasonsStillMapThrough) {
	// These two are refusals the book would also make; a client must not be able
	// to tell which boundary answered.
	EXPECT_EQ(first_reason(breach::NON_POSITIVE_QUANTITY),
			  reject_reason::NON_POSITIVE_QUANTITY);
	EXPECT_EQ(first_reason(breach::DUPLICATE_ORDER),
			  reject_reason::DUPLICATE_ORDER_ID);
}

TEST(RiskBreach, TheGeneratedTableAgreesWithTheSwitchItCameFrom) {
	for (std::size_t i = 0; i < BREACH_BIT_COUNT; ++i) {
		const auto bit = static_cast<breach>(std::uint32_t{1} << i);
		EXPECT_EQ(REASON_BY_BIT[i], reason_for(bit))
			<< "bit " << i << " (" << to_string(bit) << ')';
	}
}

TEST(RiskBreach, EveryRuleReportsSomethingOtherThanNone) {
	for (std::size_t i = 0; i < BREACH_BIT_COUNT; ++i) {
		const auto bit = static_cast<breach>(std::uint32_t{1} << i);
		EXPECT_NE(REASON_BY_BIT[i], reject_reason::NONE)
			<< to_string(bit) << " has no reason to tell a client";
	}
}

TEST(RiskBreach, EveryRuleHasANameAndADescription) {
	for (std::size_t i = 0; i < BREACH_BIT_COUNT; ++i) {
		const auto bit = static_cast<breach>(std::uint32_t{1} << i);
		EXPECT_FALSE(to_string(bit).empty()) << "bit " << i;
		EXPECT_FALSE(describe(bit).empty()) << to_string(bit);
	}
}

TEST(RiskBreach, AMaskWithNoEnumeratorReportsNoneRatherThanReadingPastTheTable) {
	const auto beyond = breach_set::from_bits(std::uint32_t{1}
											  << BREACH_BIT_COUNT);
	EXPECT_EQ(first_reason(beyond), reject_reason::NONE);
}

TEST(RiskBreach, TheSetCarriesEveryRuleEvenThoughOnlyOneIsReported) {
	const breach_set both = breach::HALTED | breach::EXPOSURE_LIMIT;
	EXPECT_TRUE(both.test(breach::HALTED));
	EXPECT_TRUE(both.test(breach::EXPOSURE_LIMIT));
	EXPECT_FALSE(both.test(breach::PRICE_BAND));
	EXPECT_EQ(both.count(), 2U);
}

} // namespace
