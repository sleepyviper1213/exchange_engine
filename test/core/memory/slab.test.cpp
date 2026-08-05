#include "core/memory/slab.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <new>
#include <unordered_set>
#include <vector>

using exchange::core::memory::slab;

namespace {

TEST(Slab, RoundsBlockSizeUpToHoldFreeListLink) {
	slab slab(/*block_size=*/1);
	// A block must be large enough to store the intrusive free-list pointer.
	EXPECT_GE(slab.block_size(), sizeof(void *));
}

TEST(Slab, RoundsBlockSizeUpToAlignment) {
	slab slab(/*block_size=*/12, /*block_align=*/std::align_val_t{16});
	EXPECT_EQ(slab.block_size() % 16u, 0u);
	EXPECT_GE(slab.block_size(), 12u);
}

TEST(Slab, HandsOutDistinctAlignedBlocks) {
	constexpr std::size_t kAlign = 64;
	slab slab(kAlign, std::align_val_t{kAlign}, /*blocks_per_slab=*/8);

	std::unordered_set<void *> seen;
	for (int i = 0; i < 100; ++i) { // > blocks_per_slab, forces growth
		void *p = slab.allocate();
		ASSERT_NE(p, nullptr);
		EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % kAlign, 0u)
			<< "block not aligned";
		EXPECT_TRUE(seen.insert(p).second) << "block handed out twice";
	}
	EXPECT_EQ(slab.outstanding(), 100u);
}

TEST(Slab, FreedBlockIsReused) {
	slab slab(32, std::align_val_t{32}, 16);
	void *a = slab.allocate();
	slab.deallocate(a);
	void *b = slab.allocate();
	EXPECT_EQ(a, b) << "freed block should be reused (LIFO)";
	EXPECT_EQ(slab.outstanding(), 1u);
}

TEST(Slab, BlockStorageIsWritableWithoutOverlap) {
	slab slab(sizeof(std::uint64_t),
			  std::align_val_t{alignof(std::uint64_t)},
			  4);
	std::vector<std::uint64_t *> blocks;
	for (std::uint64_t i = 0; i < 50; ++i) {
		auto *p = static_cast<std::uint64_t *>(slab.allocate());
		*p      = i; // write full block
		blocks.push_back(p);
	}
	// If any two blocks overlapped, an earlier write would have been clobbered.
	for (std::uint64_t i = 0; i < blocks.size(); ++i) EXPECT_EQ(*blocks[i], i);
	for (auto *p : blocks) slab.deallocate(p);
	EXPECT_EQ(slab.outstanding(), 0u);
}

TEST(Slab, OutstandingCountTracksAllocations) {
	slab slab(16, std::align_val_t{16}, 8);
	EXPECT_EQ(slab.outstanding(), 0u);
	void *a = slab.allocate();
	void *b = slab.allocate();
	EXPECT_EQ(slab.outstanding(), 2u);
	slab.deallocate(a);
	EXPECT_EQ(slab.outstanding(), 1u);
	slab.deallocate(b);
	EXPECT_EQ(slab.outstanding(), 0u);
}

} // namespace
