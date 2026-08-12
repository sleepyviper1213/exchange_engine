#pragma once
// fmt formatter for the metrics subsystem's composite value types.
//
// Opt-in, like fmt's own fmt/std.h and fmt/ranges.h: only translation units
// that actually print a snapshot pay for <fmt/format.h>, so histogram.hpp
// stays includable by the hot path without it. This sits at core/metrics/
// rather than at core/, for the same reason core/concurrency/affinity/format.hpp
// does — core/ spans concurrency, memory, persistence and metrics, and a
// module-wide format.hpp there would make every one of those vocabularies a
// dependency of formatting any single one of them.
//
// counter is not given a formatter: it is one uint64_t, and printing
// counter.load() directly needs no help. A snapshot is the composite worth
// naming.

#include "histogram.hpp"
#include "registry.hpp"
#include "text_exposition.hpp"

#include <fmt/format.h>

#include <string_view>

/**
 * @brief A histogram snapshot, as
 *        @c "histogram[n=10696 p50=255ns p99=511ns p999=2047ns]".
 *
 * Percentiles lead, ahead of the count, because they are almost always the
 * reason anyone is looking — @c n is there to say how much they should be
 * trusted, the same role @c order_manager's @c peak/@c capacity pair plays in
 * trading-engine/format.hpp. Not a bucket dump: histogram.hpp already
 * documents why the type keeps 65 buckets rather than exact samples, and a
 * log line is not where that detail belongs. The unit is always nanoseconds —
 * the only unit this module's histograms are ever recorded in.
 */
template <>
struct fmt::formatter<exchange::core::metrics::histogram::snapshot>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::core::metrics::histogram::snapshot &snapshot,
			   format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "histogram[n={} p50={}ns p99={}ns p999={}ns]",
								  snapshot.total,
								  snapshot.quantile(0.50),
								  snapshot.quantile(0.99),
								  snapshot.quantile(0.999));
		});
	}
};

/**
 * @brief A registry, as its full Prometheus text exposition
 *        (@c text_exposition.hpp's @c render_prometheus_text, verbatim).
 *
 * Not a @c nested_formatter like every other type in this header, and
 * deliberately so: fill/align/width apply to *one padded record*, and a
 * multi-line, multi-metric scrape dump is not that — there is no sensible
 * reading of `{:>60}` against several newline-separated lines. This follows
 * fmt's own plain formatter shape instead (fmt.dev's user-defined-type
 * example: a `parse` that consumes nothing, a `format` that writes straight
 * to `ctx.out()`).
 *
 * @c fmt::format("{}", registry) and @c render_prometheus_text(registry) are
 * the same rendering; this exists so a registry composes into a larger
 * @c fmt::format call (e.g. a log line that names the registry once and
 * appends its dump) without a caller reaching for the function by name.
 */
template <>
struct fmt::formatter<exchange::core::metrics::registry> {
	static constexpr auto parse(format_parse_context &ctx)
		-> format_parse_context::iterator {
		return ctx.begin();
	}

	auto format(const exchange::core::metrics::registry &reg,
			   format_context &ctx) const -> format_context::iterator {
		fmt::memory_buffer text;
		exchange::core::metrics::render_prometheus_text(reg, text);
		return fmt::format_to(ctx.out(),
							  "{}",
							  std::string_view{text.data(), text.size()});
	}
};
