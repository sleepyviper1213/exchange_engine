#include "venue/binance/api_error.hpp"

#include <gtest/gtest.h>

#include <string>

// The venue's own explanation of a refusal.
//
// Binance answers every rejected REST request with the same two-field envelope
// whatever the HTTP status. The status says which class of thing went wrong;
// only the body says what, so reporting the status alone leaves an operator
// holding "HTTP 400" and a JSON blob.

using namespace exchange::venue::binance;

TEST(BinanceParseApiError, TheEnvelopeTheVenueActuallySends) {
	// Verbatim from /api/v3/depth?symbol=NOTAPAIR, which answers 400 with this.
	const auto parsed =
		parse_api_error(R"({"code":-1121,"msg":"Invalid symbol."})");

	ASSERT_TRUE(parsed.has_value());
	EXPECT_EQ(parsed->code, -1121);
	EXPECT_EQ(parsed->msg, "Invalid symbol.");
}

TEST(BinanceParseApiError, ACodeWithNoMessageIsStillAnEnvelope) {
	// The code is the part that identifies it, so an absent `msg` is tolerated
	// rather than treated as "this is not an error" - which would fall back to
	// the status line and lose the code.
	const auto parsed = parse_api_error(R"({"code":-1003})");

	ASSERT_TRUE(parsed.has_value());
	EXPECT_EQ(parsed->code, -1003);
	EXPECT_TRUE(parsed->msg.empty());
}

TEST(BinanceParseApiError, ASuccessfulPayloadIsNotAnErrorEnvelope) {
	// The important negative: a depth response has no `code`, and mistaking one
	// for an error would turn a good fetch into a reported failure.
	EXPECT_FALSE(parse_api_error(R"({"lastUpdateId":1,"bids":[],"asks":[]})")
					 .has_value());
}

TEST(BinanceParseApiError, WhatIsNotJsonIsNotAnEnvelope) {
	// An edge proxy or a WAF answers with HTML, and 403 from a WAF is a
	// documented response. It has to fall through to the status rather than
	// become a parse failure of its own.
	EXPECT_FALSE(
		parse_api_error("<html><body>403 Forbidden</body></html>").has_value());
	EXPECT_FALSE(parse_api_error("").has_value());
	EXPECT_FALSE(parse_api_error("{").has_value());
}

TEST(BinanceParseApiError, ACodeThatIsNotANumberIsNotAnEnvelope) {
	EXPECT_FALSE(parse_api_error(R"({"code":"-1121","msg":"Invalid symbol."})")
					 .has_value())
		<< "the venue sends an integer; a string here means this is some other "
		   "document that happens to have a `code` field";
}

// --- the line a log gets ---------------------------------------------------

TEST(BinanceDescribeApiError, TheVenuesWordsBeatTheStatusLine) {
	const std::string described =
		describe_api_error(R"({"code":-1121,"msg":"Invalid symbol."})",
						   "HTTP 400");

	EXPECT_TRUE(described.contains("Invalid symbol."));
	EXPECT_TRUE(described.contains("-1121"));
	EXPECT_FALSE(described.contains("HTTP 400"))
		<< "the fallback is not appended - it is what is used *instead* when "
		   "there is nothing better";
}

TEST(BinanceDescribeApiError, TheFallbackIsUsedWhenThereIsNoEnvelope) {
	EXPECT_EQ(
		describe_api_error("<html>502</html>", "HTTP 502: <html>502</html>"),
		"HTTP 502: <html>502</html>");
}

TEST(BinanceDescribeApiError, ACodeWithNoMessageStillSaysSomething) {
	const std::string described =
		describe_api_error(R"({"code":-1003})", "HTTP 429");
	EXPECT_TRUE(described.contains("-1003")) << described;
}
