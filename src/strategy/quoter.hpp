#pragma once
// The reference trader: one bid and one ask a tick inside the venue's touch.
//
// What it is not: a strategy in the sense the concepts next door mean. It has
// no edge, no inventory view and no opinion about the market, and nobody should
// put money behind it. What it *is* is the only thing in the tree that
// originates orders from a venue's book, which makes it what both commands with
// an order flow drive - `EXCHANGE_tool backtest` over a capture and
// `EXCHANGE_tool serve` against a live feed. The lifecycle it exercises is the
// point: amend-against-a-moving-market, which is where the races are.
//
// It lived under `backtest/` while the harness was its only caller. It moved up
// when `serve` needed it, because a component two callers share should not sit
// inside one of them - the same reasoning that put `depth_feed_bridge` beside
// its caller, applied the other way round now that there are two.
//
// --- why it is not in strategy.hpp ----------------------------------------
//
// Its market hook takes a `market_data::l2_book`, so including it pulls
// market_data in. Every other strategy here is a function of what the *engine*
// published and needs none of it, and `strategy.hpp` keeps that property: a
// translation unit that only defines a strategy pays for no decoder. So this is
// opt-in by its own header, for the same reason `strategy/backtest.hpp` is.

#include "event/command.hpp"
#include "fwd.hpp"
#include "market_data/l2_book.hpp"
#include "order_book/outcome.hpp"
#include "order_book/trade.hpp"
#include "orders/amendment.hpp"
#include "orders/order.hpp"
#include "orders/time_in_force_instruction.hpp"
#include "orders/types.hpp"
#include "symbol/symbol_spec.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace exchange::strategy {

/// @brief How the reference quoter behaves.
struct quoter_options {
	/**
	 * @brief Ticks inside the venue's touch to quote, per side.
	 *
	 * Must be at least one for the quoter to ever fill passively, and the
	 * reason is the fill model's crossing test rather than an arbitrary choice:
	 * an order *joining* the venue's best bid can only be traded through by an
	 * offer below the venue's own best bid, which is a crossed venue book and
	 * therefore never happens. Improving on the touch is what puts the quote
	 * somewhere the market can come to it.
	 */
	price_t improve_ticks = 1;

	/// @brief Lots shown on each side.
	quantity_t lots = 1;

	/**
	 * @brief Minimum market time between requotes, in nanoseconds. Zero
	 *        requotes on every event.
	 *
	 * Market time, from @c core::chrono::feed_clock - so a run's requote
	 * cadence is a property of the capture and not of how fast the machine
	 * replayed it.
	 */
	std::uint64_t requote_interval_ns = 0;

	/**
	 * @brief Cross the venue's touch instead of improving on it.
	 *
	 * @par Why a passive quoter cannot be the whole story
	 * Because in this engine a passive quote never fills. The liquidity a
	 * strategy trades against here is what @c depth_feed_bridge seeds from the
	 * venue, and it is seeded with @c exchange::add_order - anonymous
	 * liquidity that *rests without matching*. So an order improving on the
	 * touch sits inside the spread with nothing to cross it, and when the
	 * market later moves through it the bridge publishes depth at a price that
	 * crosses *it* - which, since the seeding path does not match either,
	 * leaves the engine's book crossed rather than the order filled.
	 *
	 * A backtest resolves that with @c crossing_fill_model, which infers what a
	 * resting order would have traded from the venue's own depth. That
	 * inference cannot cross a thread boundary: it reads the partition's
	 * @c order_manager, which belongs to the matching thread. So it is offline
	 * only, and a live run that has to actually trade has to *take*.
	 *
	 * Set this and each requote sends one @c IMMEDIATE_OR_CANCEL through the
	 * opposite touch instead of resting a quote inside it. Every consequence
	 * follows from the two words "immediate or cancel": the order either
	 * matches the seeded depth now or is dropped, nothing of ours ever rests,
	 * and no order of ours can leave the book crossed. Sides alternate, so the
	 * position walks about flat rather than running one way.
	 *
	 * @note Strictly worse than resting, as trading: it pays the spread every
	 *       time. It is not here to make money - it is here so the path a
	 *       deployment runs has real fills in it, and so the position,
	 *       exposure, drawdown and post-trade rules have something to measure.
	 */
	bool take_liquidity = false;
};

/**
 * @brief Shows one bid and one ask a tick inside the venue's touch, and
 *        replaces them when the touch moves.
 *
 * @tparam Sink Where commands go - @c session::sink(), which is the risk gate.
 *
 * @par Two modes, and why the second exists
 * Passively it improves on the touch and rests, which is what a quoter is and
 * what a backtest can measure through its fill model. Aggressively - @c
 * quoter_options::take_liquidity - it crosses the touch with an IOC instead,
 * because a resting order cannot fill against liquidity that was seeded without
 * matching and the inference that covers that offline cannot be run live. The
 * two share everything except what they do once they know where the touch is.
 *
 * @par The lifecycle it exercises, which is the point
 * Cancel-replace against a moving market is where the interesting races are: a
 * cancel racing the fill it was trying to beat, a replacement placed under an
 * id whose predecessor has not been retired, an order that filled while its
 * cancel sat in the queue. All three occur naturally here, and all three come
 * back as
 * @c order_outcome%s the run counts. A quoter that only ever placed orders and
 * never withdrew them would exercise none of it.
 *
 * @par Ids
 * Monotonically increasing and never reused, because @c order_manager refuses
 * an id it still remembers - a replacement quote at the same price is a
 * different order and says so.
 *
 * @note One listing, like everything else on this side of the queue. @see
 *       strategy_engine's note on why a host is built per listing.
 */
template <class Sink>
class spread_quoter {
public:
	using command = engine::event::command;

	/**
	 * @brief The most commands one look at the market can produce.
	 *
	 * Two: a two-sided requote is one command per side, whether that command is
	 * an amendment or the place that starts a fresh quote, and @c on_market's
	 * other paths are no larger - @c pull_both is two cancels and a take is one
	 * place. It was four while a requote was a cancel *and* a place per side.
	 * @see requote
	 *
	 * Stated as a constant because something downstream now needs it. Anything
	 * that holds this quoter's batch in a bounded buffer has to be at least
	 * this big or it can never accept one, however long it waits - and a caller
	 * that retries a refusal, which is what @c live_session does, turns "too
	 * small" into a hang rather than a slow run. @see session::latency_pipe
	 *
	 * @note Deliberately not spelled @c MAX_COMMANDS_PER_EVENT: that name is
	 * the
	 *       @c bounded_emitter concept's, and a quoter is not a hosted strategy
	 * - it writes through its own buffer rather than a @c command_writer.
	 *       Borrowing the name would make it satisfy a concept it has no other
	 *       business satisfying.
	 */
	static constexpr std::size_t MAX_COMMANDS_PER_REQUOTE = 2;

	/**
	 * @brief Quote @p spec's listing into @p sink.
	 * @param sink Must outlive the quoter; held by pointer.
	 * @param spec The listing. Supplies the tick grid the venue's scaled prices
	 *        are converted on. Must outlive the quoter.
	 * @param options Size, aggression and cadence.
	 */
	spread_quoter(Sink &sink, const engine::symbol_spec &spec,
				  quoter_options options = {}) noexcept
		: sink_(&sink), spec_(&spec), symbol_(spec.id()), options_(options) {}

	// --- the market hook, which is what makes this a quoter -----------------

	/**
	 * @brief Look at the venue's book and adjust the quotes.
	 *
	 * Does nothing while the replica has no two-sided market, and nothing while
	 * a quote already sits where this would place one - a requote that changes
	 * nothing is a cancel and a place for no reason, and it would give the run
	 * a churn figure that says more about the quoter than about the market.
	 */
	void on_market(const market_data::l2_book &replica, std::uint64_t now_ns) {
		if (is_throttled(now_ns)) return;

		const auto bid = replica.best_bid();
		const auto ask = replica.best_ask();
		if (!bid.has_value() || !ask.has_value()) {
			pull_both();
			return;
		}

		const auto touch_bid = to_ticks(*bid);
		const auto touch_ask = to_ticks(*ask);
		if (!touch_bid.has_value() || !touch_ask.has_value()) {
			// The venue's touch is not on this listing's tick grid, so the spec
			// and the feed disagree about the instrument. Quoting a price we
			// had to invent would be worse than not quoting.
			++off_grid_;
			return;
		}

		if (options_.take_liquidity) {
			take(*touch_bid, *touch_ask);
			quoted_        = true;
			last_quote_ns_ = now_ns;
			return;
		}

		// Improve on both sides, but never through the other one: a quote that
		// crosses the venue's own book is an aggressive order wearing a quote's
		// clothes, and it would fill instantly against the mirrored depth on
		// every single event.
		const price_t want_bid =
			clamp_below(*touch_bid + options_.improve_ticks,
						*touch_ask,
						*touch_bid);
		const price_t want_ask =
			clamp_above(*touch_ask - options_.improve_ticks,
						*touch_bid,
						*touch_ask);
		// Both sides improving by `improve_ticks` needs a spread wider than
		// twice it, and a venue that quotes tight will not give one. Counted
		// rather than silent: the symptom is a run that places no orders at
		// all, and "the spread was never wide enough" is not a conclusion
		// anybody reaches from a report full of zeroes.
		if (want_bid >= want_ask) {
			++no_room_;
			return;
		}

		const bool move_bid = live_bid_ == 0 || bid_price_ != want_bid;
		const bool move_ask = live_ask_ == 0 || ask_price_ != want_ask;
		if (!move_bid && !move_ask) return;

		// This used to withdraw both sides before showing either, and not for
		// tidiness: replacing one side at a time lets the new bid reach the
		// book while the old ask is still resting on it, and once the market
		// has moved further than the spread the two cross - the venue prints a
		// trade between two of our own orders. A real venue has self-trade
		// prevention for exactly this; this engine does not yet (TODO.md #10).
		//
		// An amendment cannot be split into a withdrawal and a replacement, so
		// that instrument is gone and this is what takes its place: move
		// whichever side is in the way *first*. At most one side can be, and
		// the argument is short. want_bid >= ask_price_ says the market moved
		// up past our offer; want_ask <= bid_price_ says it moved down past our
		// bid; and bid_price_ < ask_price_ together with want_bid < want_ask
		// makes the two mutually exclusive. Whichever holds, moving that side
		// out of the way first leaves the other landing into an open spread.
		if (move_bid && move_ask) {
			if (live_ask_ != 0 && want_bid >= ask_price_) {
				requote(side_t::ask, want_ask);
				requote(side_t::bid, want_bid);
			} else {
				requote(side_t::bid, want_bid);
				requote(side_t::ask, want_ask);
			}
		} else if (move_bid) {
			// One side moving cannot cross the other, which is not resting
			// where it is by accident: !move_ask means ask_price_ == want_ask,
			// and want_bid < want_ask is the check above.
			requote(side_t::bid, want_bid);
		} else requote(side_t::ask, want_ask);

		quoted_        = true;
		last_quote_ns_ = now_ns;
	}

	// --- the trader interface -----------------------------------------------

	/// @brief Prints do not move this quoter; its own fills reach it as
	///        outcomes, which is the channel that names *its* order.
	std::size_t on_trades(std::span<const engine::trade> trades) noexcept {
		return trades.size();
	}

	/**
	 * @brief Forget a quote the venue has finished with.
	 *
	 * The feedback channel doing the one job the quoter cannot do without: a
	 * filled quote is gone from the book, and a quoter that still believed it
	 * was there would cancel an id that no longer exists on every requote and
	 * never replace the side.
	 */
	std::size_t
	on_outcomes(std::span<const engine::order_outcome> outcomes) noexcept {
		for (const engine::order_outcome &record : outcomes) {
			side_t side{};
			if (!side_of(record.id, side)) continue;
			switch (record.type) {
			case engine::OutcomeType::FILL:
				// Kept because the next amendment has to ask for it back: an
				// amendment names the order's quantity, so restoring a
				// partially filled quote to its full showing size means asking
				// for traded plus lots. @see requote
				filled(side) = record.traded;
				if (record.remaining > 0) break; // still working
				[[fallthrough]];
			case engine::OutcomeType::REJECTED:
			case engine::OutcomeType::CANCELLED: live(side) = 0; break;
			case engine::OutcomeType::MODIFY_REJECTED:
				// The venue would not amend it, so the quote is still resting
				// where it was - but this quoter has already written down the
				// price it asked for, and the two now disagree. Forgetting the
				// order is the conservative repair: the next requote places a
				// fresh one rather than amending against a price that is not
				// there, and no later command names an order this quoter can no
				// longer describe.
				live(side) = 0;
				break;
			case engine::OutcomeType::ACCEPTED:
			case engine::OutcomeType::MODIFIED:
			case engine::OutcomeType::CANCEL_REJECTED: break;
			}
		}
		return outcomes.size();
	}

	/// @brief Hand the accumulated commands to the sink and empty the buffer.
	/// @return @c false if the sink refused; the batch is kept for a retry.
	bool flush() {
		if (pending_.empty()) return true;
		if (!sink_->submit_range(pending_)) {
			++stalls_;
			return false;
		}
		submitted_ += pending_.size();
		pending_.clear();
		return true;
	}

	// --- what a report reads -------------------------------------------------

	/// @brief Commands the sink has accepted since construction.
	[[nodiscard]] std::uint64_t submitted() const noexcept {
		return submitted_;
	}

	/// @brief Times the sink refused a batch. Saturation, not error.
	[[nodiscard]] std::uint64_t stalls() const noexcept { return stalls_; }

	/// @brief Quotes placed since construction.
	[[nodiscard]] std::uint64_t quotes() const noexcept { return quotes_; }

	/// @brief Orders sent through the touch, in @c take_liquidity mode.
	[[nodiscard]] std::uint64_t takes() const noexcept { return takes_; }

	/// @brief Events skipped because the venue's touch was off the tick grid.
	[[nodiscard]] std::uint64_t off_grid() const noexcept { return off_grid_; }

	/// @brief Events where the spread was too tight to improve on both sides.
	///        A run reporting no quotes at all is explained by this or by
	///        @c off_grid, and by nothing else.
	[[nodiscard]] std::uint64_t no_room() const noexcept { return no_room_; }

	/// @brief The order id currently believed live on @p side, or 0.
	[[nodiscard]] order_id_t live_order(side_t side) const noexcept {
		return side == side_t::bid ? live_bid_ : live_ask_;
	}

	/// @brief The price the quote on @p side was placed at. Meaningful only
	///        while @c live_order is non-zero.
	[[nodiscard]] price_t quoted_price(side_t side) const noexcept {
		return side == side_t::bid ? bid_price_ : ask_price_;
	}

private:
	/// @brief Whether the requote interval has not elapsed yet.
	[[nodiscard]] bool is_throttled(std::uint64_t now_ns) const noexcept {
		return options_.requote_interval_ns != 0 && quoted_ &&
			   now_ns - last_quote_ns_ < options_.requote_interval_ns;
	}

	/**
	 * @brief Send one order through the touch, alternating which side.
	 *
	 * Nothing is withdrawn first and nothing is recorded as live, because an
	 * IOC cannot be either: it matches what is there and its remainder is
	 * dropped in the same command. One side per requote rather than both -
	 * taking both would buy the offer and sell the bid on the same touch, which
	 * is a round trip that pays the spread twice for a position that never
	 * moves, and the point of taking at all is that the position *does* move.
	 */
	void take(price_t touch_bid, price_t touch_ask) {
		const side_t side = taking_bid_ ? side_t::bid : side_t::ask;
		// To buy, cross to the offer; to sell, cross to the bid. Exactly the
		// touch and no further: the seeded depth is what is being traded
		// against, and a price through it would only reach levels the venue
		// publishes behind the touch.
		cross(side, side == side_t::bid ? touch_ask : touch_bid);
		taking_bid_ = !taking_bid_;
	}

	/// @brief Place one IOC at @p price on @p side.
	void cross(side_t side, price_t price) {
		const order_id_t id = ++next_id_;
		pending_.push_back(command::place(engine::orders::order{
			.id        = id,
			.symbol_id = symbol_,
			.side      = side,
			.tif =
				engine::orders::time_in_force_instruction::IMMEDIATE_OR_CANCEL,
			.price = price,
			.qty   = options_.lots,
		}));
		++takes_;
	}

	[[nodiscard]] order_id_t &live(side_t side) noexcept {
		return side == side_t::bid ? live_bid_ : live_ask_;
	}

	[[nodiscard]] price_t &resting_price(side_t side) noexcept {
		return side == side_t::bid ? bid_price_ : ask_price_;
	}

	/// @brief Lots this side's live quote has executed. Reset when a fresh
	///        order is shown, and carried across an amendment because an
	///        amendment leaves the same order in place. @see requote
	[[nodiscard]] quantity_t &filled(side_t side) noexcept {
		return side == side_t::bid ? filled_bid_ : filled_ask_;
	}

	/// @brief Which side @p id was quoted on, if it is still one of ours.
	[[nodiscard]] bool side_of(order_id_t id, side_t &side) const noexcept {
		if (id != 0 && id == live_bid_) {
			side = side_t::bid;
			return true;
		}
		if (id != 0 && id == live_ask_) {
			side = side_t::ask;
			return true;
		}
		return false;
	}

	[[nodiscard]] std::optional<price_t>
	to_ticks(market_data::scaled_price_t scaled) const noexcept {
		const auto ticks = spec_->price_from_scaled(scaled);
		if (!ticks.has_value()) return std::nullopt;
		return *ticks;
	}

	/// @brief @p want, held strictly below @p ceiling, falling back to @p
	/// floor.
	[[nodiscard]] static price_t clamp_below(price_t want, price_t ceiling,
											 price_t floor) noexcept {
		return want < ceiling ? want : floor;
	}

	/// @brief @p want, held strictly above @p floor, falling back to @p
	/// ceiling.
	[[nodiscard]] static price_t clamp_above(price_t want, price_t floor,
											 price_t ceiling) noexcept {
		return want > floor ? want : ceiling;
	}

	/// @brief Cancel whatever is resting on @p side, if anything.
	void withdraw(side_t side) {
		if (live(side) == 0) return;
		pending_.push_back(command::cancel(symbol_, live(side)));
		live(side) = 0;
	}

	/**
	 * @brief Put this side's quote at @p price, amending what is there rather
	 *        than replacing it.
	 *
	 * One command where withdraw-and-show was two, and the order keeps its id -
	 * so a requote no longer burns a client order id per side per event, and
	 * the venue's record of the quote is one lifecycle rather than a chain of
	 * them. A price move still costs the queue position it was always going to
	 * cost; what it stops costing is the round trip.
	 *
	 * @note The quantity asked for is what the quote has already traded *plus*
	 *       the size it should show, because an amendment names the order's
	 *       quantity and not its remainder. Asking for @c options_.lots flat
	 *       would quietly shrink a partially filled quote to @c lots - traded,
	 *       where cancel-and-replace used to put a full @c lots back on the
	 *       book. @see engine::orders::amendment
	 */
	void requote(side_t side, price_t price) {
		if (live(side) == 0) {
			show(side, price);
			return;
		}
		pending_.push_back(
			command::modify(symbol_,
							engine::orders::amendment{
								.id       = live(side),
								.price    = price,
								.quantity = filled(side) + options_.lots}));
		resting_price(side) = price;
		++quotes_;
	}

	/// @brief Show a fresh quote on @p side at @p price.
	/// @pre Nothing of ours is resting there - @c withdraw ran first.
	void show(side_t side, price_t price) {
		const order_id_t id = ++next_id_;
		pending_.push_back(command::place(engine::orders::order{
			.id        = id,
			.symbol_id = symbol_,
			.side      = side,
			.tif =
				engine::orders::time_in_force_instruction::GOOD_TILL_CANCELLED,
			.price = price,
			.qty   = options_.lots,
		}));
		live(side)          = id;
		resting_price(side) = price;
		filled(side)        = 0;
		++quotes_;
	}

	/// @brief Withdraw both sides - the venue stopped showing a two-sided book.
	void pull_both() {
		withdraw(side_t::bid);
		withdraw(side_t::ask);
		quoted_ = false;
	}

	Sink *sink_;
	const engine::symbol_spec *spec_;
	symbol_id_t symbol_;
	quoter_options options_;

	std::vector<command> pending_;
	order_id_t next_id_          = 0;
	order_id_t live_bid_         = 0;
	order_id_t live_ask_         = 0;
	price_t bid_price_           = 0;
	price_t ask_price_           = 0;
	quantity_t filled_bid_       = 0;
	quantity_t filled_ask_       = 0;
	bool quoted_                 = false;
	std::uint64_t last_quote_ns_ = 0;

	/// @brief Which side the next take crosses to. @see take
	bool taking_bid_ = true;

	std::uint64_t submitted_ = 0;
	std::uint64_t stalls_    = 0;
	std::uint64_t quotes_    = 0;
	std::uint64_t takes_     = 0;
	std::uint64_t off_grid_  = 0;
	std::uint64_t no_room_   = 0;
};

} // namespace exchange::strategy
