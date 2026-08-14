#include "position_entry.hpp"

#include <atomic>

namespace exchange::risk::detail {

void bump(std::atomic<volume_t> &counter, volume_t delta) noexcept {
	counter.store(counter.load(std::memory_order_relaxed) + delta,
				  std::memory_order_relaxed);
}

[[nodiscard]] std::atomic<volume_t> &working(position_entry &e,
											 side_t side) noexcept {
	return side == side_t::bid ? e.working_bid_lots : e.working_ask_lots;
}

[[nodiscard]] const std::atomic<volume_t> &working(const position_entry &e,
												   side_t side) noexcept {
	return side == side_t::bid ? e.working_bid_lots : e.working_ask_lots;
}

} // namespace exchange::risk::detail
