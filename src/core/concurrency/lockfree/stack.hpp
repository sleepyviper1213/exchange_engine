#pragma once
#include "core/concurrency/synchronisation/hazard_pointer/hazard_pointer.hpp"
#include "core/concurrency/synchronisation/hazard_pointer/hazard_pointer_obj_base.hpp"
#include "fwd.hpp"

#include <atomic>
#include <optional>
#include <utility>


namespace exchange::core::concurrency::lockfree {

// A lock-free Treiber stack whose node reclamation is made safe with hazard
// pointers. The hard part of a concurrent stack is not the push/pop CAS but
// freeing a popped node: another thread may have loaded the same node pointer
// and be about to dereference it. Each popping thread first publishes the head
// it is about to touch through a hazard pointer, so the node is retired rather
// than deleted while anyone still protects it, and reclaimed later once no
// hazard pointer references it.
template <typename T>
class stack {
	struct node
		: exchange::core::concurrency::synchronisation::hazard_pointer_obj_base<
			  node> {
		T value;
		std::atomic<node *> next{nullptr};

		explicit node(T v) : value(std::move(v)) {}
	};

	std::atomic<node *> head_{nullptr};

public:
	stack()                         = default;
	stack(stack &&)                 = delete;
	stack &operator=(stack &&)      = delete;
	stack(const stack &)            = delete;
	stack &operator=(const stack &) = delete;

	~stack() {
		// No concurrent access at destruction: drain and delete directly.
		node *n = head_.load(std::memory_order_relaxed);
		while (n != nullptr) {
			node *next = n->next.load(std::memory_order_relaxed);
			delete n;
			n = next;
		}
	}

	void push(T value) {
		node *n       = new node(std::move(value));
		node *current = head_.load(std::memory_order_relaxed);
		do {
			n->next.store(current, std::memory_order_relaxed);
			// On failure `current` is refreshed to the observed head.
		} while (!head_.compare_exchange_weak(current,
											  n,
											  std::memory_order_release,
											  std::memory_order_relaxed));
	}

	std::optional<T> pop() {
		auto hp =
			exchange::core::concurrency::synchronisation::make_hazard_pointer();

		node *old = nullptr;
		while (true) {
			// Protect the head we intend to pop; protect() republishes and
			// re-validates until the hazard pointer covers a stable head.
			old = hp.protect(head_);
			if (old == nullptr) return std::nullopt; // empty
			// old is protected, so old->next is safe to read: no other thread
			// can have reclaimed it since we published our hazard pointer.
			node *next = old->next.load(std::memory_order_acquire);
			if (head_.compare_exchange_weak(old,
											next,
											std::memory_order_acquire,
											std::memory_order_relaxed)) {
				break; // we own `old`
			}
			// Lost the race; loop and protect the new head.
		}

		std::optional<T> result(std::move(old->value));
		hp.reset_protection(); // stop protecting before retiring
		old->retire();         // freed once no hazard pointer references it
		return result;
	}
};

} // namespace exchange::core::concurrency::lockfree
