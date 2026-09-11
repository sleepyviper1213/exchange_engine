#include "venue/binance/signing.hpp"

#include <gtest/gtest.h>

#include <string>

// HMAC-SHA256 against the vector Binance publishes in its own documentation.
//
// This is the one test in the tree that proves an external protocol rather than
// our own behaviour: if it passes, requests we sign will authenticate, and if
// it fails every signed request comes back -1022 "Signature for this request is
// not valid" with nothing local to debug.
//
// @see
// https://developers.binance.com/docs/binance-spot-api-docs/rest-api/endpoint-security-type

using exchange::venue::binance::sign;
using exchange::venue::binance::sign_query;
using exchange::venue::binance::SIGNATURE_CHARS;

namespace {

/// The example key from Binance's SIGNED-endpoint documentation.
constexpr auto SIGN_TEST_SECRET =
	"NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j";

/// Its example query string, in the documented parameter order.
constexpr auto SIGN_TEST_QUERY =
	"symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC&quantity=1&price=0.1"
	"&recvWindow=5000&timestamp=1499827319559";

/// The signature Binance states for that pair.
constexpr auto SIGN_TEST_EXPECTED =
	"c8db56825ae71d6d79447849e617115f4a920fa2acdcab2b053c4b2838bd6b71";

} // namespace

TEST(BinanceSignQuery, MatchesTheVendorsPublishedVector) {
	EXPECT_EQ(sign(SIGN_TEST_QUERY, SIGN_TEST_SECRET), SIGN_TEST_EXPECTED);
}

TEST(BinanceSignQuery, ProducesLowercaseHexOfTheDigestLength) {
	const std::string signature = sign("anything", SIGN_TEST_SECRET);

	EXPECT_EQ(signature.size(), SIGNATURE_CHARS);
	for (const char c : signature)
		EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
			<< "uppercase or non-hex character: " << c;
}

TEST(BinanceSignQuery, IsDeterministic) {
	// Not a tautology: an HMAC seeded with anything per-call would authenticate
	// once and then stop, which is a failure mode that only shows up in
	// production.
	EXPECT_EQ(sign(SIGN_TEST_QUERY, SIGN_TEST_SECRET),
			  sign(SIGN_TEST_QUERY, SIGN_TEST_SECRET));
}

TEST(BinanceSignQuery, ADifferentPayloadSignsDifferently) {
	EXPECT_NE(sign(SIGN_TEST_QUERY, SIGN_TEST_SECRET),
			  sign(std::string(SIGN_TEST_QUERY) + "&x=1", SIGN_TEST_SECRET));
}

TEST(BinanceSignQuery, ADifferentSecretSignsDifferently) {
	EXPECT_NE(sign(SIGN_TEST_QUERY, SIGN_TEST_SECRET),
			  sign(SIGN_TEST_QUERY, "a different secret"));
}

TEST(BinanceSignQuery, TheSignatureGoesLastAndCoversEverythingBeforeIt) {
	const std::string full = sign_query(SIGN_TEST_QUERY, SIGN_TEST_SECRET);

	EXPECT_EQ(full,
			  std::string(SIGN_TEST_QUERY) +
				  "&signature=" + SIGN_TEST_EXPECTED);
}

TEST(BinanceSignQuery, AnEmptySecretLeavesTheQueryUnsigned) {
	// Not "signed with an empty key": a request that looks signed and is not
	// gets a confusing refusal, where an unsigned one is refused by name.
	EXPECT_EQ(sign_query(SIGN_TEST_QUERY, ""), SIGN_TEST_QUERY);
}
