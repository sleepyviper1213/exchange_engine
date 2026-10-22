#pragma once
// The one shape everything on the command path agrees on. It lives with the
// command rather than with either side that uses it: `strategy` writes into a
// sink and `risk_management` both is one and wraps one, and the two never link
// each other, so the concept they meet through has to sit below both.

#include "command.hpp"

#include <concepts>
#include <span>

namespace exchange::engine::event {

/**
 * @brief Somewhere to put a finished batch - @c execution::engine_partition, a
 *        @c risk::risk_gate in front of one, or a test double.
 *
 * All-or-nothing by contract: @c false must leave the sink exactly as it was, so
 * the caller can keep the batch and retry rather than having to work out which
 * prefix got through. @c spsc_queue::try_emplace_range already guarantees this,
 * which is why the signature is shaped around it.
 */
template <class T>
concept command_sink = requires(T &sink, std::span<const command> batch) {
	{ sink.submit_range(batch) } -> std::same_as<bool>;
};

} // namespace exchange::engine::event
