#include "working_ledger.hpp"

#include <algorithm>
#include <bit>

namespace exchange::risk {
working_ledger::working_ledger(std::uint32_t max_orders)
	: limit_(max_orders),
	  slots_(std::bit_ceil(std::max<std::size_t>(
		  MIN_SLOTS, (std::size_t{max_orders} * 10 + 6) / 7))),
	  mask_(slots_.size() - 1),
	  shift_(static_cast<unsigned>(64 - std::countr_zero(slots_.size()))) {}

[[nodiscard]] std::uint32_t working_ledger::size() const noexcept {
	return size_;
}

[[nodiscard]] std::uint32_t working_ledger::limit() const noexcept {
	return limit_;
}

[[nodiscard]] std::size_t working_ledger::slot_count() const noexcept {
	return slots_.size();
}

[[nodiscard]] bool working_ledger::empty() const noexcept { return size_ == 0; }

[[nodiscard]] bool working_ledger::full() const noexcept {
	return size_ >= limit_;
}

[[nodiscard]] bool working_ledger::contains(order_id_t id) const noexcept {
	return id != 0 && find_slot(id) != NOT_FOUND;
}

[[nodiscard]] std::optional<working_order>
working_ledger::find(order_id_t id) const noexcept {
	if (id == 0) return std::nullopt;
	const std::size_t at = find_slot(id);
	if (at == NOT_FOUND) return std::nullopt;
	return unpack(slots_[at]);
}

bool working_ledger::insert(order_id_t id, side_t side, price_t price,
							quantity_t lots) noexcept {
	if (id == 0 || lots <= 0 || full()) return false;

	std::size_t at = home(id);
	while (slots_[at].id != 0) {
		if (slots_[at].id == id) return false;
		at = (at + 1) & mask_;
	}
	slots_[at] = {.id = id, .price = price, .signed_lots = pack(side, lots)};
	++size_;
	return true;
}

std::optional<ledger_take> working_ledger::take(order_id_t id,
												quantity_t lots) noexcept {
	if (id == 0 || lots <= 0) return std::nullopt;
	const std::size_t at = find_slot(id);
	if (at == NOT_FOUND) return std::nullopt;

	const working_order entry = unpack(slots_[at]);
	const quantity_t taken    = lots < entry.lots ? lots : entry.lots;
	const quantity_t left     = entry.lots - taken;

	if (left == 0) erase_at(at);
	else slots_[at].signed_lots = pack(entry.side, left);

	return ledger_take{.side      = entry.side,
					   .price     = entry.price,
					   .taken     = taken,
					   .remaining = left};
}

std::optional<ledger_take> working_ledger::retire(order_id_t id) noexcept {
	if (id == 0) return std::nullopt;
	const std::size_t at = find_slot(id);
	if (at == NOT_FOUND) return std::nullopt;

	const working_order entry = unpack(slots_[at]);
	erase_at(at);
	return ledger_take{.side      = entry.side,
					   .price     = entry.price,
					   .taken     = entry.lots,
					   .remaining = 0};
}

void working_ledger::clear() noexcept {
	for (slot &s : slots_) s = {};
	size_ = 0;
}

[[nodiscard]] working_order working_ledger::unpack(const slot &s) noexcept {
	const bool is_bid = s.signed_lots > 0;
	return {.id    = s.id,
			.side  = is_bid ? side_t::bid : side_t::ask,
			.price = s.price,
			.lots  = is_bid ? s.signed_lots : -s.signed_lots};
}

[[nodiscard]] std::size_t working_ledger::home(order_id_t id) const noexcept {
	constexpr std::uint64_t GOLDEN = 0x9E37'79B9'7F4A'7C15ULL;
	return static_cast<std::size_t>((id * GOLDEN) >> shift_);
}

[[nodiscard]] std::size_t
working_ledger::find_slot(order_id_t id) const noexcept {
	std::size_t at = home(id);
	while (slots_[at].id != 0) {
		if (slots_[at].id == id) return at;
		at = (at + 1) & mask_;
	}
	return NOT_FOUND;
}

void working_ledger::erase_at(std::size_t at) noexcept {
	std::size_t hole = at;
	for (;;) {
		slots_[hole]      = {};
		std::size_t probe = hole;
		for (;;) {
			probe = (probe + 1) & mask_;
			if (slots_[probe].id == 0) {
				--size_;
				return;
			}
			const std::size_t ideal = home(slots_[probe].id);
			// Is `ideal` cyclically inside (hole, probe]? If so this entry
			// is already found by a probe starting at its home and must not
			// move; if not, moving it into the hole keeps its chain intact.
			const bool must_stay = hole <= probe
									   ? (hole < ideal && ideal <= probe)
									   : (hole < ideal || ideal <= probe);
			if (!must_stay) break;
		}
		slots_[hole] = slots_[probe];
		hole         = probe;
	}
}

[[nodiscard]] quantity_t working_ledger::pack(side_t side,
											  quantity_t lots) noexcept {
	return side == side_t::bid ? lots : -lots;
}
} // namespace exchange::risk
