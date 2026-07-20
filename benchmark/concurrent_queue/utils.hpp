#pragma once
#include <atomic>
#include <concepts>
#include <numeric>
#include <thread>
#include <utility>

namespace utils {
inline constexpr size_t kQueueCapacity = 1UL << 14UL;

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
		auto _ = pin_current_thread_to_core(kProducerCore);

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
