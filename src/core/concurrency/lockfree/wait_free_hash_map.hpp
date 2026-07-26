#pragma once
#include "fwd.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <new>
#include <optional>
#include <type_traits>

namespace exchange::core::concurrency::lockfree {

/**
 * @brief Read-optimized, direct-mapped concurrent hash map (seqlock buckets).
 *
 * A fixed array of @c Size cache-line-isolated buckets, each holding at most
 * one entry. The bucket for a key is @c hash(key) % Size — there is no probing
 * or chaining, so two keys that collide share a slot and the later @c insert
 * overwrites the earlier one. This trades worst-case occupancy for a
 * branch-free O(1) lookup with no pointer chasing, which is the right shape for
 * a small, read-heavy index (e.g. order-id -> location) sized well above its
 * live count.
 *
 * Each bucket is a seqlock: an even, non-zero @c version means "published and
 * occupied", an odd @c version means "a write is in progress", and @c 0 means
 * "never written". A reader snapshots the value between two version reads and
 * retries if they differ (or the version is odd), so it never observes a torn
 * write and never blocks a writer. The payload is read and written one machine
 * word at a time through @c std::atomic_ref (relaxed): concurrent access to the
 * same word is therefore a well-defined atomic race, not undefined behaviour,
 * and the version re-check discards any snapshot torn across words.
 *
 * @tparam Key   Trivially copyable, equality-comparable key.
 * @tparam Value Trivially copyable value.
 * @tparam Size  Number of buckets (>= 1). Need not be a power of two.
 * @tparam Hash  Key hasher; defaults to @c std::hash<Key>.
 *
 * @par Threading contract
 * Single-writer, multi-reader. At most one thread may call @c insert at a time;
 * any number of threads may call @c get concurrently with it and with each
 * other. Concurrent writers are not supported (there is no CAS on the version),
 * mirroring the single-producer discipline of this directory's queues.
 *
 * @note Ported from a Rust @c WaitFreeHashTable. The port fixes two defects in
 *       the original: an update bumped an occupied bucket's version by one
 *       (flipping it back to "empty" so the value was lost on the next read),
 *       and it published the data before the version with no fence (a torn-read
 *       window). Both are corrected by the seqlock ordering below.
 *
 * @note Reads are lock-free and validated, not strictly wait-free: a reader
 *       retries while a writer is actively rewriting the same bucket. Under the
 *       single-writer contract a lookup of an untouched bucket never retries.
 */
template <class Key, class Value, std::size_t Size, class Hash>
class wait_free_hash_map {
public:
	static_assert(Size >= 1U, "wait_free_hash_map needs at least one bucket");
	static_assert(std::is_trivially_copyable_v<Key>,
				  "Key must be trivially copyable to snapshot without tearing");
	static_assert(
		std::is_trivially_copyable_v<Value>,
		"Value must be trivially copyable to snapshot without tearing");
	static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
				  "the seqlock version counter must be lock-free");

	wait_free_hash_map() = default;

	/**
	 * @brief Insert @p key, or overwrite whatever currently occupies its
	 * bucket.
	 * @return The previous value when @p key already occupied the bucket;
	 *         @c std::nullopt when the bucket was empty or held a different
	 * key.
	 * @pre Called by at most one thread at a time (see threading contract).
	 */
	std::optional<Value> insert(const Key &key, const Value &value) {
		Bucket &bucket = buckets_[index_of(key)];

		// Relaxed is enough: the single writer owns this counter, and at rest
		// it is always even, so the low bit reliably reports "occupied".
		const std::uint64_t version =
			bucket.version.load(std::memory_order_relaxed);

		std::optional<Value> previous;
		if (version != 0U && bucket.load_key() == key)
			previous = bucket.load_value();

		// Odd version publishes "write in progress"; the release fence keeps
		// the data stores below from being reordered ahead of it, so a reader
		// that sees the new data is guaranteed to also see a changed version.
		bucket.version.store(version + 1U, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);

		bucket.store_key(key);
		bucket.store_value(value);

		// Even again: the release store publishes both fields to acquiring
		// readers.
		bucket.version.store(version + 2U, std::memory_order_release);
		return previous;
	}

	/**
	 * @brief Look up @p key.
	 * @return Its value, or @c std::nullopt if the bucket is empty or holds a
	 *         different key. Safe to call concurrently from any thread.
	 */
	[[nodiscard]] std::optional<Value> get(const Key &key) const {
		const Bucket &bucket = buckets_[index_of(key)];

		for (;;) {
			const std::uint64_t before =
				bucket.version.load(std::memory_order_acquire);
			if (before == 0U) return std::nullopt; // never written
			if (before & 1U) continue;             // writer mid-flight; retry

			const Key snap_key     = bucket.load_key();
			const Value snap_value = bucket.load_value();

			// Re-read the version; if a write straddled the snapshot the two
			// reads disagree and we retry, so the snapshot is never torn.
			std::atomic_thread_fence(std::memory_order_acquire);
			if (bucket.version.load(std::memory_order_relaxed) != before)
				continue;

			if (snap_key == key) return snap_value;
			return std::nullopt; // bucket occupied by a colliding key
		}
	}

private:
	/// One entry, isolated on its own cache line so writers do not false-share
	/// the version counters of neighbouring buckets. The key/value live as raw
	/// machine-word storage (the equivalent of Rust's @c MaybeUninit<K>):
	/// nothing is constructed up front, so @c Key / @c Value need not be
	/// default-constructible, and each word is touched only through
	/// @c std::atomic_ref so concurrent reads and writes never form a data
	/// race.
	struct alignas(std::hardware_destructive_interference_size) Bucket {
		using Word = std::uint64_t;

		template <class T>
		static constexpr std::size_t word_count =
			(sizeof(T) + sizeof(Word) - 1U) / sizeof(Word);

		std::atomic<std::uint64_t> version{
			0}; ///< 0 empty, odd writing, even live
		// mutable: std::atomic_ref binds a non-const reference, so a read
		// through get() const still needs a modifiable lvalue even though it
		// only loads.
		mutable std::array<Word, word_count<Key>> key{};
		mutable std::array<Word, word_count<Value>> value{};

		void store_key(const Key &k) { store_words(key, k); }

		void store_value(const Value &v) { store_words(value, v); }

		[[nodiscard]] Key load_key() const { return load_words<Key>(key); }

		[[nodiscard]] Value load_value() const {
			return load_words<Value>(value);
		}

	private:
		template <class T, std::size_t N>
		static void store_words(std::array<Word, N> &dst, const T &v) {
			std::array<Word, N> tmp{}; // local, zero-padded tail
			std::memcpy(tmp.data(), &v, sizeof(T));
			for (std::size_t i = 0; i < N; ++i)
				std::atomic_ref<Word>(dst[i]).store(tmp[i],
													std::memory_order_relaxed);
		}

		template <class T, std::size_t N>
		static T load_words(std::array<Word, N> &src) {
			std::array<Word, N> tmp;
			for (std::size_t i = 0; i < N; ++i)
				tmp[i] = std::atomic_ref<Word>(src[i]).load(
					std::memory_order_relaxed);
			// bit_cast from exactly sizeof(T) bytes materialises T without ever
			// default-constructing one.
			std::array<std::byte, sizeof(T)> bytes;
			std::memcpy(bytes.data(), tmp.data(), sizeof(T));
			return std::bit_cast<T>(bytes);
		}
	};

	static_assert(std::atomic_ref<typename Bucket::Word>::is_always_lock_free,
				  "payload word access must be lock-free for a lock-free read");

	[[nodiscard]] std::size_t index_of(const Key &key) const {
		return hasher_(key) % Size;
	}

	std::array<Bucket, Size> buckets_{};
	[[no_unique_address]] Hash hasher_{};
};

} // namespace concurrency::lockfree
