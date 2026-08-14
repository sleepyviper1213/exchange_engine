#pragma once
// fmt formatters for the trading engine's composite value types.
//
// Opt-in, like fmt's own fmt/std.h and fmt/ranges.h: only translation units
// that actually print an Order, trade or Level pay for <fmt/format.h>, so the
// domain headers stay free of it. Include this wherever you format one; a
// missing include is a compile error, never a silently different rendering.
//
// Every formatter below derives from fmt::nested_formatter<std::string_view>:
// each type renders as text, so standard fill/align/width apply to the whole
// record — `{:>32}` right-aligns a trade in a 32-column log field.
// @see https://fmt.dev/12.0/api/#formatting-user-defined-types

#include "event/engine_event.hpp"
#include "execution/order_manager.hpp"
#include "order_book/price_level.hpp"
#include "orders/order.hpp"
#include "order_book/order_book.hpp"
#include "order_book/outcome.hpp"
#include "order_book/trade.hpp"

#include <fmt/format.h>

#include <string_view>

/**
 * @brief An Order, in one of two renderings.
 *
 * | spec        | rendering |
 * | ----------- | --------- |
 * | @c "{}", @c "{:c}" | compact — @c "Order[id=1 bid 100 x 10 LIMIT GOOD_TILL_CANCELLED]" |
 * | @c "{:v}"          | verbose — every field, named, including the empty ones |
 *
 * @par Why compact is the default
 * An order is printed by the line, not by the page: it appears in every ack,
 * every reject and every trace, and a field that reads @c "stop_price=0" on
 * every one of them is noise that makes the fields that *did* change harder to
 * see. So the compact form omits what carries no information — @c stop_price
 * when the order is not a stop, @c timestamp when it was never stamped — and
 * keeps what always does.
 *
 * Both instruction fields always print, even at their defaults. They are the
 * two halves of what the book is about to do with the order, and a LIMIT that
 * rests and a LIMIT that is withdrawn unfilled differ only in the second word.
 *
 * @par Why verbose exists anyway
 * Omitting a field and a field being zero are indistinguishable in the compact
 * form, which is exactly wrong when the question is "what did we actually
 * receive?". @c "{:v}" answers that: fixed shape, named fields, nothing hidden,
 * so two dumps can be diffed against each other.
 *
 * @par Combining the mode with fill, align and width
 * The mode letter comes first, and everything after it is an ordinary spec:
 * @c "{:c >60}" is compact, space-padded, right-aligned in 60.
 *
 * The space is not optional there, and the reason is fmt's grammar rather than
 * this formatter: a leading character followed by @c < @c > or @c ^ is a *fill*
 * character, so @c "{:c>60}" already means "pad with @c c" and still does. A
 * mode letter is only a mode when no alignment follows it. Writing the fill
 * explicitly disambiguates, which is the same thing you would do to right-align
 * anything whose fill happens to look like a type.
 */
template <>
struct fmt::formatter<exchange::engine::orders::order>
	: fmt::nested_formatter<std::string_view> {
	bool verbose = false;

	constexpr auto parse(format_parse_context &ctx)
		-> format_parse_context::iterator {
		auto it        = ctx.begin();
		const auto end = ctx.end();
		// The mode letter leads. It cannot trail: fmt's grammar puts the
		// presentation type last, so it rejects an unknown letter there at
		// compile time before this ever sees it. Leading is only ambiguous with
		// a fill character, and a fill is a fill only when an alignment follows
		// — the same rule fmt itself uses to tell the two apart. So "{:v}" is
		// verbose while "{:v<10}" still means "pad with v", exactly as it did.
		if (it != end && (*it == 'v' || *it == 'c')) {
			const auto next = it + 1;
			const bool is_fill =
				next != end && (*next == '<' || *next == '>' || *next == '^');
			if (!is_fill) {
				verbose = *it == 'v';
				ctx.advance_to(++it);
			}
		}
		// Whatever is left is an ordinary spec, so fill/align/width behave here
		// exactly as they do for every other record in this header.
		return fmt::nested_formatter<std::string_view>::parse(ctx);
	}

	auto format(const exchange::engine::orders::order &order,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			if (verbose)
				return fmt::format_to(out,
									  "Order[id={} side={} price={} stop_price={}"
									  " qty={} type={} tif={} timestamp={}]",
									  order.id,
									  order.side, // format_as -> "bid" / "ask"
									  order.price,
									  order.stop_price,
									  order.qty,
									  order.type, // format_as -> enumerator name
									  order.tif,
									  order.timestamp);

			out = fmt::format_to(out,
								 "Order[id={} {} {}",
								 order.id,
								 order.side,
								 order.price);
			// Zero is the "not a stop" sentinel, so printing it would be
			// printing the absence of a trigger.
			if (order.stop_price != 0)
				out = fmt::format_to(out, " stop={}", order.stop_price);
			out = fmt::format_to(out,
								 " x {} {} {}",
								 order.qty,
								 order.type,
								 order.tif);
			if (order.timestamp != 0)
				out = fmt::format_to(out, " ts={}", order.timestamp);
			return fmt::format_to(out, "]");
		});
	}
};

/// @brief A trade as @c "trade[aggressor=1 hit=2 @100 x 10]" — the price is the
///        resting order's, per trade's contract.
template <>
struct fmt::formatter<exchange::engine::trade>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::engine::trade &trade,
				format_context &ctx) const -> format_context::iterator {
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
 * carries both and they answer different questions — a FILL that leaves an order
 * FILLED and one that leaves it PARTIALLY_FILLED are the same transition and
 * different news. The reason is omitted when it is NONE, which is most records;
 * on a REJECTED or CANCEL_REJECTED it is the only field that says anything.
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
 * @brief A published event as @c "event[sym=7 trade[aggressor=1 hit=2 @100 x 4]]".
 *
 * The kind is not printed as a word: the payload's own rendering already begins
 * with @c "trade[" or @c "outcome[", so naming the tag as well would say it
 * twice. What the wrapper adds is the one thing neither payload carries and the
 * whole record exists for — the listing.
 */
template <>
struct fmt::formatter<exchange::engine::event::engine_event>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::engine::event::engine_event &event,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			out = fmt::format_to(out, "event[sym={} ", event.symbol);
			switch (event.kind) {
			case exchange::engine::event::EventKind::TRADE:
				out = fmt::format_to(out, "{}", event.as_trade());
				break;
			case exchange::engine::event::EventKind::OUTCOME:
				out = fmt::format_to(out, "{}", event.as_outcome());
				break;
			}
			return fmt::format_to(out, "]");
		});
	}
};

/**
 * @brief A book's top of book, as
 *        @c "order_book[bid=100 x 30 ask=101 x 12 spread=1]".
 *
 * Exists so callers never have to reach into the book to print it — one
 * @c "{}" instead of a best_bid()/best_ask()/subtract trio at every call site,
 * which is where the "spread" arithmetic used to be duplicated. A side with no
 * resting liquidity reads @c "none", and the spread is omitted unless both
 * sides quote.
 *
 * @par Why the size and not just the price
 * A touch price on its own says where the book is, not what is there, and those
 * answer different questions: @c "bid=100" is the same line whether one lot
 * rests at it or ten thousand do. The aggregate is what tells you whether the
 * quote is real, so it prints alongside — matching @c l2_book's own summary,
 * which has always read @c "best @15000 x 7". Two books of the same depth
 * should not describe themselves differently.
 *
 * @note Top of book only. There is deliberately no ladder mode here, because
 *       @c order_book exposes no way to walk its levels — @c book_side's
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

/**
 * @brief One managed order, as
 *        @c "OrderRecord[id=42 sym=7 acct=99 ask @1250 4/30 PARTIALLY_FILLED]".
 *
 * The record store's answer to "what happened to order 42", so it leads with
 * the id and ends with the fate. In between is what identifies the order to a
 * human reading a log: its listing, its owner, and where it sat.
 *
 * @c traded/quantity rather than a remaining count, because the two numbers
 * together say how far through the order is and either alone does not — and
 * because that is the pairing @c OrderStatus is derived from, so a line whose
 * status looks wrong can be checked against the quantities on the same line.
 *
 * Fields that carry no information are omitted, on the same reasoning as
 * @c order's compact form: @c reason only when the order ended with one — a
 * cancel needs no excuse, so NONE is the common case and printing it is noise —
 * @c acct only when the order is attributed, and @c ts only when stamped.
 */
template <>
struct fmt::formatter<exchange::engine::execution::order_record>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::engine::execution::order_record &record,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			out = fmt::format_to(out,
								 "OrderRecord[id={} sym={}",
								 record.id,
								 record.symbol);
			// Zero is "unattributed", which is what anonymous depth carries and
			// what every order carries until the gateway sets an account.
			if (record.account != 0)
				out = fmt::format_to(out, " acct={}", record.account);
			out = fmt::format_to(out,
								 " {} @{} {}/{} {}",
								 record.side, // format_as -> "bid" / "ask"
								 record.price,
								 record.state.traded(),
								 record.state.quantity(),
								 record.status()); // format_as -> enumerator
			if (record.reason != exchange::engine::reject_reason::NONE)
				out = fmt::format_to(out, " {}", record.reason);
			if (record.timestamp != 0)
				out = fmt::format_to(out, " ts={}", record.timestamp);
			return fmt::format_to(out, "]");
		});
	}
};

/**
 * @brief A handle, as @c "order_handle[slot=3 gen=1]" or @c "order_handle[none]".
 *
 * Prints the generation, which is the whole reason the type is not a bare
 * index: two handles naming the same slot at different generations are
 * different orders, and a log that showed only the slot would render them
 * identically at exactly the moment the difference mattered.
 */
template <>
struct fmt::formatter<exchange::engine::execution::order_handle>
	: fmt::nested_formatter<std::string_view> {
	auto format(exchange::engine::execution::order_handle handle,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			if (!handle.valid()) return fmt::format_to(out, "order_handle[none]");
			return fmt::format_to(out,
								  "order_handle[slot={} gen={}]",
								  handle.slot,
								  handle.generation);
		});
	}
};

/**
 * @brief A record store's occupancy, as
 *        @c "order_manager[live=3 retained=120 peak=57/32768]".
 *
 * The capacity-planning line. @c peak against @c capacity is the reading that
 * matters — a store sized right reports a peak comfortably below its capacity,
 * and one that reached it has refused orders. @c live and @c retained say how
 * that capacity is being spent right now: orders the book can still act on,
 * versus history kept so a late cancel can be told what became of its order.
 *
 * @c evicted prints only when it is non-zero, and it is the one number here
 * that reports a loss: terminal records dropped to make room, each of which is
 * an order the store can no longer answer questions about. Zero is the healthy
 * case and the common one, so it stays off the line.
 *
 * @note Deliberately not the records themselves. The store holds up to its
 *       capacity of them and a formatter that walked all 32k would be a way to
 *       hang a log; print an @c order_record when you have one in hand.
 */
template <>
struct fmt::formatter<exchange::engine::execution::order_manager>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::engine::execution::order_manager &orders,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			out = fmt::format_to(out,
								 "order_manager[live={} retained={} peak={}/{}",
								 orders.live(),
								 orders.retained(),
								 orders.high_water(),
								 orders.capacity());
			if (orders.evicted() != 0)
				out = fmt::format_to(out, " evicted={}", orders.evicted());
			return fmt::format_to(out, "]");
		});
	}
};

/// @brief A Level as @c "Level[@100 x 30, 3 orders]" — aggregate size and depth,
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
