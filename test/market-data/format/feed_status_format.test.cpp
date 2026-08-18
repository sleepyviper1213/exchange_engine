#include "market-data/feed.hpp"
#include "market-data/format.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <string>

namespace md = exchange::market_data;

// feed_status / feed_run - the reason a replay stopped, in the one line an
// operator reads before deciding whether the result means anything.

namespace {

TEST(FeedStatusFormat, TheReasonAloneWhenThereIsNoPositionOrDetail) {
	const md::feed_status status{.reason = md::feed_stop::exhausted};
	EXPECT_EQ(fmt::format("{}", status), "end of feed");
}

TEST(FeedStatusFormat, NamesTheLineWhenThereIsOne) {
	const md::feed_status status{.reason   = md::feed_stop::malformed,
								 .position = 12};
	EXPECT_EQ(fmt::format("{}", status), "malformed frame (line 12)");
}

TEST(FeedStatusFormat, CarriesTheDecodersOwnWordsWhenItSuppliedThem) {
	const md::feed_status status{.reason   = md::feed_stop::malformed,
								 .detail   = "invalid JSON",
								 .position = 12};
	EXPECT_EQ(fmt::format("{}", status),
			  "malformed frame (line 12: invalid JSON)");
}

TEST(FeedStatusFormat, ADetailWithNoPositionStandsOnItsOwn) {
	const md::feed_status status{.reason = md::feed_stop::unavailable,
								 .detail = "connection reset"};
	EXPECT_EQ(fmt::format("{}", status),
			  "source unavailable (connection reset)");
}

TEST(FeedStatusFormat, TheStopReasonIsAFormattableEnumInItsOwnRight) {
	// The X-macro's format_as hook, so a reason can be logged without wrapping
	// it in a status first.
	EXPECT_EQ(fmt::format("{}", md::feed_stop::limited),
			  "message limit reached");
	EXPECT_EQ(md::describe(md::feed_stop::exhausted), "end of feed");
}

TEST(FeedStatusFormat, WidthPadsTheWholeRecord) {
	const md::feed_status status{.reason = md::feed_stop::exhausted};
	const std::string bare = fmt::format("{}", status);
	EXPECT_EQ(fmt::format("{:>15}", status), std::string(4, ' ') + bare);
}

TEST(FeedStatusFormat, ARunReportsWhatItReplayedAndWhyItStopped) {
	const md::feed_run run{
		.events    = 2984,
		.snapshots = 1,
		.stop      = md::feed_status{.reason = md::feed_stop::exhausted}};
	EXPECT_EQ(fmt::format("{}", run),
			  "feed[events=2984 snapshots=1 stopped: end of feed]");
}

TEST(FeedStatusFormat, ARunThatBrokeSaysSoAndWhere) {
	const md::feed_run run{
		.events = 3,
		.stop   = md::feed_status{.reason   = md::feed_stop::malformed,
								  .detail   = "invalid JSON",
								  .position = 4}};
	EXPECT_EQ(fmt::format("{}", run),
			  "feed[events=3 snapshots=0 stopped: malformed frame (line 4: "
			  "invalid JSON)]");
}

} // namespace
