#include "working_ledger.hpp"

#include "detail/probe_table.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>

// The side/quantity encoding, kept in the one translation unit that applies it.
// `detail::probe_table` decides where a row lives; these two decide what its
// sixteen bytes mean, and nothing outside this file has any business knowing.
namespace exchange::risk::hooks::pre_trade::detail {
namespace {

/// @brief Positive is a bid, negative an ask. @pre @p lots is positive.
[[nodiscard]] quantity_t pack(side_t side, quantity_t lots) noexcept {
	return side == side_t::bid ? lots : -lots;
}

[[nodiscard]] working_order unpack(const ledger_slot &s) noexcept {
	const bool is_bid = s.signed_lots > 0;
	return {.id    = s.id,
			.side  = is_bid ? side_t::bid : side_t::ask,
			.price = s.price,
			.lots  = is_bid ? s.signed_lots : -s.signed_lots};
}

} // namespace
} // namespace exchange::risk::hooks::pre_trade::detail

namespace exchange::risk::hooks::pre_trade {
using detail::pack;
using detail::probe_table;
using detail::unpack;

working_ledger::working_ledger(std::uint32_t max_orders)
	: limit_(max_orders),
	  // Over-allocated to keep the load factor near 0.7, and rounded up to a
	  // power of two so the wrap is an AND. @see the class note.
	  table_(std::bit_ceil(std::max<std::size_t>(
		  MIN_SLOTS, (std::size_t{max_orders} * 10 + 6) / 7))) {}

[[nodiscard]] std::uint32_t working_ledger::size() const noexcept {
	return size_;
}

[[nodiscard]] std::uint32_t working_ledger::limit() const noexcept {
	return limit_;
}

[[nodiscard]] std::size_t working_ledger::slot_count() const noexcept {
	return table_.slot_count();
}

[[nodiscard]] bool working_ledger::is_empty() const noexcept {
	return size_ == 0;
}

[[nodiscard]] bool working_ledger::is_full() const noexcept {
	return size_ >= limit_;
}

[[nodiscard]] bool working_ledger::contains(order_id_t id) const noexcept {
	return id != 0 && table_.find(id) != probe_table::NOT_FOUND;
}

[[nodiscard]] std::optional<working_order>
working_ledger::find(order_id_t id) const noexcept {
	if (id == 0) return std::nullopt;
	const std::size_t at = table_.find(id);
	if (at == probe_table::NOT_FOUND) return std::nullopt;
	return unpack(table_[at]);
}

bool working_ledger::insert(order_id_t id, side_t side, price_t price,
							quantity_t lots) noexcept {
	if (id == 0 || lots <= 0 || is_full()) return false;

	const std::size_t at = table_.vacancy_for(id);
	if (at == probe_table::NOT_FOUND) return false; // already tracked
	table_[at] = {.id = id, .price = price, .signed_lots = pack(side, lots)};
	++size_;
	return true;
}

std::optional<ledger_take> working_ledger::take(order_id_t id,
												quantity_t lots) noexcept {
	if (id == 0 || lots <= 0) return std::nullopt;
	const std::size_t at = table_.find(id);
	if (at == probe_table::NOT_FOUND) return std::nullopt;

	const working_order entry = unpack(table_[at]);
	const quantity_t taken    = lots < entry.lots ? lots : entry.lots;
	const quantity_t left     = entry.lots - taken;

	if (left == 0) {
		table_.erase(at);
		--size_;
	} else {
		table_[at].signed_lots = pack(entry.side, left);
	}

	return ledger_take{.side      = entry.side,
					   .price     = entry.price,
					   .taken     = taken,
					   .remaining = left};
}

std::optional<ledger_take> working_ledger::retire(order_id_t id) noexcept {
	if (id == 0) return std::nullopt;
	const std::size_t at = table_.find(id);
	if (at == probe_table::NOT_FOUND) return std::nullopt;

	const working_order entry = unpack(table_[at]);
	table_.erase(at);
	--size_;
	return ledger_take{.side      = entry.side,
					   .price     = entry.price,
					   .taken     = entry.lots,
					   .remaining = 0};
}

void working_ledger::clear() noexcept {
	table_.clear();
	size_ = 0;
}
} // namespace exchange::risk::hooks::pre_trade
