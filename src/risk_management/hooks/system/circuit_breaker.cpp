#include "circuit_breaker.hpp"

#include <atomic>
#include <cstdint>

// The breaker's three mutating members, and the only code in this module that
// is out of line.
//
// Everything else here stays in its header on purpose, and the reason is
// written up where each piece lives: `breach.hpp` builds its reason table at
// compile time and would lose that, `position_book`'s update is documented as
// `mov / add / mov` against roughly twenty nanoseconds for a `lock xadd`,
// `rate_limiter` and `working_ledger` sit inside the per-command check, and
// `risk_gate` is a template. Out-of-lining any of those would trade a measured
// property for a tidier build graph.
//
// These three are different. `trip` and `arm` are operator actions - once a
// session, by hand - and `record_breach` runs only when a command has *already*
// been refused, which is the path the gate deliberately does not optimise. A
// call through the module boundary costs nothing any of them can notice, and
// putting them here is what gives the module a translation unit of its own.

namespace exchange::risk::hooks::system {

void circuit_breaker::trip(trading_state to, trip_cause why) noexcept {
	if (to != trading_state::NORMAL) {
		++trips_;
		cause_.store(why, std::memory_order_relaxed);
	}
	state_.store(to, std::memory_order_relaxed);
}

void circuit_breaker::arm() noexcept {
	state_.store(trading_state::NORMAL, std::memory_order_relaxed);
}

bool circuit_breaker::record_breach(std::uint64_t now_ns) noexcept {
	const std::uint64_t epoch = now_ns >> shift_;
	// Same branchless rollover as rate_limiter: keep the count if it belongs to
	// this window, otherwise start from zero.
	breaches_ = (breaches_ & -static_cast<std::uint32_t>(epoch == epoch_)) + 1;
	epoch_    = epoch;

	if (threshold_ == NO_AUTO_TRIP || breaches_ < threshold_) return false;
	if (state_.load(std::memory_order_relaxed) != trading_state::NORMAL)
		return false;
	trip(trading_state::CANCEL_ONLY, trip_cause::BREACH_RATE);
	return true;
}

circuit_breaker::circuit_breaker(std::uint32_t breaches_to_trip,
								 unsigned window_log2_ns) noexcept
	: threshold_(breaches_to_trip), shift_(window_log2_ns) {}

[[nodiscard]] trading_state circuit_breaker::state() const noexcept {
	return state_.load(std::memory_order_relaxed);
}

[[nodiscard]] bool circuit_breaker::passes_new_orders() const noexcept {
	return state() == trading_state::NORMAL;
}

[[nodiscard]] bool circuit_breaker::passes_cancels() const noexcept {
	return state() != trading_state::HALTED;
}

[[nodiscard]] trip_cause circuit_breaker::cause() const noexcept {
	return cause_.load(std::memory_order_relaxed);
}

[[nodiscard]] std::uint32_t
circuit_breaker::breaches(std::uint64_t now_ns) const noexcept {
	const std::uint64_t epoch = now_ns >> shift_;
	return breaches_ & -static_cast<std::uint32_t>(epoch == epoch_);
}

[[nodiscard]] std::uint64_t circuit_breaker::trips() const noexcept {
	return trips_;
}

[[nodiscard]] std::uint32_t circuit_breaker::threshold() const noexcept {
	return threshold_;
}
} // namespace exchange::risk::hooks::system
