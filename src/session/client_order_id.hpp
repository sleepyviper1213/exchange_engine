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

#include "orders/types.hpp"
#include "session_export.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>


namespace exchange::session {

/**
 * @brief What every client order id this process writes begins with.
 *
 * @note Binance accepts @c ^[\.A-Z\:/a-z0-9_-]{1,36}$ for a client order id, so
 *       a hyphen is legal and a colon would be too. Kept short because the 36
 *       characters are shared with the id itself - a @c uint64_t is up to 20
 *       digits, which leaves plenty but not unlimited room.
 */
constexpr std::string_view CLIENT_ORDER_PREFIX = "ex-";

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
[[nodiscard]] SESSION_EXPORT std::string client_order_id(order_id_t id);

/**
 * @brief The engine order id inside @p text, if this process wrote it.
 *
 * @return The id, or nothing when @p text does not carry our prefix or does
 * not carry a number after it - which is what an order placed outside this
 *         process looks like, and is a legitimate finding rather than an
 * error.
 *
 * @note Rejects a leading @c + or @c - and any trailing character, so
 *       @c "ex-12x" and @c "ex--1" are not ours rather than being read as 12
 *       and 1. A partial match here would attach a venue order to an unrelated
 *       engine order.
 */
[[nodiscard]] SESSION_EXPORT std::optional<order_id_t>
engine_order_id(std::string_view text) noexcept;

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
