#include "app/heartbeat.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

// The kill switch's signal. What these pin is when the watchdog acts and when
// it must not: it must act on a writer that went quiet without saying so, and
// must not act on one that stopped cleanly, has not started, or is mid-write.
// A switch that fired on a clean exit would be noise; one that missed a death
// would leave quotes resting with nothing managing them.

using exchange::app::heartbeat_reading;
using exchange::app::heartbeat_state;
using exchange::app::judge;
using exchange::app::parse_heartbeat;
using exchange::app::read_heartbeat;
using exchange::app::watchdog_verdict;
using exchange::app::write_heartbeat;
using namespace std::chrono_literals;

namespace {

constexpr auto HEARTBEAT_STALE = 5000ms;

[[nodiscard]] std::optional<heartbeat_reading>
heartbeat_at(std::optional<heartbeat_state> state,
			 std::chrono::milliseconds age) {
	return heartbeat_reading{.state = state, .age = age};
}

/// A fresh path under the temp directory, removed when the case ends.
struct heartbeat_temp_file {
	std::filesystem::path path =
		std::filesystem::temp_directory_path() /
		("exchange_heartbeat_test_" +
		 std::to_string(
			 std::chrono::steady_clock::now().time_since_epoch().count()));

	~heartbeat_temp_file() {
		std::error_code ignored;
		std::filesystem::remove(path, ignored);
	}
};

} // namespace

TEST(AppHeartbeat, ARunningWriterThatWentQuietIsStale) {
	EXPECT_EQ(
		judge(heartbeat_at(heartbeat_state::running, 6000ms), HEARTBEAT_STALE),
		watchdog_verdict::stale);
	EXPECT_EQ(
		judge(heartbeat_at(heartbeat_state::running, 1000ms), HEARTBEAT_STALE),
		watchdog_verdict::alive);
}

TEST(AppHeartbeat, ACleanStopIsNeverStaleHoweverOld) {
	// Written after the exit sweep, so nothing of ours is left to protect.
	EXPECT_EQ(
		judge(heartbeat_at(heartbeat_state::stopped, 24h), HEARTBEAT_STALE),
		watchdog_verdict::stopped);
}

TEST(AppHeartbeat, NoFileMeansNothingHasStarted) {
	EXPECT_EQ(judge(std::nullopt, HEARTBEAT_STALE), watchdog_verdict::absent);
}

TEST(AppHeartbeat, AnUnreadableFileIsJudgedByItsAgeAlone) {
	// Fresh: a torn read of a file still being written - alive.
	EXPECT_EQ(judge(heartbeat_at(std::nullopt, 10ms), HEARTBEAT_STALE),
			  watchdog_verdict::alive);
	// Stale: the writer left garbage and went quiet - act.
	EXPECT_EQ(judge(heartbeat_at(std::nullopt, 6000ms), HEARTBEAT_STALE),
			  watchdog_verdict::stale);
}

TEST(AppHeartbeat, TheTwoStatesRoundTripAndNothingElseParses) {
	EXPECT_EQ(parse_heartbeat("running\n"), heartbeat_state::running);
	EXPECT_EQ(parse_heartbeat("stopped\n"), heartbeat_state::stopped);
	EXPECT_FALSE(parse_heartbeat("").has_value());
	EXPECT_FALSE(parse_heartbeat("runn").has_value()) << "a torn write";
}

TEST(AppHeartbeat, AWrittenFileReadsBackFreshAndAgesByItsMtime) {
	heartbeat_temp_file file;
	EXPECT_FALSE(read_heartbeat(file.path).has_value());

	ASSERT_TRUE(write_heartbeat(file.path, heartbeat_state::running));
	const auto fresh = read_heartbeat(file.path);
	ASSERT_TRUE(fresh.has_value());
	EXPECT_EQ(fresh->state, heartbeat_state::running);
	EXPECT_EQ(judge(fresh, HEARTBEAT_STALE), watchdog_verdict::alive);

	// What a dead writer leaves behind: the same file, untouched for a while.
	std::filesystem::last_write_time(
		file.path,
		std::filesystem::file_time_type::clock::now() - 1min);
	EXPECT_EQ(judge(read_heartbeat(file.path), HEARTBEAT_STALE),
			  watchdog_verdict::stale);

	ASSERT_TRUE(write_heartbeat(file.path, heartbeat_state::stopped));
	EXPECT_EQ(judge(read_heartbeat(file.path), HEARTBEAT_STALE),
			  watchdog_verdict::stopped);
}
