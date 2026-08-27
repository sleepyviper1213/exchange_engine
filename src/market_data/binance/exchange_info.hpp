#pragma once
// The venue's own trading grid for one listing.
//
// Reference data, and the reason this file exists rather than a pair of CLI
// flags: the grid differs per listing, and configuring it wrongly is *silent*.
// `parse_fixed_point` truncates surplus precision rather than refusing it - the
// right choice for a venue that pads everything to eight decimals - which means
// a step configured coarser than the venue's rounds every level below it to zero
// and still reports a clean parse. On BTCUSDT, whose stepSize is 0.00001, a
// configured lot of 0.01 discards essentially the whole book and says nothing.
//
// @see https://developers.binance.com/docs/binance-spot-api-docs/rest-api

#include "market_data_export.hpp"

#include <expected>
#include <string>
#include <string_view>

namespace exchange::market_data::binance {

/**
 * @brief The price and size grid a venue publishes for one listing.
 *
 * @note The increments are kept as the **decimal text the venue sent**, not as
 *       scaled integers. That is deliberate and it is the project's rule rather
 *       than this file's preference: a decimal is converted to an integer
 *       exactly once, at the edge, on a scale that has been chosen - and the
 *       scale is what @c decimals below carries. Converting here would mean
 *       choosing it twice.
 */
struct symbol_filters {
	std::string symbol; ///< as the venue spells it, e.g. @c SOLUSDT
	std::string status; ///< @c TRADING, @c HALT, @c BREAK ...

	/**
	 * @brief @c PRICE_FILTER.tickSize, with the venue's zero padding removed.
	 *
	 * Trimmed rather than verbatim, and that is a requirement rather than
	 * tidiness: Binance right-pads to eight decimals, and @c parse_exact_decimal
	 * refuses a decimal that is not exactly representable at the scale it is
	 * handed - so @c "0.01000000" at scale 2 comes back malformed. Correctly,
	 * since dropping digits silently is how a grid ends up wrong. Trimming
	 * removes only zeros, so nothing is lost.
	 */
	std::string tick_size;

	/// @brief @c LOT_SIZE.stepSize, trimmed on the same terms. @see tick_size
	std::string step_size;

	/// @brief Significant decimals in @c tick_size - the scale prices must be
	///        read on for the grid to be representable.
	int price_decimals = 0;

	/// @brief Significant decimals in @c step_size. Frequently *not* equal to
	///        @c price_decimals, which is the whole trap: SOLUSDT is 2 and 3,
	///        ETHUSDT 2 and 4, BTCUSDT 2 and 5.
	int qty_decimals = 0;

	/// @brief Whether the venue is currently matching this listing.
	[[nodiscard]] bool is_trading() const noexcept { return status == "TRADING"; }

	bool operator==(const symbol_filters &) const noexcept = default;
};

/**
 * @brief Significant decimals in a venue increment such as @c "0.00100000".
 *
 * Trailing zeros are padding, not precision: Binance sends every number at eight
 * decimals whatever the instrument, so the position of the last *non-zero* digit
 * is what states the grid. Returns 0 for an integral increment like @c "1.00".
 *
 * Exposed because it is the whole conversion from "what the venue said" to "what
 * scale we must read on", and a rule that decides how every price in a run is
 * quantised deserves to be testable on its own.
 */
[[nodiscard]] MARKET_DATA_EXPORT int
significant_decimals(std::string_view increment) noexcept;

/**
 * @brief Decode a single-symbol @c /api/v3/exchangeInfo response.
 *
 * @param json The response body.
 * @param symbol The listing expected in it. Checked rather than assumed: the
 *        unfiltered endpoint returns every listing on the venue, and silently
 *        reading the first entry of *that* would configure a run for whatever
 *        happens to sort first.
 * @return The grid, or why the body could not be used.
 *
 * @note A listing that is present but not @c TRADING is returned rather than
 *       refused. Whether to run against a halted instrument is the caller's
 *       decision - a capture of a halt is a legitimate thing to want - and this
 *       function's job is to report what the venue said.
 */
[[nodiscard]] MARKET_DATA_EXPORT std::expected<symbol_filters, std::string>
parse_exchange_info(std::string_view json, std::string_view symbol);

} // namespace exchange::market_data::binance
