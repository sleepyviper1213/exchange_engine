#pragma once
// The partition every execution suite drives, shared rather than copied.

#include "trading-engine/execution/engine_partition.hpp"

#include <utility>

namespace engine_partition_test {

/**
 * @brief A partition carrying exactly one listing — symbol 0.
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
 * Constructor arguments forward to @c engine_partition — (trade sink, outcome
 * sink, book capacity, order capacity) — so a suite that needs a small record
 * store to force eviction can ask for one.
 */
struct Engine : exchange::engine::execution::engine_partition<256> {
	using base = exchange::engine::execution::engine_partition<256>;

	template <typename... Args>
	explicit Engine(Args &&...args) : base(std::forward<Args>(args)...) {
		listing(0);
	}
};

} // namespace engine_partition_test
