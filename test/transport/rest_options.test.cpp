#include "transport/rest/options.hpp"

#include <gtest/gtest.h>

#include <chrono>

// How a request is carried, as opposed to what it says. One assertion here
// matters more than the rest of the file: which way the certificate policy
// defaults.

using exchange::transport::rest::request_options;
// transport::, not transport::rest:: - the WebSocket reader needs the same
// policy, so the enum sits at module scope and `rest` names it by unqualified
// lookup into its own enclosing namespace. @see transport/tls_verify.hpp
using exchange::transport::tls_verify;

TEST(RestOptions, VerificationIsOnUnlessAskedOtherwise) {
	// The asymmetry rest/options.hpp argues for: the general sender is secure
	// by default and https_get opts out by name. If this default ever flips, an
	// API key goes out over a connection an intercepting proxy can terminate -
	// and nothing else in the tree would notice.
	EXPECT_EQ(request_options{}.verify, tls_verify::peer);
}

TEST(RestOptions, TheDeadlineIsPerOperationAndNotPerConversation) {
	// Ten seconds each for connect, handshake, write and read - so a slow but
	// live server is tolerated, and a dead one is given up on four times over
	// rather than once. Pinned because the number is load-bearing for the
	// pipeline, which applies it per batch member.
	EXPECT_EQ(request_options{}.timeout, std::chrono::seconds{10});
}
