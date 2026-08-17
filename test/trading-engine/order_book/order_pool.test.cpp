#include "trading-engine/order_book/detail/order_pool.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// The pool's contract is three properties the matching path depends on, none of
// which the book's own tests can observe:
//
//   1. The first block exists once the constructor returns, so no acquire pays
//      for it. This is the whole reason warm() exists — left lazy, the first
//      order a book ever rests absorbed the block allocation, the free-list
//      threading and every page fault it triggered (measured at ~1.2 ms for a
//      32k-cell pool, against ~0.1 us once warmed).
//   2. Cells within the first block are one dense ascending run, which is what
//      makes an intrusive walk over them a sequential read.
//   3. Overrunning the capacity is *visible*: either refused outright, or
//      chained and reported by overran(). A silent second block is the case
//      that makes latency unexplainable after the fact.
//
// These are exercised through a stand-in node rather than detail::resting_order.
// basic_pool is a template and none of the three properties depend on what it
// stores, while resting_order is internal to the trading_engine DLL and has no
// export annotation — testing against it directly would mean widening the
// library's ABI to suit a test. What does need saying about the real node is its
// size, and that is a static_assert, which needs no linkage at all.

using exchange::engine::detail::basic_pool;
using exchange::engine::detail::pool_growth;
using exchange::engine::detail::resting_order;

namespace {

/// @brief A stand-in for @c detail::resting_order: same size, same alignment,
///        and constructible from the same shape of arguments.
struct node {
	std::uint64_t id;
	std::uint64_t hook_a;
	std::uint64_t hook_b;
	std::int32_t qty;
	std::int32_t pad;

	node(std::uint64_t node_id, std::int32_t quantity) noexcept
		: id(node_id), hook_a(0), hook_b(0), qty(quantity), pad(0) {}
};

// The point of the stand-in is that it is the same shape as the real thing, so
// the density figures this suite checks are the ones the book actually gets.
static_assert(sizeof(node) == sizeof(resting_order),
			  "the stand-in must match the node the order pool really holds");
static_assert(alignof(node) == alignof(resting_order));

// And the real node's own budget, restated where the pool's density is tested:
// 32 bytes is two per cache line, which is the reason any of this matters.
static_assert(sizeof(resting_order) == 32,
			  "a resting order must stay half a cache line");

using test_pool = basic_pool<node>;

/// @brief Acquire @p n cells, returning them in acquisition order. A null entry
///        is a refusal and is kept, since which acquire refused is the point.
std::vector<node *> acquire_n(test_pool &pool, std::size_t n) {
	std::vector<node *> nodes;
	nodes.reserve(n);
	for (std::size_t i = 0; i < n; ++i)
		nodes.push_back(pool.acquire(static_cast<std::uint64_t>(i + 1), 1));
	return nodes;
}

void release_all(test_pool &pool, const std::vector<node *> &nodes) {
	for (auto *cell : nodes) pool.release(cell);
}

} // namespace

// --------------------------------------------------------------------------
// Warm-up: the block is taken at construction
// --------------------------------------------------------------------------

TEST(OrderPool, ConstructorReportsItsCapacityAndNothingLive) {
	const test_pool pool{64};
	EXPECT_EQ(pool.capacity(), 64U);
	EXPECT_EQ(pool.is_alive(), 0U);
	EXPECT_EQ(pool.high_water(), 0U);
	EXPECT_FALSE(pool.overran());
}

TEST(OrderPool, ZeroCapacityFallsBackToTheDefault) {
	const test_pool pool{0};
	EXPECT_EQ(pool.capacity(), test_pool::DEFAULT_CAPACITY);
}

TEST(OrderPool, EveryCellOfTheFirstBlockIsAvailableWithoutChaining) {
	constexpr std::size_t CAP = 256;
	test_pool pool{CAP};

	const auto nodes = acquire_n(pool, CAP);
	for (auto *cell : nodes) ASSERT_NE(cell, nullptr);

	// Exactly at capacity is the boundary that must *not* count as an overrun:
	// the block holds CAP cells, so the CAP-th is the last one that fits.
	EXPECT_EQ(pool.is_alive(), CAP);
	EXPECT_EQ(pool.high_water(), CAP);
	EXPECT_FALSE(pool.overran());

	release_all(pool, nodes);
	EXPECT_EQ(pool.is_alive(), 0U);
}

// --------------------------------------------------------------------------
// Density: the first block is one ascending run
// --------------------------------------------------------------------------

TEST(OrderPool, FirstBlockCellsAreContiguousAndAscending) {
	constexpr std::size_t CAP = 512;
	test_pool pool{CAP};
	const auto nodes = acquire_n(pool, CAP);

	for (std::size_t i = 1; i < nodes.size(); ++i) {
		ASSERT_NE(nodes[i], nullptr);
		EXPECT_GT(nodes[i], nodes[i - 1])
			<< "cell " << i << " is not above its predecessor";
	}

	// One dense run: first and last cell are exactly (CAP-1) strides apart, so
	// there is no padding and no second block hiding in the middle.
	const auto span = static_cast<std::size_t>(
		reinterpret_cast<char *>(nodes.back()) -  // NOLINT: address arithmetic
		reinterpret_cast<char *>(nodes.front())); // NOLINT
	EXPECT_EQ(span, (CAP - 1) * sizeof(node));

	release_all(pool, nodes);
}

TEST(OrderPool, ReleasedCellIsHandedStraightBack) {
	test_pool pool{16};
	node *first = pool.acquire(1, 5);
	ASSERT_NE(first, nullptr);
	pool.release(first);

	// LIFO: the hottest cell is the one just returned.
	node *again = pool.acquire(2, 7);
	EXPECT_EQ(again, first);
	EXPECT_EQ(again->id, 2U);
	pool.release(again);
}

// --------------------------------------------------------------------------
// Overrun is visible
// --------------------------------------------------------------------------

TEST(OrderPool, ChainedGrowthSucceedsButReportsTheOverrun) {
	constexpr std::size_t CAP = 32;
	test_pool pool{CAP, pool_growth::chained};

	auto nodes = acquire_n(pool, CAP);
	ASSERT_FALSE(pool.overran());

	node *extra = pool.acquire(999, 1);
	EXPECT_NE(extra, nullptr) << "chained growth must still serve the order";
	EXPECT_TRUE(pool.overran());
	EXPECT_EQ(pool.high_water(), CAP + 1);

	pool.release(extra);
	release_all(pool, nodes);

	// high_water outlives the orders it counted — it is the capacity-planning
	// reading, not a live gauge, and emptying the book must not erase it.
	EXPECT_EQ(pool.is_alive(), 0U);
	EXPECT_EQ(pool.high_water(), CAP + 1);
	EXPECT_TRUE(pool.overran());
}

TEST(OrderPool, FixedGrowthRefusesRatherThanChaining) {
	constexpr std::size_t CAP = 8;
	test_pool pool{CAP, pool_growth::fixed};

	const auto nodes = acquire_n(pool, CAP + 4);
	for (std::size_t i = 0; i < CAP; ++i)
		EXPECT_NE(nodes[i], nullptr) << "cell " << i << " is inside capacity";
	for (std::size_t i = CAP; i < nodes.size(); ++i)
		EXPECT_EQ(nodes[i], nullptr) << "cell " << i << " is past capacity";

	EXPECT_EQ(pool.is_alive(), CAP);
	EXPECT_EQ(pool.high_water(), CAP);
	EXPECT_FALSE(pool.overran())
		<< "a refusal is not an overrun: no second block was taken";

	release_all(pool, nodes); // release(nullptr) is a no-op
	EXPECT_EQ(pool.is_alive(), 0U);
}

TEST(OrderPool, FixedGrowthServesAgainAfterACellIsReturned) {
	test_pool pool{2, pool_growth::fixed};
	node *first  = pool.acquire(1, 1);
	node *second = pool.acquire(2, 1);
	ASSERT_NE(first, nullptr);
	ASSERT_NE(second, nullptr);
	EXPECT_EQ(pool.acquire(3, 1), nullptr);

	pool.release(first);
	node *third = pool.acquire(3, 1);
	EXPECT_NE(third, nullptr) << "a returned cell must become available again";

	pool.release(second);
	pool.release(third);
}

// --------------------------------------------------------------------------
// Bookkeeping
// --------------------------------------------------------------------------

TEST(OrderPool, ReleasingNullIsANoOp) {
	test_pool pool{4};
	node *cell = pool.acquire(1, 1);
	ASSERT_NE(cell, nullptr);
	EXPECT_EQ(pool.is_alive(), 1U);

	pool.release(nullptr);
	EXPECT_EQ(pool.is_alive(), 1U) << "a null release must not move the counter";

	pool.release(cell);
	EXPECT_EQ(pool.is_alive(), 0U);
}

TEST(OrderPool, ConstructedNodeCarriesItsArguments) {
	test_pool pool{4};
	node *cell = pool.acquire(42, 9);
	ASSERT_NE(cell, nullptr);
	EXPECT_EQ(cell->id, 42U);
	EXPECT_EQ(cell->qty, 9);
	pool.release(cell);
}

// The level ladder draws its cells from the same template with a different node,
// so the policy and the counters have to hold for a second instantiation and not
// only for the one shaped like a resting order.
TEST(OrderPool, PolicyAndCountersHoldForAnyNodeType) {
	struct small {
		int value;
	};
	basic_pool<small> pool{4, pool_growth::fixed};

	std::vector<small *> cells;
	for (int i = 0; i < 6; ++i) cells.push_back(pool.acquire(small{i}));

	EXPECT_NE(cells[3], nullptr);
	EXPECT_EQ(cells[4], nullptr);
	EXPECT_EQ(pool.is_alive(), 4U);
	EXPECT_EQ(pool.capacity(), 4U);

	for (auto *cell : cells) pool.release(cell);
	EXPECT_EQ(pool.is_alive(), 0U);
}
