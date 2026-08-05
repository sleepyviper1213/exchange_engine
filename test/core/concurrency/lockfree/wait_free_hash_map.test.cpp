#include "core/concurrency/lockfree/wait_free_hash_map.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

// A trivially copyable but NOT default-constructible payload: it proves the map
// stores keys/values as raw storage (MaybeUninit-style) and never default-
// constructs them.
struct Loc {
	int side;
	std::uint64_t price;
	Loc() = delete;

	constexpr Loc(int s, std::uint64_t p) noexcept : side(s), price(p) {}

	bool operator==(const Loc &) const noexcept = default;
};

using Map =
	exchange::core::concurrency::lockfree::wait_free_hash_map<std::uint64_t,
															  Loc, 1024>;

// --------------------------------------------------------------------------
// Single-threaded semantics
// --------------------------------------------------------------------------

TEST(WaitFreeHashMap, GetOnEmptyBucketIsNullopt) {
	Map map;
	EXPECT_FALSE(map.get(42).has_value());
}

TEST(WaitFreeHashMap, InsertFreshKeyReturnsNoPreviousThenReadsBack) {
	Map map;
	EXPECT_FALSE(map.insert(7, Loc{0, 100}).has_value());

	const auto got = map.get(7);
	ASSERT_TRUE(got.has_value());
	EXPECT_EQ(got->side, 0);
	EXPECT_EQ(got->price, 100u);
}

TEST(WaitFreeHashMap, UpdateReturnsPreviousAndValueSurvives) {
	// The Rust original lost the value here (its update flipped the bucket back
	// to "empty"); this is the regression guard for the fixed seqlock parity.
	Map map;
	map.insert(7, Loc{0, 100});

	const auto previous = map.insert(7, Loc{1, 200});
	ASSERT_TRUE(previous.has_value());
	EXPECT_EQ(previous->price, 100u);

	const auto got = map.get(7);
	ASSERT_TRUE(got.has_value());
	EXPECT_EQ(got->side, 1);
	EXPECT_EQ(got->price, 200u);
}

TEST(WaitFreeHashMap, AbsentKeyIsNullopt) {
	Map map;
	map.insert(7, Loc{0, 100});
	EXPECT_FALSE(map.get(8).has_value());
}

TEST(WaitFreeHashMap, CollidingKeyOverwritesAndOriginalReadsAsAbsent) {
	// Direct-mapped: two keys that land in the same bucket share the slot, and
	// the later insert wins. A lookup of the evicted key must report absent
	// (it must not return the colliding key's value).
	exchange::core::concurrency::lockfree::
		wait_free_hash_map<std::uint64_t, Loc, 8>
			map;                        // small table forces a collision
	map.insert(1, Loc{0, 111});
	map.insert(1 + 8, Loc{0, 999}); // hashes to the same bucket as key 1

	const auto evicted = map.get(1);
	ASSERT_TRUE(evicted.has_value() == false)
		<< "evicted key must not read back the colliding value";

	const auto winner = map.get(1 + 8);
	ASSERT_TRUE(winner.has_value());
	EXPECT_EQ(winner->price, 999u);
}

TEST(WaitFreeHashMap, ManyDistinctKeysCoexist) {
	Map map;
	for (std::uint64_t k = 0; k < 500; ++k) map.insert(k, Loc{0, k * 10});
	for (std::uint64_t k = 0; k < 500; ++k) {
		const auto got = map.get(k);
		ASSERT_TRUE(got.has_value()) << "missing key " << k;
		EXPECT_EQ(got->price, k * 10);
	}
}

// --------------------------------------------------------------------------
// Concurrency: single writer, many readers (the map's threading contract).
// Meant to run under the ThreadSanitizer config, which flags a torn access on
// interleavings that happen not to fail the invariant here.
// --------------------------------------------------------------------------

TEST(WaitFreeHashMap, SingleWriterManyReadersNeverTear) {
	// Each value carries a field and its bitwise complement. A snapshot torn
	// across the two payload words would break hi == ~lo.
	struct Pair {
		std::uint64_t lo;
		std::uint64_t hi;
	};

	static_assert(
		sizeof(Pair) > sizeof(std::uint64_t),
		"Pair must span multiple words to exercise the torn-read path");

	constexpr std::uint64_t kKeys = 256;
	exchange::core::concurrency::lockfree::wait_free_hash_map<std::uint64_t, Pair, 1024> map;
	for (std::uint64_t k = 0; k < kKeys; ++k) map.insert(k, Pair{k, ~k});

	std::atomic<bool> stop{false};
	std::atomic<std::uint64_t> tears{0};

	std::vector<std::thread> readers;
	for (int r = 0; r < 4; ++r)
		readers.emplace_back([&] {
			while (!stop.load(std::memory_order_relaxed))
				for (std::uint64_t k = 0; k < kKeys; ++k)
					if (const auto v = map.get(k); v && v->hi != ~v->lo)
						tears.fetch_add(1, std::memory_order_relaxed);
		});

	for (int pass = 0; pass < 5000; ++pass)
		for (std::uint64_t k = 0; k < kKeys; ++k) {
			const std::uint64_t val =
				(k << 20) ^ static_cast<std::uint64_t>(pass);
			map.insert(k, Pair{val, ~val});
		}

	stop.store(true, std::memory_order_relaxed);
	for (auto &t : readers) t.join();

	EXPECT_EQ(tears.load(), 0u) << "readers observed a torn seqlock snapshot";

	// Every key is still present and self-consistent after the churn.
	for (std::uint64_t k = 0; k < kKeys; ++k) {
		const auto v = map.get(k);
		ASSERT_TRUE(v.has_value()) << "key " << k << " vanished";
		EXPECT_EQ(v->hi, ~v->lo);
	}
}

} // namespace
