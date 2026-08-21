#pragma once
// The partition every execution suite drives, shared rather than copied.

#include "trading-engine/execution/engine_partition.hpp"

#include <utility>
#include <vector>

namespace engine_partition_test {

/**
 * @brief A partition carrying exactly one listing - symbol 0.
 *
 * Symbol 0 is the "unspecified" id, which is what a single-book deployment uses
 * and what most commands in these suites are addressed to. Registering it in the
 * constructor keeps the tests about matching and record-keeping rather than
 * about setup; routing a command to the right listing is routing.test.cpp's
 * subject.
 *
 * The ring is 256 rather than the 1<<14 default because the queue's storage is
 * inline: a default-sized partition as a local would overflow the stack. A
 * long-lived production partition lives on the heap.
 *
 * Constructor arguments forward to @c engine_partition - (trade sink, outcome
 * sink, book capacity, order capacity) - so a suite that needs a small record
 * store to force eviction can ask for one.
 */
struct Engine : exchange::engine::execution::engine_partition<256> {
	using base = exchange::engine::execution::engine_partition<256>;

	template <typename... Args>
	explicit Engine(Args &&...args) : base(std::forward<Args>(args)...) {
		listing(0);
	}
};

/**
 * @brief Everything a partition published, in the order it published it.
 *
 * The shape every journal and recovery suite compares against: a replay is only
 * interesting if it reproduces the original run *record for record*, so the
 * comparison is on the records themselves rather than on counts. Defaulted
 * equality is what makes that a one-line assertion, and it reads every field of
 * every trade and outcome.
 *
 * Shared rather than copied because two suites were carrying their own near
 * identical version, and the one that had drifted was missing @c operator== -
 * which is precisely the member the comparison depends on.
 */
struct recording {
	std::vector<exchange::engine::trade> trades;
	std::vector<exchange::engine::order_outcome> outcomes;

	bool operator==(const recording &) const noexcept = default;
};

/**
 * @brief Sinks that append into @p into.
 *
 * Free functions because @c recording is a pair of public vectors with nothing
 * to protect between them - a member returning a lambda would dress an aggregate
 * up as something with behaviour. Hand both to a partition's constructor; the
 * recording has to outlive it, which is the ordinary case since a test declares
 * the recording first and reads it after.
 */
[[nodiscard]] inline auto trade_sink(recording &into) {
	return [&into](const std::vector<exchange::engine::trade> &batch) {
		into.trades.insert(into.trades.end(), batch.begin(), batch.end());
	};
}

[[nodiscard]] inline auto outcome_sink(recording &into) {
	return [&into](
			   const std::vector<exchange::engine::order_outcome> &batch) {
		into.outcomes.insert(into.outcomes.end(), batch.begin(), batch.end());
	};
}

} // namespace engine_partition_test
