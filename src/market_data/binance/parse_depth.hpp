#pragma once
// One-shot depth parsing: construct, decode, discard.
//
// Every function here builds a fresh simdjson parser and a fresh padded input
// buffer per call. That is the right trade for a snapshot fetched once or a
// capture decoded offline, and the wrong one for a live @depth stream - which
// is what depth_parser.hpp exists for.

#include "depth_parse_error.hpp"
#include "depth_snapshot.hpp"
#include "depth_update.hpp"
#include "depth_update_meta.hpp"
#include "fwd.hpp"
#include "market_data/l2_book.hpp"
#include "market_data_export.hpp" // MARKET_DATA_EXPORT (generated)

#include <expected>
#include <string_view>
#include <vector>

namespace exchange::market_data::binance {

/**
 * @brief Parse a Binance REST depth payload into a depth_snapshot.
 * @param json The raw JSON body.
 * @param priceDecimals Tick precision for the symbol (e.g. SOLUSDT uses 2).
 * @param qtyDecimals Step precision for the symbol (e.g. SOLUSDT uses 2).
 * @return The parsed snapshot, or an error message on malformed input.
 * @see Binance exchangeInfo tickSize/stepSize.
 */
[[nodiscard]] MARKET_DATA_EXPORT
	std::expected<depth_snapshot, depth_parse_error>
	parse_binance_depth(std::string_view json, int priceDecimals,
						int qtyDecimals);

/**
 * @brief Parse one Binance @c depthUpdate WebSocket message into a
 * depth_update.
 * @param json The raw JSON of a single @c depthUpdate frame.
 * @param priceDecimals Tick precision for the symbol.
 * @param qtyDecimals Step precision for the symbol.
 * @return The parsed diff event, or an error message on malformed input.
 * @note @c e (event type) and @c s (symbol) fields, if present, are ignored.
 */
[[nodiscard]] MARKET_DATA_EXPORT std::expected<depth_update, depth_parse_error>
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
	std::expected<std::vector<depth_update>, depth_parse_error>
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
										   const depth_update &update);

/**
 * @brief Parse a @c depthUpdate frame and stream its levels straight into
 *        @p book, without building an intermediate depth_update.
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
	std::expected<depth_update_meta, depth_parse_error>
	apply_binance_depth_update(l2_book &book, std::string_view json,
							   int priceDecimals, int qtyDecimals);

} // namespace exchange::market_data::binance
