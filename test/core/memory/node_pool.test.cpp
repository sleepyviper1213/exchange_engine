#include "core/memory/node_pool.hpp"

#include <gtest/gtest.h>

#include <unordered_set>
#include <vector>

namespace {
struct node_payload {
	std::uint64_t id    = 0;
	std::uint64_t token = 0;
};

using Pool  = exchange::core::memory::node_pool<node_payload>;
using Index = Pool::Index;

TEST(NodePool, IndexZeroIsReservedNull) {
	Pool pool;
	// The very first allocation must not be the null sentinel.
	const Index a = pool.allocate();
	EXPECT_NE(a, Pool::NO_NODE);
}

TEST(NodePool, AllocatedNodesAreDistinctAndUsable) {
	Pool pool;
	std::unordered_set<Index> seen;
	for (int i = 0; i < 1000; ++i) {
		const Index idx = pool.allocate();
		ASSERT_NE(idx, Pool::NO_NODE);
		EXPECT_TRUE(seen.insert(idx).second)
			<< "index handed out twice: " << idx;
		pool.get(idx).value.id = static_cast<std::uint64_t>(i); // writable
		EXPECT_EQ(pool.get(idx).value.id, static_cast<std::uint64_t>(i));
	}
}

TEST(NodePool, FreshNodeHasNullLinks) {
	Pool pool;
	const Index idx = pool.allocate();
	EXPECT_EQ(pool.get(idx).next, Pool::NO_NODE);
	EXPECT_EQ(pool.get(idx).prev, Pool::NO_NODE);
}

TEST(NodePool, DeallocatedSlotIsReused) {
	Pool pool;
	const Index a = pool.allocate();
	pool.deallocate(a);
	const Index b = pool.allocate();
	// The most-recently freed slot is handed back (LIFO free list).
	EXPECT_EQ(a, b);
}

TEST(NodePool, DeallocateClearsPayloadForNextUser) {
	Pool pool;
	const Index a           = pool.allocate();
	pool.get(a).value.id    = 42;
	pool.get(a).value.token = 7;
	pool.deallocate(a);

	const Index b = pool.allocate(); // reuses the same slot
	ASSERT_EQ(a, b);
	EXPECT_EQ(pool.get(b).value.id, 0u) << "stale payload leaked to next user";
	EXPECT_EQ(pool.get(b).value.token, 0u);
}

TEST(NodePool, IndicesSurviveGrowthReallocation) {
	// Handles are indices, so growth (which reallocates the vector) must not
	// invalidate an index taken before the growth.
	Pool pool(2);
	const Index first        = pool.allocate();
	pool.get(first).value.id = 0xAB'CDEF;

	// Force many allocations to reallocate the backing storage several times.
	for (int i = 0; i < 10000; ++i) (void)pool.allocate();

	EXPECT_EQ(pool.get(first).value.id, 0xAB'CDEFu)
		<< "index dangled across storage growth";
}

TEST(NodePool, LinkNodesIntoAFifoByIndex) {
	// Exercise the intrusive links the way OrderList does.
	Pool pool;
	const Index a    = pool.allocate();
	const Index b    = pool.allocate();
	const Index c    = pool.allocate();
	pool.get(a).next = b;
	pool.get(b).prev = a;
	pool.get(b).next = c;
	pool.get(c).prev = b;

	// Walk forward a -> b -> c.
	std::vector<Index> forward;
	for (Index n = a; n != Pool::NO_NODE; n = pool.get(n).next)
		forward.push_back(n);
	EXPECT_EQ(forward, (std::vector<Index>{a, b, c}));
}

TEST(NodePool, CapacityTracksBackingStorage) {
	Pool pool;
	EXPECT_EQ(pool.capacity(), 0u);
	(void)pool.allocate();
	(void)pool.allocate();
	EXPECT_GE(pool.capacity(), 2u);
}
} // namespace