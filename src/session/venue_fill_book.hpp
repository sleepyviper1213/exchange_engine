#pragma once
// What the venue says traded, turned into what the gate books - once per
// execution.
//
// Split out of live_session, which used to hold all of it inline beside the
// depth handler, the mass-cancel policy and the reaction metrics. What is here
// has one subject: an execution the venue reports, from the account stream or
// recovered from the trade history. It is the dedup ring that makes a fill
// delivered twice one fill, the map from the venue's order id back to ours that
// lets a recovered trade be attributed, and the arithmetic that values a fill
// and its fee on the listing's grid. None of it touches a gate, a quoter or a
// router - the session decides what to do with the answer and counts the
// refusals in its own report.

#include "core/util/enum_format.hpp"
#include "orders/types.hpp"
#include "session_export.hpp"
#include "symbol/symbol_spec.hpp"
#include "venue/execution_report.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <utility>

namespace exchange::session {

using core::util::format_as;

/// @brief Why a venue execution was not booked. Each maps to its own counter
///        in @c live_session_report. Prints, and reads through
///        @c magic_enum::enum_name, as the reason in words.
enum class unbooked_fill : std::uint8_t { NO_QUANTITY, OFF_GRID, SIDE_UNKNOWN };

/// @brief A venue execution valued on the listing's grid, ready for the gate.
struct valued_fill {
	side_t side;           ///< our side
	price_t price;         ///< on the tick grid
	quantity_t lots;       ///< strictly positive
	timestamp_t at;        ///< the venue's transaction time, or zero
	side_t aggressor_side; ///< ours if we took, the other side if we rested
};

/// @brief A commission valued in the listing's money unit.
struct valued_fee {
	notional_t charge;          ///< rounded *up* to the next tick-lot
	std::int64_t base_scaled{}; ///< the fee when it was taken in the base asset
};

/**
 * @brief The venue's executions, booked once each and attributed to our orders.
 *
 * @par Two rings, and why neither is a set
 * A duplicate fill arrives close behind its original, and an order old enough
 * to have left the id map is long finished - so both are bounded rings that
 * never allocate, rather than sets that grow for the length of a run.
 */
class venue_fill_book {
public:
	SESSION_EXPORT venue_fill_book() noexcept;

	/// @brief Note that venue order @p venue_id is our order @p id.
	SESSION_EXPORT void remember(std::int64_t venue_id, order_id_t id) noexcept;

	/// @brief Our order behind venue order @p venue_id, or 0 if not one we
	///        heard of.
	[[nodiscard]] SESSION_EXPORT order_id_t
	engine_id_of(std::int64_t venue_id) const noexcept;

	/// @brief Whether @p trade_id was already booked; records it if not. A
	///        negative id - a report that names no trade - is never a repeat.
	[[nodiscard]] SESSION_EXPORT bool
	is_repeated_trade(std::int64_t trade_id) noexcept;

	/// @brief The highest venue trade id booked, or -1 before the first. The
	///        venue numbers a listing's trades in order, so anything above it
	///        is a fill the account stream never delivered.
	[[nodiscard]] SESSION_EXPORT std::int64_t newest_trade_id() const noexcept;

	/**
	 * @brief Value @p report's execution on @p spec's grid.
	 *
	 * @param side_hint Our side when the report does not say - the quoter's
	 *        view. Only a fallback: it fails for an order the quoter already
	 * let go, and used to book a filled bid as a sale that way. A one-hour Demo
	 * Mode run drifted 46 lots.
	 *
	 * @par Why the aggressor side is derived rather than read
	 * @c is_maker says whether our order was the resting one; the trade the
	 * gate reads needs the direction of the print. It changes nothing the gate
	 * books
	 * - both slots are looked up the same way - but a log line and a post-trade
	 * rule should not be told we aggressed when we did not.
	 */
	[[nodiscard]] SESSION_EXPORT static std::expected<valued_fill,
													  unbooked_fill>
	value_fill(const venue::execution_report &report,
			   const engine::symbol_spec &spec,
			   std::optional<side_t> side_hint);

	/**
	 * @brief Value @p report's commission, or nothing if it cannot be valued.
	 *
	 * Rounded *up* to the next tick-lot, the opposite of how a stated loss
	 * limit is converted: both roundings make the loss floor trip sooner rather
	 * than later. Exact integer arithmetic at the listing's own scales.
	 *
	 * @return Zero charge for a report with no commission; nothing for a fee in
	 *         an asset that is neither @p quote_asset nor @p base_asset, or on
	 * a listing with no USDT grid.
	 */
	[[nodiscard]] SESSION_EXPORT static std::optional<valued_fee>
	value_fee(const venue::execution_report &report,
			  const engine::symbol_spec &spec, std::string_view quote_asset,
			  std::string_view base_asset);

private:
	static constexpr std::size_t RECENT_TRADES = 1024;
	static constexpr std::size_t VENUE_ORDERS  = 4096;

	std::array<std::int64_t, RECENT_TRADES> recent_trades_;
	std::size_t next_trade_slot_  = 0;
	std::int64_t newest_trade_id_ = -1;

	std::array<std::pair<std::int64_t, order_id_t>, VENUE_ORDERS>
		venue_orders_{};
	std::size_t next_venue_order_ = 0;
};

} // namespace exchange::session

template <>
constexpr magic_enum::customize::customize_t magic_enum::customize::enum_name(
	exchange::session::unbooked_fill value) noexcept {
	switch (value) {
		using enum exchange::session::unbooked_fill;
	case NO_QUANTITY: return "the report carries no tradeable quantity";
	case OFF_GRID: return "the price is off the listing's tick grid";
	case SIDE_UNKNOWN:
		return "neither the venue nor the quoter says which side";
	}
	return default_tag;
}
