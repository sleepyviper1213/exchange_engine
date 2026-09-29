#pragma once
// Where a venue's *stream* is reachable, as a value - and nothing about what it
// says there.
//
// This is here rather than in `market_data/binance/` because both directions of
// traffic resolve to an endpoint before transport sees one. Depth streams and
// order placements differ in everything except this: a host, a port and a
// target, handed to something else to move bytes over. @see http_endpoint.hpp
//
// `transport/` deliberately does not know this type either - it takes host and
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

} // namespace exchange::venue
