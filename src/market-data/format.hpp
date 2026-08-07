#pragma once

// fmt formatters for the market-data composite value types.

#include "binance/binance_depth.hpp"
#include "binance/endpoints.hpp"
#include "l2_book.hpp"
#include "normalised.hpp"
#include "sequencer.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace exchange::market_data {

/**
 * @brief A book plus the precision needed to print its integers in human units.
 *
 * @c l2_book stores prices and sizes as integers scaled by the symbol's tick
 * and step, and deliberately carries no record of what those decimals were —
 * the book is precision-agnostic, which is what lets it compare and sort
 * exactly. The consequence is that formatting a book on its own can only print
 * raw ticks:
 * @c "@7866 x 54233700000".
 *
 * Callers that do know the symbol's precision (the CLI takes it as an option)
 * wrap the book in this to get @c "@78.66 x 542.33700000" instead. Formatting a
 * @c book_ladder is otherwise identical to formatting the book directly.
 */
struct book_ladder {
	const l2_book *book;        ///< Never null.
	int price_decimals     = 0; ///< Tick precision; 0 prints raw ticks.
	int qty_decimals       = 0; ///< Step precision; 0 prints raw units.
	std::size_t max_levels = 0; ///< Levels per side; 0 prints every one.
};

namespace detail {

/// @brief Render @p value scaled by 10^@p decimals, e.g. (7866, 2) -> "78.66".
///        A non-positive @p decimals writes the integer exactly as stored.
inline std::string scaled_text(std::int64_t value, int decimals) {
	if (decimals <= 0) return fmt::format("{}", value);
	std::int64_t unit = 1;
	for (int i = 0; i < decimals; ++i) unit *= 10;
	// A resting level never carries a negative size — l2_book erases at qty <=
	// 0 — and a published price is positive. Both scaled types are signed all
	// the same, so a negative one is representable and reachable through a
	// malformed frame. Sign is still handled, because a diagnostic printer must
	// not be the component that hides malformed data.
	const bool negative          = value < 0;
	const std::int64_t magnitude = negative ? -value : value;
	return fmt::format("{}{}.{:0{}}",
					   negative ? "-" : "",
					   magnitude / unit,
					   magnitude % unit,
					   decimals);
}

/// @brief One ladder cell, e.g. @c "@78.66 x 542.33700000".
inline std::string level_text(const l2_book::price_level &level, int price_decimals,
							  int qty_decimals) {
	return fmt::format(
		"@{} x {}",
		scaled_text(static_cast<std::int64_t>(level.price), price_decimals),
		scaled_text(level.qty, qty_decimals));
}

/// Column width for the bid cell. "@78.66 x 542.33700000" is 21 characters, so
/// 28 keeps an 8-decimal size on a 2-decimal tick aligned without wrapping.
inline constexpr int BID_COLUMN = 28;

/**
 * @brief Write a book as a header line plus a bid | ask ladder, best first.
 *
 * Row count is the deeper side's, so an asymmetric book (the usual case when
 * diffs are replayed without a snapshot seed) still shows every level it holds
 * rather than truncating to the shallower side. @p max_levels of 0 prints all
 * of them; anything else prints that many and states how many were withheld,
 * because a silently truncated book reads as a shallow one.
 */
template <typename Out>
Out write_ladder(Out out, const l2_book &book, int price_decimals,
				 int qty_decimals, std::size_t max_levels) {
	const auto &bids = book.bid_levels();
	const auto &asks = book.ask_levels();
	out              = fmt::format_to(out,
									  "l2_book[bids={} asks={}]",
									  bids.size(),
									  asks.size());

	const std::size_t deepest = std::max(bids.size(), asks.size());
	const std::size_t rows =
		max_levels == 0 ? deepest : std::min(deepest, max_levels);
	for (std::size_t i = 0; i < rows; ++i) {
		const std::string bid =
			i < bids.size() ? level_text(bids[i], price_decimals, qty_decimals)
							: std::string();
		const std::string ask =
			i < asks.size() ? level_text(asks[i], price_decimals, qty_decimals)
							: std::string();
		// The empty-ask row omits the trailing separator space rather than
		// emitting invisible trailing whitespace.
		out = ask.empty()
				  ? fmt::format_to(out, "\n{:>{}} |", bid, BID_COLUMN)
				  : fmt::format_to(out, "\n{:>{}} | {}", bid, BID_COLUMN, ask);
	}
	if (rows < deepest)
		out = fmt::format_to(out,
							 "\n{:>{}} | ... {} deeper level(s) not shown",
							 "",
							 BID_COLUMN,
							 deepest - rows);
	return out;
}

} // namespace detail
} // namespace exchange::market_data

/// @brief An aggregated level as @c "@15000 x 100" (price, absolute size).
template <>
struct fmt::formatter<exchange::market_data::l2_book::price_level>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::market_data::l2_book::price_level &level,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out, "@{} x {}", level.price, level.qty);
		});
	}
};

// No separate formatter for binance::PriceLevel: it is an alias for
// l2_book::price_level, so the specialisation above already covers it. Declaring both
// would be a redefinition of the same specialisation, which is how the alias
// makes "a diff level drops straight into the book" true of the printing too.

/**
 * @brief A reconstructed book. @c "{}" prints every level as a bid | ask
 * ladder:
 *
 * @code
 * l2_book[bids=2 asks=1]
 *                 @15000 x 7 | @15001 x 4
 *                 @14999 x 3 |
 * @endcode
 *
 * Printing a book means wanting to see the book, so full depth is the default.
 * Two specs narrow it:
 *
 * - @c "{:s}" — the one-line summary
 *   @c "l2_book[bids=2 asks=1 best @15000 x 7 / @15001 x 4]", which is what a
 * log line wants. Sides with no levels read @c "none".
 * - @c "{:.N}" — the ladder capped at N levels per side, stating how many were
 *   withheld. Worth reaching for: a snapshot seeded at Binance's maximum depth
 * is 5000 levels a side, and printing that unguarded is 5000 lines.
 *
 * Prices and sizes appear as the scaled integers the book stores, because an
 * @c l2_book carries no record of the symbol's precision. Wrap it in a
 * @c book_ladder to print human units.
 */
template <>
struct fmt::formatter<exchange::market_data::l2_book> {
	bool summary           = false;
	std::size_t max_levels = 0;

	constexpr auto parse(format_parse_context &ctx)
		-> format_parse_context::iterator {
		const auto *it        = ctx.begin();
		const auto *const end = ctx.end();
		if (it != end && *it == '.') {
			++it;
			std::size_t levels = 0;
			for (; it != end && *it >= '0' && *it <= '9'; ++it)
				levels = levels * 10 + static_cast<std::size_t>(*it - '0');
			max_levels = levels;
		}
		if (it != end && *it == 's') {
			summary = true;
			++it;
		}
		// Anything left before '}' is not ours; returning here lets fmt raise
		// its own "invalid format specifier" rather than us inventing a
		// message.
		return it;
	}

	auto format(const exchange::market_data::l2_book &book,
				format_context &ctx) const -> format_context::iterator {
		using exchange::side_t;
		if (!summary)
			return exchange::market_data::detail::write_ladder(ctx.out(),
															   book,
															   0,
															   0,
															   max_levels);

		const auto &bids = book.bid_levels();
		const auto &asks = book.ask_levels();
		auto out         = fmt::format_to(ctx.out(),
										  "l2_book[bids={} asks={} best ",
										  bids.size(),
										  asks.size());
		if (bids.empty()) out = fmt::format_to(out, "none");
		else out = fmt::format_to(out, "{}", bids.front());
		out = fmt::format_to(out, " / ");
		if (asks.empty()) out = fmt::format_to(out, "none");
		else out = fmt::format_to(out, "{}", asks.front());
		return fmt::format_to(out, "]");
	}
};

/// @brief A book rendered in human units, e.g. @c "@78.66 x 542.33700000".
///        Same layout as @c "{}" on an @c l2_book; see @c book_ladder for why
///        the precision has to be supplied from outside.
template <>
struct fmt::formatter<exchange::market_data::book_ladder> {
	constexpr auto parse(format_parse_context &ctx)
		-> format_parse_context::iterator {
		return ctx.begin();
	}

	auto format(const exchange::market_data::book_ladder &ladder,
				format_context &ctx) const -> format_context::iterator {
		return exchange::market_data::detail::write_ladder(
			ctx.out(),
			*ladder.book,
			ladder.price_decimals,
			ladder.qty_decimals,
			ladder.max_levels);
	}
};

/// @brief A depth-parse failure as @c "[line L: ][context: ]category" — the
///        same text @c binance::message() returns, which is now defined in
///        terms of this so the two cannot drift.
template <>
struct fmt::formatter<exchange::market_data::binance::depth_parse_error>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::market_data::binance::depth_parse_error &error,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			if (error.line) out = fmt::format_to(out, "line {}: ", error.line);
			if (!error.context.empty())
				out = fmt::format_to(out, "{}: ", error.context);
			// error.code goes through its format_as -> category message.
			return fmt::format_to(out, "{}", error.code);
		});
	}
};

/// @brief A WebSocket endpoint as the @c wss:// URL it denotes — paste-able
///        straight into a client when a capture misbehaves.
template <>
struct fmt::formatter<exchange::market_data::binance::stream_endpoint>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::market_data::binance::stream_endpoint &endpoint,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "wss://{}:{}{}",
								  endpoint.host,
								  endpoint.port,
								  endpoint.target);
		});
	}
};

/// @brief A REST endpoint as the @c https:// URL it denotes.
template <>
struct fmt::formatter<exchange::market_data::binance::http_endpoint>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::market_data::binance::http_endpoint &endpoint,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "https://{}{}",
								  endpoint.host,
								  endpoint.target);
		});
	}
};

/// @brief A REST snapshot as @c "DepthSnapshot[lastUpdateId=1 bids=100
///        asks=100]" — its sequencing id and shape, not its levels.
template <>
struct fmt::formatter<exchange::market_data::binance::DepthSnapshot>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::market_data::binance::DepthSnapshot &snapshot,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "DepthSnapshot[lastUpdateId={} bids={} "
								  "asks={}]",
								  snapshot.lastUpdateId,
								  snapshot.bids.size(),
								  snapshot.asks.size());
		});
	}
};

/// @brief A diff event as @c "depthUpdate[U=1 u=5 bids=3 asks=2]", naming the
///        update-id bounds the way Binance's own field letters do — those are
///        what you compare against lastUpdateId to sequence the local book.
template <>
struct fmt::formatter<exchange::market_data::binance::DepthUpdate>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::market_data::binance::DepthUpdate &update,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "depthUpdate[U={} u={} bids={} asks={}]",
								  update.firstUpdateId,
								  update.finalUpdateId,
								  update.bids.size(),
								  update.asks.size());
		});
	}
};

/// @brief The bookkeeping half of a diff event, as @c "depthUpdate[U=1 u=5]".
template <>
struct fmt::formatter<exchange::market_data::binance::DepthUpdateMeta>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::market_data::binance::DepthUpdateMeta &meta,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "depthUpdate[U={} u={}]",
								  meta.firstUpdateId,
								  meta.finalUpdateId);
		});
	}
};

/// @brief A sequence span as @c "1..5", or just @c "5" when it covers a single
///        number — the range notation reads the same for every venue, which is
///        the point of normalising away @c U / @c u.
template <typename T>
struct fmt::formatter<exchange::core::util::inclusive_range<T>>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::core::util::inclusive_range<T> &sequence,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			if (sequence.is_identity())
				return fmt::format_to(out, "{}", sequence.first());
			return fmt::format_to(out,
								  "{}..{}",
								  sequence.first(),
								  sequence.last());
		});
	}
};

/// @brief A normalised diff as @c "depth_event[1..5 bids=3 asks=2]".
template <>
struct fmt::formatter<exchange::market_data::depth_event>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::market_data::depth_event &event,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "depth_event[{} bids={} asks={}]",
								  event.sequence,
								  event.bids.size(),
								  event.asks.size());
		});
	}
};

/// @brief A normalised snapshot as @c "book_snapshot[seq=42 bids=100
///        asks=100]" — the sequence it seeds from and its shape.
template <>
struct fmt::formatter<exchange::market_data::book_snapshot>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::market_data::book_snapshot &snapshot,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "book_snapshot[seq={} bids={} asks={}]",
								  snapshot.sequence,
								  snapshot.bids.size(),
								  snapshot.asks.size());
		});
	}
};

/// @brief Feed health as @c "seq[applied=5 discarded=1 buffered=2 overlapped=0
///        gaps=1]" — one log line that says whether the replica can be trusted.
template <>
struct fmt::formatter<exchange::market_data::sequencer_stats>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::market_data::sequencer_stats &stats,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "seq[applied={} discarded={} buffered={} "
								  "overlapped={} gaps={}]",
								  stats.applied,
								  stats.discarded,
								  stats.buffered,
								  stats.overlapped,
								  stats.gaps);
		});
	}
};
