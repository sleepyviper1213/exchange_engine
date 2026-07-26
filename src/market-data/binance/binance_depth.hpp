#pragma once

#include "fwd.hpp"
#include "trading-engine/order_book/order_book.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

//class order_book;


namespace exchange::market_data::binance {

using exchange::engine::order_book;

/**
 * @brief One aggregated price level from a Binance depth snapshot.
 *
 * Prices and sizes are stored as integers scaled by 10^decimals (no floating
 * point), so they drop straight into OrderBook's integral Price/Volume.
 */
struct PriceLevel {
	Price price;
	Volume volume;
};

/**
 * @brief A parsed @c /api/v3/depth payload.
 *
 * Binance returns bids best-first (descending) and asks best-first (ascending)
 * — already in OrderBook's preferred order.
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
 * @return The scaled integer, or an error message on malformed input.
 */
[[nodiscard]] MARKET_DATA_EXPORT std::expected<std::int64_t, std::string>
parse_scaled(std::string_view text, int decimals);

/**
 * @brief Parse a Binance REST depth payload into a DepthSnapshot.
 * @param json The raw JSON body.
 * @param priceDecimals Tick precision for the symbol (e.g. SOLUSDT uses 2).
 * @param qtyDecimals Step precision for the symbol (e.g. SOLUSDT uses 2).
 * @return The parsed snapshot, or an error message on malformed input.
 * @see Binance exchangeInfo tickSize/stepSize.
 */
[[nodiscard]] MARKET_DATA_EXPORT std::expected<DepthSnapshot, std::string>
parse_binance_depth(std::string_view json, int priceDecimals, int qtyDecimals);

/**
 * @brief One @c depthUpdate diff event from the WebSocket @c \<symbol\>@depth
 * stream.
 *
 * Unlike a snapshot, each level here is an @em absolute aggregated quantity,
 * not a delta: a level whose @c volume is 0 means "remove this price".
 * Replaying these onto a book seeded from a REST snapshot reconstructs the live
 * book — this is the managed-local-order-book procedure Binance documents.
 *
 * @see
 * https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams
 */
struct DepthUpdate {
	std::uint64_t eventTime = 0; ///< @c E — event time (ms since epoch)
	std::uint64_t firstUpdateId =
		0;    ///< @c U — first update id covered by the event
	std::uint64_t finalUpdateId =
		0;    ///< @c u — last update id covered by the event
	std::vector<PriceLevel>
		bids; ///< @c b — bid_ levels, absolute qty (0 = remove)
	std::vector<PriceLevel>
		asks; ///< @c a — ask levels, absolute qty (0 = remove)
};

/**
 * @brief Parse one Binance @c depthUpdate WebSocket message into a DepthUpdate.
 * @param json The raw JSON of a single @c depthUpdate frame.
 * @param priceDecimals Tick precision for the symbol.
 * @param qtyDecimals Step precision for the symbol.
 * @return The parsed diff event, or an error message on malformed input.
 * @note @c e (event type) and @c s (symbol) fields, if present, are ignored.
 */
[[nodiscard]] MARKET_DATA_EXPORT std::expected<DepthUpdate, std::string>
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
	std::expected<std::vector<DepthUpdate>, std::string>
	parse_binance_depth_updates(std::string_view jsonl, int priceDecimals,
								int qtyDecimals);

/**
 * @brief Apply one @c depthUpdate diff to an OrderBook via absolute set_level.
 *
 * Each level in @p update is an absolute aggregated size, so it maps directly
 * to
 * @c OrderBook::set_level; a level whose volume is 0 removes that price. This
 * is the per-event step of the managed-local-order-book replay (seed from a
 * REST snapshot, then stream diffs through this).
 * @param book The book to mutate.
 * @param update The diff event whose bid/ask levels are set.
 */
MARKET_DATA_EXPORT void apply_depth_update(order_book &book,
										   const DepthUpdate &update);

/**
 * @brief A reusable depth parser for the steady-state hot path.
 *
 * The free @c parse_binance_depth* functions construct a fresh simdjson parser
 * and a fresh padded input buffer on every call — fine for one-shot use,
 * wasteful when decoding a @c \@depth WebSocket stream frame after frame. This
 * owns both across calls: simdjson amortizes its internal structural-index/tape
 * buffers, and the input buffer's allocation is reused (only regrown when a
 * frame is larger than any seen so far). On a steady feed this removes the
 * per-frame allocations that dominate tick-to-book latency.
 *
 * @note Stateful and @b not thread-safe — use one instance per consuming
 * thread. Each returned view/snapshot is independent of the parser's buffers
 * (levels are materialized into owned vectors before returning), so results
 * outlive the next @c parse_* call.
 */
class MARKET_DATA_EXPORT DepthParser {
public:
	DepthParser();
	~DepthParser();
	DepthParser(DepthParser &&) noexcept;
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
	[[nodiscard]] std::expected<DepthSnapshot, std::string>
	parse_snapshot(std::string_view json, int priceDecimals, int qtyDecimals);

	/**
	 * @brief Parse one @c depthUpdate frame, reusing this parser's buffers.
	 * @param json The raw JSON of a single @c depthUpdate frame.
	 * @param priceDecimals Tick precision for the symbol.
	 * @param qtyDecimals Step precision for the symbol.
	 * @return The parsed diff event, or an error message on malformed input.
	 * @see parse_binance_depth_update
	 */
	[[nodiscard]] std::expected<DepthUpdate, std::string>
	parse_update(std::string_view json, int priceDecimals, int qtyDecimals);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace exchange::market_data::binance
