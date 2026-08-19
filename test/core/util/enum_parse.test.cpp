#include "core/util/enum_string.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string_view>

// The reverse direction of the X-macro families: text -> enumerator, generated
// from the same list as the accessor, so the two cannot disagree.

namespace {

// A plain list and a valued one, declared here rather than borrowed from a
// project enum: what is under test is the macro, and a local list can be
// pushed into the shapes a real enum will not hold still for.
#define TEST_COLOUR_LIST(X) X(red, "crimson") X(green, "moss") X(blue, "cobalt")

enum class colour : std::uint8_t { EXCHANGE_ENUM_VALUES(TEST_COLOUR_LIST) };
EXCHANGE_ENUM_LABEL(colour, colour_label, TEST_COLOUR_LIST)
EXCHANGE_ENUM_NAME_ONLY(colour, colour_name, TEST_COLOUR_LIST)
EXCHANGE_ENUM_FROM_LABEL(colour, colour_from_label, TEST_COLOUR_LIST)
EXCHANGE_ENUM_FROM_NAME(colour, colour_from_name, TEST_COLOUR_LIST)

#undef TEST_COLOUR_LIST

#define TEST_BREACH_LIST(X)                                                    \
	X(NONE, 0, "no rule was broken")                                           \
	X(HALTED, 1U << 0, "the circuit breaker is open")                          \
	X(PRICE_BAND, 1U << 3, "price is outside the band")

enum class breach : std::uint32_t {
	EXCHANGE_ENUM_VALUED_VALUES(TEST_BREACH_LIST)
};
EXCHANGE_ENUM_VALUED_NAME(breach, breach_name, TEST_BREACH_LIST)
EXCHANGE_ENUM_VALUED_LABEL_ONLY(breach, breach_label, TEST_BREACH_LIST)
EXCHANGE_ENUM_VALUED_FROM_NAME(breach, breach_from_name, TEST_BREACH_LIST)
EXCHANGE_ENUM_VALUED_FROM_LABEL(breach, breach_from_label, TEST_BREACH_LIST)

#undef TEST_BREACH_LIST

TEST(EnumParse, ParseIsTheInverseOfTheAccessorItPairsWith) {
	// The property that makes one list worth having: every enumerator survives
	// the round trip, and it holds for both texts a list carries.
	for (const auto value : {colour::red, colour::green, colour::blue}) {
		EXPECT_EQ(colour_from_label(colour_label(value)), value);
		EXPECT_EQ(colour_from_name(colour_name(value)), value);
	}
	for (const auto value :
		 {breach::NONE, breach::HALTED, breach::PRICE_BAND}) {
		EXPECT_EQ(breach_from_label(breach_label(value)), value);
		EXPECT_EQ(breach_from_name(breach_name(value)), value);
	}
}

TEST(EnumParse, ValuedListParsesToTheValueTheListAssigns) {
	// The gaps in a valued list are the point of the family - parsing must
	// yield the author's number, not the enumerator's position.
	EXPECT_EQ(static_cast<std::uint32_t>(*breach_from_name("PRICE_BAND")), 8U);
	EXPECT_EQ(static_cast<std::uint32_t>(*breach_from_label("no rule was "
															"broken")),
			  0U);
}

TEST(EnumParse, UnknownTextYieldsNulloptRatherThanAnEnumerator) {
	// Unfamiliar input is the expected failure, so it has to be representable:
	// no enumerator may stand in for it, least of all the first one.
	EXPECT_EQ(colour_from_label("vermilion"), std::nullopt);
	EXPECT_EQ(colour_from_name("VERMILION"), std::nullopt);
	EXPECT_EQ(colour_from_label(""), std::nullopt);
	EXPECT_EQ(breach_from_name("halted"), std::nullopt); // case is significant
	// The two texts of one list are not interchangeable: a label parser must
	// not accept the identifier, nor the identifier parser the label.
	EXPECT_EQ(colour_from_label("red"), std::nullopt);
	EXPECT_EQ(colour_from_name("crimson"), std::nullopt);
}

TEST(EnumParse, ParseIsUsableInAConstantExpression) {
	// Same guarantee as the accessors: a literal cadence or flag name resolves
	// at compile time, so a caller can put one in a constexpr.
	static_assert(colour_from_label("moss") == colour::green);
	static_assert(colour_from_name("blue") == colour::blue);
	static_assert(breach_from_name("HALTED") == breach::HALTED);
	static_assert(!colour_from_label("chartreuse").has_value());
	static_assert(noexcept(colour_from_label("moss")));
	SUCCEED();
}

// The documented consequence of the if-chain: duplicate text cannot be a
// compile error the way a duplicate switch case is, so the entry listed first
// is what a caller gets. Pinned here so the resolution is a decision rather
// than whatever the expansion happens to do.
#define TEST_ALIAS_LIST(X) X(canonical, "same") X(shadowed, "same")

enum class aliased : std::uint8_t { EXCHANGE_ENUM_VALUES(TEST_ALIAS_LIST) };
EXCHANGE_ENUM_FROM_LABEL(aliased, aliased_from_label, TEST_ALIAS_LIST)

#undef TEST_ALIAS_LIST

TEST(EnumParse, DuplicateTextResolvesToTheEntryListedFirst) {
	EXPECT_EQ(aliased_from_label("same"), aliased::canonical);
}

} // namespace
