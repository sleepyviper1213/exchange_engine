#pragma once

#include "core_export.hpp"
#include "fwd.hpp"
#include "registry.hpp"

#include <fmt/format.h>

#include <string>

// Renders a registry as Prometheus text exposition format.
//
// File-based, not served: transport/ owns protocols and network endpoints,
// and giving metrics an HTTP listener would be a new transport concern this
// module does not add. What this produces is exactly what Prometheus's
// node_exporter textfile collector, or any sidecar that tails a known path,
// already expects — periodic overwrite of one file. core::metrics::settings
// names that path; nothing here opens a socket.
namespace exchange::core::metrics {

/**
 * @brief Append every metric in @p reg to @p out, Prometheus text format.
 *
 * A counter renders as one `# TYPE name counter` line plus one value line. A
 * histogram renders as Prometheus's own cumulative bucket shape — one
 * `name_bucket{le="..."}` line per bucket boundary, running total, plus
 * `name_count`. There is no `name_sum`: the bucketed histogram never retains
 * the exact values that would sum to, only which bucket each landed in (see
 * histogram.hpp on why that trade was made).
 *
 * @c fmt::memory_buffer rather than @c std::string: fmt's own growable
 * buffer, with an inline first chunk and fmt's growth policy rather than
 * `std::string`'s, and it is what @c fmt::appender (the writer this and
 * `format.hpp`'s formatters both use) is built to target. This is a cold,
 * periodic path — once per @c metrics-interval-ms, not once per message —
 * so the buffer choice is about writing idiomatic fmt rather than a latency
 * budget.
 *
 * @param out Appended to, not cleared first, so a caller can compose several
 *        registries into one buffer.
 */
CORE_EXPORT void render_prometheus_text(const registry &reg,
										fmt::memory_buffer &out);

/// @brief Convenience overload: render straight to a new, owned string.
[[nodiscard]] CORE_EXPORT std::string
render_prometheus_text(const registry &reg);

} // namespace exchange::core::metrics
