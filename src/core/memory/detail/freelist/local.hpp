#pragma once

#include <cstddef>
#include <new>

namespace exchange::core::memory {
inline namespace local {

/**
 * @brief Intrusive LIFO free list for storage owned by one thread.
 *
 * A released block stores its link in its own unused bytes.  This is the arena
 * fast path: no atomics, CAS retries, or deferred reclamation.  It is correct
 * only when one owner performs every push and pop; cross-thread returns must
 * first be sent back to that owner through a bounded queue.
 */
class free_list {
	struct Node {
		Node *next;
	};

public:
	static constexpr std::size_t kMinBlockBytes = sizeof(Node);

	free_list() noexcept                    = default;
	free_list(const free_list &)            = delete;
	free_list &operator=(const free_list &) = delete;

	/// @brief Return a block to the list. The block must be at least
	///        @c kMinBlockBytes and must not be used by another owner.
	void push(void *block) noexcept {
		auto *node = ::new (block) Node{head_};
		head_      = node;
	}

	/// @brief Take the most recently returned block, or @c nullptr when empty.
	[[nodiscard]] void *pop() noexcept {
		Node *node = head_;
		if (node == nullptr) return nullptr;
		head_ = node->next;
		return node;
	}

	[[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }

private:
	Node *head_{nullptr};
};

} // namespace local
} // namespace memory
