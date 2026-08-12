#include "text_exposition.hpp"

#include <cstddef>
#include <cstdint>

namespace exchange::core::metrics {

namespace {

void render_counter(const registry::entry &e, fmt::memory_buffer &out) {
	fmt::format_to(fmt::appender(out),
				  "# TYPE {} counter\n{} {}\n",
				  e.name,
				  e.name,
				  e.as_counter->load());
}

void render_histogram(const registry::entry &e, fmt::memory_buffer &out) {
	const histogram::snapshot snap = e.as_histogram->read();
	fmt::format_to(fmt::appender(out), "# TYPE {} histogram\n", e.name);

	std::uint64_t cumulative = 0;
	for (std::size_t i = 0; i < histogram::NUM_BUCKETS; ++i) {
		cumulative += snap.counts[i];
		if (i + 1 == histogram::NUM_BUCKETS)
			fmt::format_to(fmt::appender(out),
						  "{}_bucket{{le=\"+Inf\"}} {}\n",
						  e.name,
						  cumulative);
		else
			fmt::format_to(fmt::appender(out),
						  "{}_bucket{{le=\"{}\"}} {}\n",
						  e.name,
						  histogram::upper_bound(i),
						  cumulative);
	}
	fmt::format_to(fmt::appender(out), "{}_count {}\n", e.name, snap.total);
}

} // namespace

void render_prometheus_text(const registry &reg, fmt::memory_buffer &out) {
	for (const registry::entry &e : reg.entries()) {
		switch (e.entry_kind) {
		case registry::kind::counter_metric: render_counter(e, out); break;
		case registry::kind::histogram_metric: render_histogram(e, out); break;
		}
	}
}

std::string render_prometheus_text(const registry &reg) {
	fmt::memory_buffer out;
	render_prometheus_text(reg, out);
	return fmt::to_string(out);
}

} // namespace exchange::core::metrics
