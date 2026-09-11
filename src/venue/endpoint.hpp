#pragma once
// Where a venue is reachable, as a value - and nothing about what it says
// there.
//
// These are the two shapes any venue conversation resolves to before transport
// sees it, and they are here rather than in `market_data/binance/` because both
// directions of traffic produce them. Depth streams and order placements differ
// in everything except this: a host, a port and a target, handed to something
// else to move bytes over.
//
// `transport/` deliberately does not know them either - it takes host and
// target as arguments. That keeps this module free of a transport edge, which
// is what lets `market_data/` depend on it without acquiring one. @see
// docs/directory_layout.md

#include "venue_export.hpp" // VENUE_EXPORT (generated)

#include <string>

namespace exchange::venue {

/// @brief A resolved WebSocket endpoint: everything transport needs to open a
///        connection, and nothing about what the frames mean.
struct stream_endpoint {
	std::string host{};   ///< TLS host, also the SNI
	std::string port{};   ///< TLS port
	std::string target{}; ///< stream path, e.g. @c /ws/solusdt@depth@100ms

	bool operator==(const stream_endpoint &) const noexcept = default;
};

/// @brief A resolved HTTPS endpoint - the request path, not the request.
///
/// @note Carries no verb, headers or body. Those live in
///       @c transport::rest::request, which is built from this by whoever knows
///       what it is asking for. The split is what keeps an endpoint builder a
///       pure function of its arguments.
struct http_endpoint {
	std::string host{};   ///< TLS host, also the SNI + Host header
	std::string target{}; ///< request path with query

	bool operator==(const http_endpoint &) const noexcept = default;
};

} // namespace exchange::venue
