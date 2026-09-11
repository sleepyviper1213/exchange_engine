#pragma once
// Whether a TLS connection checks the peer it reached.
//
// Lives at transport scope rather than inside `rest/` because all three of this
// module's TLS users need it and none of them owns it: the two REST senders
// (`rest::https_request`, `rest::request_pipeline`) and the WebSocket reader
// (`ws::stream_reader`). `rest/` nests inside this namespace, so its own code
// still names the type unqualified and needed no change when it moved.

#include "fwd.hpp"

namespace exchange::transport {

/**
 * @brief Whether the server's certificate is checked against the trust store.
 *
 * @par Why this is a choice and not a constant
 * Public market data is read over TLS for integrity, not secrecy, and this tree
 * has always fetched it with verification off so that a machine without a CA
 * bundle installed can still run - @c rest::https_get keeps that behaviour. A
 * request carrying a credential cannot: an unverified connection is one an
 * intercepting proxy can terminate, and the API key is in a header on the very
 * first flight. So @c rest::request_options defaults the other way, and the
 * insecure setting has to be asked for by name.
 *
 * @par And why the feed defaults the same way
 * A depth stream carries no credential, so the reasoning above would let it
 * default to @c none - and that is the reasoning it was read with for as long
 * as @c ws::stream_reader hardcoded exactly that. It is wrong here. The feed is
 * what a strategy decides from: an intercepted stream does not steal anything,
 * it *dictates* the book this process believes in, and every quote and every
 * order that follows is derived from it. Integrity is the whole point of
 * reading market data over TLS, and verification is what makes integrity mean
 * "from the venue" rather than merely "from whoever terminated the connection".
 * So @c ws::stream_reader defaults to @c peer too, and a machine with no CA
 * bundle asks for @c none by name the way every other insecure read here does.
 *
 * @note @c peer is only half of the check. The certificate must also be matched
 *       against the host asked for, which is a separate
 *       @c ssl::host_name_verification callback plus the SNI name - a chain
 *       that verifies but belongs to somebody else is not a connection to the
 *       venue. Every user here sets all three together.
 */
enum class tls_verify : std::uint8_t {
	/// @brief Accept any certificate. Public reads only, never a credential,
	///        and never a feed whose contents are traded on.
	none,
	/// @brief Verify against the default trust store, and fail the handshake
	///        if the chain does not check out.
	peer,
};

} // namespace exchange::transport
