#pragma once

#include "market_data_export.hpp" // MARKET_DATA_EXPORT (generated)
#include "core/util/enum_string.hpp"
#include "fwd.hpp"
#include "depth_error.hpp" // IWYU pragma: export
#include "market-data/l2_book.hpp"    // the reconstruction target
#include "market-data/parser/fwd.hpp" // parser::parse_error
#include "market-data/types.hpp"      // scaled_price_t / scaled_qty_t

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace exchange::market_data::binance {

/// @brief Category of a depth-parsing failure.
/**
 * @brief A depth-parse failure: a category plus optional static context.
 *
 * @c context is always a static string (an offending field name, simdjson's own
 * message, or the numeric @c parse_error message) - never a view into the
 * parsed buffer, so it outlives the parse call. @c line is the 1-based line in
 * a JSONL feed, or 0 when not applicable.
 */
struct depth_parse_error {
	depth_error code;
	std::string_view context{}; ///< Default-initialised so a brace-init may
	                            ///< name only the code. @see feed_status::detail
	std::uint32_t line = 0;
};

/// @brief Render a @c depth_parse_error as "[line L: ][context: ]category".
[[nodiscard]] MARKET_DATA_EXPORT std::string
message(const depth_parse_error &error);

/**
 * @brief One aggregated price level from a Binance depth snapshot.
 *
 * Prices and sizes are integers scaled by 10^decimals (no floating point), so
 * they drop straight into @c l2_book - and this is literally that type, not a
 * struct shaped like it.
 *
 * @par Why an alias and not its own struct
 * It was its own struct with exactly these two fields - two types of identical
 * layout, unrelated to the compiler purely because they were spelled twice. The
 * separation was supposed to keep the venue-neutral layer independent of a venue
 * decoder, but this header already includes @c l2_book.hpp (the reconstruction
 * target is the whole point of the decoder), so the dependency it was protecting
 * did not exist.
 *
 * @note Merging them did @b not speed anything up, which was the original
 *       motivation and was wrong. @c binance::normalise still copies level by
 *       level, because collapsing that to a whole-vector copy - which the shared
 *       type now permits - measured ~15% @em slower on
 *       @c BM_Reconstructor_SteadyState. See the note on @c to_levels in
 *       normalise.cpp for the measurement and the likely reason. What the alias
 *       actually bought was one type instead of two, one fmt formatter instead
 *       of two, and one less forward declaration.
 *
 * The name stays because a Binance frame reads better with it, and because a
 * venue whose levels ever carry more than a price and a size gets its own
 * struct back at that point - the alias is what makes that a local change.
 */
using PriceLevel = l2_book::price_level;

/**
 * @brief A parsed @c /api/v3/depth payload.
 *
 * Binance returns bids best-first (descending) and asks best-first (ascending)
 * - already in @c l2_book's preferred order.
 */
struct DepthSnapshot {
	std::uint64_t lastUpdateId = 0;
	std::vector<PriceLevel> bids;
	std::vector<PriceLevel> asks;
};

/**
 * @brief Convert a decimal string to an integer scaled by 10^decimals.
 *
 * Parses without floating point. Extra fractional digits are truncated; missing
 * ones are zero-padded. For example @c parse_scaled("153.45000000", 8) yields
 * @c 15345000000.
 * @param text The decimal string (optionally signed).
 * @param decimals Number of fractional digits to scale by; must be >= 0.
 * @return The scaled integer, or a @c parser::parse_error on malformed input.
 */
[[nodiscard]] MARKET_DATA_EXPORT
	std::expected<std::int64_t, parser::parse_error>
	parse_scaled(std::string_view text, int decimals);

/**
 * @brief Parse a Binance REST depth payload into a DepthSnapshot.
 * @param json The raw JSON body.
 * @param priceDecimals Tick precision for the symbol (e.g. SOLUSDT uses 2).
 * @param qtyDecimals Step precision for the symbol (e.g. SOLUSDT uses 2).
 * @return The parsed snapshot, or an error message on malformed input.
 * @see Binance exchangeInfo tickSize/stepSize.
 */
[[nodiscard]] MARKET_DATA_EXPORT std::expected<DepthSnapshot, depth_parse_error>
parse_binance_depth(std::string_view json, int priceDecimals, int qtyDecimals);

/**
 * @brief One @c depthUpdate diff event from the WebSocket @c \<symbol\>@depth
 * stream.
 *
 * Unlike a snapshot, each level here is an @em absolute aggregated quantity,
 * not a delta: a level whose @c qty is 0 means "remove this price".
 * Replaying these onto a book seeded from a REST snapshot reconstructs the live
 * book - this is the managed-local-order-book procedure Binance documents.
 *
 * @see
 * https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams
 */
struct DepthUpdate {
	std::uint64_t eventTime = 0; ///< @c E - event time (ms since epoch)
	std::uint64_t firstUpdateId =
		0;    ///< @c U - first update id covered by the event
	std::uint64_t finalUpdateId =
		0;    ///< @c u - last update id covered by the event
	std::vector<PriceLevel>
		bids; ///< @c b - bid_ levels, absolute qty (0 = remove)
	std::vector<PriceLevel>
		asks; ///< @c a - ask levels, absolute qty (0 = remove)
};

/**
 * @brief The bookkeeping fields of a @c depthUpdate - everything except the
 * levels, which the streaming apply path writes straight to the book.
 *
 * Returned by @c apply_binance_depth_update / @c DepthParser::apply_update so
 * the caller still gets the update ids needed to sequence the managed local
 * order book (drop events already covered, detect gaps against lastUpdateId).
 */
struct DepthUpdateMeta {
	std::uint64_t eventTime     = 0; ///< @c E - event time (ms since epoch)
	std::uint64_t firstUpdateId = 0; ///< @c U - first update id covered
	std::uint64_t finalUpdateId = 0; ///< @c u - last update id covered
};

/**
 * @brief Parse one Binance @c depthUpdate WebSocket message into a DepthUpdate.
 * @param json The raw JSON of a single @c depthUpdate frame.
 * @param priceDecimals Tick precision for the symbol.
 * @param qtyDecimals Step precision for the symbol.
 * @return The parsed diff event, or an error message on malformed input.
 * @note @c e (event type) and @c s (symbol) fields, if present, are ignored.
 */
[[nodiscard]] MARKET_DATA_EXPORT std::expected<DepthUpdate, depth_parse_error>
parse_binance_depth_update(std::string_view json, int priceDecimals,
						   int qtyDecimals);

/**
 * @brief Parse a newline-delimited capture of @c depthUpdate frames (JSONL).
 *
 * One JSON object per line, as produced by piping the @c \<symbol\>@depth
 * stream to a file (e.g. via @c websocat). Blank lines are skipped. This is the
 * offline feed for the market-replay benchmark.
 * @param jsonl The whole file contents; each non-blank line is one frame.
 * @param priceDecimals Tick precision for the symbol.
 * @param qtyDecimals Step precision for the symbol.
 * @return The parsed events in file order, or the first line's error
 * (1-indexed).
 */
[[nodiscard]] MARKET_DATA_EXPORT
	std::expected<std::vector<DepthUpdate>, depth_parse_error>
	parse_binance_depth_updates(std::string_view jsonl, int priceDecimals,
								int qtyDecimals);

/**
 * @brief Apply one @c depthUpdate diff to an @c l2_book via absolute set_level.
 *
 * Each level in @p update is an absolute aggregated size, so it maps directly
 * to @c l2_book::set_level; a level whose qty is 0 removes that price. This
 * is the per-event step of the managed-local-order-book replay (seed from a
 * REST snapshot, then stream diffs through this).
 *
 * The target is the L2 reconstruction book, never @c engine::order_book: a diff
 * feed carries no order identity or queue position, so there is nothing to fill
 * an order-by-order book's per-level FIFO with beyond one synthetic anonymous
 * entry. Reconstructed depth and this process's own resting orders are separate
 * state and must not share a book.
 * @param book The book to mutate.
 * @param update The diff event whose bid/ask levels are set.
 */
MARKET_DATA_EXPORT void apply_depth_update(l2_book &book,
										   const DepthUpdate &update);

/**
 * @brief Parse a @c depthUpdate frame and stream its levels straight into
 *        @p book, without building an intermediate DepthUpdate.
 *
 * The zero-copy alternative to @c parse_binance_depth_update followed by
 * @c apply_depth_update: each @c set_level fires as the level is parsed, so no
 * per-frame level vectors are allocated. Use it on the steady @c \@depth feed
 * where the frame is applied immediately and never retained.
 *
 * @param book The book to mutate (levels set to their absolute size; 0
 * removes).
 * @param json The raw JSON of a single @c depthUpdate frame.
 * @param priceDecimals Tick precision for the symbol.
 * @param qtyDecimals Step precision for the symbol.
 * @return The update's ids/time, or an error message on malformed input.
 * @warning Not atomic: a malformed level aborts the frame with the levels
 *          before it already applied. Prefer the parse-then-apply pair when a
 *          frame must be all-or-nothing.
 */
[[nodiscard]] MARKET_DATA_EXPORT
	std::expected<DepthUpdateMeta, depth_parse_error>
	apply_binance_depth_update(l2_book &book, std::string_view json,
							   int priceDecimals, int qtyDecimals);

/**
 * @brief A reusable depth parser for the steady-state hot path.
 *
 * The free @c parse_binance_depth* functions construct a fresh simdjson parser
 * and a fresh padded input buffer on every call - fine for one-shot use,
 * wasteful when decoding a @c \@depth WebSocket stream frame after frame. This
 * owns both across calls: simdjson amortizes its internal structural-index/tape
 * buffers, and the input buffer's allocation is reused (only regrown when a
 * frame is larger than any seen so far). On a steady feed this removes the
 * per-frame allocations that dominate tick-to-book latency.
 *
 * @note Stateful and @b not thread-safe - use one instance per consuming
 * thread. Each returned view/snapshot is independent of the parser's buffers
 * (levels are materialised into owned vectors before returning), so results
 * outlive the next @c parse_* call.
 */
class DepthParser {
public:
	MARKET_DATA_EXPORT DepthParser();
	MARKET_DATA_EXPORT ~DepthParser();
	MARKET_DATA_EXPORT DepthParser(DepthParser &&) noexcept;
	DepthParser &operator=(DepthParser &&) noexcept;
	DepthParser(const DepthParser &)            = delete;
	DepthParser &operator=(const DepthParser &) = delete;

	/**
	 * @brief Parse a REST depth snapshot, reusing this parser's buffers.
	 * @param json The raw JSON body.
	 * @param priceDecimals Tick precision for the symbol.
	 * @param qtyDecimals Step precision for the symbol.
	 * @return The parsed snapshot, or an error message on malformed input.
	 * @see parse_binance_depth
	 */
	[[nodiscard]] std::expected<DepthSnapshot, depth_parse_error>
	parse_snapshot(std::string_view json, int priceDecimals, int qtyDecimals);

	/**
	 * @brief Parse one @c depthUpdate frame, reusing this parser's buffers.
	 * @param json The raw JSON of a single @c depthUpdate frame.
	 * @param priceDecimals Tick precision for the symbol.
	 * @param qtyDecimals Step precision for the symbol.
	 * @return The parsed diff event, or an error message on malformed input.
	 * @see parse_binance_depth_update
	 */
	[[nodiscard]] std::expected<DepthUpdate, depth_parse_error>
	parse_update(std::string_view json, int priceDecimals, int qtyDecimals);

	/**
	 * @brief Parse one @c depthUpdate frame and stream its levels straight into
	 *        @p book, reusing this parser's buffers.
	 *
	 * The hot-path form of @c apply_binance_depth_update: it reuses the parser
	 * and input buffer across frames @b and skips the per-frame level vectors.
	 * @param book The book to mutate.
	 * @param json The raw JSON of a single @c depthUpdate frame.
	 * @param priceDecimals Tick precision for the symbol.
	 * @param qtyDecimals Step precision for the symbol.
	 * @return The update's ids/time, or an error message on malformed input.
	 * @warning Not atomic (see @c apply_binance_depth_update).
	 */
	[[nodiscard]] MARKET_DATA_EXPORT std::expected<DepthUpdateMeta, depth_parse_error>
	apply_update(l2_book &book, std::string_view json, int priceDecimals,
				 int qtyDecimals);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace exchange::market_data::binance
