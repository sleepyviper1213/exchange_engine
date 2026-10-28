#include "core/util/enum_format.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string_view>

// The reverse direction: text -> enumerator through magic_enum::enum_cast, for
// each of the three shapes an enum here takes - named by its identifiers, named
// by phrases it specialises, and a flag enum whose values the author chose.

namespace {

// Declared here rather than borrowed from a project enum: what is under test is
// the convention, and a local enum can be pushed into the shapes a real one
// will not hold still for.
enum class enum_parse_colour : std::uint8_t { red, green, blue };

enum class enum_parse_phrase : std::uint8_t { red, green, blue, alias };

enum class enum_parse_flag : std::uint32_t {
	NONE       = 0U,
	HALTED     = 1U << 0U,
	PRICE_BAND = 1U << 3U,
};

} // namespace

template <>
constexpr magic_enum::customize::customize_t
magic_enum::customize::enum_name(enum_parse_phrase value) noexcept {
	switch (value) {
		using enum enum_parse_phrase;
	case red: return "crimson";
	case green: return "moss";
	case blue: return "cobalt";
	case alias: return "moss";
	}
	return default_tag;
}

template <>
struct magic_enum::customize::enum_range<enum_parse_flag> {
	static constexpr bool is_flags = true; // NOLINT(*-identifier-naming)
};

namespace {

using magic_enum::enum_cast;
using magic_enum::enum_name;

TEST(EnumParse, ParseIsTheInverseOfTheName) {
	// The property that makes the enum body the only list worth having: every
	// enumerator survives the round trip, whichever text names it.
	for (const auto value : magic_enum::enum_values<enum_parse_colour>())
		EXPECT_EQ(enum_cast<enum_parse_colour>(enum_name(value)), value);
	for (const auto value : {enum_parse_phrase::red, enum_parse_phrase::blue})
		EXPECT_EQ(enum_cast<enum_parse_phrase>(enum_name(value)), value);
	for (const auto value : magic_enum::enum_values<enum_parse_flag>())
		EXPECT_EQ(enum_cast<enum_parse_flag>(enum_name(value)), value);
}

TEST(EnumParse, FlagParsesToTheValueTheAuthorAssigned) {
	// The gaps in a flag enum are the point of it - parsing must yield the
	// author's number, not the enumerator's position.
	EXPECT_EQ(
		magic_enum::enum_integer(*enum_cast<enum_parse_flag>("PRICE_BAND")),
		8U);
}

TEST(EnumParse, FlagZeroHasNoNameAndDoesNotParse) {
	// Flag reflection probes single bits, so the empty enumerator is invisible
	// to it - the documented cost of is_flags, pinned so it stays a decision.
	EXPECT_TRUE(enum_name(enum_parse_flag::NONE).empty());
	EXPECT_EQ(enum_cast<enum_parse_flag>("NONE"), std::nullopt);
	EXPECT_EQ(magic_enum::enum_count<enum_parse_flag>(), 2U);
}

TEST(EnumParse, APhraseReplacesTheIdentifierRatherThanJoiningIt) {
	// A phrase-named enum is spelled one way: the identifier no longer parses,
	// which is what lets a venue spelling such as "100ms" be the whole grammar.
	EXPECT_EQ(enum_cast<enum_parse_phrase>("cobalt"), enum_parse_phrase::blue);
	EXPECT_EQ(enum_cast<enum_parse_phrase>("blue"), std::nullopt);
	EXPECT_EQ(enum_cast<enum_parse_colour>("cobalt"), std::nullopt);
}

TEST(EnumParse, UnknownTextYieldsNulloptRatherThanAnEnumerator) {
	// Unfamiliar input is the expected failure, so it has to be representable:
	// no enumerator may stand in for it, least of all the first one.
	EXPECT_EQ(enum_cast<enum_parse_colour>("vermilion"), std::nullopt);
	EXPECT_EQ(enum_cast<enum_parse_colour>(""), std::nullopt);
	EXPECT_EQ(enum_cast<enum_parse_colour>("RED"), std::nullopt); // case counts
	EXPECT_EQ(enum_cast<enum_parse_flag>("halted"), std::nullopt);
}

TEST(EnumParse, ParseIsUsableInAConstantExpression) {
	// A literal cadence or flag name resolves at compile time, so a caller can
	// put one in a constexpr.
	static_assert(enum_cast<enum_parse_phrase>("moss") ==
				  enum_parse_phrase::green);
	static_assert(enum_cast<enum_parse_colour>("blue") ==
				  enum_parse_colour::blue);
	static_assert(enum_cast<enum_parse_flag>("HALTED") ==
				  enum_parse_flag::HALTED);
	static_assert(!enum_cast<enum_parse_colour>("chartreuse").has_value());
	static_assert(noexcept(enum_cast<enum_parse_colour>("red")));
	SUCCEED();
}

TEST(EnumParse, DuplicatePhraseResolvesToTheLowestValue) {
	// Two enumerators specialised to one phrase is not a compile error the way
	// a duplicate switch case is, so the lowest value wins. Pinned here so the
	// resolution is a decision rather than whatever the lookup happens to do.
	EXPECT_EQ(enum_cast<enum_parse_phrase>("moss"), enum_parse_phrase::green);
	EXPECT_EQ(enum_name(enum_parse_phrase::alias), "moss");
}

} // namespace
