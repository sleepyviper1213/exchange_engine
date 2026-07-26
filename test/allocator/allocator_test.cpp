#include "core/memory.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <type_traits>
#include <vector>

namespace {

using namespace exchange::core::memory;

TEST(Allocator, DefaultMallocAllocatorsShareResourceAndCompareEqual) {
	allocator<int> a;
	allocator<int> b;
	EXPECT_EQ(a, b); // both point at the shared default_resource()
}

TEST(Allocator, RebindProducesSameResourceAllocatorForU) {
	using IntAlloc    = allocator<int, malloc_resource>;
	using ReboundChar = IntAlloc::rebind<char>::other;
	static_assert(
		std::is_same_v<ReboundChar, allocator<char, malloc_resource>>);
	SUCCEED();
}

TEST(Allocator, DrivesStdVectorOffTheDefaultHeap) {
	std::vector<int, allocator<int>> v;
	for (int i = 0; i < 1000; ++i) v.push_back(i);
	// Data survives the reallocations the growth triggered.
	int expected = 0;
	for (int x : v) EXPECT_EQ(x, expected++);
	EXPECT_EQ(std::accumulate(v.begin(), v.end(), 0), 999 * 1000 / 2);
}

TEST(Allocator, DrivesStdVectorOffAnArena) {
	arena arena;
	arena.init(1u << 20); // 1 MiB portable pool
	arena_resource res(arena);

	std::vector<int, allocator<int, arena_resource>> v{
		allocator<int, arena_resource>(res)};
	for (int i = 0; i < 2000; ++i) v.push_back(i);

	int expected = 0;
	for (int x : v) EXPECT_EQ(x, expected++);
}

TEST(Allocator, ArenaDoesNotRecycleABlockIntoALargerRequest) {
	arena arena;
	arena.init(1u << 16);

	// Free a small block, then ask for one that cannot fit in it. Serving the
	// request from the freed block would overlap whatever the arena hands out
	// next — the corruption std::vector growth used to hit.
	void *small = arena.allocate(8, std::align_val_t{8});
	ASSERT_NE(small, nullptr);
	arena.deallocate(small, 8, std::align_val_t{8});

	void *large = arena.allocate(64, std::align_val_t{8});
	ASSERT_NE(large, nullptr);
	EXPECT_NE(large, small);

	void *neighbour = arena.allocate(64, std::align_val_t{8});
	ASSERT_NE(neighbour, nullptr);
	const auto lo = static_cast<std::uint8_t *>(large);
	const auto hi = static_cast<std::uint8_t *>(neighbour);
	EXPECT_GE(hi < lo ? lo - hi : hi - lo, 64); // no overlap

	// A same-size request may reuse the freed block.
	EXPECT_EQ(arena.allocate(8, std::align_val_t{8}), small);
}

TEST(Allocator, SlabBackedSingleObjectRoundTrip) {
	struct Node {
		std::uint64_t a, b, c;
	};

	slab s(sizeof(Node), std::align_val_t{alignof(Node)}, 32);
	allocator<Node, slab> alloc(s);

	Node *p = alloc.allocate(1);
	ASSERT_NE(p, nullptr);
	p->a = 1;
	p->b = 2;
	p->c = 3;
	EXPECT_EQ(p->a + p->b + p->c, 6u);
	alloc.deallocate(p, 1);
	EXPECT_EQ(s.outstanding(), 0u);
}

TEST(Allocator, ConvertingConstructorSharesResource) {
	arena arena;
	arena.init(1u << 16);
	arena_resource res(arena);
	allocator<int, arena_resource> ai(res);
	allocator<double, arena_resource> ad(ai); // rebinding conversion
	EXPECT_EQ(ai.resource(), ad.resource());
}

} // namespace
