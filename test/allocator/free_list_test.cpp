#include "memory/free_list.hpp"
#include "memory/free_list_hazard.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <unordered_set>
#include <vector>

// Both FreeList implementations share one interface (push(void*)/pop()->void*),
// so every correctness property is checked against both via a typed test. The
// two differ only in how they defeat ABA (version tag vs hazard pointers), which
// is invisible to the contract but is exactly what the concurrency test stresses.
namespace {

// Cache-line-sized, cache-aligned blocks: comfortably above either FreeList's
// kMinBlockBytes and roomy enough to stamp while a block is checked out.
struct alignas(64) Block {
	std::byte bytes[64];
};

// Hand out raw block pointers backed by stable storage, mirroring the arena's
// "addresses never move" guarantee that both FreeLists rely on.
class BlockArena {
public:
	explicit BlockArena(std::size_t count) : storage_(count) {}

	[[nodiscard]] void *block(std::size_t i) noexcept { return &storage_[i]; }
	[[nodiscard]] std::size_t size() const noexcept { return storage_.size(); }

	/// Index of a block previously handed out by @c block(); the contiguous
	/// backing makes this plain pointer arithmetic.
	[[nodiscard]] std::size_t index_of(void *p) const noexcept {
		return static_cast<std::size_t>(static_cast<Block *>(p) - storage_.data());
	}

private:
	std::vector<Block> storage_;
};

template <class FL>
class FreeListTest : public testing::Test {};

using FreeListTypes =
	testing::Types<memory::tagged::FreeList, memory::hazard::FreeList>;
TYPED_TEST_SUITE(FreeListTest, FreeListTypes);

// --------------------------------------------------------------------------
// Single-threaded correctness
// --------------------------------------------------------------------------

TYPED_TEST(FreeListTest, PopFromEmptyReturnsNull) {
	TypeParam fl;
	EXPECT_EQ(fl.pop(), nullptr);
}

TYPED_TEST(FreeListTest, PushThenPopYieldsABlock) {
	TypeParam fl;
	BlockArena arena(1);
	fl.push(arena.block(0));

	void *got = fl.pop();
	EXPECT_NE(got, nullptr);
	EXPECT_EQ(fl.pop(), nullptr); // only one was pushed
}

// Order differs between the two (the tagged stack is strict LIFO; the hazard
// version reorders through retirement), so the portable invariant is that the
// exact set of pushed blocks comes back — nothing lost, nothing invented.
TYPED_TEST(FreeListTest, ConservesEveryPushedBlock) {
	constexpr std::size_t kBlocks = 32;
	TypeParam fl;
	BlockArena arena(kBlocks);

	std::unordered_set<void *> pushed;
	for (std::size_t i = 0; i < kBlocks; ++i) {
		fl.push(arena.block(i));
		pushed.insert(arena.block(i));
	}

	std::unordered_set<void *> popped;
	while (void *b = fl.pop()) {
		EXPECT_TRUE(pushed.contains(b)) << "popped a block that was never pushed";
		EXPECT_TRUE(popped.insert(b).second) << "same block popped twice";
	}
	EXPECT_EQ(popped, pushed);
}

TYPED_TEST(FreeListTest, RecyclesAcrossManyRounds) {
	// One block cycled repeatedly: the tagged version bumps its version every
	// round, the hazard version retires and reclaims it every round.
	TypeParam fl;
	BlockArena arena(1);
	for (int i = 0; i < 100; ++i) {
		fl.push(arena.block(0));
		void *b = fl.pop();
		ASSERT_EQ(b, arena.block(0)) << "round " << i;
	}
}

// --------------------------------------------------------------------------
// Concurrency: designed to expose a double hand-out, the symptom an ABA bug
// produces — two threads holding the same block at once. Each worker stamps its
// checked-out block with a unique nonce, spins, and checks the stamp survived; a
// double hand-out lets another thread overwrite it and the check fails. gtest
// macros are not thread-safe, so anomalies go into an atomic the main thread
// asserts on.
// --------------------------------------------------------------------------

TYPED_TEST(FreeListTest, ConcurrentPushPopNeverDoubleHandsOut) {
	constexpr int kThreads       = 8;
	constexpr int kOpsEach       = 40000;
	constexpr std::size_t kBlocks = 16; // fewer than threads -> heavy contention

	TypeParam fl;
	BlockArena arena(kBlocks);
	for (std::size_t i = 0; i < kBlocks; ++i) fl.push(arena.block(i));

	std::atomic<std::uint64_t> corruption{0};
	// One stamp cell per block, kept off to the side so the test never aliases
	// the storage the FreeList threads its own node through.
	std::vector<std::atomic<std::uint64_t>> stamps(kBlocks);

	std::vector<std::thread> threads;
	threads.reserve(kThreads);
	for (int t = 0; t < kThreads; ++t) {
		threads.emplace_back([&, t] {
			std::uint64_t nonce = static_cast<std::uint64_t>(t + 1) << 40;
			for (int i = 0; i < kOpsEach; ++i) {
				void *block = fl.pop();
				if (block == nullptr) continue; // momentarily drained; retry
				auto &stamp              = stamps[arena.index_of(block)];
				const std::uint64_t mine = ++nonce;
				stamp.store(mine, std::memory_order_relaxed);
				for (int spin = 0; spin < 4; ++spin)
					std::atomic_signal_fence(std::memory_order_seq_cst);
				if (stamp.load(std::memory_order_relaxed) != mine)
					corruption.fetch_add(1, std::memory_order_relaxed);
				fl.push(block);
			}
		});
	}
	for (auto &th : threads) th.join();

	EXPECT_EQ(corruption.load(), 0u)
		<< "a block was handed to two threads at once (ABA / double pop)";

	// Every block must survive the run: draining yields exactly kBlocks distinct
	// pointers, none lost to a corrupted CAS and none duplicated.
	std::unordered_set<void *> drained;
	while (void *b = fl.pop()) drained.insert(b);
	EXPECT_EQ(drained.size(), kBlocks)
		<< "blocks were lost or duplicated under contention";
}

} // namespace
