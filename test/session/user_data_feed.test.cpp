#include "session/user_data_feed.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

// The account stream's contract with whoever consumes it.
//
// The socket itself is not exercised here - that needs a venue - but the shape
// the consumer sees is, and it is the half with a rule in it: a gap has to be
// announced, because a consumer cannot tell a quiet account from a dead socket
// by looking at the reports.

using exchange::session::user_data_handler;
using exchange::session::user_data_options;
using exchange::session::user_data_report;
using exchange::venue::environment;
using exchange::venue::execution_kind;
using exchange::venue::execution_report;
using exchange::venue::execution_status;

namespace {

/// A consumer that records what it was told, in order.
struct user_data_recorder {
	std::vector<std::string> seen;

	void on_report(const execution_report &report) {
		seen.push_back("report:" + report.client_order_id);
	}

	void on_gap(std::string_view why) {
		seen.emplace_back("gap:" + std::string(why));
	}
};

/// A consumer missing @c on_gap - the mistake the concept exists to catch.
struct user_data_reports_only {
	void on_report(const execution_report &) {}
};

} // namespace

TEST(UserDataFeed, AConsumerMustBeAbleToHearAboutAGap) {
	// Not a courtesy: from a sequence of reports alone, an account where
	// nothing happened is identical to one whose socket died. A handler that
	// cannot be told must not compile.
	static_assert(user_data_handler<user_data_recorder>);
	static_assert(!user_data_handler<user_data_reports_only>);
	SUCCEED();
}

TEST(UserDataFeed, AGapAndAReportAreDistinguishableAtTheConsumer) {
	user_data_recorder recorder;
	execution_report report;
	report.client_order_id = "ex-42";
	report.status          = execution_status::filled;
	report.kind            = execution_kind::trade;

	recorder.on_gap("the account stream was rebuilt");
	recorder.on_report(report);

	ASSERT_EQ(recorder.seen.size(), 2U);
	EXPECT_TRUE(recorder.seen[0].starts_with("gap:"));
	EXPECT_EQ(recorder.seen[1], "report:ex-42");
}

TEST(UserDataFeed, TheDefaultsAreTheSafeOnes) {
	const user_data_options options;

	// The sandbox, matching order entry: a stream pointed at one deployment
	// while orders go to the other reports on an account this process is not
	// trading.
	EXPECT_EQ(options.env, environment::testnet);
	// Verified, because the stream's URL carries a bearer credential.
	EXPECT_EQ(options.verify, exchange::transport::tls_verify::peer);
	// Unlimited reconnects and no deadline: an account stream that gives up
	// leaves the engine blind to its own fills, which is worse than retrying.
	EXPECT_EQ(options.max_reconnects, 0U);
	EXPECT_EQ(options.duration.count(), 0);
	// Not zero: a venue that just closed on us is not helped by an immediate
	// retry, and a tight loop against a rate-limited endpoint earns a ban.
	EXPECT_GT(options.reconnect_delay.count(), 0);
}

TEST(UserDataFeed, AFreshReportCountsNothing) {
	const user_data_report report;

	EXPECT_EQ(report.frames, 0U);
	EXPECT_EQ(report.reports, 0U);
	EXPECT_EQ(report.malformed, 0U);
	EXPECT_EQ(report.reconnects, 0U);
	// Nothing was subscribed, which is the difference between a run that could
	// not start and one that started and heard nothing.
	EXPECT_EQ(report.subscribes, 0U);
	EXPECT_TRUE(report.stopped.empty());
}
