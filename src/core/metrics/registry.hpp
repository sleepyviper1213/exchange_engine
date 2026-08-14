#pragma once
// Non-owning, fixed-capacity directory of a component's own metrics.
//
// A component builds its counters and histograms as ordinary members, then
// names each one here, once, at startup — the cold path, not the one
// counter.hpp and histogram.hpp are built for. text_exposition.hpp walks a
// registry to render every metric a component chose to publish, without
// either side depending on the other's type: recording code includes only
// counter.hpp/histogram.hpp, exposition code includes only registry.hpp.
//
// Deliberately no global instance. Each owner (an engine_partition today)
// holds its own registry beside the metrics it names — the "single
// ownership" rule the rest of the engine applies to every other piece of
// mutable state applies here too.

#include "core_export.hpp" // CORE_EXPORT (generated)
#include "fwd.hpp"

#include "counter.hpp"
#include "histogram.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace exchange::core::metrics {

/// Members are exported individually, not the class — see counter.hpp's
/// class note on why.
class registry {
public:
	/// @brief Named metrics one registry can hold. A component's metric set
	///        is fixed at compile time, so this is sized generously rather
	///        than grown — see @c add's assertion if it is ever not enough.
	static constexpr std::size_t MAX_METRICS = 64;

	enum class kind : std::uint8_t { counter_metric, histogram_metric };

	/// @brief One named entry. @c name must outlive the registry — pass a
	///        string literal or a name owned by whoever owns the metric.
	struct entry {
		std::string_view name;
		kind entry_kind;
		const counter *as_counter     = nullptr;
		const histogram *as_histogram = nullptr;
	};

	/// @brief Register @p value under @p name. @p value must outlive the
	///        registry, and its address must not change afterwards — the same
	///        contract counter/histogram already carry by being non-movable.
	CORE_EXPORT void add(std::string_view name, const counter &value);

	/// @copydoc add(std::string_view, const counter &)
	CORE_EXPORT void add(std::string_view name, const histogram &value);

	/// @brief Every metric named so far, in registration order.
	[[nodiscard]] CORE_EXPORT std::span<const entry> entries() const noexcept;

private:
	std::array<entry, MAX_METRICS> entries_{};
	std::size_t count_ = 0;
};

} // namespace exchange::core::metrics
