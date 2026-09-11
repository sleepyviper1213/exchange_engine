#pragma once
// fmt formatter for the metrics subsystem's composite value types.

#include "histogram.hpp"
#include "quantile.hpp"
#include "registry.hpp"

#include <fmt/base.h>
#include <fmt/format.h>

#include <string_view>
#include <utility>

/**
 * @brief A histogram snapshot, as
 *        @c "histogram[n=10696 p50=255ns p99=511ns p999=2047ns]".
 *
 * Percentiles lead, ahead of the count, because they are almost always the
 * reason anyone is looking - @c n is there to say how much they should be
 * trusted, the same role @c order_manager's @c peak/@c capacity pair plays. Not
 * a bucket dump: histogram.hpp already documents why the type keeps 65 buckets
 * rather than exact samples, and a log line is not where that detail belongs.
 * The unit is always nanoseconds - the only unit this module's histograms are
 * ever recorded in.
 */
template <>
struct fmt::formatter<exchange::core::metrics::histogram::snapshot>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::core::metrics::histogram::snapshot &snapshot,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto text) {
			using namespace exchange::core::metrics;
			return fmt::format_to(text,
								  "histogram[n={} p50={}ns p99={}ns p999={}ns]",
								  snapshot.total,
								  snapshot.quantile(percentile::P50),
								  snapshot.quantile(percentile::P99),
								  snapshot.quantile(percentile::P999));
		});
	}
};

/**
 * @brief A registry, as its full Prometheus text exposition
 */
template <>
struct fmt::formatter<exchange::core::metrics::registry> {
	static constexpr auto parse(format_parse_context &ctx)
		-> format_parse_context::iterator {
		return ctx.begin();
	}

	auto format(const exchange::core::metrics::registry &reg,
				format_context &ctx) const -> format_context::iterator {
		using exchange::core::metrics::histogram;

		auto out = ctx.out();

		for (const auto &e : reg.entries()) {
			switch (e.entry_kind) {
				using enum exchange::core::metrics::registry::kind;

			case counter_metric:
				out = fmt::format_to(out,
									 "# TYPE {} counter\n{} {}\n",
									 e.name,
									 e.name,
									 e.as_counter->load());
				break;

			case histogram_metric: {
				const auto snap = e.as_histogram->read();
				out = fmt::format_to(out, "# TYPE {} histogram\n", e.name);

				std::uint64_t cumulative = 0;
				for (std::size_t i = 0; i < histogram::NUM_BUCKETS; ++i) {
					cumulative += snap.counts[i];
					if (i + 1 == histogram::NUM_BUCKETS) {
						out = fmt::format_to(out,
											 "{}_bucket{{le=\"+Inf\"}} {}\n",
											 e.name,
											 cumulative);
					} else {
						out = fmt::format_to(out,
											 "{}_bucket{{le=\"{}\"}} {}\n",
											 e.name,
											 histogram::upper_bound(i),
											 cumulative);
					}
				}
				out = fmt::format_to(out, "{}_count {}\n", e.name, snap.total);
				break;
			}
			default: std::unreachable();
			}
		}

		return out;
	}
};
