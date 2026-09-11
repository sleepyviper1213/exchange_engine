#include "session/live_feed.hpp"

#include <gtest/gtest.h>

// One thing, and it is a default rather than a behaviour: whether the live
// depth feed verifies the certificate it is handed.
//
// It is pinned for the same reason RestOptions pins the credentialed sender's -
// a security default that flips is silent, and nothing else in the tree would
// notice. The feed's case is the less obvious of the two and the worse one. A
// depth stream carries no credential, so the "unverified reads are fine for
// public data" reasoning that governs `https_get` reaches for it too, and that
// is exactly the reasoning this reader ran under while it hardcoded
// ssl::verify_none. It does not hold: an intercepted feed leaks nothing and
// instead *decides* the book this process believes in, which every quote and
// every order downstream is derived from.
//
// Behaviour is not testable from here - proving the handshake rejects a bad
// chain needs a server presenting one - so what is pinned is the setting an
// operator actually configures, one layer above the socket.

using exchange::session::live_feed_options;
using exchange::transport::tls_verify;

TEST(LiveFeedTlsDefault, TheDepthFeedIsVerifiedUnlessAskedOtherwise) {
	EXPECT_EQ(live_feed_options{}.verify, tls_verify::peer);
}
