#include "binance_trade.fixture.hpp"
#include "market_data/binance/trade_feed.hpp"
#include "market_data/feed.hpp"
#include "market_data/trade_feed.hpp"

#include <gtest/gtest.h>

#include <string>

using exchange::market_data::feed_stop;
using exchange::market_data::binance::jsonl_trade_feed;

// jsonl_trade_feed - a recorded tape, pulled one print at a time.

namespace {

TEST(JsonlTradeFeed, YieldsOnePrintPerLineInFileOrder) {
	const std::string jsonl = binance_trade_tape(3);
	jsonl_trade_feed feed(jsonl,
						  BINANCE_TRADE_PRICE_DECIMALS,
						  BINANCE_TRADE_QTY_DECIMALS);

	for (int i = 0; i < 3; ++i) {
		const auto print = feed.next();
		ASSERT_TRUE(print.has_value()) << "pull " << i;
		EXPECT_EQ(print->id, static_cast<long long>(BINANCE_TRADE_ID) + i);
	}
	EXPECT_EQ(feed.frames(), 3u);
}

TEST(JsonlTradeFeed, ReportsExhaustedAtTheEndAndKeepsSayingSo) {
	// The contract depth_feed states and trade_feed inherits: exhausted means
	// genuinely ended, and a further pull must not resume.
	const std::string jsonl = binance_trade_tape(1);
	jsonl_trade_feed feed(jsonl, 2, 2);

	ASSERT_TRUE(feed.next().has_value());
	const auto first = feed.next();
	ASSERT_FALSE(first.has_value());
	EXPECT_EQ(first.error().reason, feed_stop::exhausted);

	const auto again = feed.next();
	ASSERT_FALSE(again.has_value());
	EXPECT_EQ(again.error().reason, feed_stop::exhausted);
}

TEST(JsonlTradeFeed, ReportsAnEmptyTapeAsExhaustedRatherThanMalformed) {
	jsonl_trade_feed feed("", 2, 2);
	const auto print = feed.next();
	ASSERT_FALSE(print.has_value());
	EXPECT_EQ(print.error().reason, feed_stop::exhausted);
	EXPECT_EQ(feed.malformed(), 0u);
}

TEST(JsonlTradeFeed, SkipsBlankLinesWithoutCountingThemAsFrames) {
	std::string jsonl(BINANCE_TRADE_JSON);
	jsonl.append("\n\n   \n");
	jsonl.append(BINANCE_TRADE_JSON);
	jsonl.push_back('\n');

	jsonl_trade_feed feed(jsonl, 2, 2);
	EXPECT_TRUE(feed.next().has_value());
	EXPECT_TRUE(feed.next().has_value());
	EXPECT_EQ(feed.frames(), 2u);
	EXPECT_EQ(feed.malformed(), 0u);
}

TEST(JsonlTradeFeed, NamesTheOneBasedLineThatFailed) {
	std::string jsonl = binance_trade_tape(2);
	jsonl.append(R"({"t":)");
	jsonl.push_back('\n');

	jsonl_trade_feed feed(jsonl, 2, 2);
	ASSERT_TRUE(feed.next().has_value());
	ASSERT_TRUE(feed.next().has_value());

	const auto failed = feed.next();
	ASSERT_FALSE(failed.has_value());
	EXPECT_EQ(failed.error().reason, feed_stop::malformed);
	EXPECT_EQ(failed.error().position, 3u);
	EXPECT_EQ(feed.malformed(), 1u);
}

TEST(JsonlTradeFeed, IsResumableAfterAMalformedFrame) {
	// The failing line has already been stepped over when the status is
	// returned, so a caller that decides a corrupt frame is survivable simply
	// pulls again. It costs a trade id, which is the tape's only continuity
	// signal - so the choice is real, and it is the caller's.
	std::string jsonl(BINANCE_TRADE_JSON);
	jsonl.append("\n");
	jsonl.append(R"({"t":)");
	jsonl.append("\n");
	jsonl.append(BINANCE_TRADE_JSON);
	jsonl.push_back('\n');

	jsonl_trade_feed feed(jsonl, 2, 2);
	ASSERT_TRUE(feed.next().has_value());
	ASSERT_FALSE(feed.next().has_value());

	const auto resumed = feed.next();
	ASSERT_TRUE(resumed.has_value());
	EXPECT_EQ(resumed->price, BINANCE_TRADE_PRICE);
	EXPECT_EQ(feed.frames(), 2u);
	EXPECT_EQ(feed.malformed(), 1u);
}

TEST(JsonlTradeFeed, DecodesAFinalLineWithNoTrailingNewline) {
	// A capture cut short by a signal has no terminator on its last frame, and
	// dropping it would lose a print for a reason that is not about the data.
	const std::string jsonl(BINANCE_TRADE_JSON);
	jsonl_trade_feed feed(jsonl, 2, 2);
	EXPECT_TRUE(feed.next().has_value());
	EXPECT_EQ(feed.frames(), 1u);
}

} // namespace
