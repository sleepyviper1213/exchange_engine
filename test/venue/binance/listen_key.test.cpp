#include "venue/binance/user_data.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

// The user data stream's lifecycle: obtaining a listen key, keeping it alive,
// and where the events arrive.
//
// The property worth pinning is what these requests do *not* carry. They are
// Binance's USER_STREAM security type - key header, no signature - and a
// signature the venue did not ask for is a rejection rather than a harmless
// extra.

using exchange::venue::credentials;
using exchange::venue::environment;
using exchange::venue::binance::close_listen_key;
using exchange::venue::binance::KEEPALIVE_INTERVAL;
using exchange::venue::binance::keepalive_listen_key;
using exchange::venue::binance::LISTEN_KEY_LIFETIME;
using exchange::venue::binance::LISTEN_KEY_WEIGHT;
using exchange::venue::binance::open_listen_key;
using exchange::venue::binance::parse_listen_key;
using exchange::venue::binance::user_data_error;
using exchange::venue::binance::user_data_stream;

namespace {

constexpr auto LISTEN_KEY_SAMPLE =
	"pqia91ma19a5s61cv6a81va65sdf19v8a65a1a5s61cv6a81va65sdf19v8a65a1";

[[nodiscard]] credentials listen_key_credentials() {
	return credentials{.key = "test-key", .secret = "test-secret"};
}

} // namespace

TEST(BinanceListenKey, OpeningCarriesTheKeyAndNoSignature) {
	const auto request =
		open_listen_key(listen_key_credentials(), environment::testnet);
	ASSERT_TRUE(request.has_value()) << message(request.error());

	EXPECT_EQ(request->endpoint.target, "/api/v3/userDataStream");
	EXPECT_EQ(request->api_key, "test-key");
	// A signature the venue did not ask for is refused, not ignored.
	EXPECT_FALSE(request->endpoint.target.contains("signature="));
	EXPECT_FALSE(request->endpoint.target.contains("timestamp="));
	// And the secret is neither read nor sent.
	EXPECT_FALSE(request->endpoint.target.contains("test-secret"));
}

TEST(BinanceListenKey, OpeningNeedsOnlyTheKeyHalfOfTheCredential) {
	// Unlike a signed request: a USER_STREAM endpoint says *whose* stream, and
	// needs nothing to prove it with. Refusing a key-only credential here would
	// make the stream unreachable for a read-only deployment.
	const credentials key_only{.key = "test-key", .secret = ""};
	EXPECT_TRUE(open_listen_key(key_only).has_value());

	EXPECT_EQ(open_listen_key(credentials{}).error(),
			  user_data_error::no_credentials);
}

TEST(BinanceListenKey, KeepaliveAndCloseNameTheKeyInTheQuery) {
	const auto alive =
		keepalive_listen_key(LISTEN_KEY_SAMPLE, listen_key_credentials());
	const auto closed =
		close_listen_key(LISTEN_KEY_SAMPLE, listen_key_credentials());
	ASSERT_TRUE(alive.has_value());
	ASSERT_TRUE(closed.has_value());

	const std::string expected =
		std::string("/api/v3/userDataStream?listenKey=") + LISTEN_KEY_SAMPLE;
	EXPECT_EQ(alive->endpoint.target, expected);
	// The verb is the caller's to apply - this module hands out an endpoint,
	// and PUT and DELETE address the same one.
	EXPECT_EQ(closed->endpoint.target, expected);
}

TEST(BinanceListenKey, AnEmptyKeyIsRefusedRatherThanSentAsBlank) {
	// A blank listenKey would extend nothing and report success, which is the
	// shape of a stream that dies silently half an hour later.
	EXPECT_EQ(keepalive_listen_key("", listen_key_credentials()).error(),
			  user_data_error::no_listen_key);
	EXPECT_EQ(close_listen_key("", listen_key_credentials()).error(),
			  user_data_error::no_listen_key);
}

TEST(BinanceListenKey, EveryRequestReportsItsWeight) {
	EXPECT_EQ(open_listen_key(listen_key_credentials())->weight,
			  LISTEN_KEY_WEIGHT);
	EXPECT_EQ(keepalive_listen_key(LISTEN_KEY_SAMPLE, listen_key_credentials())
				  ->weight,
			  LISTEN_KEY_WEIGHT);
}

TEST(BinanceListenKey, TheKeepaliveIntervalLeavesRoomForOneFailure) {
	// The number that keeps the stream alive: at half the lifetime, a single
	// failed keepalive still has a whole interval to be retried in. Equal to
	// the lifetime would mean one lost request costs the stream.
	EXPECT_LT(KEEPALIVE_INTERVAL, LISTEN_KEY_LIFETIME);
	EXPECT_LE(KEEPALIVE_INTERVAL * 2, LISTEN_KEY_LIFETIME);
}

TEST(BinanceListenKey, TheEnvironmentChoosesTheHost) {
	EXPECT_EQ(open_listen_key(listen_key_credentials(), environment::testnet)
				  ->endpoint.host,
			  "testnet.binance.vision");
	EXPECT_EQ(open_listen_key(listen_key_credentials(), environment::production)
				  ->endpoint.host,
			  "api.binance.com");
	// The same default as order entry: a caller who did not say must not reach
	// production.
	EXPECT_EQ(open_listen_key(listen_key_credentials())->endpoint.host,
			  "testnet.binance.vision");
}

TEST(BinanceListenKey, TheStreamEndpointPutsTheKeyInThePath) {
	const auto stream =
		user_data_stream(LISTEN_KEY_SAMPLE, environment::testnet);

	EXPECT_EQ(stream.host, "stream.testnet.binance.vision");
	// Market data's port, not 443 - the venue publishes both streams there.
	EXPECT_EQ(stream.port, "9443");
	EXPECT_EQ(stream.target, std::string("/ws/") + LISTEN_KEY_SAMPLE);
}

TEST(BinanceListenKey, TheKeyIsReadOutOfTheOpenResponse) {
	const std::string body =
		std::string(R"({"listenKey":")") + LISTEN_KEY_SAMPLE + R"("})";

	const auto key = parse_listen_key(body);
	ASSERT_TRUE(key.has_value()) << message(key.error());
	EXPECT_EQ(*key, LISTEN_KEY_SAMPLE);
}

TEST(BinanceListenKey, AResponseWithNoUsableKeyIsRefused) {
	// An empty string is refused as hard as an absent field: it would build a
	// `/ws/` stream URL that connects to nothing and never delivers an event.
	EXPECT_EQ(parse_listen_key(R"({"listenKey":""})").error(),
			  user_data_error::missing_field);
	EXPECT_EQ(parse_listen_key(R"({"code":-1125,"msg":"bad key"})").error(),
			  user_data_error::missing_field);
	EXPECT_EQ(parse_listen_key("<html>502</html>").error(),
			  user_data_error::invalid_json);
}
