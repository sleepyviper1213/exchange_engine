// Formatters for the lifecycle records a session writes at its edges.

#include "event/format.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace life = exchange::engine::event::lifecycle;

namespace {

// The three lifecycle formatters print the instant as a raw count on purpose -
// rendering it needs a time zone and a calendar, and a log line is wanted
// diffable against the bytes a journal holds. So the record carries a typed
// instant and the rendering is still a number.
const life::wall_time AT{std::chrono::nanoseconds{1'700'000'000'000'000'000LL}};

TEST(EventLifecycleFormat, StartupNamesTheSessionAndWhatBecameOfTheLastOne) {
	EXPECT_EQ(fmt::format("{}",
						  life::startup{.session   = 7,
										.timestamp = AT,
										.mode      = life::start_mode::COLD}),
			  "startup[session=7 COLD at_ns=1700000000000000000]");
}

TEST(EventLifecycleFormat, ShutdownPrintsItsCountsEvenAtZero) {
	// A session that applied nothing is news, not an omission - unlike an
	// order's absent trigger, which the compact form drops precisely because it
	// means nothing.
	EXPECT_EQ(
		fmt::format("{}",
					life::shutdown{.session          = 7,
								   .timestamp        = AT,
								   .reason           = life::stop_reason::HALTED,
								   .commands_applied = 0,
								   .events_published = 0}),
		"shutdown[session=7 HALTED at_ns=1700000000000000000 cmds=0 events=0]");
}

TEST(EventLifecycleFormat, RecoveryPrintsTheSessionItContinues) {
	EXPECT_EQ(
		fmt::format("{}",
					life::recovery{.session        = 8,
								   .recovered_from = 7,
								   .timestamp      = AT,
								   .source = life::recovery_mode::SNAPSHOT |
											 life::recovery_mode::JOURNAL,
								   .entries_replayed = 95,
								   .orders_restored  = 12}),
		"recovery[session=8 from=7 SNAPSHOT|JOURNAL "
		"at_ns=1700000000000000000 replayed=95 orders=12]");
}

} // namespace
