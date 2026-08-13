#include "working_ledger.hpp"

namespace exchange::risk {
working_ledger::working_ledger(std::uint32_t max_orders)
	: limit_(max_orders),
	  slots_(std::bit_ceil(std::max<std::size_t>(
		  MIN_SLOTS, (std::size_t{max_orders} * 10 + 6) / 7))),
	  mask_(slots_.size() - 1),
	  shift_(static_cast<unsigned>(64 - std::countr_zero(slots_.size()))) {}
} // namespace exchange::risk