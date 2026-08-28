#include "ledger_view.hpp"

#include "risk_management/hooks/pre_trade/working_ledger.hpp"

namespace exchange::session {

ledger_view::ledger_view(
	const risk::hooks::pre_trade::working_ledger &ledger) noexcept
	: ledger_(&ledger) {}

[[nodiscard]] std::optional<strategy::backtest::resting_quote>
ledger_view::resting(order_id_t id) const noexcept {
	const auto entry = ledger_->find(id);
	if (!entry.has_value()) return std::nullopt;
	if (entry->lots <= 0) return std::nullopt;
	return strategy::backtest::resting_quote{.side  = entry->side,
											 .price = entry->price,
											 .lots  = entry->lots};
}

} // namespace exchange::session