#pragma once
// The book's records, as text: a trade, an outcome, a level, a book.
// Opt-in, like fmt's own fmt/std.h and fmt/ranges.h: only translation units that
// actually print one of these pay for <fmt/format.h>, so the domain headers stay
// free of it. Include this wherever you format one; a missing include is a
// compile error, never a silently different rendering.
//
// Every formatter here derives from fmt::nested_formatter<std::string_view>: each
// type renders as text, so standard fill/align/width apply to the whole record -
// `{:>32}` right-aligns one in a 32-column log field.
// @see https://fmt.dev/12.0/api/#formatting-user-defined-types
//
// One sidecar per module, which is what CLAUDE.md asks for: a cross-cutting
// facility is an opt-in header *inside* a module, never a central one, because a
// central one would point an edge back up the graph. These four used to be a
// single trading-engine/format.hpp, which was correct while the engine was a
// single library.

#include "order_book.hpp"
#include "outcome.hpp"
#include "price_level.hpp"
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
