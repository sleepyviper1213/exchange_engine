#pragma once
// How a request is carried, as opposed to what it says. Shared by both senders,
// which is why it is neither's header.

#include "transport/tls_verify.hpp" // IWYU pragma: export - `verify`'s type

#include <chrono>

namespace exchange::transport::rest {

/// @brief How a request is carried, as opposed to what it says.
struct request_options {
	/// @brief Certificate policy. Secure by default; @see tls_verify.
	tls_verify verify = tls_verify::peer;

	/// @brief Per-operation deadline on the underlying socket - applied to the
	///        connect, the handshake, the write and the read separately, not to
	///        the conversation as a whole.
	std::chrono::seconds timeout{10};
};

} // namespace exchange::transport::rest
