#pragma once
// Decoding Binance's `<symbol>@trade` stream: the venue's own shape.
//
// The mirror of binance_depth.hpp for the tape. The split between this and
// normalise.hpp is the same one that file already draws: here the frame keeps
// Binance's vocabulary - `t`, `p`, `q`, `T`, and the maker flag `m` - and
// normalise() is the single place that turns it into a venue-neutral
// trade_print. Nothing downstream of normalise() knows what `m` meant.
// @see
// https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams

#include "core/util/indirect.hpp"
#include "fwd.hpp"
#include "market_data/types.hpp" // scaled_price_t, scaled_qty_t
#include "market_data_export.hpp"
#include "trade_error.hpp"       // IWYU pragma: export

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace exchange::market_data::binance {

/**
 * @brief A trade-parsing failure: a category plus optional static context.
 *
 * @c context is always a static string - an offending field name, simdjson's
 * own message, or the numeric @c parse_error message - and never a view into
 * the parsed buffer, so it outlives the parse call. That is the contract
 * @c feed_status::detail requires of anything handed across the feed seam.
 * @c line is the 1-based line in a JSONL tape, or 0 when not applicable.
 */
struct trade_parse_error {
	trade_error code;
	std::string_view context{}; ///< Default-initialised so a brace-init may
								///< name only the code.
	std::uint32_t line = 0;
};

/// @brief Render a @c trade_parse_error as "[line L: ][context: ]category".
[[nodiscard]] MARKET_DATA_EXPORT std::string
message(const trade_parse_error &error);

/**
 * @brief One @c trade frame from the WebSocket @c \<symbol\>@trade stream.
 *
 * Prices and sizes are already integers scaled by 10^decimals - no floating
 * point anywhere on the path - exactly as the depth decoder produces them.
 *
 * @par Why `@trade` rather than `@aggTrade`
 * Because they answer different questions and only one of them is the tape.
 * @c aggTrade collapses every fill that one taker order got against one price
 * into a single message, so counting its messages counts *aggressing orders*
 * while counting these counts *fills*. The finer feed is the one that can be
 * aggregated afterwards; the coarser one cannot be un-aggregated. A decoder for
 * @c aggTrade is a second struct and a second normalise() beside these, and
 * nothing above them changes.
 */
struct trade_message {
	std::uint64_t event_time = 0; ///< @c E - when the venue sent it (ms)
	std::uint64_t trade_time = 0; ///< @c T - when the trade executed (ms)
	std::uint64_t trade_id   = 0; ///< @c t - the venue's trade id
	scaled_price_t price     = 0; ///< @c p - execution price, scaled
	scaled_qty_t qty         = 0; ///< @c q - executed size, scaled
	/**
	 * @brief @c m - was the *buyer* the maker?
	 *
	 * Kept in the venue's own spelling rather than converted here, because the
	 * conversion is a sign flip and this file is not where sign conventions are
	 * decided. True means the resting order was a bid and the taker therefore
	 * sold. @see normalise(const trade_message &).
	 */
	bool buyer_is_maker = false;
};

/**
 * @brief Parse one @c trade frame into a @ref trade_message.
 * @param json The raw JSON of a single @c trade frame.
 * @param price_decimals Tick precision for the symbol.
 * @param qty_decimals Step precision for the symbol.
 * @return The parsed print, or an error on malformed input.
 * @note @c e (event type), @c s (symbol) and @c M (ignore flag) are skipped.
 */
[[nodiscard]] MARKET_DATA_EXPORT std::expected<trade_message, trade_parse_error>
parse_binance_trade(std::string_view json, int price_decimals,
					int qty_decimals);

/**
 * @brief Parse a newline-delimited capture of @c trade frames (JSONL).
 *
 * One JSON object per line, as @c transport::ws::capture writes it. Blank lines
 * are skipped.
 * @param jsonl The whole file contents; each non-blank line is one frame.
 * @param price_decimals Tick precision for the symbol.
 * @param qty_decimals Step precision for the symbol.
 * @return The prints in file order, or the first failing line's error
 *         (1-indexed).
 * @note Materialises the whole tape. That is right for a test corpus and wrong
 *       for a recording - a busy symbol prints tens of thousands of trades a
 *       minute - so a driver reading a capture should use @ref jsonl_trade_feed
 *       instead, which decodes one frame per pull.
 */
[[nodiscard]] MARKET_DATA_EXPORT
	std::expected<std::vector<trade_message>, trade_parse_error>
	parse_binance_trades(std::string_view jsonl, int price_decimals,
						 int qty_decimals);

/**
 * @brief A reusable trade parser for the steady-state hot path.
 *
 * The free @c parse_binance_trade constructs a fresh simdjson parser and a
 * fresh padded input buffer per call - fine once, wasteful frame after frame.
 * This owns both across calls, exactly as @ref depth_parser does, so a steady
 * tape does no per-frame allocation.
 *
 * @note Stateful and @b not thread-safe: one instance per consuming thread.
 *       Each returned @c trade_message is a handful of scalars and holds no
 *       view into the parser's buffers, so it outlives the next call
 *       unconditionally - the caveat depth_parser has to state about level
 *       vectors does not arise here.
 */
class trade_parser {
public:
	MARKET_DATA_EXPORT trade_parser();
	MARKET_DATA_EXPORT ~trade_parser();
	/**
	 * @brief Not copyable, and with @c core::util::indirect that is now a
	 *        decision rather than a consequence.
	 *
	 * A @c unique_ptr pimpl cannot be copied, so the deletion used to be the
	 * language's doing. @c indirect *is* copyable - deep-copying what it owns
	 * is the whole reason C++26 adds it - so a copyable trade_parser is only a
	 * `= default` away, and it is refused because the simdjson parser and its
	 * reused buffers behind
	 * @c impl are not copyable either.
	 */
	trade_parser(const trade_parser &)            = delete;
	trade_parser &operator=(const trade_parser &) = delete;

	/**
	 * @brief Movable, and both halves defined out of line.
	 *
	 * @c impl is incomplete here, and moving out of an @c indirect steals a
	 * pointer while move-*assignment* first destroys what this one owns - which
	 * needs the complete type. So does the destructor. All three are declared
	 * here and defined in the .cpp beside @c impl; a compiler-generated one in
	 * this header would not compile.
	 *
	 * @post The moved-from object owns no @c impl. @see
	 * indirect::valueless_after_move
	 */
	MARKET_DATA_EXPORT trade_parser(trade_parser &&) noexcept;
	MARKET_DATA_EXPORT trade_parser &operator=(trade_parser &&) noexcept;

	/**
	 * @brief Parse one @c trade frame, reusing this parser's buffers.
	 * @param json The raw JSON of a single @c trade frame.
	 * @param price_decimals Tick precision for the symbol.
	 * @param qty_decimals Step precision for the symbol.
	 * @return The parsed print, or an error on malformed input.
	 * @see parse_binance_trade
	 */
	[[nodiscard]] MARKET_DATA_EXPORT
		std::expected<trade_message, trade_parse_error>
		parse_trade(std::string_view json, int price_decimals,
					int qty_decimals);

private:
	struct impl;
	core::util::indirect<impl> impl_;
};

} // namespace exchange::market_data::binance
