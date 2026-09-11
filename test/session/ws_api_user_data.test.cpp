#include "venue/binance/signing.hpp"
#include "venue/binance/user_data.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

// Subscribing to the account stream over the WebSocket API.
//
// The listen-key flow this replaced was withdrawn without warning - `POST
// /api/v3/userDataStream` began answering 410 Gone from nginx - so what is
// pinned here is the *shape* of the new conversation rather than any behaviour
// of ours: which host, what the request is signed over, how a response is told
// from a push, and where in a push the report actually lives. Every one of
// those is a fact about the venue, and a test is the only place this tree gets
// to state one it cannot otherwise check without a socket.

using exchange::venue::credentials;
using exchange::venue::environment;
using exchange::venue::binance::is_user_data_event;
using exchange::venue::binance::parse_subscribe_reply;
using exchange::venue::binance::sign;
using exchange::venue::binance::subscribe_request;
using exchange::venue::binance::unwrap_event;
using exchange::venue::binance::user_data_error;
using exchange::venue::binance::ws_api_endpoint;

namespace {

constexpr std::int64_t WS_API_TIME_MS = 1'499'827'319'559;

[[nodiscard]] credentials ws_api_credentials() {
	return credentials{.key = "test-key", .secret = "test-secret"};
}

} // namespace

TEST(WsApiUserData, EachEnvironmentHasItsOwnRequestResponseHost) {
	// Three names again, none derivable from the others, and none of them the
	// market-data stream host. Guessing one from another is the mistake
	// `host_for` exists to stop, and this is the case that keeps the table
	// honest.
	EXPECT_EQ(ws_api_endpoint(environment::production).host,
			  "ws-api.binance.com");
	EXPECT_EQ(ws_api_endpoint(environment::testnet).host,
			  "ws-api.testnet.binance.vision");
	EXPECT_EQ(ws_api_endpoint(environment::demo).host,
			  "demo-ws-api.binance.com");
	// 443, not the 9443 the depth streams use.
	EXPECT_EQ(ws_api_endpoint(environment::demo).port, "443");
	EXPECT_EQ(ws_api_endpoint(environment::demo).target, "/ws-api/v3");
}

TEST(WsApiUserData, TheSubscribeRequestIsSignedOverExactlyWhatItSends) {
	const credentials creds = ws_api_credentials();
	const auto request = subscribe_request(creds, WS_API_TIME_MS, "uds-1");
	ASSERT_TRUE(request.has_value());

	EXPECT_TRUE(
		request->contains(R"("method":"userDataStream.subscribe.signature")"))
		<< *request;
	EXPECT_TRUE(request->contains(R"("id":"uds-1")")) << *request;
	EXPECT_TRUE(request->contains(R"("timestamp":1499827319559)")) << *request;

	// Recomputed here for the same reason the REST placement suite recomputes
	// its own: what was signed has to be exactly the bytes the venue will
	// reassemble, or the answer is -1022 and no amount of reading the JSON says
	// why.
	const std::string expected =
		sign("apiKey=test-key&timestamp=1499827319559", creds.secret);
	EXPECT_TRUE(request->contains(expected)) << *request;
	// And the secret itself never travels.
	EXPECT_FALSE(request->contains("test-secret")) << *request;
}

TEST(WsApiUserData, ASubscribeNeedsBothHalvesOfTheCredential) {
	// Unlike the listen key it replaced, which was a USER_STREAM endpoint and
	// took the key alone. Half a credential cannot sign, so it is refused here
	// rather than by the venue a round trip later.
	EXPECT_EQ(subscribe_request(credentials{}, WS_API_TIME_MS, "x").error(),
			  user_data_error::no_credentials);
	EXPECT_EQ(subscribe_request(credentials{.key = "k", .secret = ""},
								WS_API_TIME_MS,
								"x")
				  .error(),
			  user_data_error::no_credentials);
}

TEST(WsApiUserData, AResponseIsToldFromAPushByItsStatus) {
	// The discriminator, and the direction matters: read as "no status means a
	// push", an event type nobody has modelled yet is still handled as an
	// event. Read the other way round it would be reported as a malformed
	// response.
	EXPECT_FALSE(is_user_data_event(
		R"({"id":"uds-1","status":200,"result":{"subscriptionId":0}})"));
	EXPECT_TRUE(is_user_data_event(
		R"({"subscriptionId":0,"event":{"e":"executionReport","E":1}})"));
	// A frame that will not scan at all is not classified as an event: it goes
	// down the response path, which reports it, rather than being discarded.
	EXPECT_FALSE(is_user_data_event("<html>410</html>"));
}

TEST(WsApiUserData, ASubscribeReplyYieldsItsSubscriptionId) {
	const auto confirmed = parse_subscribe_reply(
		R"({"id":"uds-1","status":200,"result":{"subscriptionId":7}})");
	ASSERT_TRUE(confirmed.has_value());
	EXPECT_EQ(*confirmed, 7);

	// Zero is a real id - it is an index and the first one is the first - so it
	// must not be read as "no subscription".
	const auto first = parse_subscribe_reply(
		R"({"id":"uds-1","status":200,"result":{"subscriptionId":0}})");
	ASSERT_TRUE(first.has_value());
	EXPECT_EQ(*first, 0);
}

TEST(WsApiUserData, ARefusedSubscribeIsAFailureRatherThanAnEmptyStream) {
	// The failure that matters most, because the alternative is a socket that
	// is open, silent, and indistinguishable from a quiet account. A run that
	// read this as success would place orders and never hear about them.
	EXPECT_EQ(parse_subscribe_reply(
				  R"({"id":"uds-1","status":401,"error":{"code":-2015,)"
				  R"("msg":"Invalid API-key."}})")
				  .error(),
			  user_data_error::subscribe_refused);
	EXPECT_EQ(parse_subscribe_reply("<html>410 Gone</html>").error(),
			  user_data_error::invalid_json);
}

TEST(WsApiUserData, AnEventIsUnwrappedToTheReportInsideIt) {
	const std::string frame =
		R"({"subscriptionId":0,"event":{"e":"executionReport","s":"SOLUSDT"}})";
	EXPECT_EQ(unwrap_event(frame),
			  R"({"e":"executionReport","s":"SOLUSDT"})");
}

TEST(WsApiUserData, UnwrappingSurvivesABraceInsideAClientOrderId) {
	// A client order id is caller-chosen text, and Binance's accepted character
	// set is wide. Searching for the last `}` would take the wrong one here; so
	// would counting braces without skipping strings.
	const std::string frame =
		R"({"subscriptionId":0,"event":{"c":"ex-1}","X":"NEW"},"x":1})";
	EXPECT_EQ(unwrap_event(frame), R"({"c":"ex-1}","X":"NEW"})");
}

TEST(WsApiUserData, AFrameWithNoWrapperIsHandedBackWhole) {
	// What the withdrawn listen-key stream sent, and what every existing test
	// hands the parser directly. One decoder serving both shapes cannot
	// disagree with itself; two would.
	const std::string bare = R"({"e":"executionReport","s":"SOLUSDT"})";
	EXPECT_EQ(unwrap_event(bare), bare);
	// An unbalanced frame has no object to hand back, so the parser gets the
	// whole thing and reports it as malformed rather than this inventing one.
	const std::string truncated = R"({"subscriptionId":0,"event":{"e":"exec)";
	EXPECT_EQ(unwrap_event(truncated), truncated);
}
