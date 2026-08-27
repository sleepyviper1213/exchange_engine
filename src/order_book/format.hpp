#pragma once


#include "order_book.hpp"
#include "outcome.hpp"
#include "price_level.hpp"
#include "queue_position.hpp"
#include "sweep_estimate.hpp"
#include "trade.hpp"

#include <fmt/format.h>

#include <string_view>

/// @brief A trade as @c "trade[aggressor=1 hit=2 @100 x 10]" - the price is the
///        resting order's, per trade's contract.
template <>
struct fmt::formatter<exchange::engine::trade>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::engine::trade &trade, format_context &ctx) const
		-> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "trade[aggressor={} hit={} @{} x {}]",
								  trade.aggressor,
								  trade.resting,
								  trade.price,
								  trade.volume);
		});
	}
};

/**
 * @brief A lifecycle record as @c "outcome[id=1 FILL PARTIALLY_FILLED traded=4
 *        left=6]".
 *
 * The transition and the resulting status both print, because @c order_outcome
 * carries both and they answer different questions - a FILL that leaves an
 * order FILLED and one that leaves it PARTIALLY_FILLED are the same transition
 * and different news. The reason is omitted when it is NONE, which is most
 * records; on a REJECTED or CANCEL_REJECTED it is the only field that says
 * anything.
 *
 * @note A CANCEL_REJECTED prints @c "traded=0 left=0" because that is what the
 *       record holds: the book had no order to report on. @see order_outcome
 */
template <>
struct fmt::formatter<exchange::engine::order_outcome>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::engine::order_outcome &outcome,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			out = fmt::format_to(out,
								 "outcome[id={} {} {}",
								 outcome.id,
								 outcome.type,
								 outcome.status);
			if (outcome.reason != exchange::engine::reject_reason::NONE)
				out = fmt::format_to(out, " {}", outcome.reason);
			return fmt::format_to(out,
								  " traded={} left={}]",
								  outcome.traded,
								  outcome.remaining);
		});
	}
};

/**
 * @brief A book's top of book, as
 *        @c "order_book[bid=100 x 30 ask=101 x 12 spread=1]".
 *
 * Exists so callers never have to reach into the book to print it - one
 * @c "{}" instead of a best_bid()/best_ask()/subtract trio at every call site,
 * which is where the "spread" arithmetic used to be duplicated. A side with no
 * resting liquidity reads @c "none", and the spread is omitted unless both
 * sides quote.
 *
 * @par Why the size and not just the price
 * A touch price on its own says where the book is, not what is there, and those
 * answer different questions: @c "bid=100" is the same line whether one lot
 * rests at it or ten thousand do. The aggregate is what tells you whether the
 * quote is real, so it prints alongside - matching @c l2_book's own summary,
 * which has always read @c "best @15000 x 7". Two books of the same depth
 * should not describe themselves differently.
 *
 * @note Top of book only. There is deliberately no ladder mode here, because
 *       @c order_book exposes no way to walk its levels - @c book_side's
 *       iterators are @c detail. Printing depth would mean widening the book's
 *       public surface, which is a bigger decision than a formatter should make
 *       on its own.
 */
template <>
struct fmt::formatter<exchange::engine::order_book>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::engine::order_book &book,
				format_context &ctx) const -> format_context::iterator {
		using exchange::side_t;
		const auto bid = book.best_bid();
		const auto ask = book.best_ask();
		return write_padded(ctx, [&](auto out) {
			out = fmt::format_to(out, "order_book[bid=");
			if (bid)
				out = fmt::format_to(out,
									 "{} x {}",
									 *bid,
									 book.volume_at_price(*bid, side_t::bid));
			else out = fmt::format_to(out, "none");
			out = fmt::format_to(out, " ask=");
			if (ask)
				out = fmt::format_to(out,
									 "{} x {}",
									 *ask,
									 book.volume_at_price(*ask, side_t::ask));
			else out = fmt::format_to(out, "none");
			if (bid && ask)
				out = fmt::format_to(out, " spread={}", *ask - *bid);
			return fmt::format_to(out, "]");
		});
	}
};

/// @brief A Level as @c "Level[@100 x 30, 3 orders]" - aggregate size and
/// depth,
///        not the individual orders, which are rarely what a log line wants.
template <>
struct fmt::formatter<exchange::engine::price_level>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::engine::price_level &level,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "Level[@{} x {}, {} orders]",
								  level.price,
								  level.total_volume(),
								  level.order_count());
		});
	}
};

/**
 * @brief A queue position as
 *        @c "queue[@100 bid mine=4 ahead=30 in 3 behind=12 PRICE_TIME]".
 *
 * The policy prints because without it the interesting number is ambiguous:
 * @c ahead is a threshold to be cleared under PRICE_TIME and a piece of
 * arithmetic trivia under PRO_RATA, and a log line that omits which one is in
 * force is a log line that cannot be read afterwards. @see allocation_policy
 */
template <>
struct fmt::formatter<exchange::engine::queue_position>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::engine::queue_position &queued,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "queue[@{} {} mine={} ahead={} in {} "
								  "behind={} {}]",
								  queued.price,
								  queued.side,
								  queued.remaining,
								  queued.lots_ahead,
								  queued.orders_ahead,
								  queued.lots_behind,
								  queued.policy);
		});
	}
};

/**
 * @brief A sweep estimate as
 *        @c "sweep[ask 500/500 lots, 3 levels, @100 -> @104, impact 4,
 *        notional 50120, slippage 120]".
 *
 * Slippage prints alongside impact because they disagree in the case that
 * matters: one thin level at the end of a long walk gives a large impact and a
 * small slippage, and a whole book priced away from the touch gives the
 * opposite. @see sweep_estimate
 */
template <>
struct fmt::formatter<exchange::engine::sweep_estimate>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::engine::sweep_estimate &sweep,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			out = fmt::format_to(out,
								 "sweep[{} {}/{} lots",
								 sweep.side,
								 sweep.filled,
								 sweep.requested);
			if (!sweep.is_complete()) out = fmt::format_to(out, " INCOMPLETE");
			if (!sweep.has_liquidity())
				return fmt::format_to(out, ", no depth]");
			return fmt::format_to(out,
								  ", {} levels, @{} -> @{}, impact {}, "
								  "notional {}, slippage {}]",
								  sweep.levels,
								  sweep.touch,
								  sweep.last,
								  sweep.impact(),
								  sweep.notional,
								  sweep.slippage());
		});
	}
};
