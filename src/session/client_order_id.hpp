#pragma once
// The engine's order id, spelled the way a venue will hand it back.
//
// Every execution report the venue sends names the order by the
// `newClientOrderId` we chose when we placed it. That string is therefore the
// only key we get, and how it is built decides whether reconciliation is a
// lookup or a guess.
//
// --- why an encoding and not a map ------------------------------------------
//
// A side table from client id to `order_id_t` works right up to the two moments
// it is needed most. A fill can arrive before the placement's HTTP response
// does, so the entry may not be written yet; and a process that restarts has
// lost the table entirely while the venue still holds the orders. Deriving the
// id from the string instead means any process holding this header can read any
// report, including for orders it did not place and does not remember.
//
// --- why a prefix ----------------------------------------------------------
//
// An account is not ours alone. `GET /api/v3/openOrders` returns *everything*
// working on it, including orders somebody placed by hand in the venue's web UI
// while this process was running. Reconciliation has to tell those apart, and
// the only thing distinguishing them is the shape of the id: ours parse, theirs
// do not. Cancelling an order a human placed because it was not in our book
// would be the worst possible reading of "the venue and we disagree".
//
// --- why one engine order can carry several ids -----------------------------
//
// A venue that cannot reprice in place - Binance spot cannot - amends by
// cancel-and-replace, and the replacement is a new venue order that needs an
// id of its own: the venue refuses one still attached to a working order. Each
// such order is a *leg* of one engine order, spelled `ex-<id>_<leg>`, and leg
// zero keeps the plain `ex-<id>` so a run that never amends writes exactly the
// ids it always did. Every leg decodes to the same engine id, which is what
// lets a fill on any of them reach the order the engine knows.
// @see session::venue_legs

#include "orders/types.hpp"
#include "session_export.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>


namespace exchange::session {

/// @brief Which venue order of an engine order's cancel-replace chain this is.
///        Zero is the original placement. @see the file header
using venue_leg_t = std::uint32_t;

/**
 * @brief What every client order id this process writes begins with.
 *
 * @note Binance enforces @c ^[a-zA-Z0-9-_]{1,36}$ - measured against testnet,
 *       which refuses a @c . with @c -1100 although older documentation lists
 *       it. Kept short because the 36 characters are shared with the id, the
 *       leg and the cancel suffix: 3 + 20 + 1 + 10 + 2 is exactly 36.
 */
constexpr std::string_view CLIENT_ORDER_PREFIX = "ex-";

/// @brief Separates the engine id from a non-zero leg: @c ex-42_3.
constexpr char LEG_SEPARATOR = '_';

/**
 * @brief What a *cancel request*'s client order id ends with.
 *
 * The venue gives a cancel its own identity and reports it in @c c, with the
 * cancelled order's id in @c C. The suffix keeps the two distinguishable while
 * leaving both parseable - a cancel we sent is still recognisably ours, which
 * reconciliation depends on.
 */
constexpr std::string_view CANCEL_SUFFIX = "-c";

/// @brief Longest client order id a venue will accept. @see CLIENT_ORDER_PREFIX
constexpr std::size_t CLIENT_ORDER_ID_MAX = 36;

/**
 * @brief @p id as the client order id to send with a placement.
 *
 * @note Decimal rather than hex or base-36: the id appears in the venue's own
 *       UI and in its trade history exports, and an operator comparing it
 *       against a log line should not have to convert a base.
 */
[[nodiscard]] SESSION_EXPORT std::string client_order_id(order_id_t id,
														 venue_leg_t leg = 0);

/**
 * @brief The engine order id inside @p text, if this process wrote it.
 *
 * @return The id, or nothing when @p text does not carry our prefix or does
 * not carry a number after it - which is what an order placed outside this
 *         process looks like, and is a legitimate finding rather than an
 * error. Every leg of an order decodes to the same id.
 *
 * @note Rejects a leading @c + or @c - and any trailing character, so
 *       @c "ex-12x" and @c "ex--1" are not ours rather than being read as 12
 *       and 1. A partial match here would attach a venue order to an unrelated
 *       engine order.
 */
[[nodiscard]] SESSION_EXPORT std::optional<order_id_t>
engine_order_id(std::string_view text) noexcept;

/// @brief The leg @p text names, if this process wrote it. Zero for an id
///        with no leg suffix. @see engine_order_id for what is refused.
[[nodiscard]] SESSION_EXPORT std::optional<venue_leg_t>
venue_leg(std::string_view text) noexcept;

/// @brief Whether @p text names an order this process placed.
[[nodiscard]] SESSION_EXPORT bool is_ours(std::string_view text) noexcept;

/**
 * @brief The client order id to send with a *cancel* of @p id.
 *
 * The venue gives the cancel request its own identity and reports it in @c c,
 * with the cancelled order's id in @c C. Making the two distinguishable means
 * a cancel report cannot be mistaken for a report about a fresh order - and
 * the suffix keeps it parseable, so a cancel we sent is still recognisably
 * ours.
 */
[[nodiscard]] SESSION_EXPORT std::string cancel_order_id(order_id_t id);

/// @brief Whether @p text is one of our *cancel* requests rather than an
/// order.
[[nodiscard]] SESSION_EXPORT bool is_cancel_id(std::string_view text) noexcept;

} // namespace exchange::session
