#include "order_book.hpp"

#include "detail/book_side.hpp"
#include "order_state.hpp"
#include "outcome.hpp"
#include "price_level.hpp"
#include "trade.hpp"
#include "trading-engine/orders/order.hpp"
#include "trading-engine/orders/types.hpp"

#include <algorithm>
#include <cassert>

namespace exchange::engine {

using detail::book_side;

namespace {

/// @brief How many level cells to take up front for a book sized to @p capacity
///        orders.
///
/// Levels are far fewer than orders — a book with a thousand resting orders
/// quotes tens of prices, not a thousand — so sizing the level pool like the
/// order pool would reserve a block that is mostly never touched. Overshooting
/// the hint chains another block rather than failing, so this only has to be
/// the right order of magnitude.
constexpr std::size_t level_hint(std::size_t capacity) {
	return std::max<std::size_t>(capacity / 8, 64);
}

} // namespace

order_book::order_book(std::size_t capacity)
	: pool_(capacity),
	  bid_(side_t::bid, pool_, level_hint(capacity)),
	  ask_(side_t::ask, pool_, level_hint(capacity)) {
	index_.reserve(capacity);
}

bool order_book::reject_if_invalid(const orders::order &incoming,
								   std::vector<order_outcome> &outcomes) const {
	// order_state has no representation for a non-positive order, so this is
	// the boundary that keeps the invariant true rather than merely asserted.
	if (incoming.qty <= 0) {
		if (incoming.id != kAnonymous)
			outcomes.push_back(
				order_outcome::rejected(incoming.id,
									   reject_reason::NON_POSITIVE_QUANTITY,
									   incoming.qty));
		return true;
	}

	// Nothing here watches a trigger price, and a stop order that goes live the
	// instant it arrives is not a stop order. Refusing is the only answer that
	// does not quietly turn one instruction into a different one.
	if (incoming.type == orders::order_type::STOP) {
		if (incoming.id != kAnonymous)
			outcomes.push_back(
				order_outcome::rejected(incoming.id,
									   reject_reason::UNSUPPORTED_ORDER_TYPE,
									   incoming.qty));
		return true;
	}

	// Admitting a duplicate id would overwrite index_[id], orphaning the first
	// order's node: it keeps resting and filling, but no cancel can ever reach
	// it. Refusing the second order is the only outcome that leaves every
	// resting order reachable.
	if (incoming.id != kAnonymous && index_.contains(incoming.id)) {
		outcomes.push_back(
			order_outcome::rejected(incoming.id,
								   reject_reason::DUPLICATE_ORDER_ID,
								   incoming.qty));
		return true;
	}
	return false;
}

void order_book::place_order(const orders::order &incoming,
							 std::vector<trade> &trades,
							 std::vector<order_outcome> &outcomes) {
	if (reject_if_invalid(incoming, outcomes)) return;

	book_side &opposite    = side_levels(opposed(incoming.side));
	const bool is_reported = incoming.id != kAnonymous;

	// Two instructions refuse to be filled in part, and they differ only in
	// what happens when the book cannot fill them whole: fill-or-kill
	// withdraws, all-or-none waits. Both must therefore ask the same question
	// first, and neither may enter the matching loop unless the answer is yes —
	// a partial execution is the one outcome both exist to rule out.
	const bool refuses_partial_fill =
		incoming.tif == orders::time_in_force_instruction::FILL_OR_KILL ||
		incoming.tif == orders::time_in_force_instruction::ALL_OR_NONE;
	const bool fillable_in_full =
		!refuses_partial_fill ||
		can_fully_fill(opposite, incoming.side, incoming.price, incoming.qty);

	if (incoming.tif == orders::time_in_force_instruction::FILL_OR_KILL &&
		!fillable_in_full) {
		if (is_reported)
			outcomes.push_back(
				order_outcome::rejected(incoming.id,
									   reject_reason::INSUFFICIENT_LIQUIDITY,
									   incoming.qty));
		return;
	}

	if (is_reported)
		outcomes.push_back(order_outcome::accepted(incoming.id, incoming.qty));

	// The aggressor's lifecycle. It outlives the matching loop: if a remainder
	// rests, this same state moves onto the pool node, so the order's traded
	// total keeps accumulating across the crossing and everything after it.
	order_state aggressor{incoming.qty};

	// An all-or-none that cannot be filled whole skips crossing altogether and
	// goes straight to resting. Entering the loop would fill it in part, which
	// is the single thing the instruction forbids.
	while (fillable_in_full && aggressor.remaining() > 0 && !opposite.empty()) {
		price_level &best = opposite.best();
		if (!is_price_crossing(incoming.side, incoming.price, best.price))
			break;

		while (aggressor.remaining() > 0 && !best.has_empty_orders()) {
			detail::resting_order &resting = best.front();
			const order_id_t resting_id    = resting.id();
			const quantity_t traded =
				std::min(aggressor.remaining(), resting.qty());

			trades.emplace_back(incoming.id, resting_id, best.price, traded);
			aggressor.apply_fill(traded);
			// Through the level, so its cached aggregate tracks the fill; the
			// reference stays valid, it is the same node in the same place.
			best.fill_front(traded);

			// Read the passive side's state before pop_front returns its cell
			// to the pool — after that the reference is dangling.
			if (resting_id != kAnonymous)
				outcomes.push_back(
					order_outcome::fill(resting_id, resting.state()));
			if (is_reported)
				outcomes.push_back(order_outcome::fill(incoming.id, aggressor));

			if (!resting.has_quantity()) pop_front(best);
		}
		opposite.remove_best_level_if_empty();
	}

	if (aggressor.remaining() == 0) return;

	// GTC and all-or-none rest a remainder — the second by definition, since it
	// "stays on the book until it is finished or cancelled", and what rests is
	// its whole quantity because it never filled in part. IOC (and a
	// partially-filled FOK, which the pre-check rules out) drop it.
	reject_reason dropped_because = reject_reason::TIME_IN_FORCE;
	if (incoming.tif ==
			orders::time_in_force_instruction::GOOD_TILL_CANCELLED ||
		incoming.tif == orders::time_in_force_instruction::ALL_OR_NONE) {
		book_side &own     = side_levels(incoming.side);
		price_level *level = own.insert(incoming.id, incoming.price, aggressor);
		if (level != nullptr) {
			if (is_reported)
				index_[incoming.id] =
					detail::order_location{incoming.side, level,
										   &level->orders.back()};
			return;
		}
		// The pools are out of cells, so there is nowhere to rest what did not
		// cross. Whatever executed stands — the trades are printed and the
		// fills reported — and the remainder is withdrawn with a reason that
		// says the book, not the order, is why.
		dropped_because = reject_reason::BOOK_AT_CAPACITY;
	}

	if (is_reported) {
		// A dropped remainder is a cancellation with a cause, not a rejection:
		// the order was accepted and may well have executed first.
		order_state dropped = aggressor;
		dropped.cancel();
		outcomes.push_back(
			order_outcome::cancelled(incoming.id, dropped, dropped_because));
	}
}

void order_book::place_order(const orders::order &incoming,
							 std::vector<trade> &trades) {
	std::vector<order_outcome> discarded;
	place_order(incoming, trades, discarded);
}

std::vector<trade> order_book::place_order(const orders::order &incoming) {
	std::vector<trade> trades;
	place_order(incoming, trades);
	return trades;
}

void order_book::add_order(side_t side, price_t price, quantity_t volume) {
	// Anonymous resting liquidity: no id (untracked for cancel), no matching.
	// Nobody placed it, so an exhausted pool has no one to report to — the
	// liquidity simply does not appear.
	const price_level *rested =
		side_levels(side).insert(orders::order{.id    = kAnonymous,
											   .side  = side,
											   .price = price,
											   .qty   = volume});
	assert(rested != nullptr && "order pool exhausted seeding liquidity");
	(void)rested;
}

void order_book::for_each_resting(resting_visitor visit) const {
	// Bids then asks, each side best-first because that is the ladder's own order,
	// and oldest-first within a level because that is the FIFO's. The result is
	// exactly fill order — and restore_order appends, so handing this output back
	// to it rebuilds every queue as it was. @see for_each_resting's contract
	for (const side_t side : {side_t::bid, side_t::ask}) {
		const detail::book_side &levels = side_levels(side);
		for (const price_level &level : levels)
			for (const detail::resting_order &order : level.orders)
				visit(resting_view{.id    = order.id(),
								   .state = order.state(),
								   .price = level.price,
								   .side  = side});
	}
}

bool order_book::restore_order(const resting_view &order) {
	// Nothing left to rest is not an error to report, it is a record that should
	// not have been written — a terminal order has no place in a book snapshot,
	// because the book has no representation for one.
	if (order.state.remaining() <= 0) return false;

	// The same rule place_order enforces, for the same reason: a second entry for
	// one id would overwrite index_[id] and orphan the first node, leaving an
	// order that rests and fills but that no cancel can reach. Anonymous
	// liquidity is exempt because it is never indexed at all.
	const bool reported = order.id != kAnonymous;
	if (reported && index_.contains(order.id)) return false;

	detail::book_side &own = side_levels(order.side);
	price_level *level     = own.insert(order.id, order.price, order.state);
	if (level == nullptr) return false; // pools exhausted; nowhere to put it

	// insert appends, so restoring a level's orders in the order for_each_resting
	// produced them rebuilds that level's FIFO exactly. @see for_each_resting
	if (reported)
		index_[order.id] = detail::order_location{order.side,
												 level,
												 &level->orders.back()};
	return true;
}

void order_book::cancel_order(order_id_t id,
							  std::vector<order_outcome> &outcomes) {
	const auto found = index_.find(id);
	if (found == index_.end()) {
		// The fill/cancel race, resolved in the fill's favour: the order filled
		// and left before this request landed — or was already cancelled, or
		// never existed. One empty index probe for all three, so the report
		// says only that the cancel could not be applied.
		outcomes.push_back(
			order_outcome::cancel_rejected(id, reject_reason::UNKNOWN_ORDER));
		return;
	}

	const auto [side, level, node] = found->second;
	assert(level != nullptr && node != nullptr && "index entry names no order");

	// Cancel the node's state before unlinking so the outcome carries what it
	// executed: a cancellation withdraws the remainder and freezes the rest, it
	// does not undo the fills.
	node->cancel();
	outcomes.push_back(order_outcome::cancelled(id, node->state()));

	// The location carries the node, so this is a splice, not a search.
	level->unlink(pool_, *node);
	if (level->has_empty_orders()) side_levels(side).erase(level->price);
	index_.erase(found);
}

void order_book::cancel_order(order_id_t id) {
	std::vector<order_outcome> discarded;
	cancel_order(id, discarded);
}

void order_book::delete_order(side_t side, price_t price, volume_t volume) {
	book_side &levels  = side_levels(side);
	price_level *level = levels.find(price);
	if (level == nullptr) return;

	auto node      = level->orders.begin();
	const auto end = level->orders.end();
	while (volume > 0 && node != end) {
		// Anonymous depth only. An identified order belongs to a client and is
		// withdrawn by cancel_order, which reports; a reduction carries no
		// identity and emits nothing, so draining one here would destroy an order
		// the venue's record store still believes is live and tell nobody. That
		// is the same class of bug as the set_level use-after-free this helper
		// outlived — walk past it instead.
		if (node->id() != kAnonymous) {
			++node;
			continue;
		}

		detail::resting_order &anonymous = *node;
		// The reduction is a volume_t and the node's remainder a quantity_t, so
		// the comparison happens wide and the result narrows only once it is
		// known to be bounded by qty().
		const auto take =
			static_cast<quantity_t>(std::min<volume_t>(volume, anonymous.qty()));
		level->fill(anonymous, take);
		volume -= take;
		if (anonymous.has_quantity()) continue; // partial: volume is spent

		// Step off the node before unlinking it. safe_link zeroes a node's hooks
		// on removal, so an iterator still sitting on this one could not advance
		// afterwards.
		//
		// unlink, not pop_front: this walk does not always stand at the head, and
		// unlink is the splice that works anywhere. Note what that gives up —
		// pop_front clears the node's index_ entry and unlink does not, because
		// its other caller (cancel_order) erases the entry itself. Sound here
		// only because the identity check above means every node reaching this
		// line is anonymous and therefore was never indexed. Delete that check
		// and this becomes a use-after-free: index_ would keep naming a cell the
		// pool has taken back, and the next cancel_order for that id would
		// unlink a recycled node. That is the set_level bug, reintroduced.
		++node;
		level->unlink(pool_, anonymous);
	}
	if (level->has_empty_orders()) levels.erase(price);
}

volume_t order_book::volume_at_price(price_t price, side_t side) const {
	return side_levels(side).volume_at_price(price);
}

std::optional<price_t> order_book::best_bid() const {
	return bid_.best_price();
}

std::optional<price_t> order_book::best_ask() const {
	return ask_.best_price();
}

book_side &order_book::side_levels(side_t s) {
	return s == side_t::bid ? bid_ : ask_;
}

const book_side &order_book::side_levels(side_t s) const {
	return s == side_t::bid ? bid_ : ask_;
}

void order_book::pop_front(price_level &level) {
	const order_id_t id = level.front().id();
	if (id != kAnonymous) index_.erase(id);
	level.pop_front(pool_);
}

void order_book::clear() noexcept {
	bid_.clear();
	ask_.clear();
	// Cleared alongside the sides, never on its own: the entries name nodes the
	// sides just released, so an index outliving them would hand cancel_order a
	// pointer into a free cell.
	index_.clear();
}

bool order_book::is_price_crossing(side_t side, price_t price,
								   price_t book_price) {
	// A bid crosses an ask priced at or below it; an ask crosses a bid priced
	// at or above it.
	return side == side_t::bid ? price >= book_price : price <= book_price;
}

bool order_book::can_fully_fill(const book_side &opposite, side_t side,
								price_t price, volume_t volume) const {
	// volume_t throughout: this walks every crossing level and adds their
	// aggregates together, so it is the one accumulator in the book most able
	// to exceed a single order's range. A quantity_t here would wrap on a deep
	// book and report a fill-or-kill as fillable when it is not.
	volume_t available = 0;
	for (const price_level &level : opposite) {
		if (!is_price_crossing(side, price, level.price)) break;
		available += level.total_volume();
		if (available >= volume) return true;
	}
	return false;
}

} // namespace exchange::engine
