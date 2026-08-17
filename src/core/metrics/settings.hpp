#pragma once
#include <filesystem>

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
	std::filesystem::path output_file = "exchange_tool_metrics.prom";

	/// @brief How often the file above is rewritten.
	std::uint32_t interval_ms = 1000;

	/// @brief Drain-latency tail budgets, in nanoseconds — p99, p99.9 and
	///        max. Each independently 0 by default, disabling that one
	///        check: same "off by default" reasoning as @c enabled, an
	///        operator who never states a budget gets no opinion on one.
	/// @{
	std::uint64_t drain_p99_budget_ns  = 0;
	std::uint64_t drain_p999_budget_ns = 0;
	std::uint64_t drain_max_budget_ns  = 0;
	/// @}
};

} // namespace exchange::core::metrics
