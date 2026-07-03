#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstring>
#include <optional>
#include <ranges>
#include <type_traits>

/**
 * Lock-free queue for single producer single consumer
 * @tparam T
 * @tparam N
 * @link https://www.youtube.com/watch?v=5uIsadq-nyk
 */
template<class T, size_t N>
    requires std::is_standard_layout_v<T>
class SPSCQueue {
public:
    SPSCQueue() = default;

    SPSCQueue(const SPSCQueue &) = delete;

    SPSCQueue &operator=(const SPSCQueue &) = delete;

    SPSCQueue(SPSCQueue &&) = default;

    SPSCQueue &operator=(SPSCQueue &&) = default;

    ~SPSCQueue() = default;

    template<class... Args>
    bool try_emplace(Args &&... args) {
        const auto old_write = write_position.load(std::memory_order_relaxed);
        const auto new_write = get_position_after(old_write);
        if (new_write == write_position.load(std::memory_order_acquire)) {
            return false;
        }
        ring_buffer[old_write] = T(std::forward<Args>(args)...);
        write_position.store(new_write, std::memory_order_release);
        return true;
    }


    /**
     * @brief Bulk-emplace a contiguous range of elements in a single reservation.
     * @details Copies @p r into the ring buffer, splitting into two @c memcpy
     * calls when the range straddles the wrap boundary. The reservation is
     * all-or-nothing: if fewer than @c r.size() free slots are available the
     * queue is left untouched and @c false is returned. One slot is always kept
     * free to disambiguate the full and empty states, so the effective capacity
     * is @c N.
     * @param r Source elements to copy; read-only.
     * @return @c true if the whole range was enqueued, @c false if it did not fit.
     * @note Uses @c std::memcpy, hence @c T must be trivially copyable.
     */
    template<std::ranges::input_range Rg>
        requires std::same_as<std::ranges::range_value_t<Rg>, T>
    bool try_emplace_range(Rg &&r) {
        const size_t count = std::ranges::size(r);
        const size_t old_write_position =
                write_position.load(std::memory_order_relaxed);
        const size_t old_read_position =
                read_position.load(std::memory_order_acquire);

        const size_t used =
                (max_size + old_write_position - old_read_position) % max_size;
        if (const size_t free_slots = max_size - 1U - used;
            count > free_slots) { return false; }

        const size_t first_chunk =
                std::min(count, max_size - old_write_position);
        std::memcpy(ring_buffer.data() + old_write_position,
                    std::ranges::data(r),
                    first_chunk * sizeof(T));
        if (first_chunk < count) {
            std::memcpy(ring_buffer.data(), std::ranges::data(r) + first_chunk,
                        (count - first_chunk) * sizeof(T));
        }
        write_position.store((old_write_position + count) % max_size,
                             std::memory_order_release);
        return true;
    }

    /**
     * @brief Returns true if the queue currently holds no elements.
     * @return @c true when the read and write cursors coincide.
     */
    [[nodiscard]] bool is_empty() const noexcept {
        std::atomic_thread_fence(std::memory_order_acquire);
        return read_position.load(std::memory_order_acquire) ==
               write_position.load(std::memory_order_acquire);
    }

    /**
     * @brief Number of elements currently enqueued.
     * @return A count in the range @c [0, N]; likewise a momentary observation.
     */
    [[nodiscard]] size_t size() const noexcept {
        const size_t old_write_position =
                write_position.load(std::memory_order_acquire);
        const size_t old_read_position =
                read_position.load(std::memory_order_acquire);
        return (max_size + old_write_position - old_read_position) % max_size;
    }

    std::optional<T> try_pop() {
        const auto old_read_position = read_position.load(
            std::memory_order_relaxed);
        if (old_read_position == write_position.load(
                std::memory_order_acquire)) {
            return std::nullopt;
        }
        // TODO: Is std::move here necessary if T is non-trivial?
        const auto ret = std::make_optional(
            std::move(ring_buffer[old_read_position]));
        read_position.store(get_position_after(old_read_position),
                            std::memory_order_release);
        return ret;
    }

    /**
     * @brief Discard every queued element.
     * @details Advances the read cursor up to the write cursor, so all pending
     * elements are dropped. Intended to be called from the consumer side only
     * (it moves @c read_position); invoking it concurrently with @c try_pop
     * from another thread breaks the single-consumer contract.
     */
    void clear() noexcept {
        read_position.store(write_position.load(std::memory_order_acquire),
                            std::memory_order_release);
    }

private:
    static constexpr size_t max_size = N + 1;

    static constexpr size_t get_position_after(size_t pos) noexcept {
        return ++pos == max_size ? 0 : pos;
    }

    std::array<T, max_size> ring_buffer;
    alignas(std::hardware_destructive_interference_size) std::atomic<size_t>
            read_position = 0, write_position = 0;
};
