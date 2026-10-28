#pragma once
// Which deployment of a venue a run is talking to.
//
// One switch, module-wide, and that is the point of it being here rather than a
// flag on each endpoint builder. A run that reads production depth and places
// orders on testnet is not a safer version of a live run - it is a run whose
// fills are meaningless, because testnet has its own book and its own
// liquidity. The two directions have to agree, so they read one value.

#include "core/util/enum_format.hpp"

#include <cstdint>

namespace exchange::venue {

using core::util::format_as;

/**
 * @brief Which deployment of the venue a run is talking to.
 *
 * @par Two ways to not be production, and they are not interchangeable
 * @c testnet is a separate order_book: its own book, its own participants, its
 * own thin liquidity. A strategy tested there is tested against a market that
 * exists nowhere else - fine for proving a request is well formed, useless for
 * proving a quote is sensibly priced. @c demo runs against depth that tracks
 * the live order_book, with balances that do not, which is what makes it the
 * one to measure a strategy in. Binance is explicit that "realistic market data
 * is not equal to real market data", so it is still not the live venue; it is
 * far closer to it.
 *
 * @note Neither is @c production, and nothing here enforces which is the
 *       default - the CLI does. It is the reason this is a type rather than a
 *       @c bool named @c live: "not production" is not one state.
 *
 * Prints as its enumerator name, e.g. @c "testnet".
 */
enum class environment : std::uint8_t {
	production, ///< the real venue - real orders, real money
	testnet,    ///< the venue's sandbox - its own book, its own liquidity
	demo,       ///< the venue's demo mode - fake money against realistic depth
};

/// @brief Whether @p env is the one where a mistake costs money.
/// @note Only @c production. Both of the others take orders and report
///       fills against balances nobody can withdraw.
[[nodiscard]] constexpr bool is_production(environment env) noexcept {
	return env == environment::production;
}

} // namespace exchange::venue
