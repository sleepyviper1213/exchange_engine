#pragma once
// `exchange_tool demo` — run the matching engine end to end over synthetic flow.
//
// The one command that exercises the whole execution path: a producer thread
// submitting commands, a pinned consumer thread draining them, and the SPSC
// queue between the two. What it reports is a throughput figure, so everything
// it does is in service of that figure being honest — pinned cores, a book that
// returns to empty, and a refusal count that makes a run measuring rejections
// impossible to mistake for a fast one.

#include "core/metrics/fwd.hpp"

#include <cstdint>

namespace exchange::app {

/**
 * @brief Submit @p num_orders synthetic orders through a partition and report.
 *
 * @param num_orders How many to submit. Must be positive.
 * @param metrics_settings Whether to record and where to write the exposition.
 * @return @c EXIT_SUCCESS, or @c EXIT_FAILURE when orders were refused — a
 *         throughput figure taken from a partial run is worse than none.
 */
int cmd_demo(std::uint64_t num_orders,
			 const core::metrics::settings &metrics_settings);

} // namespace exchange::app
