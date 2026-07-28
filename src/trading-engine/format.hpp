#pragma once
// fmt formatters for the trading engine's composite value types.
//
// Opt-in, like fmt's own fmt/std.h and fmt/ranges.h: only translation units
// that actually print an Order, Trade or Level pay for <fmt/format.h>, so the
// domain headers stay free of it. Include this wherever you format one; a
// missing include is a compile error, never a silently different rendering.
//
// Enums do NOT appear here. Their `format_as` hook is emitted by the
// EXCHANGE_ENUM_* macro that declares them (OrderType in order_book/order.hpp,
// Side in core/types.hpp), which needs no fmt dependency at all — see
// core/util/enum_string.hpp. Providing both a formatter specialisation and a
// format_as overload for one type is disallowed.
//
// Every formatter below derives from fmt::nested_formatter<std::string_view>:
// each type renders as text, so standard fill/align/width apply to the whole
// record — `{:>32}` right-aligns a Trade in a 32-column log field.
// @see https://fmt.dev/12.0/api/#formatting-user-defined-types

#include "order_book/level.hpp"
#include "order_book/order.hpp"
#include "order_book/order_book.hpp"
#include "order_book/trade.hpp"

#include <fmt/format.h>

#include <string_view>

/// @brief An Order as @c "Order[id=1 BID 100 x 10 GOOD_TILL_CANCELLED]".
template <>
struct fmt::formatter<exchange::engine::Order>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::engine::Order &order,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "Order[id={} {} {} x {} {}]",
								  order.id,
								  order.side, // format_as -> "BID" / "ASK"
								  order.price,
								  order.volume,
								  order.type); // format_as -> enumerator name
		});
	}
};

/// @brief A Trade as @c "Trade[aggressor=1 hit=2 @100 x 10]" — the price is the
///        resting order's, per Trade's contract.
template <>
struct fmt::formatter<exchange::engine::Trade>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::engine::Trade &trade,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "Trade[aggressor={} hit={} @{} x {}]",
								  trade.aggressor,
								  trade.resting,
								  trade.price,
								  trade.volume);
		});
	}
};

/**
 * @brief A book's top of book, as @c "order_book[bid=100 ask=101 spread=1]".
 *
 * Exists so callers never have to reach into the book to print it — one
 * @c "{}" instead of a best_bid()/best_ask()/subtract trio at every call site,
 * which is where the "spread" arithmetic used to be duplicated. A side with no
 * resting liquidity reads @c "none", and the spread is omitted unless both
 * sides quote.
 */
template <>
struct fmt::formatter<exchange::engine::order_book>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::engine::order_book &book,
				format_context &ctx) const -> format_context::iterator {
		const auto bid = book.best_bid();
		const auto ask = book.best_ask();
		return write_padded(ctx, [&](auto out) {
			out = fmt::format_to(out, "order_book[bid=");
			if (bid) out = fmt::format_to(out, "{}", *bid);
			else out = fmt::format_to(out, "none");
			out = fmt::format_to(out, " ask=");
			if (ask) out = fmt::format_to(out, "{}", *ask);
			else out = fmt::format_to(out, "none");
			if (bid && ask)
				out = fmt::format_to(out, " spread={}", *ask - *bid);
			return fmt::format_to(out, "]");
		});
	}
};

/// @brief A Level as @c "Level[@100 x 30, 3 orders]" — aggregate size and depth,
///        not the individual orders, which are rarely what a log line wants.
template <>
struct fmt::formatter<exchange::engine::Level>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::engine::Level &level,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "Level[@{} x {}, {} orders]",
								  level.price,
								  level.total_volume(),
								  level.orders.size());
		});
	}
};
