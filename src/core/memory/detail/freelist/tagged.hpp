#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>

/**
 * @brief Intrusive, lock-free (Treiber) stack of free blocks, made ABA-safe
 * with a versioned (tagged) head pointer.
 *
 * Each freed block is at least @c kMinBlockBytes, so the intrusive @c next link
 * is threaded through the block's own storage rather than allocating node
 * objects. @c push / @c pop are lock-free via a single CAS on the head.
 *
 * @par Why this is ABA-safe where a plain Treiber stack is not
 * A freelist recycles blocks by design, so the same address legitimately
 * reappears at the head — the exact condition a plain Treiber pop cannot tell
 * apart from "nothing changed", so it hands a block out twice. Two properties
 * close that gap here, and both are needed:
 *   1. @b Stable @b addresses. The arena backing these blocks never returns
 *      memory to the OS, so a block's storage is always mapped. The transient
 *      read of @c head->next during a pop therefore cannot fault, even when
 *      another thread has already popped that block and its owner is writing
 *      over it — the value read is simply stale and discarded.
 *   2. @b A @b version @b tag. The head packs a 16-bit counter alongside the
 *      pointer, bumped on every successful push and pop. A block popped and
 *      pushed back returns with a different tag, so a stalled popper's CAS sees
 *      the tag move and fails instead of succeeding against a stale @c next.
 * The stable-address half was already true of this arena; the tag is what the
 * previous plain-Treiber version was missing.
 *
 * @note The pointer and tag share one 64-bit word: user-space x86-64 addresses
 * occupy at most 48 bits (bit 47 clear, top 16 bits zero), so those top 16 bits
 * carry the tag and the whole head updates in one always-lock-free 64-bit CAS.
 * A debug assert guards the 48-bit assumption.
 *
 * @par Threading contract
 * @c push and @c pop are safe from any number of threads concurrently.
 */
namespace memory::tagged {

class free_list {
	struct Node {
		std::atomic<Node *> next{nullptr};
	};

public:
	/// Smallest block this list can thread a node through.
	static constexpr std::size_t kMinBlockBytes = sizeof(Node);

	free_list() noexcept                    = default;
	free_list(const free_list &)            = delete;
	free_list &operator=(const free_list &) = delete;

	/// @brief Return a block to the list (block must be >= @c kMinBlockBytes).
	void push(void *block) noexcept {
		// Begin the node's lifetime in the block's raw storage (Node is
		// trivially destructible, so repeated reuse across push/pop needs no
		// destroy).
		auto *node             = ::new (block) Node();
		std::uint64_t expected = head_.load(std::memory_order_relaxed);
		std::uint64_t desired  = 0;
		do {
			node->next.store(ptr(expected), std::memory_order_relaxed);
			desired = pack(node, next_tag(expected));
		} while (!head_.compare_exchange_weak(expected,
											  desired,
											  std::memory_order_release,
											  std::memory_order_relaxed));
	}

	/// @brief Pop a block for reuse, or @c nullptr if the list is empty.
	[[nodiscard]] void *pop() noexcept {
		std::uint64_t expected = head_.load(std::memory_order_acquire);
		std::uint64_t desired  = 0;
		Node *node             = nullptr;
		do {
			node = ptr(expected);
			if (node == nullptr) return nullptr; // empty
			// Stable addresses make this read non-faulting even if `node` was
			// concurrently popped and is being overwritten; a stale value is
			// caught by the tag and the CAS retries.
			Node *next = node->next.load(std::memory_order_acquire);
			desired    = pack(next, next_tag(expected));
		} while (!head_.compare_exchange_weak(expected,
											  desired,
											  std::memory_order_acquire,
											  std::memory_order_relaxed));
		return node;
	}

private:
	static constexpr int kTagShift = 48;
	static constexpr std::uint64_t kPtrMask =
		(std::uint64_t{1} << kTagShift) - 1U;

	static Node *ptr(std::uint64_t head) noexcept {
		return reinterpret_cast<Node *>(head & kPtrMask);
	}

	static std::uint16_t tag(std::uint64_t head) noexcept {
		return static_cast<std::uint16_t>(head >> kTagShift);
	}

	/// Tag to publish after this modification: the current one plus one,
	/// wrapping at 2^16 — wide enough that a stalled popper cannot observe a
	/// full lap.
	static std::uint16_t next_tag(std::uint64_t head) noexcept {
		return static_cast<std::uint16_t>(tag(head) + 1U);
	}

	static std::uint64_t pack(Node *p, std::uint16_t t) noexcept {
		const auto address = reinterpret_cast<std::uint64_t>(p);
		assert(
			(address & ~kPtrMask) == 0 &&
			"block address exceeds 48 bits; high-bit tag packing is invalid");
		return address | (std::uint64_t{t} << kTagShift);
	}

	std::atomic<std::uint64_t> head_{0};
};

} // namespace memory::tagged
