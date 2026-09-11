#include "market_data/feed.hpp"
#include "market_data/normalised.hpp"
#include "market_data/trade_feed.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <vector>

using exchange::side_t;
using exchange::market_data::drive;
using exchange::market_data::feed_status;
using exchange::market_data::feed_stop;
using exchange::market_data::is_clean;
using exchange::market_data::replay_trade_feed;
using exchange::market_data::trade_print;
using exchange::market_data::trade_pull;
using exchange::market_data::trade_run;

// drive(trade_feed, trade_handler) - the tape half of the driver, and the
// overload resolution that lets it share a name with the depth one.

namespace {

/// A handler that keeps what it was given, so a test can assert on order as
/// well as on count.
struct drive_trades_recorder {
	std::vector<trade_print> prints;

	void on_trade(trade_print print) { prints.push_back(print); }
};

/// A feed that yields @c count prints and then fails rather than ending, so the
/// fault path can be driven without a malformed corpus.
class drive_trades_faulty_feed {
public:
	explicit drive_trades_faulty_feed(std::uint64_t before_fault) noexcept
		: before_fault_(before_fault) {}

	[[nodiscard]] trade_pull next() {
		if (yielded_ >= before_fault_)
			return std::unexpected(feed_status{.reason   = feed_stop::malformed,
											   .detail   = "synthetic",
											   .position = yielded_ + 1});
		return trade_print{.id = static_cast<long long>(++yielded_)};
	}

private:
	std::uint64_t before_fault_ = 0;
	std::uint64_t yielded_      = 0;
};

static_assert(exchange::market_data::trade_feed<drive_trades_faulty_feed>);

/// A short corpus with alternating aggressors.
std::vector<trade_print> drive_trades_corpus(int count) {
	std::vector<trade_print> prints;
	prints.reserve(static_cast<std::size_t>(count));
	for (int i = 0; i < count; ++i)
		prints.push_back(
			trade_print{.id        = i + 1,
						.price     = 15345 + i,
						.qty       = 100,
						.aggressor = (i % 2) == 0 ? side_t::bid : side_t::ask});
	return prints;
}

TEST(DriveTrades, HandsEveryPrintToTheHandlerInOrder) {
	const auto corpus = drive_trades_corpus(4);
	replay_trade_feed feed(corpus);
	drive_trades_recorder recorder;

	const trade_run run = drive(feed, recorder);

	EXPECT_EQ(run.trades, 4u);
	EXPECT_EQ(run.stop.reason, feed_stop::exhausted);
	EXPECT_TRUE(is_clean(run));
	ASSERT_EQ(recorder.prints.size(), 4u);
	EXPECT_EQ(recorder.prints.front().id, 1);
	EXPECT_EQ(recorder.prints.back().id, 4);
	EXPECT_EQ(recorder.prints.back().aggressor, side_t::ask);
}

TEST(DriveTrades, StopsAtTheBoundAndCallsThatClean) {
	// Reaching the caller's own limit is not a failure - a driver that reported
	// it as one would make every bounded run look broken.
	const auto corpus = drive_trades_corpus(10);
	replay_trade_feed feed(corpus);
	drive_trades_recorder recorder;

	const trade_run run = drive(feed, recorder, 3);

	EXPECT_EQ(run.trades, 3u);
	EXPECT_EQ(run.stop.reason, feed_stop::limited);
	EXPECT_EQ(run.stop.position, 3u);
	EXPECT_TRUE(is_clean(run));
	EXPECT_EQ(recorder.prints.size(), 3u);
}

TEST(DriveTrades, AZeroBoundMeansNoBound) {
	const auto corpus = drive_trades_corpus(5);
	replay_trade_feed feed(corpus);
	drive_trades_recorder recorder;

	const trade_run run = drive(feed, recorder, 0);

	EXPECT_EQ(run.trades, 5u);
	EXPECT_EQ(run.stop.reason, feed_stop::exhausted);
}

TEST(DriveTrades, StopsAtTheFirstFaultAndReportsItUnclean) {
	drive_trades_faulty_feed feed(2);
	drive_trades_recorder recorder;

	const trade_run run = drive(feed, recorder);

	EXPECT_EQ(run.trades, 2u);
	EXPECT_EQ(run.stop.reason, feed_stop::malformed);
	EXPECT_EQ(run.stop.detail, "synthetic");
	EXPECT_FALSE(is_clean(run));
	EXPECT_EQ(recorder.prints.size(), 2u);
}

TEST(DriveTrades, ResumesOnTheSameFeedAfterAFault) {
	// The contract that makes stopping at a fault the caller's decision rather
	// than the driver's: driving again continues where it left off.
	const auto corpus = drive_trades_corpus(4);
	replay_trade_feed feed(corpus);
	drive_trades_recorder recorder;

	drive(feed, recorder, 2);
	const trade_run rest = drive(feed, recorder);

	EXPECT_EQ(rest.trades, 2u);
	ASSERT_EQ(recorder.prints.size(), 4u);
	EXPECT_EQ(recorder.prints.back().id, 4);
}

TEST(DriveTrades, RewindReplaysTheWholeCorpus) {
	const auto corpus = drive_trades_corpus(3);
	replay_trade_feed feed(corpus);
	drive_trades_recorder recorder;

	drive(feed, recorder);
	feed.rewind();
	drive(feed, recorder);

	EXPECT_EQ(recorder.prints.size(), 6u);
	EXPECT_EQ(feed.position(), 3u);
}

TEST(DriveTrades, AnEmptyCorpusIsExhaustedRatherThanFailed) {
	const std::vector<trade_print> corpus;
	replay_trade_feed feed(corpus);
	drive_trades_recorder recorder;

	const trade_run run = drive(feed, recorder);

	EXPECT_EQ(run.trades, 0u);
	EXPECT_TRUE(is_clean(run));
	EXPECT_TRUE(recorder.prints.empty());
}

} // namespace
