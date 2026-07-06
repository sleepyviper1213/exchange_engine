#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>

/**
 * @brief Lock-free bounded queue for a single producer and a single consumer.
 *
 * @tparam T Element type. Must be nothrow-move-constructible. Trivially copyable
 * types take a @c memcpy fast path in @c try_emplace_range; non-trivial types
 * (e.g. @c std::string, move-only types) are supported via per-element
 * construction and destruction, at the cost of the batch @c memcpy.
 * @tparam N Element capacity; must be a power of two so the physical slot is
 * selected with a bitmask (@c cursor & (N-1)) rather than a modulo.
 *
 * @link https://www.youtube.com/watch?v=5uIsadq-nyk
 *
 * @par Threading contract (precondition on every mutator)
 * Exactly one producer thread may call @c try_emplace / @c try_emplace_range,
 * and exactly one consumer thread may call @c try_pop / @c clear. The two roles
 * may be different threads or the same thread, but never two producers or two
 * consumers concurrently. The observers (@c size, @c is_empty, @c is_full)
 * return a momentary snapshot and may be called from either side.
 *
 * @invariant @c read_position_ <= @c write_position_ (the consumer never overtakes
 * the producer).
 * @invariant @c write_position_ - @c read_position_ is in @c [0, N] (never
 * overfull; the difference is exact even across a 2^64 wrap because it is bounded
 * by @c N).
 * @invariant A ring slot holds a live @c T exactly while its index lies in
 * @c [read_position_, write_position_); all other slots are raw storage.
 *
 * @note No allocation, non-blocking, bounded, and no operation throws. Cursors
 * are monotonic counters that are never wrapped; the physical slot is
 * @c cursor & kMask. Because the counters carry absolute counts, empty
 * (@c write==read) and full (@c write-read==N) are distinguishable by value, so
 * no sentinel slot is reserved and all @c N slots hold data. The 64-bit counters
 * would take centuries to overflow at any realistic rate.
 */
template<class T, size_t N>
    requires std::move_constructible<T>
class spsc_queue {
    static_assert(N >= 1U && std::has_single_bit(N),
                  "SPSCQueue capacity N must be a power of two");
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "SPSCQueue requires a nothrow-move-constructible element type "
                  "so the pop path cannot throw part-way through a dequeue");

public:
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
     */
    template<class... Args>
    [[using gnu: hot, flatten]] [[nodiscard]]
    bool try_emplace(Args &&... args) noexcept {
        const size_t old_write = write_position_local_;
        if (old_write - read_position_cache_ == N) {
            read_position_cache_ = read_position_.load(
                std::memory_order_acquire);
            if (old_write - read_position_cache_ == N) {
                return false;
            }
        }
        assert(old_write - read_position_cache_ < N && "free slot reserved");
        std::construct_at(slot(old_write), std::forward<Args>(args)...);
        write_position_local_ = old_write + 1U;
        write_position_.store(write_position_local_, std::memory_order_release);
        return true;
    }


    /**
     * @brief Insert a whole range of elements in one all-or-nothing reservation.
     * @details Trivially copyable @c T is bulk-copied with up to two @c memcpy
     * calls (one per side of the wrap boundary); other types are copy-constructed
     * element by element.
     * @pre Called only by the single producer thread. @p r is a sized range whose
     * value type is @c T.
     * @post On @c true, every element of @p r is enqueued in order and @c size()
     * has grown by @c r.size(); on @c false the queue is entirely unchanged. All
     * @c N slots are usable, so a range of up to @c N elements can fit an empty
     * queue.
     * @param r Source elements to copy; read-only and left unmodified.
     * @return @c true if the whole range was enqueued, @c false if it did not fit.
     * @note Copies from @p r, so a move-only @c T cannot use this overload — push
     * such elements one at a time with @c try_emplace.
     */
    template<std::ranges::input_range Rg>
        requires std::same_as<std::ranges::range_value_t<Rg>, T>
    [[using gnu: hot, flatten]] [[nodiscard]]
    bool try_emplace_range(Rg &&r) noexcept {
        const size_t count = std::ranges::size(r);
        const size_t old_write_position = write_position_local_;

        const auto not_enough_space = [&] {
            const auto free_slots =
                    N - (old_write_position - read_position_cache_);
            return count > free_slots;
        };

        if (not_enough_space()) {
            read_position_cache_ = read_position_.load(
                std::memory_order_acquire);
            if (not_enough_space()) { return false; }
        }

        if constexpr (std::is_trivially_copyable_v<T>) {
            const size_t write_index = old_write_position & kMask;
            const size_t first_chunk = std::min(count, N - write_index);
            T *const base = ring_data();
            std::memcpy(base + write_index, std::ranges::data(r),
                        first_chunk * sizeof(T));
            if (first_chunk < count) {
                std::memcpy(base, std::ranges::data(r) + first_chunk,
                            (count - first_chunk) * sizeof(T));
            }
        } else {
            size_t pos = old_write_position;
            for (const T &element: r) {
                std::construct_at(slot(pos), element);
                ++pos;
            }
        }
        write_position_local_ = old_write_position + count;
        write_position_.store(write_position_local_, std::memory_order_release);
        return true;
    }

    /**
     * @brief Whether the queue currently holds no elements.
     * @return @c true when the read and write cursors coincide.
     * @note Momentary snapshot; the result may be stale the instant it returns.
     */
    [[nodiscard]] bool is_empty() const noexcept {
        return read_position_.load(std::memory_order_acquire) ==
               write_position_.load(std::memory_order_acquire);
    }

    /**
     * @brief Whether the queue currently holds @c N elements.
     * @return @c true when no free slot remains.
     * @note Momentary snapshot; the result may be stale the instant it returns.
     */
    [[nodiscard]]
    bool is_full() const noexcept {
        return write_position_.load(std::memory_order_relaxed) -
               read_position_.load(std::memory_order_acquire) == N;
    }

    /**
     * @brief Number of elements currently enqueued.
     * @return A count in @c [0, N].
     * @note Momentary snapshot; the result may be stale the instant it returns.
     */
    [[nodiscard]] size_t size() const noexcept {
        const size_t old_write_position =
                write_position_.load(std::memory_order_acquire);
        const size_t old_read_position =
                read_position_.load(std::memory_order_acquire);
        return old_write_position - old_read_position;
    }

    /**
     * @brief Pop the front element, returning it by value.
     * @pre Called only by the single consumer thread.
     * @post On engagement, the front element has been removed and @c size() has
     * shrunk by one; on @c std::nullopt (empty) the queue is unchanged.
     * @return The dequeued element, or @c std::nullopt if the queue was empty.
     * @note Constructs a @c std::optional on the hot path; prefer @c try_pop(T&)
     * in latency-critical loops.
     */
    [[using gnu: hot, flatten]] [[nodiscard]]
    std::optional<T> try_pop() noexcept {
        const size_t old_read = read_position_local_;
        if (old_read == write_position_cache_) {
            write_position_cache_ = write_position_.load(
                std::memory_order_acquire);
            if (old_read == write_position_cache_) {
                return std::nullopt;
            }
        }
        assert(old_read != write_position_cache_ && "element available");
        T *const cell = slot(old_read);
        std::optional<T> ret(std::move(*cell));
        std::destroy_at(cell);
        read_position_local_ = old_read + 1U;
        read_position_.store(read_position_local_, std::memory_order_release);
        return ret;
    }

    /**
     * @brief Pop the front element into a caller-provided slot.
     * @details Lower-latency alternative to the @c std::optional returning
     * @c try_pop(): it avoids constructing and materialising an @c optional, so
     * the hot consumer path is a plain move into @p out. Prefer it in
     * latency-critical loops and reserve the @c optional overload for ergonomic,
     * non-hot call sites.
     * @pre Called only by the single consumer thread.
     * @post On @c true, @p out holds the dequeued element and @c size() has shrunk
     * by one; on @c false (empty) both @p out and the queue are unchanged.
     * @param[out] out Assigned the popped element on success; untouched on failure.
     * @return @c true if an element was dequeued, @c false if the queue was empty.
     */
    [[using gnu: hot, flatten]] [[nodiscard]]
    bool try_pop(T &out) noexcept {
        const size_t old_read = read_position_local_;
        if (old_read == write_position_cache_) {
            write_position_cache_ = write_position_.load(
                std::memory_order_acquire);
            if (old_read == write_position_cache_) {
                return false;
            }
        }
        assert(old_read != write_position_cache_ && "element available");
        T *const cell = slot(old_read);
        out = std::move(*cell);
        std::destroy_at(cell);
        read_position_local_ = old_read + 1U;
        read_position_.store(read_position_local_, std::memory_order_release);
        return true;
    }

    /**
     * @brief Discard every queued element.
     * @pre Called only by the single consumer thread (it advances the read
     * cursor); concurrent use with @c try_pop breaks the single-consumer contract.
     * @post The queue is empty, every previously pending element has been
     * destroyed, and the read cursor has caught up to the write cursor.
     */
    void clear() noexcept {
        const size_t write_end = write_position_.
                load(std::memory_order_acquire);
        destroy_range(read_position_local_, write_end);
        read_position_local_ = write_end;
        write_position_cache_ = write_end;
        read_position_.store(write_end, std::memory_order_release);
    }

private:
    /// Bitmask that maps a monotonic cursor to a physical ring slot.
    static constexpr size_t kMask = N - 1U;

    /**
     * @brief Destroy the live elements in the cursor range @c [from, to).
     * @details Re-masks each cursor to its physical slot, so a range that wraps
     * the ring is handled correctly. A no-op for trivially destructible @c T.
     * @pre @p from and @p to bound a range of currently-live elements and no
     * other thread is concurrently touching them.
     */
    void destroy_range(size_t from, size_t to) noexcept {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            for (size_t pos = from; pos != to; ++pos) {
                std::destroy_at(slot(pos));
            }
        }
    }

    /**
     * @brief Pointer to the ring cell for cursor @p pos.
     * @details Addresses the raw storage via a @c static_cast through @c void*
     * (never a @c reinterpret_cast). The cell holds a live @c T only when
     * @p pos lies in @c [read_position_, write_position_); otherwise it is raw
     * storage awaiting @c std::construct_at.
     */
    [[nodiscard]] T *slot(size_t pos) noexcept {
        return static_cast<T *>(
            static_cast<void *>(storage_.data() + (pos & kMask) * sizeof(T)));
    }

    /**
     * @brief Base of the ring viewed as a contiguous @c T array, for the
     * trivially-copyable @c memcpy fast path.
     * @details Uses @c std::start_lifetime_as_array (C++23) where the toolchain
     * provides it, to begin the element lifetimes without @c reinterpret_cast;
     * otherwise falls back to a @c static_cast through @c void*. Only ever called
     * in the @c is_trivially_copyable_v<T> branch.
     */
    [[nodiscard]] T *ring_data() noexcept {
#ifdef __cpp_lib_start_lifetime_as
        return std::start_lifetime_as_array<T>(storage_.data(), N);
#else
        return static_cast<T *>(static_cast<void *>(storage_.data()));
#endif
    }

    /// Raw, @c T-aligned backing store; element lifetimes are managed explicitly.
    alignas(T) std::array<std::byte, sizeof(T) * N> storage_;
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Winterference-size"
#endif
    /// Consumer's cache line: the shared read cursor it publishes (read by the
    /// producer), its own private authoritative copy it reads and increments in
    /// a register without an atomic load, and its last-seen copy of the
    /// producer's write cursor so the hot @c try_pop path only reloads the
    /// shared @c write_position_ when the queue looks empty.
    alignas(std::hardware_destructive_interference_size) std::atomic_size_t
    read_position_ = 0;
    size_t read_position_local_ = 0;
    size_t write_position_cache_ = 0;

    /// Producer's cache line: mirror image of the above for the @c try_emplace
    /// and @c try_emplace_range push paths.
    alignas(std::hardware_destructive_interference_size) std::atomic_size_t
    write_position_ = 0;
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic pop
#endif
    size_t write_position_local_ = 0;
    size_t read_position_cache_ = 0;
};
