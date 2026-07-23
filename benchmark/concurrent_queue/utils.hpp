#pragma once
#include <atomic>
#include <concepts>
#include <thread>
#include <utility>

#include "concurrency/affinity.hpp"

namespace utils {
namespace affinity = concurrency::affinity;

inline constexpr size_t kQueueCapacity = 1UL << 14UL;

/// Topology-driven core placement for the producer and consumer, resolved once.
/// The allocator puts each on its own physical core where the hardware allows,
/// so the two roles do not share one core's L1/L2 yet still pay real cross-core
/// coherency traffic — no hand-picked core numbers or sibling-numbering
/// assumptions. Reserved at Normal priority: these benchmarks measure the queue,
/// not the scheduler, and boosting pinned spin-wait threads only distorts that
/// (see priority_compare.cpp, which studies the Normal-vs-High effect head-on).
/// Reserving here (function-local static) keeps a single shared assignment
/// across every benchmark in the TU.
[[nodiscard]] inline affinity::CoreAllocator &bench_cores() {
	static affinity::CoreAllocator cores = [] {
		affinity::CoreAllocator c(affinity::discover());
		static_cast<void>(c.reserve("producer"));
		static_cast<void>(c.reserve("consumer"));
		return c;
	}();
	return cores;
}

/**
 * @brief Spawn a core-pinned producer that enqueues an increasing integer
 *        sequence until @p done is observed.
 * @tparam T Element type produced; must be default-constructible and support
 *         @c operator++.
 * @tparam Enqueue Callable modelling @c bool(const T&): the queue's
 * non-blocking push, returning @c true once the element is accepted and @c
 * false when the queue is full. Taken as a callable rather than a @c
 * &Queue::push member pointer because every queue's push is either overloaded
 *         (@c try_enqueue, @c push) or a variadic template (@c write,
 *         @c try_emplace), so its address is ambiguous/unformable; a lambda
 *         resolves the exact overload at the call site.
 * @param done Stop flag observed with acquire ordering.
 * @param enqueue Push operation, e.g. `[&](const T& v){ return q.push(v); }`.
 * @return The running producer thread; hand it to stop_producer() to join.
 * @pre @p enqueue must fail fast (never block) when the queue is full, so the
 *      loop can re-check @p done and exit.
 */
template <typename T, std::predicate<const T &> Enqueue>
std::thread spawn_single_producer(std::atomic<bool> &done, Enqueue enqueue) {
	return std::thread{[&done, enqueue = std::move(enqueue)] {
		// Best-effort pin; a failure only costs measurement stability.
		static_cast<void>(bench_cores().pin_this_thread_to("producer"));

		for (T value{};; ++value) {
			if (done.load(std::memory_order_acquire)) return;

			while (!enqueue(value))
				if (done.load(std::memory_order_acquire)) return;
		}
	}};
}

/**
 * @brief Signal the producer to stop, drain any leftover elements, then join.
 * @tparam T Element type used for the throwaway drain sink.
 * @tparam DequeueFunc Callable modelling @c bool(T&): the queue's non-blocking
 * pop.
 * @param done Stop flag; released before draining so the producer observes it.
 * @param producer Thread returned by spawn_single_producer().
 * @param dequeue_to Pop operation, e.g. `[&](T& out){ return q.pop(out); }`.
 * @post @p producer is joined and the queue is drained empty.
 */
template <typename T, std::predicate<T &> DequeueFunc>
void stop_producer(std::atomic<bool> &done, std::thread &producer,
				   DequeueFunc dequeue_to) {
	done.store(true, std::memory_order_release);

	for (T sink{}; dequeue_to(sink);) {}

	producer.join();
}
} // namespace utils
