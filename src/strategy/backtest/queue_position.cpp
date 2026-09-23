#include "queue_position.hpp"

#include <algorithm>

namespace exchange::strategy::backtest {

void queue_position_book::open_side(side_t side) noexcept {
	for (row &entry : rows_)
		if (entry.side == side) entry.is_live = false;
}

void queue_position_book::track(side_t side, price_t price,
								volume_t published_lots) {
	const volume_t published = std::max<volume_t>(published_lots, 0);
	if (row *at = find(side, price); at != nullptr) {
		at->is_live = true;
		at->ahead   = std::min(at->ahead, published);
		return;
	}
	rows_.emplace_back(price, published, side, true);
}

void queue_position_book::hold(side_t side, price_t price) {
	if (row *at = find(side, price); at != nullptr) {
		at->is_live = true;
		return;
	}
	rows_.emplace_back(price, 0, side, true);
}

void queue_position_book::close_side(side_t side) {
	(void)std::erase_if(rows_, [side](const row &entry) noexcept {
		return entry.side == side && !entry.is_live;
	});
}

volume_t queue_position_book::absorb(side_t side, price_t price,
									 volume_t lots) noexcept {
	if (lots <= 0) return 0;
	row *at = find(side, price);
	if (at == nullptr || at->ahead <= 0) return 0;
	const volume_t taken = std::min(at->ahead, lots);
	at->ahead -= taken;
	absorbed_ += taken;
	return taken;
}

[[nodiscard]] volume_t
queue_position_book::ahead(side_t side, price_t price) const noexcept {
	const row *at = find(side, price);
	return at != nullptr ? at->ahead : 0;
}

void queue_position_book::clear() noexcept { rows_.clear(); }

[[nodiscard]] volume_t queue_position_book::absorbed_lots() const noexcept {
	return absorbed_;
}

[[nodiscard]] std::size_t queue_position_book::tracked() const noexcept {
	return rows_.size();
}

[[nodiscard]] queue_position_book::row *
queue_position_book::find(side_t side, price_t price) noexcept {
	const auto at = std::ranges::find_if(rows_,

										 [&](const row &entry) noexcept {
											 return entry.side == side &&
													entry.price == price;
										 });
	return at != rows_.end() ? &*at : nullptr;
}

[[nodiscard]] const queue_position_book::row *
queue_position_book::find(side_t side, price_t price) const noexcept {
	const auto at = std::ranges::find_if(rows_, [&](const row &entry) noexcept {
		return entry.side == side && entry.price == price;
	});
	return at != rows_.end() ? &*at : nullptr;
}


} // namespace exchange::strategy::backtest