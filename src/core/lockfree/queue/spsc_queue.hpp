#pragma once

#include "utils/start_lifetime_as.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>

namespace core::lockfree {

/**
 * @brief Lock-free bounded queue for a single producer and a single consumer.
 *
 * @tparam T Element type. Must be nothrow-move-constructible. Trivially
 * copyable types take a @c memcpy fast path in @c try_emplace_range;
 * non-trivial types (e.g. @c std::string, move-only types) are supported via
 * per-element construction and destruction, at the cost of the batch @c memcpy.
 * @tparam N Element capacity
 *
 * <a href="https://www.youtube.com/watch?v=5uIsadq-nyk">Low Latency C++</a>
 *
 * @par Threading contract (precondition on every mutator)
 * Exactly one producer thread may call @c try_emplace / @c try_emplace_range,
 * and exactly one consumer thread may call @c try_dequeue / @c clear. The two
 * roles may be different threads or the same thread, but never two producers or
 * two consumers concurrently. The observers (@c size, @c is_empty, @c is_full)
 * return a momentary snapshot and may be called from either side.
 *
 * @invariant @c read_position_ <= @c write_position_ (the consumer never
 * overtakes the producer).
 * @invariant @c write_position_ - @c read_position_ (the live element count) is
 * in @c [0, N]: never overfull, and exact even after the raw 2^64 counters wrap
 * because it is bounded by @c N. Its two boundaries never collide — @c 0 is the
 * sole empty encoding and @c N the sole full one — so full and empty are told
 * apart by value and all @c N slots hold data with no sentinel slot reserved.
 * @invariant No enqueued element is ever overwritten or dropped: a push on a
 * full queue fails (returns @c false) instead of evicting the oldest — a
 * lossless back-pressure FIFO, not an overwriting ring.
 * @invariant A ring slot holds a live @c T exactly while its index lies in
 * @c [read_position_, write_position_); all other slots are raw storage.
 *
 * @note No allocation, non-blocking, bounded, and no operation throws. The
 * cursors are absolute, never-wrapped counts of everything ever pushed and
 * popped; the physical slot is derived only at access time as @c cursor &
 * kMask. Carrying absolute counts (rather than pre-wrapped indices) is what
 * lets the second invariant tell full from empty by value.
 */
template <class T, size_t N>
class spsc_queue {
public:
	static_assert(N >= 1U && std::has_single_bit(N),
				  "capacity N must be a power of two");

	static_assert(std::is_nothrow_move_constructible_v<T>,
				  "A nothrow-move-constructible element type cannot throw "
				  "part-way through a dequeue");

	static_assert(std::atomic<size_t>::is_always_lock_free);

	spsc_queue() = default;

	spsc_queue(const spsc_queue &) = delete;

	spsc_queue &operator=(const spsc_queue &) = delete;

	spsc_queue(spsc_queue &&) = delete;

	spsc_queue &operator=(spsc_queue &&) = delete;

	/**
	 * @brief Destroy any elements still enqueued.
	 * @pre No producer or consumer is still running (destruction is quiescent),
	 * so the final cursor values are observed without synchronisation.
	 * @post All live elements have been destroyed; the raw storage is released.
	 */
	~spsc_queue() {
		destroy_range(read_position_.load(std::memory_order_relaxed),
					  write_position_.load(std::memory_order_relaxed));
	}

	/**
	 * @brief Construct one element in place at the back of the queue.
	 * @pre Called only by the single producer thread.
	 * @post On @c true, the element is enqueued and @c size() has grown by one;
	 * on @c false (queue full) the queue is entirely unchanged and no @c T is
	 * constructed.
	 * @param args Constructor arguments forwarded to @c T.
	 * @return @c true if enqueued, @c false if the queue was full.
	 *
	 * @par Example
	 * @code{.cpp}
	 * spsc_queue<int, 1024> q;
	 * const bool ok = q.try_emplace(42);   // false when the queue is full
	 * @endcode
	 */
	template <class... Args>
	[[using gnu: hot, flatten]] [[nodiscard]]
	bool try_emplace(Args &&...args) noexcept {
		const size_t old_write = write_position_local_;
		if (!has_room(1U)) [[unlikely]]
			return false;
		assert(!is_full() && "a free slot is available");
		std::construct_at(slot(old_write), std::forward<Args>(args)...);
		publish_write(old_write + 1U);
		return true;
	}

	/**
	 * @brief Insert a whole range of elements in one all-or-nothing
	 * reservation.
	 * @details Trivially copyable @c T is bulk-copied with up to two @c memcpy
	 * calls (one per side of the wrap boundary); other types are
	 * copy-constructed element by element.
	 * @pre Called only by the single producer thread. @p r is a sized range
	 * whose value type is @c T.
	 * @post On @c true, every element of @p r is enqueued in order and @c
	 * size() has grown by @c r.size(); on @c false the queue is entirely
	 * unchanged. All @c N slots are usable, so a range of up to @c N elements
	 * can fit an empty queue.
	 * @param r Source elements to copy; read-only and left unmodified.
	 * @return @c true if the whole range was enqueued, @c false if it did not
	 * fit.
	 * @note Copies from @p r, so a move-only @c T cannot use this overload —
	 * push such elements one at a time with @c try_emplace.
	 * @par Example
	 * @code{.cpp}
	 * std::array batch{1, 2, 3, 4};
	 * const bool ok = q.try_emplace_range(batch);   // all-or-nothing
	 * @endcode
	 */
	template <std::ranges::input_range Rg>
		requires std::convertible_to<std::ranges::range_reference_t<Rg>, T>
	[[using gnu: hot, flatten]] [[nodiscard]]
	bool try_emplace_range(Rg &&r) noexcept {
		const size_t count              = std::ranges::size(r);
		const size_t old_write_position = write_position_local_;

		if (!has_room(count)) [[unlikely]]
			return false;

		if constexpr (std::is_trivially_copyable_v<T>) {
			const size_t write_index = calculate_index(old_write_position);
			const size_t first_chunk = std::min(count, N - write_index);
			T *base                  = ring_data();
			const T *src             = std::ranges::data(r);
			std::memcpy(base + write_index, src, first_chunk * sizeof(T));
			if (first_chunk < count)
				std::memcpy(base,
							src + first_chunk,
							(count - first_chunk) * sizeof(T));
		} else {
			using elem_ref = std::ranges::range_reference_t<Rg>;
			for (size_t pos = old_write_position;
				 elem_ref element : std::forward<Rg>(r)) {
				std::construct_at(slot(pos), std::forward<elem_ref>(element));
				++pos;
			}
		}
		publish_write(old_write_position + count);
		return true;
	}

	/**
	 * @brief Dequeue a batch of elements into a caller-provided buffer.
	 * @details Trivially copyable @c T is bulk-copied with up to two @c memcpy
	 * calls (one per side of the wrap boundary); other types are move-assigned
	 * element by element and the ring cell then destroyed. Either way, the
	 * elements are removed from the queue.
	 * @pre Called only by the single consumer thread.
	 * @pre @p out is a sized, contiguous output range: exactly @c out.size()
	 * slots are available to write, so at most @c out.size() elements are
	 * dequeued. Size the buffer to the maximum you want to dequeue — an empty
	 * range dequeues nothing.
	 * @pre For non-trivial @c T, the elements of @p out are already constructed
	 * and move-assignable; they are assigned into, not constructed.
	 * @post The first <code> min(out.size(), size())</code> elements have been
	 * removed in order and the read cursor advanced by the returned count; on a
	 * @c 0 return the queue is unchanged.
	 * @param[out] out Destination buffer; its leading elements are overwritten
	 * with the dequeued values.
	 * @return The number of elements dequeued, in @c [0, out.size()].
	 * @note Non-trivial @c T is @b moved out (move-assignment), not copied. The
	 * @c static_assert on nothrow-move-assignable keeps the batch loop from
	 * throwing part-way and desyncing the ring against a @c noexcept guarantee.
	 * @par Example
	 * @code{.cpp}
	 * std::array<int, 64> buf;
	 * const size_t n = q.try_dequeue_range(buf);   // dequeues up to buf.size()
	 * for (size_t i = 0; i < n; ++i) process(buf[i]);
	 * @endcode
	 */
	template <std::ranges::output_range<T> Rg>
	[[using gnu: hot, flatten]] [[nodiscard]]
	size_t try_dequeue_range(Rg &&out) noexcept {
		static_assert(std::is_nothrow_move_assignable_v<T>);

		const size_t old_read  = read_position_local_;
		const size_t available = readable();
		if (available == 0U) [[unlikely]]
			return 0;

		const size_t count = std::min(available, std::ranges::size(out));

		T *dst = std::ranges::data(out);

		if constexpr (std::is_trivially_copyable_v<T>) {
			const size_t read_index = calculate_index(old_read);
			const size_t first      = std::min(count, N - read_index);

			std::memcpy(dst, ring_data() + read_index, first * sizeof(T));

			if (first != count)
				std::memcpy(dst + first,
							ring_data(),
							(count - first) * sizeof(T));
		} else {
			for (size_t i = 0; i < count; ++i) {
				T *cell = slot(old_read + i);
				dst[i]  = std::move(*cell);
				std::destroy_at(cell);
			}
		}

		publish_read(old_read + count);

		return count;
	}

	/**
	 * @brief Whether the queue currently holds no elements.
	 * @return @c true when the read and write cursors coincide.
	 * @note Momentary snapshot; the result may be stale the instant it returns.
	 */
	[[nodiscard]] bool is_empty() const noexcept { return size() == 0U; }

	/**
	 * @brief Whether the queue currently holds @c N elements.
	 * @return @c true when no free slot remains.
	 * @note Momentary snapshot; the result may be stale the instant it returns.
	 */
	[[nodiscard]]
	bool is_full() const noexcept {
		return size() == N;
	}

	/**
	 * @brief Number of elements currently enqueued.
	 * @return A count in @c [0, N].
	 * @note Momentary snapshot; the result may be stale the instant it returns.
	 */
	[[nodiscard]] size_t size() const noexcept {
		const size_t write_position =
			write_position_.load(std::memory_order_acquire);
		const size_t read_position =
			read_position_.load(std::memory_order_acquire);
		return write_position - read_position;
	}

	/**
	 * @brief dequeue the front element, returning it by value.
	 * @pre Called only by the single consumer thread.
	 * @post On engagement, the front element has been removed and @c size() has
	 * shrunk by one; on @c std::nullopt (empty) the queue is unchanged.
	 * @return The dequeued element, or @c std::nullopt if the queue was empty.
	 * @note Constructs a @c std::optional on the hot path; prefer @c
	 * try_dequeue(T&) in latency-critical loops.
	 * @par Example
	 * @code{.cpp}
	 * while (std::optional<int> v = q.try_dequeue()) process(*v);
	 * @endcode
	 */
	[[using gnu: hot, flatten]] [[nodiscard]]
	std::optional<T> try_dequeue() noexcept {
		const size_t old_read = read_position_local_;
		if (readable() == 0U) return std::nullopt;

		assert(!is_empty() && "an element is available");
		T *cell = slot(old_read);
		std::optional<T> ret(std::move(*cell));
		std::destroy_at(cell);
		publish_read(old_read + 1U);
		return ret;
	}

	/**
	 * @brief dequeue the front element into a caller-provided slot.
	 * @details Lower-latency alternative to the @c std::optional returning
	 * @c try_dequeue(): it avoids constructing and materialising an @c
	 * optional, so the hot consumer path is a plain move into @p out. Prefer it
	 * in latency-critical loops and reserve the @c optional overload for
	 * ergonomic, non-hot call sites.
	 * @pre Called only by the single consumer thread.
	 * @post On @c true, @p out holds the dequeued element and @c size() has
	 * shrunk by one; on @c false (empty) both @p out and the queue are
	 * unchanged.
	 * @param[out] out Assigned the dequeued element on success; untouched on
	 * failure.
	 * @return @c true if an element was dequeued, @c false if the queue was
	 * empty.
	 * @par Example
	 * @code{.cpp}
	 * for (int v; q.try_dequeue(v);) process(v);   // hot consumer loop
	 * @endcode
	 */
	[[using gnu: hot, flatten]] [[nodiscard]]
	bool try_dequeue(T &out) noexcept {
		static_assert(std::is_nothrow_move_assignable_v<T>);

		const size_t old_read = read_position_local_;
		if (readable() == 0U) return false;

		assert(!is_empty() && "an element is available");
		T *cell = slot(old_read);
		out     = std::move(*cell);
		std::destroy_at(cell);
		publish_read(old_read + 1U);

		return true;
	}

	/**
	 * @brief Discard every queued element.
	 * @pre Called only by the single consumer thread (it advances the read
	 * cursor); concurrent use with @c try_dequeue breaks the single-consumer
	 * contract.
	 * @post The queue is empty, every previously pending element has been
	 * destroyed, and the read cursor has caught up to the write cursor.
	 */
	void clear() noexcept {
		const size_t write_end =
			write_position_.load(std::memory_order_acquire);
		destroy_range(read_position_local_, write_end);
		write_position_cache_ = write_end;
		publish_read(write_end);
	}

	/**
	 * @brief Apply @p fn to every queued element in place, then empty the
	 * queue.
	 * @details Equivalent to @c consume_up_to with no limit: drains all
	 * elements visible at the moment the write cursor is observed. Each element
	 * is passed to @p fn by reference and destroyed immediately after.
	 * @pre Called only by the single consumer thread.
	 * @pre @p fn is nothrow-invocable as @c void(T&) (enforced) and must
	 * neither throw nor allocate — it runs inside this @c noexcept method
	 * before each element is destroyed and before the read cursor is published,
	 * so a throw would @c std::terminate. For throwing/allocating consumers,
	 * use
	 * @c try_dequeue_range and process the buffer afterwards.
	 * @post The queue is empty (the read cursor has caught up to the observed
	 * write cursor) and every drained element was passed to @p fn and
	 * destroyed.
	 * @param fn Nothrow callable invoked once per element as @c fn(T&).
	 * @return The number of elements consumed.
	 * @example
	 * const size_t drained = q.consume_all([&](int &v) noexcept { sink += v;
	 * });
	 */
	template <class Func>
		requires std::is_nothrow_invocable_r_v<void, Func, T &>
	[[using gnu: hot, flatten]] [[nodiscard]]
	size_t consume_all(Func &&fn) noexcept {
		const size_t old_read = read_position_local_;
		const size_t count    = readable();

		size_t pos = old_read;
		for (size_t i = 0; i < count; ++i, ++pos) {
			T *cell = slot(pos);
			std::invoke(std::forward<Func>(fn), *cell);
			std::destroy_at(cell);
		}
		publish_read(pos);

		return count;
	}

	/**
	 * @brief Apply @p fn to up to @p limit queued elements in place, then
	 * remove them.
	 * @details Each element is passed to @p fn by reference while it still
	 * lives in the ring and is destroyed immediately after; no copy-out to a
	 * buffer. Lower overhead than @c try_dequeue_range for non-trivial @c T,
	 * but see the callback precondition.
	 * @pre Called only by the single consumer thread.
	 * @pre @p fn is nothrow-invocable as @c void(T&) (enforced by the
	 * constraint) and must neither throw nor allocate. It runs inside this
	 * @c noexcept method, in between reading and destroying each element and
	 * before the read cursor is published; a throw would @c std::terminate. For
	 * throwing or allocating per-element work, dequeue with @c
	 * try_dequeue_range and process the buffer afterwards, off the hot path.
	 * @post The first @c min(limit, size()) elements have been passed to @p fn
	 * in order, destroyed, and removed; the read cursor advanced by the
	 * returned count.
	 * @param limit Maximum number of elements to consume.
	 * @param fn Nothrow callable invoked once per element as @c fn(T&).
	 * @return The number of elements consumed, in @c [0, limit].
	 * @par Example
	 * @code{.cpp}
	 * const size_t n = q.consume_up_to(64, [&](int &v) noexcept { sink += v;
	 * });
	 * @endcode
	 */
	template <class Func>
		requires std::is_nothrow_invocable_r_v<void, Func, T &>
	[[nodiscard]]
	size_t consume_up_to(size_t limit, Func &&fn) noexcept {
		const size_t old_read  = read_position_local_;
		const size_t available = readable();
		if (available == 0U) return 0;

		const size_t count = std::min(limit, available);
		size_t pos         = old_read;
		for (size_t i = 0; i < count; ++i, ++pos) {
			T *cell = slot(pos);
			std::invoke(std::forward<Func>(fn), *cell);
			std::destroy_at(cell);
		}
		publish_read(pos);

		return count;
	}

private:
	[[nodiscard]] static size_t calculate_index(size_t old_read) {
		/// Bitmask that maps a monotonic cursor to a physical ring slot.
		constexpr size_t mask = N - 1U;

		return old_read & mask;
	}

	/**
	 * @brief Producer-side space check with a lazy read-cursor reload.
	 * @details Measures free space from the producer's own write cursor
	 * (@c write_position_local_) using its cached copy of the read cursor
	 * first; only when that stale view reports too few free slots does it pay
	 * for one acquire load of the shared @c read_position_ and re-check. This
	 * is the single point where the producer synchronises with the consumer.
	 * @pre Called only by the single producer thread.
	 * @param count How many elements the caller wants to enqueue.
	 * @return @c true if at least @p count slots are free.
	 */
	[[nodiscard]] bool has_room(size_t count) noexcept {
		const auto free_slots = [&] {
			return N - (write_position_local_ - read_position_cache_);
		};
		if (free_slots() < count) [[unlikely]] {
			read_position_cache_ =
				read_position_.load(std::memory_order_acquire);
			return free_slots() >= count;
		}
		return true;
	}

	/**
	 * @brief Publish an advanced write cursor to the consumer.
	 * @details Updates the producer's private copy and then releases the new
	 * value through the shared @c write_position_, so every element written
	 * before this call happens-before the consumer's matching acquire load.
	 * @pre Called only by the single producer thread.
	 */
	void publish_write(size_t new_write) noexcept {
		write_position_local_ = new_write;
		write_position_.store(new_write, std::memory_order_release);
	}

	/**
	 * @brief Consumer-side element count with a lazy write-cursor reload.
	 * @details Measures available elements from the consumer's own read cursor
	 * (@c read_position_local_) using its cached copy of the write cursor
	 * first; only when that stale view reports the queue empty does it pay for
	 * one acquire load of the shared @c write_position_. This is the single
	 * point where the consumer synchronises with the producer.
	 * @pre Called only by the single consumer thread.
	 * @return The number of elements available to read; @c 0 only when the
	 * queue is genuinely empty.
	 */
	[[nodiscard]] size_t readable() noexcept {
		if (read_position_local_ == write_position_cache_)
			write_position_cache_ =
				write_position_.load(std::memory_order_acquire);
		return write_position_cache_ - read_position_local_;
	}

	/**
	 * @brief Publish an advanced read cursor to the producer.
	 * @details Updates the consumer's private copy and then releases the new
	 * value through the shared @c read_position_, freeing the drained slots for
	 * the producer to reuse.
	 * @pre Called only by the single consumer thread.
	 */
	void publish_read(size_t new_read) noexcept {
		read_position_local_ = new_read;
		read_position_.store(new_read, std::memory_order_release);
	}

	/**
	 * @brief Destroy the live elements in the cursor range @c [from, to).
	 * @details Re-masks each cursor to its physical slot, so a range that wraps
	 * the ring is handled correctly. A no-op for trivially destructible @c T.
	 * @pre @p from and @p to bound a range of currently-live elements and no
	 * other thread is concurrently touching them.
	 */
	void destroy_range(size_t from, size_t to) noexcept {
		if constexpr (!std::is_trivially_destructible_v<T>)
			for (size_t pos = from; pos != to; ++pos)
				std::destroy_at(slot(pos));
	}

	/**
	 * @brief Pointer to the ring cell for cursor @p pos.
	 * @details The cell holds a live @c T only when
	 * @p pos lies in @c [read_position_, write_position_); otherwise it is raw
	 * storage awaiting @c std::construct_at.
	 */
	[[nodiscard]] T *slot(size_t pos) noexcept {
		auto *addr = storage_.data() + calculate_index(pos) * sizeof(T);
		return std::launder(reinterpret_cast<T *>(addr));
	}

	/**
	 * @brief Base of the ring viewed as a contiguous @c T array, for the
	 * trivially-copyable @c memcpy fast path.
	 * @details Uses @c std::start_lifetime_as_array (C++23) where the toolchain
	 * provides it, to begin the element lifetimes without @c reinterpret_cast.
	 * Only ever called in the @c is_trivially_copyable_v<T> branch.
	 */
	[[nodiscard]] T *ring_data() noexcept {
#ifdef __cpp_lib_start_lifetime_as
		return std::start_lifetime_as_array<T>(storage_.data(), N);
#else
		return utils::start_lifetime_as_array<T>(storage_.data(), N);
#endif
	}

	/// Raw, @c T-aligned backing store; element lifetimes are managed
	/// explicitly.
	alignas(T) std::array<std::byte, sizeof(T) * N> storage_;
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winterference-size"
#endif
	/// Consumer's cache line: the shared read cursor it publishes (read by the
	/// producer) via @c publish_read, its own private authoritative copy it
	/// reads and increments in a register without an atomic load, and its
	/// last-seen copy of the producer's write cursor so @c readable only
	/// reloads the shared @c write_position_ when the queue looks empty.
	alignas(std::hardware_destructive_interference_size) std::atomic_size_t
		read_position_           = 0;
	size_t read_position_local_  = 0;
	size_t write_position_cache_ = 0;

	/// Producer's cache line: mirror image of the above, driven by @c has_room
	/// and @c publish_write on the push paths.
	alignas(std::hardware_destructive_interference_size) std::atomic_size_t
		write_position_ = 0;
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
	size_t write_position_local_ = 0;
	size_t read_position_cache_  = 0;
};

} // namespace core::lockfree
