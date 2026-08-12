#include "timer.hpp"

#include "histogram.hpp"

#include <cstdint>

namespace exchange::core::metrics {

scoped_timer::scoped_timer(histogram &target) noexcept
	: target_(&target), start_(std::chrono::steady_clock::now()) {}

scoped_timer::~scoped_timer() {
	const auto elapsed = std::chrono::steady_clock::now() - start_;
	const auto ns =
		std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
	target_->record(static_cast<std::uint64_t>(ns));
}
} // namespace exchange::core::metrics