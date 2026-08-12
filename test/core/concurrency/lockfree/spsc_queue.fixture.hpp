#pragma once
// Shared by every spsc_queue suite: the drain helper, plus the signature
// constraints the batch APIs impose.

#include "core/concurrency/lockfree/spsc_queue.hpp"

#include <cstddef>
#include <list>
#include <vector>


using exchange::core::concurrency::lockfree::spsc_queue;

// Spelled as concepts rather than inline requires-expressions: MSVC resolves a
// requires-expression over concrete, non-dependent types eagerly and reports a
// hard error instead of an unsatisfied requirement, so the queue and range
// types have to stay dependent for the negative cases to compile.
template <class Q, class Rg>
concept is_range_emplacable = requires(Q q, Rg r) { q.try_emplace_range(r); };

template <class Q, class Rg>
concept is_range_dequeueable = requires(Q q, Rg r) { q.try_dequeue_range(r); };

// The batch APIs size the reservation from ranges::size and copy through
// ranges::data, so they take sized, contiguous ranges. A node-based container
// has neither contiguous storage nor a pointer to memcpy against, and must be
// rejected at the signature rather than deep inside the template body.
static_assert(is_range_emplacable<spsc_queue<int, 8>, std::vector<int> >);
static_assert(is_range_dequeueable<spsc_queue<int, 8>, std::vector<int> >);
static_assert(!is_range_emplacable<spsc_queue<int, 8>, std::list<int> >);
static_assert(!is_range_dequeueable<spsc_queue<int, 8>, std::list<int> >);

/// @brief Drain the queue into a vector, preserving FIFO order.
template <class T, std::size_t N>
std::vector<T> drain(spsc_queue<T, N> &queue) {
	std::vector<T> out;
	while (auto value = queue.try_dequeue()) out.emplace_back(*value);
	return out;
}

