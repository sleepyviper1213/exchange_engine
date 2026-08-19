#include "duplicate.hpp"

namespace exchange::risk::hooks::pre_trade {
breach_bits claim(working_ledger &ledger,
				  const engine::orders::order &o) noexcept {
	if (ledger.is_full())
		return static_cast<breach_bits>(breach::WORKING_ORDERS);
	if (!ledger.insert(o.id, o.side, o.price, o.qty))
		return static_cast<breach_bits>(breach::DUPLICATE_ORDER);
	return 0;
}
} // namespace exchange::risk::hooks::pre_trade