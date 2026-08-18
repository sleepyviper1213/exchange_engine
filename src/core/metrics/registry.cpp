#include "registry.hpp"

#include <cassert>

namespace exchange::core::metrics {

void registry::add(std::string_view name, const counter &value) {
	assert(count_ < MAX_METRICS && "registry is full - raise MAX_METRICS");
	entries_[count_++] = {.name       = name,
						  .entry_kind = kind::counter_metric,
						  .as_counter = &value};
}

void registry::add(std::string_view name, const histogram &value) {
	assert(count_ < MAX_METRICS && "registry is full - raise MAX_METRICS");
	entries_[count_++] = {.name         = name,
						  .entry_kind   = kind::histogram_metric,
						  .as_histogram = &value};
}

std::span<const registry::entry> registry::entries() const noexcept {
	return {entries_.data(), count_};
}

} // namespace exchange::core::metrics
