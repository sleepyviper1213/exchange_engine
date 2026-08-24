#pragma once
// The only part of a backtest that is not simply "run the real code".
//
// Everything else in the harness is the shipped engine: the same order_book,
// the same matching_engine, the same risk gate. This file is where a judgement
// has to be made that the recording cannot answer on its own - when a resting
// order of ours would have been filled - and it is therefore the file to read
// before believing any number a backtest produces.

#include "detail/our_level.hpp"
#include "fwd.hpp"
#include "market-data/l2_book.hpp"
#include "queue_position.hpp"
#include "event/command.hpp"
#include "execution/order_manager.hpp"
#include "order_book/order_state.hpp"
#include "orders/order.hpp"
#include "orders/time_in_force_instruction.hpp"
#include "orders/types.hpp"
#include "symbol/symbol_spec.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace exchange::strategy::backtest {

/// @brief How generous the passive fill inference is allowed to be.
struct fill_model_options {
	/**
	 * @brief Require the venue to trade *through* our price, not merely to it.
	 *
	 * With this set - the default, and the conservative reading - a resting bid
	 * at P fills only once the venue publishes an offer *below* P. A venue
	 * offer exactly at P locks the market and is not, on its own, evidence that
	 * our order traded: we would have been behind whatever was already queued
	 * there, and the rule has no way to ask how much of that queue was left.
	 *
	 * Clearing it fills on a locked market too, which is the right choice when
	 * the strategy under test quotes a venue whose feed conflates a momentary
	 * lock with a print. It is strictly more optimistic; every number
	 * downstream inherits that.
	 *
	 * @note This rule and @c model_queue_position overlap on purpose, and the
	 *       overlap is worth understanding before either is changed. Requiring
	 *       a trade-through is a *proxy* for queue position - it refuses the
	 *       case where being behind somebody is most likely to matter, because
	 *       it has no way to ask. With the queue modelled the question can be
	 *       asked directly, and clearing this becomes a defensible choice
	 *       rather than pure optimism: an order that has reached the front of
	 *       its queue really should fill on a locked market. Both default to
	 *       the pessimistic setting regardless, because a backtest's defaults
	 *       should not flatter anything.
	 */
	bool require_trade_through = true;

	/**
	 * @brief Account for the venue's own liquidity resting ahead of ours.
	 *
	 * With this set - the default - a resting order does not fill until the
	 * volume that reached its price has first paid down whatever the venue was
	 * publishing there when we joined. Clear it and every order of ours is
	 * treated as first in line at its price the instant it is placed, which is
	 * how this harness behaved before the queue existed and is the single most
	 * flattering assumption available to a backtest.
	 *
	 * @see queue_position_book, for what the estimate is derived from and the
	 *      one optimism it still carries.
	 */
	bool model_queue_position = true;
};

/**
 * @brief Infers what the venue's published depth must have executed against our
 *        resting orders, and injects it as ordinary aggressing flow.
 *
 * @tparam Sink Where forwarded commands continue to - @c engine_partition, in
 *         the harness. This type is itself a @c strategy::command_sink, so it
 *         splices into the chain the way a second gate would.
 *
 * @par The problem this exists to solve
 * A diff-depth capture is a record of *quotes*, not of *prints*. It says what
 * the venue was showing, frame by frame, and nothing about who traded with
 * whom. Our aggressive orders need no help - they cross the mirrored depth in
 * the real @c order_book and fill at real published prices for real published
 * sizes, which is as honest as a simulation gets. Our *passive* orders are the
 * hard half: nothing in the recording will ever aggress against them, so
 * without a model a market-making strategy backtests to exactly zero fills.
 *
 * @par The model: fill on a trade-through
 * The one thing a depth-only feed does say about executions is that when the
 * venue's offer drops below our resting bid, somebody was willing to sell lower
 * than we were willing to buy - and in a continuous-matching venue our order,
 * being the better bid, is what they would have hit. So:
 *
 * > a resting bid at P fills against venue ask liquidity offered below P, up to
 * > the size the venue actually published there.
 *
 * and its mirror image for asks. The fill is executed by placing an *anonymous*
 * (id 0) immediate-or-cancel order on the opposite side at exactly P and
 * letting the real matching engine do the rest. That choice is load-bearing:
 *
 * - the execution goes through @c order_book::place_order, so price-time
 *   priority between two of our own orders is decided by the book that will
 *   decide it in production rather than by anything written here;
 * - the fill prints at P, our own limit, which is what a passive order gets;
 * - id 0 produces no @c order_outcome and takes no @c order_manager record, so
 *   the venue's side of the trade stays out of our order flow entirely, and a
 *   passive fill is recognisable downstream by @c trade::aggressor being 0.
 *
 * @par Why the injected order can only ever hit our own resting orders
 * Worth stating, because it is what makes the model safe rather than merely
 * plausible. The engine's book holds our orders plus the anonymous mirror of
 * the venue's depth. An aggressor selling at limit P can reach bids at P and
 * above. Every anonymous bid in the book is one the venue published, and the
 * venue's own book is not crossed - the reconstructor tears down a replica that
 * is (@c reconstructor_options::resync_on_cross) - so every anonymous bid sits
 * strictly below the venue's best ask, which is at or below P by the crossing
 * test that got us here. There is therefore no anonymous bid at or above P for
 * the aggressor to reach, and it lands on our orders or on nothing.
 *
 * @par Queue position, which it does model
 * Volume that reaches one of our prices pays down the venue liquidity that was
 * already resting there before any of it fills us. @c queue_position_book holds
 * that estimate and states exactly what it is derived from; the two things
 * worth knowing here are that the estimate is per price rather than per order,
 * and that it survives across events, so a resting order works its way forward
 * over a run rather than starting each frame at the back of the queue. Clearing
 * @c fill_model_options::model_queue_position restores the front-of-queue
 * behaviour this file had before.
 *
 * @par What it does not model
 * - **Market impact.** Our fills do not remove the venue's liquidity, in either
 *   direction. Depth an aggressive order of ours consumed is restored by the
 *   next diff (@c depth_feed_bridge::consumed), and depth a passive fill traded
 *   against is never touched. Both say "we were small", which is the standard
 *   backtest assumption and the standard reason a backtest flatters size.
 * - **Latency**, which is not this file's to model either way: a command
 *   reaches the engine when @c wire hands it over, and this model only ever
 *   sees an order the engine has already been told about.
 * - **An order left resting *through* the touch.** The budget below is released
 *   once per event, so a bid sitting above the venue's best offer fills against
 *   that offer again on the very next frame, and the frame after that. This is
 *   not a bug in the budget, it is what a depth feed means: standing liquidity
 *   is *republished*, not retired, so the replica goes on showing an offer the
 *   model has no way to know was taken. In a real market the state cannot last
 * - one side or the other moves within a tick - and a strategy that quotes
 *   *inside* the spread rather than through it never enters it. Read a run
 * whose passive fills grow linearly with the event count as the strategy having
 *   quoted through the market, not as a discovery.
 *
 * @note Offline tooling, and priced as such: @c infer sorts a small vector and
 *       walks the replica per price level. It is not on any latency budget and
 *       must not grow one - the moment it is fast rather than obviously correct
 *       it stops being auditable.
 */
template <class Sink>
class crossing_fill_model {
public:
	using command = engine::event::command;

	/**
	 * @brief Build the model over @p sink for @p spec's listing.
	 * @param sink Where forwarded commands go. Must outlive the model.
	 * @param spec The listing. Supplies the tick and lot grid the replica's
	 *        scaled decimals are compared and converted on. Must outlive the
	 *        model.
	 * @param options The conservatism knobs. @see fill_model_options
	 */
	crossing_fill_model(Sink &sink, const engine::symbol_spec &spec,
						fill_model_options options = {}) noexcept
		: sink_(&sink), spec_(&spec), symbol_(spec.id()), options_(options) {}

	// --- the sink side: watch what we place, then forward ------------------

	/**
	 * @brief Forward @p batch, and remember every order it places.
	 *
	 * The model has to know which resting orders are ours, and the honest place
	 * to learn it is the command stream - the same stream the partition sees,
	 * so the two cannot disagree. Only the *ids* are kept: price, side and
	 * remaining quantity are read back from the partition's @c order_manager,
	 * which is the venue's own record and is updated by the matching engine
	 * rather than by anything here. There is no second state machine to drift.
	 *
	 * @return What the sink said. Nothing is recorded on a refusal, so the
	 *         all-or-nothing contract the gate's rollback depends on survives.
	 */
	[[nodiscard]] bool submit_range(std::span<const command> batch) {
		if (!sink_->submit_range(batch)) return false;
		for (const command &cmd : batch) {
			if (cmd.type != command::Type::PLACE) continue;
			if (const order_id_t id = cmd.as_place().id; id != 0)
				working_.push_back(id);
		}
		return true;
	}

	/// @brief @c submit_range for one command.
	[[nodiscard]] bool submit(const command &cmd) {
		return submit_range(std::span<const command>{&cmd, 1});
	}

	// --- the model ---------------------------------------------------------

	/// @brief Begin a new feed event, releasing the liquidity budget the last
	///        one consumed. @see infer on why the budget exists.
	void open_step() noexcept {
		consumed_[0] = 0;
		consumed_[1] = 0;
	}

	/**
	 * @brief Append the aggressing orders the replica implies must have
	 *        executed against our resting orders.
	 *
	 * @param replica The venue's published depth, as reconstructed. Read only.
	 * @param orders The partition's record store, for our orders' current
	 *        price, side and remaining quantity.
	 * @param out Commands are appended, never cleared. They are the *venue's*
	 *        flow and go straight to the partition - not back through this
	 *        model, and not through the risk gate, neither of which has any
	 *        business screening somebody else's order.
	 * @return How many commands were appended.
	 *
	 * @par The liquidity budget, and why it is per feed event
	 * The venue published a size once; we may not fill against it twice. But
	 * @c infer runs repeatedly within one event - the harness settles to a
	 * fixed point, because a fill can make a strategy quote again and the new
	 * quote may itself be crossed - and the replica does not move between those
	 * calls. So the volume already taken is carried in @c consumed_ and
	 * subtracted from what is available, and only @c open_step releases it.
	 * Without that, a partial fill would re-inspect the same untouched depth on
	 * the next round and fill again, and the harness would not terminate.
	 *
	 * The budget is one number per side rather than one per price because the
	 * available volume is *monotone*: liquidity offered below a worse price is
	 * a subset of that offered below a better one. Walking our prices
	 * best-first and drawing from a single running total is therefore exact,
	 * and the moment the total is exhausted no worse price can fill either -
	 * which is why the loop breaks rather than continues.
	 */
	std::size_t infer(const market_data::l2_book &replica,
					  const engine::execution::order_manager &orders,
					  std::vector<command> &out) {
		const std::size_t before = out.size();
		infer_side(side_t::bid, replica, orders, out);
		infer_side(side_t::ask, replica, orders, out);
		return out.size() - before;
	}

	/**
	 * @brief Abandon every queue-position estimate.
	 *
	 * For a feed gap: the estimates were all measured against a replica that no
	 * longer exists. @see queue_position_book::clear for why re-measuring is
	 * the conservative choice rather than merely the simple one.
	 */
	void reset_queue() noexcept { queue_.clear(); }

	/**
	 * @brief Drop the orders the venue has finished with.
	 *
	 * Filled, cancelled and rejected orders leave @c order_manager's live
	 * population, and this is what stops the working list growing for the
	 * length of a run. Called once per settled event rather than per outcome:
	 * the record store already knows, so there is nothing to be gained by
	 * reconstructing the same answer from the outcome stream.
	 */
	void retire_finished(const engine::execution::order_manager &orders) {
		const auto finished = [&orders](order_id_t id) {
			const auto *record = orders.find_record(id);
			return record == nullptr || !is_active(*record);
		};
		working_.erase(
			std::remove_if(working_.begin(), working_.end(), finished),
			working_.end());
	}

	// --- what a report reads -----------------------------------------------

	/// @brief Orders of ours the model believes are still resting.
	[[nodiscard]] std::size_t working() const noexcept {
		return working_.size();
	}

	/// @brief Aggressing orders injected since construction.
	[[nodiscard]] std::uint64_t injected() const noexcept { return injected_; }

	/// @brief Lots those orders offered - an upper bound on what they filled,
	///        since the book may hold less than the model thought.
	[[nodiscard]] volume_t injected_lots() const noexcept {
		return injected_lots_;
	}

	/// @brief The conservatism the run was measured under.
	[[nodiscard]] const fill_model_options &options() const noexcept {
		return options_;
	}

	/// @brief The queue estimates, for what they have absorbed and how many
	///        prices they cover.
	[[nodiscard]] const queue_position_book &queue() const noexcept {
		return queue_;
	}

private:
	/// @brief Would venue liquidity at @p venue_scaled trade with an order of
	///        ours on @p side priced at @p ours_scaled?
	[[nodiscard]] bool
	crosses(side_t side, market_data::scaled_price_t venue_scaled,
			market_data::scaled_price_t ours_scaled) const noexcept {
		if (venue_scaled == ours_scaled) return !options_.require_trade_through;
		return side == side_t::bid ? venue_scaled < ours_scaled
								   : venue_scaled > ours_scaled;
	}

	/**
	 * @brief Is an order of ours on @p side at @p ours_scaled still strictly
	 *        behind the venue's touch at @p touch?
	 *
	 * Deliberately not expressed through @c crosses: that rule carries the
	 * @c require_trade_through knob, and whether a published size is evidence
	 * about the queue has nothing to do with how generous the fill rule is. A
	 * missing touch means the opposite side is empty, so nothing has reached
	 * us.
	 */
	[[nodiscard]] static bool
	is_behind_touch(side_t side,
					std::optional<market_data::scaled_price_t> touch,
					market_data::scaled_price_t ours_scaled) noexcept {
		if (!touch.has_value()) return true;
		return side == side_t::bid ? *touch > ours_scaled
								   : *touch < ours_scaled;
	}

	/// @brief A scaled venue size in whole lots, rounded down.
	/// @note Truncating, not rejecting: this is an aggregate of the venue's
	///       depth being used as a *bound*, not an order quantity, so a size
	///       that is not on our lot grid should shrink to the largest one that
	///       is rather than refuse the whole level.
	[[nodiscard]] volume_t
	lots_floor(market_data::scaled_qty_t scaled) const noexcept {
		if (scaled <= 0) return 0;
		return scaled / spec_->lot_scaled();
	}

	/// @brief Our resting orders on @p side, aggregated by price, best first.
	void collect(side_t side, const engine::execution::order_manager &orders) {
		levels_.clear();
		for (const order_id_t id : working_) {
			const auto *record = orders.find_record(id);
			if (record == nullptr || !is_active(*record)) continue;
			if (record->side != side) continue;
			const quantity_t left = record->state.remaining();
			if (left <= 0) continue;

			const auto at =
				std::find_if(levels_.begin(),
							 levels_.end(),
							 [&](const detail::our_level &l) noexcept {
								 return l.price == record->price;
							 });
			if (at == levels_.end())
				levels_.push_back({.price = record->price, .lots = left});
			else at->lots += left;
		}
		// Best first: the highest bid and the lowest ask are the ones the venue
		// reaches first, and the loop's early break depends on that ordering.
		std::sort(levels_.begin(),
				  levels_.end(),
				  [side](const detail::our_level &lhs,
						 const detail::our_level &rhs) noexcept {
					  return side == side_t::bid ? lhs.price > rhs.price
												 : lhs.price < rhs.price;
				  });
	}

	/**
	 * @brief Reconcile the queue estimates on @p side against what we now hold.
	 *
	 * @param replica The venue's depth. Read on @p side - *our* side, the one
	 *        our orders are queued on - which is the opposite of the side the
	 *        fill loop reads.
	 *
	 * @par Which of our prices get measured this frame
	 * Only the ones still behind the venue's touch. A price the venue's
	 * opposite side has reached is one the market has moved through, and the
	 * level we were queued behind is missing for that reason rather than
	 * because anybody cancelled - so the last measurement stands, and it is
	 * exactly the queue the trade had to clear on its way to us. Measuring
	 * anyway would zero every estimate on the one frame it is needed, because a
	 * replica in sequence is never crossed *or locked* (@c l2_book::is_crossed
	 * counts both) and so never publishes at our price while trading through
	 * it. @see queue_position_book, which states the same boundary from the
	 * other side.
	 *
	 * @note Skipped entirely against a replica publishing nothing. A
	 *       reconstruction that has been torn down is the absence of evidence
	 *       rather than evidence of an empty queue, and reading it as the
	 * latter would put us at the front of every price we hold for free. The
	 *       session calls @c reset_queue on the gap itself. @see
	 *       queue_position_book::clear
	 */
	void reconcile_queue(side_t side, const market_data::l2_book &replica) {
		if (!options_.model_queue_position) return;
		if (replica.bid_levels().empty() && replica.ask_levels().empty())
			return;

		const std::optional<market_data::scaled_price_t> touch =
			side == side_t::bid ? replica.best_ask() : replica.best_bid();

		queue_.open_side(side);
		for (const detail::our_level &ours : levels_) {
			const auto ours_scaled = spec_->price_to_scaled(ours.price);
			if (is_behind_touch(side, touch, ours_scaled))
				queue_.track(
					side,
					ours.price,
					lots_floor(replica.volume_at_price(ours_scaled, side)));
			else queue_.hold(side, ours.price);
		}
		queue_.close_side(side);
	}

	/// @brief The half of @c infer that runs for one of our sides.
	/// @param side The side *our* orders are on.
	/// @param replica The venue's depth. Our orders trade against its opposite
	///        side and queue behind its own.
	void infer_side(side_t side, const market_data::l2_book &replica,
					const engine::execution::order_manager &orders,
					std::vector<command> &out) {
		collect(side, orders);
		// Before the early returns below: a level we have left must be dropped
		// on the event we leave it, or coming back to it later would inherit a
		// queue position we paid for on the previous visit.
		reconcile_queue(side, replica);

		const std::span<const market_data::l2_book::price_level> venue =
			side == side_t::bid ? replica.ask_levels() : replica.bid_levels();
		if (venue.empty() || levels_.empty()) return;

		volume_t &consumed = consumed_[side == side_t::bid ? 0 : 1];

		for (const detail::our_level &ours : levels_) {
			const auto ours_scaled = spec_->price_to_scaled(ours.price);

			// Everything the venue offered at a price that would have traded
			// with this one. The walk stops at the first level that would not:
			// the side is sorted best-first, so no later level can qualify.
			volume_t offered = 0;
			for (const auto &[price, qty] : venue) {
				if (!crosses(side, price, ours_scaled)) break;
				offered += lots_floor(qty);
			}

			volume_t room = offered - consumed;
			// Monotone: a worse price of ours is crossed by a subset of this
			// liquidity, so an exhausted budget here is exhausted below too.
			if (room <= 0) break;

			// The orders that were already queued at this price are hit first,
			// and what they take is spent - it cannot also fill a worse price
			// of ours, which is why it draws on the same budget. The monotone
			// argument above therefore still licenses the break.
			if (options_.model_queue_position) {
				const volume_t paid = queue_.absorb(side, ours.price, room);
				consumed += paid;
				room -= paid;
				if (room <= 0) break;
			}

			const volume_t take = std::min(ours.lots, room);
			const auto qty      = static_cast<quantity_t>(
				std::min<volume_t>(take,
								   std::numeric_limits<quantity_t>::max()));
			if (qty <= 0) continue;

			out.push_back(command::place(engine::orders::order{
				.id        = 0, // anonymous: the venue's order, not ours
				.symbol_id = symbol_,
				.side      = opposed(side),
				.tif       = engine::orders::time_in_force_instruction::
					IMMEDIATE_OR_CANCEL,
				.price = ours.price,
				.qty   = qty,
			}));
			consumed += qty;
			++injected_;
			injected_lots_ += qty;
		}
	}

	Sink *sink_;
	const engine::symbol_spec *spec_;
	symbol_id_t symbol_;
	fill_model_options options_;

	/// Ids of orders we have submitted and the venue has not finished with.
	/// Their state lives in @c order_manager; only the identity is ours.
	std::vector<order_id_t> working_;
	/// Scratch for @c collect, reused so a settle loop does not allocate per
	/// round. Rebuilt on every call.
	std::vector<detail::our_level> levels_;
	/// Venue lots already filled against, this event, per side of ours:
	/// [0] bids, [1] asks. @see infer
	std::array<volume_t, 2> consumed_{0, 0};
	/// How much of the venue's own liquidity is still in front of ours, per
	/// price. Persistent across events, unlike @c consumed_ - the whole point
	/// of it is that a resting order makes progress. @see queue_position_book
	queue_position_book queue_;

	std::uint64_t injected_ = 0;
	volume_t injected_lots_ = 0;
};

} // namespace exchange::strategy::backtest
