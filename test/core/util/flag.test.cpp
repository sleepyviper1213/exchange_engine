#include "core/util/flag.hpp"

#include <gtest/gtest.h>

#include <concepts>
#include <cstdint>
#include <type_traits>

// flag<E> is a compile-time construct, so most of what it promises is asserted
// at compile time - a runtime EXPECT that a constexpr expression equals a
// constant proves less than a static_assert that it compiles at all. The runtime
// cases below are the ones where the value, not the type, is the question.

namespace {

using exchange::core::util::flag;
using exchange::core::util::flag_enum;

/// A flag enum: the bits combine, and a set of them is meaningful.
enum class capability : std::uint8_t {
	POST_ONLY   = 1U << 0U,
	REDUCE_ONLY = 1U << 1U,
	HIDDEN      = 1U << 2U,
	SPONSORED   = 1U << 3U,
};

EXCHANGE_ENABLE_FLAGS(capability)

/// Alternatives, not bits - the case the opt-in exists to keep out.
enum class venue_state : std::uint8_t { CLOSED, AUCTION, CONTINUOUS };

/// A wider underlying type, to pin that nothing here is hard-wired to 8 bits.
enum class channel : std::uint32_t {
	TRADES = 1U << 0U,
	DEPTH  = 1U << 16U,
	NEWS   = 1U << 31U,
};

EXCHANGE_ENABLE_FLAGS(channel)

using caps = flag<capability>;

// --- the opt-in ------------------------------------------------------------

static_assert(flag_enum<capability>);
static_assert(flag_enum<channel>);
// An enum nobody opted in is not a flag enum, so `venue_state::CLOSED |
// venue_state::AUCTION` finds no operator and does not compile.
static_assert(!flag_enum<venue_state>);
// Nor is a plain integer, however much it behaves like a bit field.
static_assert(!flag_enum<std::uint8_t>);
static_assert(!flag_enum<int>);

// --- no silent integral conversion -----------------------------------------

// The whole point: a flag set is not a number and does not become one.
static_assert(!std::is_convertible_v<caps, std::uint8_t>);
static_assert(!std::is_convertible_v<caps, int>);
static_assert(!std::is_convertible_v<caps, bool>);
// ...nor does a number become a flag set. from_bits is the only door in.
static_assert(!std::is_convertible_v<std::uint8_t, caps>);
static_assert(!std::is_constructible_v<caps, std::uint8_t>);
// A single flag does convert in, because a one-flag set is unambiguously it.
static_assert(std::is_convertible_v<capability, caps>);
// But the two are still different types, so they never compare.
static_assert(!std::equality_comparable_with<caps, capability>);
// And flags of different enums never mix.
static_assert(!std::is_convertible_v<flag<channel>, caps>);

// The storage costs exactly what the enum costs.
static_assert(sizeof(caps) == sizeof(std::uint8_t));
static_assert(sizeof(flag<channel>) == sizeof(std::uint32_t));
static_assert(std::is_trivially_copyable_v<caps>);
static_assert(std::same_as<caps::underlying_type, std::uint8_t>);

// --- building a set ---------------------------------------------------------

static_assert(caps{}.is_empty());
static_assert(caps{}.count() == 0);
static_assert(!static_cast<bool>(caps{}));

inline constexpr caps TWO = capability::POST_ONLY | capability::HIDDEN;

static_assert(TWO.count() == 2);
static_assert(static_cast<bool>(TWO));
static_assert(TWO.bits() == 0b0000'0101U);
static_assert(TWO == caps::from_bits(0b0000'0101U));

// A single enumerator flows into a flag position without a cast, from either
// side, and adding one already present changes nothing.
static_assert((TWO | capability::SPONSORED).count() == 3);
static_assert((capability::POST_ONLY | TWO) == TWO);

} // namespace

TEST(Flag, TestAsksForEveryBitAndAnyOfForAtLeastOne) {
	constexpr caps HELD = capability::POST_ONLY | capability::HIDDEN;

	EXPECT_TRUE(HELD.test(capability::POST_ONLY));
	EXPECT_TRUE(HELD.test(capability::HIDDEN));
	EXPECT_FALSE(HELD.test(capability::REDUCE_ONLY));

	// A multi-bit argument is the interesting case: test() wants all of them,
	// any_of() wants one. Conflating the two is the classic flags bug.
	constexpr caps BOTH_HELD = capability::POST_ONLY | capability::HIDDEN;
	constexpr caps ONE_HELD  = capability::POST_ONLY | capability::REDUCE_ONLY;
	EXPECT_TRUE(HELD.test(BOTH_HELD));
	EXPECT_FALSE(HELD.test(ONE_HELD));
	EXPECT_TRUE(HELD.any_of(ONE_HELD));
	EXPECT_TRUE(HELD.all_of(BOTH_HELD));
	EXPECT_TRUE(HELD.none_of(capability::REDUCE_ONLY | capability::SPONSORED));
}

// Equality is "the same set", not "overlaps" - the distinction a bare integer
// comparison silently gets wrong.
TEST(Flag, EqualityIsSetEqualityNotOverlap) {
	constexpr caps HELD = capability::POST_ONLY | capability::HIDDEN;

	EXPECT_NE(HELD, caps{capability::POST_ONLY});
	EXPECT_TRUE(HELD.test(capability::POST_ONLY));
	EXPECT_EQ(HELD, capability::HIDDEN | capability::POST_ONLY) << "unordered";
}

TEST(Flag, SetResetAndFlipMoveIndividualBits) {
	caps held;
	EXPECT_TRUE(held.is_empty());

	held.set(capability::POST_ONLY);
	EXPECT_EQ(held, caps{capability::POST_ONLY});

	held.set(capability::POST_ONLY); // idempotent
	EXPECT_EQ(held.count(), 1U);

	held.set(capability::HIDDEN | capability::SPONSORED);
	EXPECT_EQ(held.count(), 3U);

	held.reset(capability::HIDDEN);
	EXPECT_FALSE(held.test(capability::HIDDEN));
	EXPECT_TRUE(held.test(capability::POST_ONLY));
	EXPECT_EQ(held.count(), 2U);

	held.reset(capability::REDUCE_ONLY); // not held; a no-op, not an error
	EXPECT_EQ(held.count(), 2U);

	held.flip(capability::POST_ONLY);
	EXPECT_FALSE(held.test(capability::POST_ONLY));
	held.flip(capability::POST_ONLY);
	EXPECT_TRUE(held.test(capability::POST_ONLY));
}

// The complement is over the storage, so it brings back bits the enum never
// named. That is documented, and it is why masking is the way to use it.
TEST(Flag, ComplementIsOverTheStorageAndWantsMasking) {
	constexpr caps KNOWN = capability::POST_ONLY | capability::REDUCE_ONLY |
						   capability::HIDDEN | capability::SPONSORED;
	constexpr caps HELD = capability::POST_ONLY;

	EXPECT_EQ((~HELD).count(), 7U) << "all eight storage bits but the one held";
	EXPECT_EQ((~HELD).masked_by(KNOWN).count(), 3U);
	EXPECT_FALSE((~HELD).test(capability::POST_ONLY));

	// reset() is the same operation with the mask already applied, which is why
	// it is the one to reach for.
	caps cleared = KNOWN;
	cleared.reset(capability::POST_ONLY);
	EXPECT_EQ(cleared, (~HELD).masked_by(KNOWN));
}

// The narrowing back from the promoted int has to be exact, or a high bit of a
// small underlying type disappears on the first operation.
TEST(Flag, HighBitsSurviveTheIntegralPromotionRoundTrip) {
	constexpr caps TOP = capability::SPONSORED; // bit 3 of a uint8_t
	EXPECT_EQ(TOP.bits(), 0b0000'1000U);
	EXPECT_EQ((~~TOP), TOP);

	// A 32-bit enum with the sign bit set is where an int-pinned narrow would
	// have shown up as a truncation. 
	constexpr flag<channel> WIDE = channel::NEWS | channel::DEPTH;
	EXPECT_EQ(WIDE.bits(), 0x8001'0000U);
	EXPECT_EQ(WIDE.count(), 2U);
	EXPECT_TRUE(WIDE.test(channel::NEWS));
	EXPECT_FALSE(WIDE.test(channel::TRADES));
	EXPECT_EQ(flag<channel>::from_bits(0x8001'0000U), WIDE);
}

TEST(Flag, IntersectionAndSymmetricDifferenceBehaveAsSets) {
	constexpr caps LEFT  = capability::POST_ONLY | capability::HIDDEN;
	constexpr caps RIGHT = capability::HIDDEN | capability::SPONSORED;

	EXPECT_EQ(LEFT & RIGHT, caps{capability::HIDDEN});
	EXPECT_EQ((LEFT | RIGHT).count(), 3U);
	EXPECT_EQ(LEFT ^ RIGHT,
			  capability::POST_ONLY | capability::SPONSORED);
	EXPECT_TRUE((caps{capability::POST_ONLY} &
				 caps{capability::REDUCE_ONLY}).is_empty());
}
