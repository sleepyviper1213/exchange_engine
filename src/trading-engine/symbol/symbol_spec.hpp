#pragma once
#include "core/types.hpp"
#include "fwd.hpp"
#include "trading-engine/order_book/reject_reason.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace exchange::engine {

/**
 * @brief One listing's trading conventions: the decimal grid a client quotes
 *        on, and the integer grid the engine matches on.
 *
 * Analogous to exchange-core's `Coresymbol_specification` and to the
 * `tickSize / sizeIncrement / referencePrice` triple on Emporia's
 * `ListingDetails`. Its job is to be the *only* place decimals turn into
 * integers, and to refuse rather than round when they do not line up.
 *
 * @par Three number systems, and why they are all here
 * - **Decimal text** — what a client sends: @c "153.45". Has a scale (how many
 *   fractional digits the venue publishes) but no notion of a tick.
 * - **Scaled integer** — the text times 10^scale: @c 15345 at scale 2. This is
 *   what @c market_data stores, and what @c parse_fixed_point produces.
 * - **Ticks and lots** — the scaled value divided by the increment. This is
 *   what @c price_t and @c quantity_t mean inside the book. Prices compare and
 *   sort as plain integers here, and the tick grid is what makes an array-
 *   indexed book possible at all.
 *
 * A tick size of 0.05 at scale 2 is @c tick_scaled == 5, so @c "153.45" is 3069
 * ticks and @c "153.47" is not on the grid and is refused. Emporia spells the
 * same rule `value.divide(increment, 0, RoundingMode.UNNECESSARY)` — the
 * division that throws instead of rounding. That is the single best idea in
 * their tree and it is the whole point of this class.
 *
 * @par Rejecting is the feature
 * A venue that rounds a misaligned price has silently changed a client's order
 * to a different one, and the client finds out from a fill at a price it never
 * asked for. Every conversion here returns @c std::expected and names the
 * reason it failed, so the failure reaches the client as a REJECTED outcome
 * with `PRICE_NOT_ON_TICK` rather than as a surprise execution.
 *
 * @note Immutable after construction, and cheap to copy — a book manager holds
 *       one per listing and hands out const references. Reference data changes
 *       between sessions, not between orders.
 */
class symbol_spec {
public:
	/// @brief Basis points: 1/100th of a percent, the unit venues state price
	///        bands in. 2000 bps is ±20%.
	static constexpr std::int64_t BPS_DENOMINATOR = 10'000;

	/// @brief A @c collar_bps meaning "no price band" — every price on the tick
	///        grid is admissible, and @c collar_span() is not meaningful.
	static constexpr std::int64_t NO_COLLAR = 0;

	/**
	 * @brief Build a listing's spec from the venue's published conventions.
	 *
	 * @param id Dense symbol identifier.
	 * @param symbol Display name, e.g. @c "BTCUSDT". Copied.
	 * @param price_scale Fractional digits in the venue's price strings.
	 * @param qty_scale Fractional digits in the venue's quantity strings.
	 * @param tick_scaled Tick size in 10^-price_scale units. Must be positive:
	 *        a tick of 0.01 at @p price_scale 2 is 1, at scale 8 it is 1'000'000.
	 * @param lot_scaled Lot size in 10^-qty_scale units. Must be positive.
	 * @param reference_scaled Reference price in 10^-price_scale units — the
	 *        previous close or a session anchor, which the collar is measured
	 *        around. Must be positive and on the tick grid.
	 * @param collar_bps Half-width of the price band in basis points, or
	 *        @c NO_COLLAR. A band of 2000 admits [0.8x, 1.2x] of the reference.
	 *
	 * @warning Preconditions are assertions, not rejections. Reference data
	 *          arrives from an operator or a static-data service, not from a
	 *          client, so a malformed listing is a deployment bug that should
	 *          fail loudly at startup rather than reject orders all session.
	 */
	TRADING_ENGINE_EXPORT symbol_spec(symbol_id_t id, std::string_view symbol,
									 int price_scale, int qty_scale,
									 std::int64_t tick_scaled,
									 std::int64_t lot_scaled,
									 std::int64_t reference_scaled,
									 std::int64_t collar_bps = NO_COLLAR);

	[[nodiscard]] symbol_id_t id() const noexcept { return id_; }
	[[nodiscard]] std::string_view symbol() const noexcept { return symbol_; }
	[[nodiscard]] int price_scale() const noexcept { return price_scale_; }
	[[nodiscard]] int qty_scale() const noexcept { return qty_scale_; }
	[[nodiscard]] std::int64_t tick_scaled() const noexcept { return tick_scaled_; }
	[[nodiscard]] std::int64_t lot_scaled() const noexcept { return lot_scaled_; }

	// --- decimal in -------------------------------------------------------

	/**
	 * @brief Convert a scaled price to ticks, refusing anything off the grid.
	 * @param scaled Price in 10^-price_scale units.
	 * @return The tick count, or @c PRICE_NOT_ON_TICK when @p scaled is not an
	 *         exact multiple of the tick, or @c MALFORMED_DECIMAL when it is
	 *         not positive.
	 */
	[[nodiscard]] TRADING_ENGINE_EXPORT std::expected<price_t, reject_reason>
	price_from_scaled(std::int64_t scaled) const noexcept;

	/// @brief The same for quantities, in lots. @c QUANTITY_NOT_ON_LOT off grid.
	[[nodiscard]] TRADING_ENGINE_EXPORT std::expected<quantity_t, reject_reason>
	quantity_from_scaled(std::int64_t scaled) const noexcept;

	/**
	 * @brief Parse decimal text and convert it to ticks in one step.
	 *
	 * The client-facing entry point: @c "153.45" becomes a tick count or a
	 * reason. Rejects text carrying more fractional digits than
	 * @c price_scale rather than truncating it — see @c parse_exact_decimal,
	 * which is where this differs from the market-data parser.
	 */
	[[nodiscard]] TRADING_ENGINE_EXPORT std::expected<price_t, reject_reason>
	price_from_text(std::string_view text) const noexcept;

	/// @brief The same for quantity text, in lots.
	[[nodiscard]] TRADING_ENGINE_EXPORT std::expected<quantity_t, reject_reason>
	quantity_from_text(std::string_view text) const noexcept;

	// --- decimal out ------------------------------------------------------

	/// @brief Ticks back to a scaled integer, for a report or a market-data
	///        frame. Exact by construction — every tick count has a decimal.
	[[nodiscard]] std::int64_t price_to_scaled(price_t ticks) const noexcept {
		return static_cast<std::int64_t>(ticks) * tick_scaled_;
	}

	/// @brief Lots back to a scaled integer. @see price_to_scaled
	[[nodiscard]] std::int64_t quantity_to_scaled(quantity_t lots) const noexcept {
		return lots * lot_scaled_;
	}

	// --- the price collar, and the array it sizes -------------------------

	/// @brief Whether this listing has a price band at all.
	[[nodiscard]] bool has_collar() const noexcept { return collar_bps_ != NO_COLLAR; }

	/// @brief Lowest admissible price, in ticks. @c 1 when uncollared (a price
	///        of 0 ticks is never a valid order).
	[[nodiscard]] price_t collar_low() const noexcept { return collar_low_; }

	/// @brief Highest admissible price, in ticks.
	[[nodiscard]] price_t collar_high() const noexcept { return collar_high_; }

	/// @brief Is @p ticks inside the band?
	[[nodiscard]] bool within_collar(price_t ticks) const noexcept {
		return ticks >= collar_low_ && ticks <= collar_high_;
	}

	/**
	 * @brief How many tick slots the collar spans — the length a price-indexed
	 *        book side would need.
	 *
	 * This is the number that decides whether a flat array book is viable for a
	 * listing, and it is why the collar belongs in the spec rather than in a
	 * risk check bolted on later. A US equity at $50 with a $0.01 tick and a
	 * ±20% band spans 2,000 slots; the same band on a $60,000 instrument with
	 * the same tick spans 2.4 million, which at 40 bytes per level is ~96 MB
	 * per side. One of those is an array and the other is not, and the spec is
	 * what lets you find out before writing the allocator.
	 *
	 * @return Slot count, or 0 when the listing is uncollared and therefore has
	 *         no bounded price domain to index.
	 */
	[[nodiscard]] TRADING_ENGINE_EXPORT std::size_t collar_span() const noexcept;

	/// @brief Byte cost of a price-indexed side holding @p level_bytes per
	///        slot — @c collar_span() multiplied out, for the decision above.
	[[nodiscard]] std::size_t
	indexed_side_bytes(std::size_t level_bytes) const noexcept {
		return collar_span() * level_bytes;
	}

	/**
	 * @brief Array index for @p ticks in a side spanning the collar.
	 * @pre @c within_collar(ticks). Callers reach here after validation, which
	 *      is where an out-of-band price is turned into a rejection.
	 */
	[[nodiscard]] std::size_t tick_index(price_t ticks) const noexcept {
		return static_cast<std::size_t>(ticks - collar_low_);
	}

	/// @brief The inverse of @c tick_index.
	[[nodiscard]] price_t price_at_index(std::size_t index) const noexcept {
		return collar_low_ + static_cast<price_t>(index);
	}

private:
	symbol_id_t id_;
	std::string symbol_;
	int price_scale_;
	int qty_scale_;
	std::int64_t tick_scaled_;
	std::int64_t lot_scaled_;
	std::int64_t collar_bps_;
	price_t collar_low_;
	price_t collar_high_;
};

/**
 * @brief Parse unsigned decimal text into an integer scaled by 10^@p scale,
 *        refusing any input the scale cannot represent exactly.
 *
 * @par Why this is not market_data::parser::parse_fixed_point
 * That one is the per-level hot path of feed decoding: it is SWAR-vectorised,
 * and it **truncates** fractional digits beyond @p scale, because a venue's own
 * frames are already on the venue's grid and the fast path should not pay to
 * re-check that. This one runs once per inbound client order, is scalar, and
 * treats an extra digit as a rejection — a client that sends @c "153.456" to a
 * two-decimal listing must be told, not quietly filled at @c "153.45".
 *
 * Same operation, opposite contracts. Sharing one implementation would mean a
 * mode flag inside the market-data loop, which is the wrong place to put a
 * branch that only the order path needs.
 *
 * @param text Decimal digits with at most one @c '.', optionally @c '+'-signed.
 *             A leading @c '-' is refused: prices and quantities are positive.
 * @param scale Fractional digits to scale by. Must be in [0, 18].
 * @return The scaled integer, or @c MALFORMED_DECIMAL.
 */
[[nodiscard]] TRADING_ENGINE_EXPORT std::expected<std::int64_t, reject_reason>
parse_exact_decimal(std::string_view text, int scale) noexcept;

} // namespace exchange::engine
