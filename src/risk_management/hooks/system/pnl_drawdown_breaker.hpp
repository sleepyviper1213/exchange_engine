#pragma once
// PnL Drawdown Breaker: the loss floor, and the one rule that stops trading
// rather than refusing an order.
//
// Every pre-trade rule is a decision about the order in front of it. This is
// not. A losing position is not that order's fault, so refusing it while
// accepting the next identical one would be incoherent - the floor trips the
// breaker instead, and a human decides whether to re-arm.

// Exported for the same reason duplicate.hpp is: the caller is the gate, and a
// template is instantiated in its consumer's translation unit.
#include "core/util/function_ref.hpp"
#include "risk_management/hooks/system/circuit_breaker.hpp"
#include "risk_management/limits.hpp"
#include "risk_management_export.hpp"

#include <cstdint>

namespace exchange::risk::hooks::system {

/**
 * @brief Whether @p pnl has fallen through @p limits' floor.
 *
 * @param pnl Realised plus unrealised, in tick-lots. Negative is a loss.
 * @return @c false when no floor is configured, and when the loss is exactly
 * the floor - the floor is the last admissible value, not the first refused
 *         one.
 */
[[nodiscard]] RISK_MANAGEMENT_EXPORT bool
through_floor(std::int64_t pnl, const risk_limits &limits) noexcept;

/**
 * @brief Trip @p breaker to @c CANCEL_ONLY if the account is through the floor.
 *
 * @param breaker The shared kill switch.
 * @param limits The policy in force.
 * @param pnl_now A callable returning the current profit in tick-lots. Must not
 *        throw.
 * @return Whether *this call* is what tripped the breaker.
 *
 * @par Why the profit arrives as a callable
 * Because reading it is the expensive part. Profit lives in @c position_book,
 * which is shared across threads, so valuing it means touching a line other
 * cores write. Two conditions can rule the whole check out without that read -
 * no floor configured, and a breaker that is already open - and the natural
 * spelling (compute the profit, then decide) would pay for the read on every
 * print of every session that never configures a floor. Deferring it keeps this
 * hook free for the deployments that do not use it.
 *
 * @par Where it is called from, and why not from the per-command screen
 * From the trade feedback, on every print - ours and everyone else's. That is
 * what catches both halves of a drawdown: a fill that realises a loss, and a
 * market that moves against a position simply being held. Checking it per
 * command would re-evaluate a number that cannot have changed, on the one path
 * that cannot afford it. @see risk_gate::on_trade
 *
 * @note Does not re-trip a breaker that is already open, so a strategy bleeding
 *       through the floor produces one trip and one cause rather than one per
 *       print. Recovering does not re-arm it either: coming back above the
 * floor means the position moved, not that anybody decided to keep trading.
 */
RISK_MANAGEMENT_EXPORT bool trip_on_drawdown(
	circuit_breaker &breaker, const risk_limits &limits,
	core::util::function_ref<int64_t() const noexcept> pnl_now) noexcept;

} // namespace exchange::risk::hooks::system
