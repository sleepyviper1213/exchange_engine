#pragma once
// The policy half of the gate: numbers an operator sets, never state the gate
// mutates. Separating the two is what makes the hot path's inputs obviously
// read-only - a limits object is copied into the gate at construction and every
// check reads it out of the same cache line.

#include "risk_management_export.hpp" // RISK_MANAGEMENT_EXPORT (generated)
#include "fwd.hpp"
#include "orders/types.hpp"

#include <cstdint>
#include <limits>

namespace exchange::risk {
/**
 * @brief One account's trading limits for one listing.
 *
 * @par Units, and why there is no currency here
 * Everything is on the listing's own integer grid: quantities in lots, prices
 * in ticks, and notionals in *tick-lots* - the plain product @c price * @c qty.
 * That product is exact, monotonic in both factors, and needs no scale factor,
 * which is what lets the notional checks be a multiply and a compare with no
 * division and no rounding anywhere.
 *
 * It is also why a limit here governs one listing and not a portfolio: summing
 * tick-lots across two listings is meaningless without their tick sizes and a
 * common currency, and doing that conversion is a decision about *reference
 * data*, not about risk. @c symbol_spec::price_to_scaled is the honest crossing
 * when a firm-wide aggregate is wanted, and it belongs in whatever aggregates -
 * not on a path that runs per command. @see position_book
 *
 * @par Defaults are permissive on purpose
 * A default-constructed @c risk_limits refuses nothing but the malformed. A
 * gate is a boundary, and a boundary whose failure mode is "silently stopped
 * trading" is worse than one whose failure mode is "behaved as if it were not
 * there" - the second is visible in the P&L the moment anything is wrong with
 * the configuration, the first looks like a quiet market.
 */
struct risk_limits {
	/// @brief Largest quantity one order may carry, in lots. The fat-finger
	///        guard on size - the digit somebody added by accident.
	quantity_t max_order_qty = std::numeric_limits<quantity_t>::max();

	/// @brief Largest @c price * @c qty one order may carry, in tick-lots.
	///
	/// Not implied by @c max_order_qty: a size that is ordinary on a penny
	/// instrument is a fortune on an expensive one, and an account that trades
	/// both wants one number that means the same thing on each.
	std::int64_t max_order_notional = std::numeric_limits<std::int64_t>::max();

	/// @brief Largest absolute net position, in lots. Signed exposure - a long
	///        and a short of the same size net to nothing here.
	volume_t max_position_lots = std::numeric_limits<volume_t>::max();

	/**
	 * @brief Largest gross exposure, in tick-lots.
	 *
	 * Gross, not net, and it counts *working* orders as well as filled
	 * position. An account holding no position but showing a thousand orders on
	 * each side is one adverse print away from holding all of it, and a limit
	 * that only looked at fills would let it get there. @see position_book
	 */
	std::int64_t max_exposure_notional =
		std::numeric_limits<std::int64_t>::max();

	/// @brief Most orders that may be working at once. Also bounds the gate's
	///        ledger, which is sized from it and never grows.
	std::uint32_t max_working_orders = 4096;

	/**
	 * @brief Half-width of the fat-finger price band, in basis points around
	 *        the last print. Zero disables the band.
	 *
	 * @par Not @c symbol_spec's collar, and not a substitute for it
	 * The collar is the *venue's* rule, measured around a session anchor that
	 * does not move, and it is what sizes a price-indexed book. This is *our*
	 * rule, measured around the last trade, and it moves all day. A venue
	 * collar of ±20% still admits an order at twice the current market in a
	 * quiet name; a 50 bp band does not, and that is the order nobody meant to
	 * send. Both apply - this one first, because it is the tighter of the two.
	 */
	std::int64_t price_band_bps = 0;

	/// @brief Messages the account may submit per rate window. @see
	/// rate_limiter
	std::uint32_t max_messages_per_window =
		std::numeric_limits<std::uint32_t>::max();

	/// @brief Base-2 log of the rate window in nanoseconds. The default is
	/// about
	///        1.05 ms; @c rate_limiter explains why short is right.
	unsigned rate_window_log2_ns = 20;

	/**
	 * @brief Loss that trips the circuit breaker, in tick-lots. Zero disables.
	 *
	 * Stated as a positive magnitude: @c 50'000 means "stop when realised plus
	 * unrealised profit falls below @c -50'000". @see position_snapshot::pnl
	 *
	 * @par Why this is not a per-order check like everything else here
	 * Every other limit refuses one command; this one stops trading, and that
	 * difference is the whole distinction between a limit and a circuit
	 * breaker. A losing position is not the fault of the order in front of you
	 * - refusing that order while accepting the next identical one would be
	 * incoherent - so the floor trips the breaker instead, and a human has to
	 * undo it.
	 *
	 * It is also why it costs nothing per command: profit only moves when
	 * something prints, so it is evaluated on the fill path and never on the
	 * submit path. @see risk_gate::on_trade
	 */
	std::int64_t max_loss = 0;

	/// @brief The @c max_loss meaning "no floor".
	static constexpr std::int64_t NO_LOSS_LIMIT = 0;

	/// @brief Whether a loss floor is configured at all.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT bool has_loss_limit() const noexcept;

	/// @brief Basis points denominator, matching @c
	/// symbol_spec::BPS_DENOMINATOR.
	static constexpr std::int64_t BPS_DENOMINATOR = 10000;

	/// @brief Whether a fat-finger band is configured at all.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT bool has_price_band() const noexcept;
};

} // namespace exchange::risk
