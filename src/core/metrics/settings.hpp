#pragma once
#include "fwd.hpp"

#include <cstdint>
#include <string>

// Configuration vocabulary — data only, no behaviour. Mirrors
// core/logging/settings.hpp: a translation unit that merely describes how the
// process should export metrics (main, a config loader, a test fixture) has
// no reason to compile counter.hpp/histogram.hpp to do it.
namespace exchange::core::metrics {

/// @brief Whether and how a process periodically exposes its metrics. The
///        @c [metrics] INI section, as a type.
struct settings {
	/// @brief Whether anything records or exports metrics at all. Off by
	///        default: a caller that never sets this up pays nothing, not
	///        even the periodic file write.
	bool enabled = false;

	/// @brief Path a running process overwrites with the current Prometheus
	///        text exposition on every interval. Consumed the way
	///        Prometheus's node_exporter textfile collector, or any sidecar
	///        that tails a known path, already expects — this process opens
	///        no listening socket of its own.
	std::string output_file = "exchange_tool_metrics.prom";

	/// @brief How often the file above is rewritten.
	std::uint32_t interval_ms = 1000;
};

} // namespace exchange::core::metrics
