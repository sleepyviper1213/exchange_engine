#include "venue_legs.hpp"

#include <algorithm>
#include <cassert>

namespace exchange::session {
namespace {

/// Fold @p chain's finished, no-longer-live legs into its settled sum.
///
/// Folding is what keeps a long-lived quote from accumulating a slot per
/// amendment. Its one cost: a terminal report delivered *again* for a leg
/// already folded is booked as a fresh leg. The venue's account stream is
/// ordered per order and does not replay, so that is a duplicate this process
/// cannot be sent rather than one it has to defend against.
void fold_finished(venue_leg_chain &chain) noexcept {
	std::erase_if(chain.open_legs,
				  [&chain](const venue_leg_chain::leg_fill &fill) {
					  if (!fill.is_done || chain.is_live(fill.leg))
						  return false;
					  chain.settled += fill.cumulative;
					  return true;
				  });
}

} // namespace

quantity_t venue_leg_chain::traded() const noexcept {
	quantity_t total = settled;
	for (const leg_fill &fill : open_legs) total += fill.cumulative;
	return total;
}

void venue_legs::open(order_id_t id, side_t side,
					  engine::orders::time_in_force_instruction tif) {
	// The quoter never reuses an id, so a chain already here is a stale one
	// for an order the venue never confirmed; the fresh placement is the one
	// its reports will name.
	forget(id);
	chains_.push_back(venue_leg_chain{.id = id, .side = side, .tif = tif});
}

venue_leg_chain *venue_legs::find(order_id_t id) noexcept {
	const auto at = std::ranges::find(chains_, id, &venue_leg_chain::id);
	return at == chains_.end() ? nullptr : &*at;
}

const venue_leg_chain *venue_legs::find(order_id_t id) const noexcept {
	const auto at = std::ranges::find(chains_, id, &venue_leg_chain::id);
	return at == chains_.end() ? nullptr : &*at;
}

venue_leg_t venue_legs::working_leg(order_id_t id) const noexcept {
	const venue_leg_chain *chain = find(id);
	return chain == nullptr ? 0 : chain->working;
}

std::optional<quantity_t> venue_legs::on_report(order_id_t id, venue_leg_t leg,
												quantity_t cumulative,
												bool is_terminal) {
	venue_leg_chain *chain = find(id);
	if (chain == nullptr) return std::nullopt;

	auto at = std::ranges::find(chain->open_legs,
								leg,
								&venue_leg_chain::leg_fill::leg);
	if (at == chain->open_legs.end()) {
		chain->open_legs.push_back({.leg = leg, .cumulative = cumulative});
		at = std::prev(chain->open_legs.end());
	}
	at->cumulative         = std::max(at->cumulative, cumulative);
	at->is_done            = at->is_done || is_terminal;
	const quantity_t total = chain->traded();

	// The working leg finishing with no replace in flight is the order
	// finishing. With one in flight it is either the replace's own withdrawal
	// of this leg or the fill that will make the replace fail, and only the
	// answer can say which - so the chain stays until it arrives.
	if (is_terminal && leg == chain->working && !chain->replacing.has_value())
		forget(id);
	else fold_finished(*chain);
	return total;
}

venue_leg_t venue_legs::begin_replace(order_id_t id) noexcept {
	venue_leg_chain *chain = find(id);
	assert(chain != nullptr && !chain->replacing.has_value() &&
		   "one replace in flight per order");
	// Never reused within an order, not even after a replace that failed: an
	// answer read as `unchanged` may have been a body we could not decode, and
	// the venue refuses an id still attached to a working order.
	chain->replacing = chain->next_leg++;
	return *chain->replacing;
}

void venue_legs::resolve(order_id_t id, replace_answer answer) noexcept {
	venue_leg_chain *chain = find(id);
	if (chain == nullptr || !chain->replacing.has_value()) return;

	switch (answer) {
	case replace_answer::replaced:
	case replace_answer::unanswered: chain->working = *chain->replacing; break;
	case replace_answer::unchanged: break;
	case replace_answer::order_gone:
	case replace_answer::withdrawn: forget(id); return;
	}
	chain->replacing.reset();

	const auto at = std::ranges::find(chain->open_legs,
									  chain->working,
									  &venue_leg_chain::leg_fill::leg);
	if (at != chain->open_legs.end() && at->is_done) {
		forget(id);
		return;
	}
	fold_finished(*chain);
}

void venue_legs::forget(order_id_t id) noexcept {
	std::erase_if(chains_, [id](const venue_leg_chain &chain) {
		return chain.id == id;
	});
}

} // namespace exchange::session
