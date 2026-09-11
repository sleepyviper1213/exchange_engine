#include "transport/rest/request.hpp"

#include <gtest/gtest.h>

#include <string>

// The request value type and the two rules read off it. Nothing here opens a
// socket: what is being pinned is the classification that decides whether a
// request the server never answered may be put back on the wire, which is the
// one decision in this module with money attached.

using exchange::transport::rest::get_request;
using exchange::transport::rest::header;
using exchange::transport::rest::is_idempotent;
using exchange::transport::rest::may_resend;
using exchange::transport::rest::method;
using exchange::transport::rest::request;
using exchange::transport::rest::to_string;

namespace {

/// An order placement: the one shape in the tree that must never be re-sent
/// without someone first asking the venue what happened to the first attempt.
[[nodiscard]] request rest_request_place_order() {
	return request{.verb    = method::post,
				   .target  = "/api/v3/order",
				   .headers = {header{.name = "X-MBX-APIKEY", .value = "k"}},
				   .body    = "symbol=SOLUSDT&side=BUY&quantity=1"};
}

} // namespace

TEST(RestRequest, PostIsTheOnlyMethodThatIsNotIdempotent) {
	// RFC 9110 §9.2.2. The negative case is the whole point of the function -
	// if this ever returns true for POST, the pipeline silently gains the
	// ability to place an order twice.
	EXPECT_FALSE(is_idempotent(method::post));

	EXPECT_TRUE(is_idempotent(method::get));
	EXPECT_TRUE(is_idempotent(method::put));
	EXPECT_TRUE(is_idempotent(method::del));
}

TEST(RestRequest, AnUnansweredPostIsNeverResent) {
	const request order = rest_request_place_order();

	// Not even when the caller has explicitly allowed degrading. The venue may
	// have acted on the first attempt; a second one is a second order, and no
	// setting available to a caller should be able to ask for that.
	EXPECT_FALSE(may_resend(order, /*degrade=*/true));
	EXPECT_FALSE(may_resend(order, /*degrade=*/false));
}

TEST(RestRequest, AnUnansweredGetIsResentOnlyWhenDegradingIsAllowed) {
	const request read = get_request("/api/v3/depth?symbol=SOLUSDT");

	EXPECT_TRUE(may_resend(read, /*degrade=*/true));
	// Idempotent is permission, not obligation: a caller that turned degrading
	// off gets its batch reported as it happened.
	EXPECT_FALSE(may_resend(read, /*degrade=*/false));
}

TEST(RestRequest, AGetRequestCarriesNothingItWasNotGiven) {
	const request read = get_request("/api/v3/depth?symbol=SOLUSDT");

	EXPECT_EQ(read.verb, method::get);
	EXPECT_EQ(read.target, "/api/v3/depth?symbol=SOLUSDT");
	// A public read must not acquire a header or a body by default - the body
	// is what would turn it into a request servers mishandle, and a header is
	// what would leak a credential onto the unverified market-data path.
	EXPECT_TRUE(read.headers.empty());
	EXPECT_TRUE(read.body.empty());
}

TEST(RestRequest, EachMethodPrintsItsWireSpelling) {
	// These strings go on the request line, so a wrong one is a 400 from the
	// venue rather than a local error.
	EXPECT_EQ(to_string(method::get), "GET");
	EXPECT_EQ(to_string(method::post), "POST");
	EXPECT_EQ(to_string(method::put), "PUT");
	EXPECT_EQ(to_string(method::del), "DELETE");
}
