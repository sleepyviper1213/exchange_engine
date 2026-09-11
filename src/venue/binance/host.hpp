#pragma once
// Where Binance lives, per environment.
//
// Four names, in one place, because they have to move together: a run reading
// production depth while placing orders on testnet has a strategy reacting to a
// book it is not trading in. Every endpoint builder in the tree - market data's
// and the order gateway's - takes its host from here rather than spelling one.
//
// @see https://developers.binance.com/docs/binance-spot-api-docs
// @see https://testnet.binance.vision

#include "venue/environment.hpp"

#include <string_view>

namespace exchange::venue::binance {

/// @brief The hosts one Binance deployment answers on.
struct hosts {
	/// @brief REST host - snapshots, reference data, order entry.
	std::string_view rest;

	/// @brief Market-data WebSocket host.
	std::string_view stream;

	/// @brief Port for @c stream. Binance publishes market data on 9443 rather
	///        than 443 in both deployments.
	std::string_view stream_port;

	/**
	 * @brief WebSocket API host - the request/response socket.
	 *
	 * A third host and a third protocol, and not a variant of either above.
	 * @c stream pushes public market data and never reads; this one takes JSON
	 * requests and answers them, and is where the account's own event stream
	 * now lives. @see ws_api_endpoint
	 */
	std::string_view ws_api;
};

/**
 * @brief The hosts for @p env.
 *
 * @note The testnet stream host is @c stream.testnet.binance.vision, which is
 *       not the REST host with a prefix - it is a different name, and guessing
 *       it from the REST one is a mistake this function exists to stop anyone
 *       making.
 * @warning Testnet keeps its own order book, its own liquidity and its own
 *          balances. Depth there is thin and its prices do not track
 *          production's - a property of the venue, not a defect here, and the
 *          reason @c demo exists: its depth tracks the live exchange while its
 *          balances do not.
 */
// Header-only and constexpr, so no VENUE_EXPORT: an inline function must not
// be declared dllimport, and a table of four names is worth resolving at
// compile time rather than through a DLL call.
[[nodiscard]] constexpr hosts host_for(environment env) noexcept {
	switch (env) {
	case environment::production:
		return hosts{.rest        = "api.binance.com",
					 .stream      = "stream.binance.com",
					 .stream_port = "9443",
					 .ws_api      = "ws-api.binance.com"};
	case environment::testnet:
		return hosts{.rest        = "testnet.binance.vision",
					 .stream      = "stream.testnet.binance.vision",
					 .stream_port = "9443",
					 .ws_api      = "ws-api.testnet.binance.vision"};
	case environment::demo:
		// A third set of names again, not production's with a prefix: the REST
		// host is `demo-api`, the stream host `demo-stream`, and there is a
		// fourth for the WebSocket order API this tree does not use.
		// @see
		// https://developers.binance.com/docs/binance-spot-api-docs/demo-mode
		return hosts{.rest        = "demo-api.binance.com",
					 .stream      = "demo-stream.binance.com",
					 .stream_port = "9443",
					 .ws_api      = "demo-ws-api.binance.com"};
	}
	// Unreachable for any valid enumerator. Production is *not* the fallback:
	// an unrecognised environment must not resolve to the one where an order
	// costs money.
	return hosts{.rest        = "testnet.binance.vision",
				 .stream      = "stream.testnet.binance.vision",
				 .stream_port = "9443",
				 .ws_api      = "ws-api.testnet.binance.vision"};
}

} // namespace exchange::venue::binance
