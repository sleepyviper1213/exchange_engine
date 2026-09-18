#include "core/optimisation/branchless_binary_search.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <ranges>
#include <vector>

using namespace exchange::core::optimisation;

using ::testing::AllOf;
using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::IsNan;
using ::testing::IsTrue;
using ::testing::ResultOf;

namespace {

using blb_float_limits   = std::numeric_limits<double>;
using blb_float_ladder_t = std::vector<double>;

constexpr double BLB_FLOAT_INF  = blb_float_limits::infinity();
constexpr double BLB_FLOAT_NAN  = blb_float_limits::quiet_NaN();
constexpr double BLB_FLOAT_BASE = 100.0;

/// @brief Lengths swept by the exhaustive comparison: enough to cross the
/// powers of two (16, 32) and their neighbours, where a halving loop that ends
/// one step early is right on the boundary and wrong either side of it -
/// while staying in range the whole time.
constexpr std::size_t BLB_FLOAT_MAX_LENGTH = 33;

[[nodiscard]] blb_float_ladder_t blb_float_ladder(std::size_t count) {
	blb_float_ladder_t ladder;
	ladder.reserve(count);
	for (std::size_t i = 0; i < count; ++i)
		ladder.push_back(BLB_FLOAT_BASE + static_cast<double>(i));
	return ladder;
}

/// @brief Below, at and just above every tick of a @c blb_float_ladder of
/// @p count levels, plus one probe past the end - the three places a rank can
/// be got wrong, at every position.
[[nodiscard]] blb_float_ladder_t blb_float_probes(std::size_t count) {
	blb_float_ladder_t probes;
	probes.reserve(3 * (count + 1));
	for (std::size_t i = 0; i <= count; ++i) {
		const double tick = BLB_FLOAT_BASE + static_cast<double>(i);
		probes.insert(probes.end(), {tick - 0.5, tick, tick + 0.5});
	}
	return probes;
}

/// @brief A strict weak ordering that survives NaN, by collapsing every NaN
/// into one maximal equivalence class.
///
/// A range holding a NaN is not ordered by @c < at all - the element is
/// unordered against every other under all of @c <, @c > and @c == - so the
/// search's precondition is gone before it is even called. This is the repair
/// a caller that can genuinely receive a NaN price has to make; the search is
/// comparator-agnostic and cannot make it on their behalf.
struct blb_float_nan_last {
	[[nodiscard]] bool operator()(double lhs, double rhs) const noexcept {
		if (std::isnan(rhs)) return !std::isnan(lhs);
		if (std::isnan(lhs)) return false;
		return lhs < rhs;
	}
};

struct blb_float_level {
	double price;
	int quantity;
};

// --- matchers, all on the value being searched for ---------------------------
//
// A rank is the thing under test, so the matchers take the searched value and
// go and rank it - `EXPECT_THAT(inf, BlbRanksAt(ladder, 17))` says what the
// case is about, and a failure prints the rank that came back rather than two
// bare integers. Every one of them takes the comparator explicitly; the
// `Blb...` helpers below supply `std::ranges::less` for the ascending cases,
// which is most of them.

MATCHER_P3(BlbRanksUnderAt, ladder, comp, index,
		   "ranks at index " + ::testing::PrintToString(index)) {
	const auto rank =
		branchless_lower_bound(ladder, arg, comp) - ladder.begin();
	*result_listener << "ranks at index " << rank;
	return rank == static_cast<std::ptrdiff_t>(index);
}

MATCHER_P2(BlbRanksUnderLikeStdLowerBound, ladder, comp,
		   "ranks where std::ranges::lower_bound ranks it") {
	const auto rank =
		branchless_lower_bound(ladder, arg, comp) - ladder.begin();
	const auto expected =
		std::ranges::lower_bound(ladder, arg, comp) - ladder.begin();
	*result_listener << "ranks at index " << rank
					 << " where std::ranges::lower_bound says " << expected;
	return rank == expected;
}

/// @brief The only guarantee left when the range is not ordered at all.
MATCHER_P(BlbRanksWithinBoundsOf, ladder, "ranks somewhere in [begin, end]") {
	const auto rank = branchless_lower_bound(ladder, arg) - ladder.begin();
	*result_listener << "ranks at index " << rank << " of " << ladder.size()
					 << " levels";
	return rank >= 0 && rank <= static_cast<std::ptrdiff_t>(ladder.size());
}

MATCHER_P(BlbIsSortedUnder, comp, "is sorted under the given ordering") {
	return std::ranges::is_sorted(arg, comp);
}

[[nodiscard]] auto BlbRanksAt(const blb_float_ladder_t &ladder,
							  std::ptrdiff_t index) {
	return BlbRanksUnderAt(ladder, std::ranges::less{}, index);
}

[[nodiscard]] auto BlbRanksAtEnd(const blb_float_ladder_t &ladder) {
	return BlbRanksAt(ladder, std::ssize(ladder));
}

[[nodiscard]] auto BlbRanksAtBegin(const blb_float_ladder_t &ladder) {
	return BlbRanksAt(ladder, 0);
}

[[nodiscard]] auto BlbRanksLikeStdLowerBound(const blb_float_ladder_t &ladder) {
	return BlbRanksUnderLikeStdLowerBound(ladder, std::ranges::less{});
}

[[nodiscard]] auto BlbIsNegativeZero() {
	return AllOf(::testing::Eq(0.0),
				 ResultOf(
					 "signbit",
					 [](double d) { return std::signbit(d); },
					 IsTrue()));
}

// --- compile-time contract ---------------------------------------------------

constexpr std::array<double, 5> BLB_FLOAT_CONSTEXPR_LADDER{
	-BLB_FLOAT_INF, -1.0, 0.0, 1.0, BLB_FLOAT_INF};

[[nodiscard]] constexpr std::ptrdiff_t blb_float_constexpr_rank(double value) {
	return branchless_lower_bound(BLB_FLOAT_CONSTEXPR_LADDER, value) -
		   BLB_FLOAT_CONSTEXPR_LADDER.begin();
}

// Constant evaluation is the cheapest guard against a non-constexpr call
// creeping back into the loop - a prefetch intrinsic is not usable here, so
// reintroducing one fails to compile rather than quietly dropping constexpr at
// every call site. No matcher reaches here; a matcher is a run-time object.
static_assert(blb_float_constexpr_rank(-BLB_FLOAT_INF) == 0);
static_assert(blb_float_constexpr_rank(0.5) == 3);
static_assert(blb_float_constexpr_rank(BLB_FLOAT_INF) == 4);
static_assert(blb_float_constexpr_rank(blb_float_limits::max()) == 4);

// The NaN answer is pinned at run time only, and the exclusion is MSVC's bug
// rather than a gap in what the search guarantees.
//
// IEEE 754 makes every *ordered* comparison with NaN false, which is what makes
// the loop take no step and land on begin. MSVC's constant evaluator disagrees
// with itself here: measured on 14.51.36231, `1.0 < NaN` folds to **true** at
// compile time and evaluates to false at run time in the same program, while
// `NaN == NaN` is correctly false in both. So the search walks to the end under
// constant evaluation and the assertion below reads 5 rather than 0 - a
// compile-time artefact of the fold, with nothing wrong at run time on any
// compiler this builds for.
//
// Nothing is lost by excluding it: `NanValueCollapsesOntoBegin` and
// `NanOnDegenerateRanges` pin exactly this property where the comparison is
// real. Keep the assertion for every other toolchain, because it is still the
// cheapest place to catch the fold changing back.
//
// The `__clang__` arm is not padding. Clang defines `_MSC_VER` too - it targets
// the MSVC triple by default on Windows, which is what lets one compile
// database serve both Windows presets - and its evaluator folds this correctly
// (checked, 22.1.8). Testing `_MSC_VER` alone would drop the assertion from
// every clang-based check on this platform to work around a bug clang does not
// have.
#if !defined(_MSC_VER) || defined(__clang__)
static_assert(blb_float_constexpr_rank(BLB_FLOAT_NAN) == 0);
#endif

} // namespace

TEST(BranchlessLowerBoundFloating, MatchesStdLowerBoundAtEveryLength) {
	// The structural test. A halving loop that drops its last step answers
	// correctly on power-of-two lengths and is off by several positions in
	// between, without ever leaving the range - so only an exhaustive sweep
	// over lengths, not a handful of hand-picked cases, catches it.
	for (std::size_t count = 0; count <= BLB_FLOAT_MAX_LENGTH; ++count) {
		const auto ladder = blb_float_ladder(count);
		EXPECT_THAT(blb_float_probes(count),
					Each(BlbRanksLikeStdLowerBound(ladder)))
			<< "count=" << count;
	}
}

TEST(BranchlessLowerBoundFloating, MatchesStdLowerBoundOnAnInexactTickLadder) {
	// 0.01 has no exact double, so the ladder's steps are not evenly spaced
	// and a probe reaching the same tick by a different arithmetic route is
	// not necessarily equal to it. Both searches have to agree on the same
	// surprising answer - the comparison is the element's own, and nothing in
	// the loop rounds.
	blb_float_ladder_t ladder;
	blb_float_ladder_t probes;
	ladder.reserve(40);
	probes.reserve(40);
	for (int i = 0; i < 40; ++i) {
		ladder.push_back(BLB_FLOAT_BASE + 0.01 * static_cast<double>(i));
		probes.push_back(BLB_FLOAT_BASE + static_cast<double>(i) / 100.0);
	}

	EXPECT_THAT(probes, Each(BlbRanksLikeStdLowerBound(ladder)));
}

TEST(BranchlessLowerBoundFloating, NegativeInfinityValueReturnsBegin) {
	const auto ladder = blb_float_ladder(17);
	EXPECT_THAT(-BLB_FLOAT_INF, BlbRanksAtBegin(ladder));
	EXPECT_THAT(blb_float_limits::lowest(), BlbRanksAtBegin(ladder));
}

TEST(BranchlessLowerBoundFloating, PositiveInfinityValueReturnsEnd) {
	const auto ladder = blb_float_ladder(17);
	EXPECT_THAT(BLB_FLOAT_INF, BlbRanksAtEnd(ladder));
	EXPECT_THAT(blb_float_limits::max(), BlbRanksAtEnd(ladder));
}

TEST(BranchlessLowerBoundFloating, InfinitiesAsElementsKeepTheirPlace) {
	// Both infinities are ordinary, fully ordered values: unlike NaN they are
	// legal elements of a sorted range and need no special handling anywhere.
	const blb_float_ladder_t ladder{-BLB_FLOAT_INF,
									-1.0,
									0.0,
									1.0,
									BLB_FLOAT_INF};
	EXPECT_THAT(ladder, BlbIsSortedUnder(std::ranges::less{}));
	EXPECT_THAT(-BLB_FLOAT_INF, BlbRanksAt(ladder, 0));
	EXPECT_THAT(blb_float_limits::lowest(), BlbRanksAt(ladder, 1));
	EXPECT_THAT(0.0, BlbRanksAt(ladder, 2));
	EXPECT_THAT(blb_float_limits::max(), BlbRanksAt(ladder, 4));
	EXPECT_THAT(BLB_FLOAT_INF, BlbRanksAt(ladder, 4));
}

TEST(BranchlessLowerBoundFloating, SignedZerosAreOneEquivalenceClass) {
	// -0.0 and 0.0 compare equal, so the search cannot separate them and must
	// not appear to: the insertion point for either is the -0.0 slot.
	const blb_float_ladder_t ladder{-1.0, -0.0, 1.0};
	EXPECT_THAT((blb_float_ladder_t{0.0, -0.0}), Each(BlbRanksAt(ladder, 1)));
	EXPECT_THAT(ladder[1], BlbIsNegativeZero());
}

TEST(BranchlessLowerBoundFloating, SubnormalsAndAdjacentValuesStayDistinct) {
	const blb_float_ladder_t ladder{0.0,
									blb_float_limits::denorm_min(),
									2.0 * blb_float_limits::denorm_min(),
									blb_float_limits::min()};
	EXPECT_THAT(blb_float_limits::denorm_min(), BlbRanksAt(ladder, 1));
	EXPECT_THAT(blb_float_limits::min(), BlbRanksAt(ladder, 3));

	// One ulp apart is still apart: nothing in the loop rounds, the comparison
	// is the element's own.
	const blb_float_ladder_t pair{1.0, std::nextafter(1.0, 2.0)};
	EXPECT_THAT(pair, ElementsAre(BlbRanksAt(pair, 0), BlbRanksAt(pair, 1)));
}

TEST(BranchlessLowerBoundFloating, NanValueCollapsesOntoBegin) {
	const auto ladder = blb_float_ladder(17);
	// `element < NaN` is false for every element, so the loop never takes a
	// step and lands on begin. That is not a rank and it is not meaningful.
	// What the test pins is that it is deterministic and in range, and that
	// std::ranges::lower_bound answers exactly the same nonsense - the
	// branchless form has not invented a failure mode of its own.
	const blb_float_ladder_t nans{BLB_FLOAT_NAN,
								  -BLB_FLOAT_NAN,
								  blb_float_limits::signaling_NaN()};
	ASSERT_THAT(nans, Each(IsNan()));
	EXPECT_THAT(nans,
				Each(AllOf(BlbRanksAtBegin(ladder),
						   BlbRanksLikeStdLowerBound(ladder))));
}

TEST(BranchlessLowerBoundFloating, NanOnDegenerateRanges) {
	const blb_float_ladder_t empty;
	EXPECT_THAT(BLB_FLOAT_NAN,
				AllOf(BlbRanksAtBegin(empty), BlbRanksAtEnd(empty)));

	const blb_float_ladder_t single{BLB_FLOAT_BASE};
	EXPECT_THAT(BLB_FLOAT_NAN, BlbRanksAtBegin(single));

	const blb_float_ladder_t all_nan{BLB_FLOAT_NAN, BLB_FLOAT_NAN};
	EXPECT_THAT(all_nan, Each(IsNan()));
	EXPECT_THAT((blb_float_ladder_t{BLB_FLOAT_BASE, BLB_FLOAT_NAN}),
				Each(BlbRanksAtBegin(all_nan)));
}

TEST(BranchlessLowerBoundFloating, NanAmongTheElementsStaysWithinTheRange) {
	// The precondition is already broken here, so no rank can be correct and
	// the test does not claim one - hence a bounds matcher rather than a rank
	// one. The guarantee that survives is that every advance is by a width the
	// loop has already subtracted from the remaining length, so the result is
	// in [begin, end] whatever the comparator answers: a wrong answer, never a
	// wild iterator. The matcher catches an out-of-range *result*; what needs
	// a sanitizer or a checked-iterator build on top is the read the loop
	// performs before returning, which would be out of bounds first.
	const auto clean = blb_float_ladder(16);
	for (std::size_t poison = 0; poison < clean.size(); ++poison) {
		auto corrupted    = clean;
		corrupted[poison] = BLB_FLOAT_NAN;
		EXPECT_THAT(blb_float_probes(corrupted.size()),
					Each(BlbRanksWithinBoundsOf(corrupted)))
			<< "poison=" << poison;
	}
}

TEST(BranchlessLowerBoundFloating, TotalOrderComparatorRanksNanLast) {
	auto ladder = blb_float_ladder(8);
	ladder.push_back(BLB_FLOAT_NAN);
	ladder.push_back(BLB_FLOAT_NAN);

	const blb_float_nan_last comp{};
	ASSERT_THAT(ladder, BlbIsSortedUnder(comp));

	EXPECT_THAT(BLB_FLOAT_BASE + 3.5, BlbRanksUnderAt(ladder, comp, 4));
	// +inf sits after every finite element and before the NaNs.
	EXPECT_THAT(BLB_FLOAT_INF, BlbRanksUnderAt(ladder, comp, 8));
	EXPECT_THAT(BLB_FLOAT_NAN,
				AllOf(BlbRanksUnderAt(ladder, comp, 8),
					  BlbRanksUnderLikeStdLowerBound(ladder, comp)));
}

TEST(BranchlessLowerBoundFloating, DescendingBidLadderWithGreater) {
	// The bid side is descending, so `greater` is the ordering - and NaN is
	// unordered under that one too, collapsing onto begin exactly as it does
	// under `less` rather than onto the far end. A NaN quote therefore reads
	// as "improves the touch" on a bid book, which is the reason the caller,
	// not the search, has to reject it.
	const blb_float_ladder_t bids{BLB_FLOAT_INF, 105.0, 100.0, -BLB_FLOAT_INF};
	const std::greater<double> comp{};
	ASSERT_THAT(bids, BlbIsSortedUnder(comp));

	EXPECT_THAT(102.5, BlbRanksUnderAt(bids, comp, 2));
	EXPECT_THAT(BLB_FLOAT_INF, BlbRanksUnderAt(bids, comp, 0));
	EXPECT_THAT(-BLB_FLOAT_INF, BlbRanksUnderAt(bids, comp, 3));
	EXPECT_THAT(BLB_FLOAT_NAN,
				AllOf(BlbRanksUnderAt(bids, comp, 0),
					  BlbRanksUnderLikeStdLowerBound(bids, comp)));
}

TEST(BranchlessLowerBoundFloating, ProjectsADoublePriceOutOfALevel) {
	const std::vector<blb_float_level> book{{100.0, 1},
											{100.5, 2},
											{BLB_FLOAT_INF, 3}};
	const auto it = branchless_lower_bound(book,
										   100.25,
										   std::ranges::less{},
										   &blb_float_level::price);
	ASSERT_NE(it, book.end());
	EXPECT_THAT(*it,
				AllOf(ResultOf(
						  "price",
						  [](const blb_float_level &l) { return l.price; },
						  ::testing::DoubleEq(100.5)),
					  ResultOf(
						  "quantity",
						  [](const blb_float_level &l) { return l.quantity; },
						  ::testing::Eq(2))));

	const auto nan_it = branchless_lower_bound(book,
											   BLB_FLOAT_NAN,
											   std::ranges::less{},
											   &blb_float_level::price);
	EXPECT_EQ(nan_it, book.begin());
}
