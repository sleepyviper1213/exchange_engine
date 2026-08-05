#include"depth_feed_bridge.hpp"

namespace exchange::app {
depth_feed_bridge::depth_feed_bridge(
	symbol_id_t symbol, market_data::reconstructor_options options) noexcept
	: symbol_(symbol), reconstructor_(options) {}

market_data::sequence_action
depth_feed_bridge::on_event(market_data::depth_event event,
							std::vector<command> &out) {
	const auto action = reconstructor_.on_event(std::move(event));
	emit_resync(out);
	return action;
}

bool depth_feed_bridge::on_snapshot(market_data::book_snapshot snapshot,
									std::vector<command> &out) {
	const bool live = reconstructor_.on_snapshot(std::move(snapshot));
	emit_resync(out);
	return live;
}

void depth_feed_bridge::invalidate(std::vector<command> &out) {
	reconstructor_.invalidate();
	emit_resync(out);
}

[[nodiscard]] symbol_id_t depth_feed_bridge::symbol() const noexcept {
	return symbol_;
}

[[nodiscard]] const market_data::l2_book &
depth_feed_bridge::replica() const noexcept {
	return reconstructor_.book();
}

[[nodiscard]] const market_data::l2_book &
depth_feed_bridge::mirror() const noexcept {
	return mirror_;
}

[[nodiscard]] bool depth_feed_bridge::is_live() const noexcept {
	return reconstructor_.live();
}

[[nodiscard]] bool depth_feed_bridge::needs_snapshot() const noexcept {
	return reconstructor_.needs_snapshot();
}

void depth_feed_bridge::is_snapshot_requested() noexcept {
	reconstructor_.snapshot_requested();
}

[[nodiscard]] const market_data::depth_reconstructor &
depth_feed_bridge::reconstructor() const noexcept {
	return reconstructor_;
}

[[nodiscard]] std::uint64_t
depth_feed_bridge::commands_emitted() const noexcept {
	return commands_emitted_;
}

void depth_feed_bridge::emit_resync(std::vector<command> &out) {
	const std::size_t before              = out.size();
	const market_data::l2_book &live_book = reconstructor_.book();

	diff_side(mirror_.bid_levels(), live_book.bid_levels(), side_t::bid, out);
	diff_side(mirror_.ask_levels(), live_book.ask_levels(), side_t::ask, out);

	// Adopt after diffing, never before. load() installs both sides
	// wholesale from storage the mirror already owns, so this allocates
	// nothing.
	mirror_.load(side_t::bid, live_book.bid_levels());
	mirror_.load(side_t::ask, live_book.ask_levels());

	commands_emitted_ += out.size() - before;
}

void depth_feed_bridge::diff_side(std::span<const level> was,
								  std::span<const level> now, side_t side,
								  std::vector<command> &out) const {
	// Best-first means descending for bids and ascending for asks, which is
	// the one place the two sides differ here.
	const auto comes_first = [side](price_t lhs, price_t rhs) noexcept {
		return side == side_t::bid ? lhs > rhs : lhs < rhs;
	};

	std::size_t old_at = 0;
	std::size_t new_at = 0;
	while (old_at < was.size() && new_at < now.size()) {
		const level &old_level = was[old_at];
		const level &new_level = now[new_at];
		if (old_level.price == new_level.price) {
			emit_delta(side,
					   old_level.price,
					   old_level.qty,
					   new_level.qty,
					   out);
			++old_at;
			++new_at;
		} else if (comes_first(old_level.price, new_level.price)) {
			// The venue no longer publishes this price at all.
			emit_delta(side, old_level.price, old_level.qty, 0, out);
			++old_at;
		} else {
			emit_delta(side, new_level.price, 0, new_level.qty, out);
			++new_at;
		}
	}
	for (; old_at < was.size(); ++old_at)
		emit_delta(side, was[old_at].price, was[old_at].qty, 0, out);
	for (; new_at < now.size(); ++new_at)
		emit_delta(side, now[new_at].price, 0, now[new_at].qty, out);
}

void depth_feed_bridge::emit_delta(side_t side, price_t price, quantity_t was,
								   quantity_t now,
								   std::vector<command> &out) const {
	if (now > was) out.push_back(command::add(symbol_, side, price, now - was));
	else if (now < was)
		out.push_back(command::reduce(symbol_, side, price, was - now));
}


void depth_feed_bridge::is_snapshot_failed() noexcept {
	reconstructor_.snapshot_failed();
}


} // namespace exchange::app