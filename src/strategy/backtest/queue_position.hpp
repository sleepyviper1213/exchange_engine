#pragma once
// How much of the venue's own liquidity sits in front of ours at our price.
//
// The single most flattering assumption a backtest can make is that it was at
// the front of the queue. This is what it costs to stop making it.

#include "fwd.hpp"
#include "orders/types.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace exchange::strategy::backtest {

/**
 * @brief The venue lots queued ahead of our own, per price we are resting at.
 *
 * @par What it is for
 * A price level is a FIFO. An order joining it goes to the back, and every lot
 * already resting there trades before any of ours does. Ignore that and a
 * resting order fills the instant the market reaches its price, which is the
 * behaviour of an order that was always first in line - a position no strategy
 * gets for free and most never get at all. On a busy level it is the difference
 * between a strategy that makes money and one that only appears to.
 *
 * @par The one thing a depth feed does say
 * At the instant we join price P, the size the venue publishes at P is exactly
 * the liquidity ahead of us: everything resting there arrived before we did,
 * and nothing has yet arrived after. So the queue is knowable at join time,
 * exactly, with no assumption at all. @c track records it.
 *
 * @par And the one inference it supports afterwards
 * Thereafter the published size is @c ahead + @c behind, because orders keep
 * joining after us. @c behind is never negative, so a published size *below*
 * the queue we recorded can only mean the queue itself has shrunk - to at most
 * what is still published. Tightening our estimate to it is therefore a
 * deduction and not a guess, and @c track applies it on every observation.
 *
 * The bound is loose once a level has built up behind us, and that is the
 * honest state of the information: it binds exactly when the level empties out,
 * which is when it matters.
 *
 * @par When an observation is not evidence, and @c hold instead
 * The deduction above holds only while our price is a price the venue could
 * still be quoting. Once the venue's touch has come to or through us, the level
 * we were queued behind is gone *because the market moved past it*, and reading
 * that as "the queue ahead was cancelled" would zero the estimate on precisely
 * the frame it is about to be needed - which is not a subtle failure, it is
 * total: @c l2_book counts a locked book as crossed, so a replica in sequence
 * never publishes at our price and on the far side of it at the same time.
 * Applied unconditionally, the tightening would therefore guarantee an empty
 * queue at every fill and this class would do nothing at all.
 *
 * So the caller measures while our price is still behind the venue's touch and
 * holds the estimate once it is not. What the trade-through then consumes is
 * the queue as last measured, which is the queue the trade actually had to go
 * through. @see crossing_fill_model::reconcile_queue
 *
 * @par What is deliberately not inferred
 * **Which part of a decrease was a fill and which was a cancel.** A diff-depth
 * capture records quotes, not prints - it never says a trade happened, only
 * that a size changed - so the two are indistinguishable in the input. The
 * usual patch is to model the participants ahead of us cancelling at some rate,
 * and it is a patch worth refusing: it advances our queue position on an
 * assumption rather than an observation, and it turns the run's answer into a
 * draw from a distribution when the whole point of the harness is that the
 * answer is a function of the capture. The cap above is what the recording
 * actually supports. Everything past it needs a trade feed, which is a change
 * to what gets captured rather than to anything here.
 *
 * @par The optimism that remains
 * Queue position is tracked per price, not per order, and the estimate belongs
 * to the *earliest* order of ours at that price. A second order we place at the
 * same price inherits it, so a venue order that joined between the two is
 * treated as sitting behind both. Modelling that exactly would need to know
 * when each venue order arrived, which an aggregate feed does not carry at any
 * price. Priority *between* two orders of ours is not modelled here at all and
 * does not need to be: they are both in the real @c order_book, which decides
 * it the way production will. @see crossing_fill_model
 *
 * @note One flat vector and linear scans. The row count is the number of
 *       distinct prices we are resting at right now - a handful for a quoter -
 *       and this is offline tooling that must stay obviously correct rather
 *       than become fast. @see crossing_fill_model's closing note.
 */
class queue_position_book {
public:
	/**
	 * @brief Begin reconciling @p side against the levels we now hold.
	 *
	 * Marks every row on @p side as unconfirmed. @c track confirms the ones we
	 * still have liquidity at and @c close_side drops the rest, so leaving a
	 * price and returning to it later re-measures the queue instead of
	 * inheriting a position we paid for on the previous visit.
	 */
	void open_side(side_t side) noexcept {
		for (row &entry : rows_)
			if (entry.side == side) entry.is_live = false;
	}

	/**
	 * @brief Note that we hold liquidity at @p price with @p published_lots of
	 *        the venue's own still showing there.
	 *
	 * Joins at the published size if this price is new to us, and otherwise
	 * tightens what we already believe. @see the class note for why those are
	 * the same call: doing them separately would leave an order in which
	 * observing before joining silently discards the join.
	 */
	void track(side_t side, price_t price, volume_t published_lots) {
		const volume_t published = std::max<volume_t>(published_lots, 0);
		if (row *at = find(side, price); at != nullptr) {
			at->is_live = true;
			at->ahead   = std::min(at->ahead, published);
			return;
		}
		rows_.emplace_back(price, published, side, true);
	}

	/**
	 * @brief Confirm we still hold @p price, without taking a measurement.
	 *
	 * For the frames where the published size at our price is not evidence
	 * about the queue. The caller decides when that is - @see
	 * crossing_fill_model, which skips the measurement exactly while our price
	 * is on the far side of the venue's touch, because a level that is missing
	 * because the market moved past it has not been cancelled, it has been
	 * traded through, and it is the queue we last measured that the trade had
	 * to go through.
	 *
	 * A price we have never measured starts at the front, which is not a
	 * concession: the only way to be resting at a price already through the
	 * venue's touch is to have got there by consuming everything that was in
	 * front, and an order that did that really is first in line.
	 */
	void hold(side_t side, price_t price) {
		if (row *at = find(side, price); at != nullptr) {
			at->is_live = true;
			return;
		}
		rows_.emplace_back(price, 0, side, true);
	}

	/// @brief Forget the prices on @p side we no longer rest at.
	void close_side(side_t side) {
		(void)std::erase_if(rows_, [side](const row &entry) noexcept {
			return entry.side == side && !entry.is_live;
		});
	}

	/**
	 * @brief Spend up to @p lots of crossing volume on the queue ahead of us at
	 *        @p price.
	 *
	 * The orders in front of ours are hit first, so volume reaching this level
	 * pays them down before any of it reaches us. Persistent by design: what is
	 * absorbed here stays absorbed, which is how a resting order works its way
	 * forward over successive events rather than starting each one at the back.
	 *
	 * @return How much was absorbed - never more than the queue that was there.
	 *         The caller's remainder is what actually reaches our orders.
	 */
	volume_t absorb(side_t side, price_t price, volume_t lots) noexcept {
		if (lots <= 0) return 0;
		row *at = find(side, price);
		if (at == nullptr || at->ahead <= 0) return 0;
		const volume_t taken = std::min(at->ahead, lots);
		at->ahead -= taken;
		absorbed_ += taken;
		return taken;
	}

	/// @brief Venue lots still ahead of us at @p price. Zero for a price we do
	///        not rest at, which is also the answer for one we are first at.
	[[nodiscard]] volume_t ahead(side_t side, price_t price) const noexcept {
		const row *at = find(side, price);
		return at != nullptr ? at->ahead : 0;
	}

	/**
	 * @brief Abandon every estimate.
	 *
	 * For when the replica dies. Every number here was measured against the
	 * venue's published depth, so a reconstruction that gapped invalidates all
	 * of them at once - and a torn-down replica publishes nothing, which would
	 * otherwise read as an empty queue and put us at the front of every price
	 * we hold for free. Re-measuring from the next snapshot loses the queue
	 * progress a surviving order had earned, which is the pessimistic
	 * direction and the right one. @see session::on_event
	 */
	void clear() noexcept { rows_.clear(); }

	/// @brief Venue liquidity that went to orders ahead of ours rather than
	///        filling us. The size of the front-of-queue assumption we are no
	///        longer making, made countable.
	[[nodiscard]] volume_t absorbed_lots() const noexcept { return absorbed_; }

	/// @brief Prices we currently hold an estimate for.
	[[nodiscard]] std::size_t tracked() const noexcept { return rows_.size(); }

private:
	struct row {
		price_t price;
		volume_t ahead;
		side_t side;
		/// Confirmed by @c track since the last @c open_side. @see close_side.
		bool is_live;
	};

	[[nodiscard]] row *find(side_t side, price_t price) noexcept {
		const auto at = std::ranges::find_if(rows_,

											 [&](const row &entry) noexcept {
												 return entry.side == side &&
														entry.price == price;
											 });
		return at != rows_.end() ? &*at : nullptr;
	}

	[[nodiscard]] const row *find(side_t side, price_t price) const noexcept {
		const auto at =
			std::ranges::find_if(rows_, [&](const row &entry) noexcept {
				return entry.side == side && entry.price == price;
			});
		return at != rows_.end() ? &*at : nullptr;
	}

	std::vector<row> rows_;
	volume_t absorbed_ = 0;
};

} // namespace exchange::strategy::backtest
