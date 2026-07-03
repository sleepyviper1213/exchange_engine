#pragma once
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <optional>
#include <span>
#include <vector>

/**
 * @brief Lock-free single-producer / single-consumer byte queue.
 *
 * A fixed-capacity ring buffer carrying length-prefixed messages. Exactly one
 * thread may call try_push() and one (other) thread try_pop() concurrently; any
 * other sharing is undefined behaviour.
 *
 * Each message is stored as a 4-byte little-endian length followed by its
 * payload. The read/write cursors are free-running 64-bit byte counters, so the
 * occupied span is simply @c write-read and the full/empty states are never
 * ambiguous (no sacrificial slot needed). The read and write cursors live on
 * separate cache lines to avoid false sharing between the two threads.
 */
class FastQueue {
public:
    /**
     * @brief Construct a queue whose capacity is rounded up to a power of two.
     * @param capacity_bytes Minimum usable capacity in bytes.
     */
    explicit FastQueue(std::size_t capacity_bytes = 1u << 20)
        : capacity_(round_up_pow2(capacity_bytes)),
          mask_(capacity_ - 1),
          buffer_(capacity_) {}

    FastQueue(const FastQueue&) = delete;
    FastQueue& operator=(const FastQueue&) = delete;
    FastQueue(FastQueue&&) = delete;
    FastQueue& operator=(FastQueue&&) = delete;
    ~FastQueue() = default;

    /**
     * @brief Enqueue one message (producer thread only).
     * @param data Payload bytes to copy into the queue.
     * @return true on success; false if the queue lacks room for the message.
     */
    [[nodiscard]] bool try_push(std::span<const std::byte> data) {
        const auto size = static_cast<std::uint32_t>(data.size());
        const std::size_t need = kHeader + size;

        const std::uint64_t write = write_.load(std::memory_order_relaxed);
        const std::uint64_t read = read_.load(std::memory_order_acquire);
        if (capacity_ - static_cast<std::size_t>(write - read) < need) {
            return false; // not enough free space
        }

        std::byte header[kHeader];
        std::memcpy(header, &size, kHeader);
        copy_in(write, std::span<const std::byte>(header, kHeader));
        copy_in(write + kHeader, data);

        // Publish the payload before advancing the cursor the consumer reads.
        write_.store(write + need, std::memory_order_release);
        return true;
    }

    /**
     * @brief Dequeue one message into @p out (consumer thread only).
     * @param out Destination span; must be at least as large as the message.
     * @return The number of payload bytes copied, or std::nullopt if empty.
     */
    [[nodiscard]] std::optional<std::size_t> try_pop(std::span<std::byte> out) {
        const std::uint64_t read = read_.load(std::memory_order_relaxed);
        const std::uint64_t write = write_.load(std::memory_order_acquire);
        if (read == write) return std::nullopt; // empty

        std::byte header[kHeader];
        copy_out(read, std::span<std::byte>(header, kHeader));
        std::uint32_t size;
        std::memcpy(&size, header, kHeader);

        assert(write - read >= kHeader + size && "queue corrupted");
        assert(size <= out.size() && "destination span too small");

        copy_out(read + kHeader, out.first(size));
        // Release the slots only after the payload has been fully read out.
        read_.store(read + kHeader + size, std::memory_order_release);
        return size;
    }

    /// @brief Usable capacity in bytes (a power of two).
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    /// @brief True if no message is currently queued.
    [[nodiscard]] bool empty() const noexcept {
        return read_.load(std::memory_order_acquire) ==
               write_.load(std::memory_order_acquire);
    }

private:
    static constexpr std::size_t kHeader = sizeof(std::uint32_t);

    /// @brief Smallest power of two that is >= @p v (and at least 1).
    static std::size_t round_up_pow2(std::size_t v) {
        std::size_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    /// @brief Copy @p src into the ring at byte position @p pos, wrapping once.
    void copy_in(std::uint64_t pos, std::span<const std::byte> src) {
        const std::size_t offset = static_cast<std::size_t>(pos) & mask_;
        const std::size_t first = std::min(src.size(), capacity_ - offset);
        std::memcpy(buffer_.data() + offset, src.data(), first);
        if (first < src.size()) {
            std::memcpy(buffer_.data(), src.data() + first, src.size() - first);
        }
    }

    /// @brief Copy @p dst.size() bytes out of the ring at @p pos, wrapping once.
    void copy_out(std::uint64_t pos, std::span<std::byte> dst) const {
        const std::size_t offset = static_cast<std::size_t>(pos) & mask_;
        const std::size_t first = std::min(dst.size(), capacity_ - offset);
        std::memcpy(dst.data(), buffer_.data() + offset, first);
        if (first < dst.size()) {
            std::memcpy(dst.data() + first, buffer_.data(), dst.size() - first);
        }
    }

    // Immutable after construction, so safe to read from both threads.
    std::size_t capacity_;
    std::size_t mask_;
    std::vector<std::byte> buffer_;

    
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Winterference-size"
#endif
    alignas(std::hardware_destructive_interference_size)
    std::atomic_uint64_t read_{0}; ///< bytes consumed (owned by the consumer)
    alignas(std::hardware_destructive_interference_size)
    std::atomic_uint64_t write_{0}; ///< bytes produced (owned by the producer)
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic pop
#endif
};
