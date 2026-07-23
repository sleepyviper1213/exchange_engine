#include "memory/allocator.hpp"
#include "memory/arena.hpp"
#include "memory/slab.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <type_traits>
#include <vector>

namespace {

using memory::Allocator;
using memory::ArenaResource;
using memory::MallocResource;

TEST(Allocator, DefaultMallocAllocatorsShareResourceAndCompareEqual) {
	Allocator<int> a;
	Allocator<int> b;
	EXPECT_EQ(a, b); // both point at the shared default_resource()
}

TEST(Allocator, RebindProducesSameResourceAllocatorForU) {
	using IntAlloc    = Allocator<int, MallocResource>;
	using ReboundChar = IntAlloc::rebind<char>::other;
	static_assert(std::is_same_v<ReboundChar, Allocator<char, MallocResource>>);
	SUCCEED();
}

TEST(Allocator, DrivesStdVectorOffTheDefaultHeap) {
	std::vector<int, Allocator<int>> v;
	for (int i = 0; i < 1000; ++i) v.push_back(i);
	// Data survives the reallocations the growth triggered.
	int expected = 0;
	for (int x : v) EXPECT_EQ(x, expected++);
	EXPECT_EQ(std::accumulate(v.begin(), v.end(), 0), 999 * 1000 / 2);
}

TEST(Allocator, DrivesStdVectorOffAnArena) {
	memory::Arena arena;
	arena.init(1u << 20); // 1 MiB portable pool
	ArenaResource res(arena);

	std::vector<int, Allocator<int, ArenaResource>> v{
		Allocator<int, ArenaResource>(res)};
	for (int i = 0; i < 2000; ++i) v.push_back(i);

	int expected = 0;
	for (int x : v) EXPECT_EQ(x, expected++);
}

TEST(Allocator, SlabBackedSingleObjectRoundTrip) {
	struct Node {
		std::uint64_t a, b, c;
	};
	memory::Slab slab(sizeof(Node), alignof(Node), 32);
	Allocator<Node, memory::Slab> alloc(slab);

	Node *p = alloc.allocate(1);
	ASSERT_NE(p, nullptr);
	p->a = 1;
	p->b = 2;
	p->c = 3;
	EXPECT_EQ(p->a + p->b + p->c, 6u);
	alloc.deallocate(p, 1);
	EXPECT_EQ(slab.outstanding(), 0u);
}

TEST(Allocator, ConvertingConstructorSharesResource) {
	memory::Arena arena;
	arena.init(1u << 16);
	ArenaResource res(arena);
	Allocator<int, ArenaResource> ai(res);
	Allocator<double, ArenaResource> ad(ai); // rebinding conversion
	EXPECT_EQ(ai.resource(), ad.resource());
}

} // namespace
