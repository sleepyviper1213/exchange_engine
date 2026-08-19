#pragma once

#ifndef __cpp_lib_start_lifetime_as
#include "core/util/start_lifetime_as.hpp"
#endif

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <utility>

namespace exchange::core::memory {
template <typename T, size_t N>
class fixed_memory_pool {
private:
	// A node in our lock-free singly-linked free list.
	// When a slot is free, its memory is interpreted as a Node.
	struct Node {
		Node *next;
	};

	// Enforce that a slot is large enough to hold either T or a tracking Node.
	static_assert(sizeof(T) >= sizeof(Node),
				  "Object size must be at least as large as a pointer for the "
				  "lock-free free-list.");
	static_assert(
		alignof(T) >= alignof(Node),
		"Object alignment must satisfy lock-free pointer requirements.");

public:
	// Custom deleter for RAII unique_ptr integration
	class pool_deleter {
	public:
		pool_deleter() noexcept : pool_(nullptr) {}

		explicit pool_deleter(fixed_memory_pool *pool) noexcept : pool_(pool) {}

		void operator()(T *object) const noexcept {
			if (pool_ && object) pool_->deallocate(object);
		}

	private:
		fixed_memory_pool *pool_;
	};

	using unique_pool_ptr = std::unique_ptr<T, pool_deleter>;

	fixed_memory_pool() noexcept {
#ifdef __cpp_lib_start_lifetime_as
		Node *nodes = std::start_lifetime_as_array<Node>(storage_.data(), N);
#else
		Node *nodes = util::start_lifetime_as_array<Node>(storage_.data(), N);
#endif

		// Safely link the nodes together using standard pointer operations
		for (size_t i = 0; i < N - 1; ++i) nodes[i].next = &nodes[i + 1];
		nodes[N - 1].next = nullptr;

		head_.store(&nodes[0], std::memory_order_relaxed);
		available_count_.store(N, std::memory_order_relaxed);
	}

	// Memory pooling locations must remain anchored in place
	fixed_memory_pool(const fixed_memory_pool &)            = delete;
	fixed_memory_pool &operator=(const fixed_memory_pool &) = delete;
	fixed_memory_pool(fixed_memory_pool &&)                 = delete;
	fixed_memory_pool &operator=(fixed_memory_pool &&)      = delete;

	~fixed_memory_pool() = default;

	template <typename... Args>
	[[nodiscard]] T *allocate(Args &&...args) noexcept {
		Node *old_head = head_.load(std::memory_order_acquire);

		// CAS Loop to pop the top node off the Treiber Stack safely
		while (old_head &&
			   !head_.compare_exchange_weak(old_head,
											old_head->next,
											std::memory_order_acq_rel,
											std::memory_order_acquire)) {
			// Loop continues until a successful pop or the pool is empty
		}

		if (!old_head) [[unlikely]]
			return nullptr;

		// Decrement the total count of available nodes
		available_count_.fetch_sub(1, std::memory_order_relaxed);

		auto *raw_ptr = reinterpret_cast<T *>(old_head);
		return std::construct_at(raw_ptr, std::forward<Args>(args)...);
	}

	// Lock-Free RAII Allocation
	template <typename... Args>
	[[nodiscard]] unique_pool_ptr allocate_smart(Args &&...args) noexcept {
		T *raw_ptr = allocate(std::forward<Args>(args)...);
		if (!raw_ptr) [[unlikely]]
			return unique_pool_ptr(nullptr, pool_deleter(nullptr));
		return unique_pool_ptr(raw_ptr, pool_deleter(this));
	}

	// Lock-Free Raw Deallocation
	void deallocate(T *object) noexcept {
		if (object == nullptr) [[unlikely]]
			return;

		// Boundary assertion to verify pointer ownership
		const auto check_idx =
			reinterpret_cast<std::byte *>(object) - storage_.data();
		assert(check_idx >= 0 &&
			   static_cast<size_t>(check_idx) < N * ItemSize &&
			   (check_idx % ItemSize == 0));

		std::destroy_at(object);

#ifdef __cpp_lib_start_lifetime_as
		auto *new_node =
			std::start_lifetime_as<Node>(static_cast<void *>(object));
#else
		auto *new_node =
			util::start_lifetime_as<Node>(static_cast<void *>(object));
#endif
		Node *old_head = head_.load(std::memory_order_acquire);

		// CAS Loop to push the node back onto the Treiber Stack
		do {
			new_node->next = old_head;
		} while (!head_.compare_exchange_weak(old_head,
											  new_node,
											  std::memory_order_acq_rel,
											  std::memory_order_acquire));

		// Increment the total count of available nodes
		available_count_.fetch_add(1, std::memory_order_relaxed);
	}

	[[nodiscard]] size_t available() const noexcept {
		return available_count_.load(std::memory_order_relaxed);
	}

	[[nodiscard]] size_t capacity() const noexcept { return N; }

private:
	// Ensure accurate sizing layout calculations to overlay Node objects
	// cleanly
	static constexpr size_t ItemSize  = std::max(sizeof(T), sizeof(Node));
	static constexpr size_t ItemAlign = std::max(alignof(T), alignof(Node));

	alignas(ItemAlign) std::array<std::byte, N * ItemSize> storage_;
	std::atomic<Node *> head_{nullptr};
	std::atomic<size_t> available_count_{0};
};
} // namespace exchange::core::memory