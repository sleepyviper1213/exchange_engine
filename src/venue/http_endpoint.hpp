#pragma once
// Where a venue's *request* path is reachable, as a value - and nothing about
// what is asked for there.
//
// This is here rather than in `market_data/binance/` for the same reason
// `stream_endpoint` is: both directions of traffic resolve to an endpoint
// before transport sees one. @see stream_endpoint.hpp, docs/directory_layout.md

#include "venue_export.hpp" // VENUE_EXPORT (generated)

#include <string>

namespace exchange::venue {

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
