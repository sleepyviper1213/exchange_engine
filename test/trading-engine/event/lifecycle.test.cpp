#include "trading-engine/event/lifecycle/lifecycle.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <type_traits>

// The records that open and close a session. They carry no behaviour, so what
// there is to pin is the two properties everything downstream leans on: they
// survive a raw byte round trip, because a journal append is a write of their
// object representation and not a serialisation step, and their equality reads
// every field, because a replay comparing what it rebuilt against what was
// recorded is only as good as the fields that comparison looks at.

using namespace exchange::engine::event::lifecycle;

namespace {

/// @brief The journal's whole encoding: the record's bytes, and back again.
template <class T>
T through_the_journal(const T &record) {
	static_assert(std::is_trivially_copyable_v<T>);
	using bytes = std::array<std::byte, sizeof(T)>;
	return std::bit_cast<T>(std::bit_cast<bytes>(record));
}

// A wall-clock instant, not a count: the records carry a time_point now, so a
// steady reading cannot reach them. @see lifecycle::wall_time
const wall_time WHEN{std::chrono::nanoseconds{1'700'000'000'000'000'000LL}};

startup an_opening() {
	return {.session = 7, .timestamp = WHEN, .mode = StartMode::COLD};
}

shutdown a_closing() {
	return {.session          = 7,
			.timestamp        = WHEN,
			.reason           = StopReason::CLEAN,
			.commands_applied = 120,
			.events_published = 310};
}

recovery a_rebuild() {
	return {.session          = 8,
			.recovered_from   = 7,
			.timestamp        = WHEN,
			.source = recovery_mode::SNAPSHOT | recovery_mode::JOURNAL,
			.entries_replayed = 95,
			.orders_restored  = 12};
}

TEST(EventLifecycle, StartupSurvivesARawJournalAppend) {
	static_assert(std::is_trivially_copyable_v<startup>);
	EXPECT_EQ(through_the_journal(an_opening()), an_opening());
}

TEST(EventLifecycle, ShutdownSurvivesARawJournalAppend) {
	static_assert(std::is_trivially_copyable_v<shutdown>);
	EXPECT_EQ(through_the_journal(a_closing()), a_closing());
}

TEST(EventLifecycle, RecoverySurvivesARawJournalAppend) {
	static_assert(std::is_trivially_copyable_v<recovery>);
	EXPECT_EQ(through_the_journal(a_rebuild()), a_rebuild());
}

// Every field, one at a time. A defaulted operator== gives this for free today;
// the test is here for the day somebody writes one by hand and forgets a field,
// which is silent - a replay would report agreement it never checked.
TEST(EventLifecycle, StartupEqualityReadsEveryField) {
	const startup opening = an_opening();
	EXPECT_EQ(opening, an_opening());

	startup other    = an_opening();
	other.session    = 8;
	EXPECT_NE(opening, other);

	other             = an_opening();
	other.timestamp = WHEN + std::chrono::nanoseconds{1};
	EXPECT_NE(opening, other);

	other      = an_opening();
	other.mode = StartMode::RECOVERED;
	EXPECT_NE(opening, other);
}

TEST(EventLifecycle, ShutdownEqualityReadsEveryField) {
	const shutdown closing = a_closing();
	EXPECT_EQ(closing, a_closing());

	shutdown other = a_closing();
	other.session  = 8;
	EXPECT_NE(closing, other);

	other              = a_closing();
	other.timestamp = WHEN + std::chrono::nanoseconds{1};
	EXPECT_NE(closing, other);

	other        = a_closing();
	other.reason = StopReason::FAULT;
	EXPECT_NE(closing, other);

	other                  = a_closing();
	other.commands_applied = 121;
	EXPECT_NE(closing, other);

	other                  = a_closing();
	other.events_published = 311;
	EXPECT_NE(closing, other);
}

TEST(EventLifecycle, RecoveryEqualityReadsEveryField) {
	const recovery rebuild = a_rebuild();
	EXPECT_EQ(rebuild, a_rebuild());

	recovery other = a_rebuild();
	other.session  = 9;
	EXPECT_NE(rebuild, other);

	// The edge to the session being continued is part of the record's identity:
	// two recoveries of different histories are not the same event.
	other                = a_rebuild();
	other.recovered_from = 6;
	EXPECT_NE(rebuild, other);

	other              = a_rebuild();
	other.timestamp = WHEN + std::chrono::nanoseconds{1};
	EXPECT_NE(rebuild, other);

	other        = a_rebuild();
	other.source = recovery_mode::JOURNAL;
	EXPECT_NE(rebuild, other);

	other                  = a_rebuild();
	other.entries_replayed = 96;
	EXPECT_NE(rebuild, other);

	other                 = a_rebuild();
	other.orders_restored = 13;
	EXPECT_NE(rebuild, other);
}

// The enumerator names are what a log line and a journal reader both show, so
// they are part of the format rather than a debugging convenience.
TEST(EventLifecycle, ModesAndReasonsPrintAsTheirNames) {
	EXPECT_EQ(to_string(StartMode::COLD), "COLD");
	EXPECT_EQ(to_string(StartMode::RECOVERED), "RECOVERED");

	EXPECT_EQ(to_string(StopReason::CLEAN), "CLEAN");
	EXPECT_EQ(to_string(StopReason::HALTED), "HALTED");
	EXPECT_EQ(to_string(StopReason::FAULT), "FAULT");

	EXPECT_EQ(to_string(recovery_mode::SNAPSHOT), "SNAPSHOT");
	EXPECT_EQ(to_string(recovery_mode::JOURNAL), "JOURNAL");
}

// The bits are a set, so "a snapshot was involved" and "a snapshot and nothing
// else" are different questions and each has its own spelling. This is the whole
// reason the combination is not a third enumerator.
TEST(EventLifecycle, ASourceSetAnswersInvolvedAndOnlySeparately) {
	const recovery_modes both =
		recovery_mode::SNAPSHOT | recovery_mode::JOURNAL;

	EXPECT_TRUE(both.test(recovery_mode::SNAPSHOT));  // was a snapshot involved
	EXPECT_TRUE(both.test(recovery_mode::JOURNAL));
	EXPECT_TRUE(both.all_of(recovery_mode::SNAPSHOT | recovery_mode::JOURNAL));
	EXPECT_EQ(both.count(), 2U);

	// ...and "only a snapshot" is a different value, not a different reading of
	// the same one.
	const recovery_modes snapshot_only = recovery_mode::SNAPSHOT;
	EXPECT_TRUE(snapshot_only.test(recovery_mode::SNAPSHOT));
	EXPECT_FALSE(snapshot_only.test(recovery_mode::JOURNAL));
	EXPECT_EQ(snapshot_only.count(), 1U);
	EXPECT_NE(both, snapshot_only);
}

// A replay from an empty book is the case worth alerting on, and it is the one a
// bare JOURNAL bit names.
TEST(EventLifecycle, AJournalOnlyRebuildIsDistinguishableFromACheckpointedOne) {
	const recovery_modes from_nothing = recovery_mode::JOURNAL;
	EXPECT_FALSE(from_nothing.test(recovery_mode::SNAPSHOT));
	EXPECT_TRUE(from_nothing.any_of(recovery_mode::JOURNAL));
}

// The empty set is representable - flag's default is the empty one - and means
// "rebuilt out of nothing", which is a cold start, which emits no recovery record
// at all. is_well_formed is what says so.
TEST(EventLifecycle, ARecoveryOutOfNothingIsNotWellFormed) {
	recovery rebuild = a_rebuild();
	EXPECT_TRUE(is_well_formed(rebuild));

	rebuild.source = recovery_modes{};
	EXPECT_FALSE(is_well_formed(rebuild));

	// Nor is a session claiming to continue itself: the edge to the past has to
	// point somewhere else to be an edge at all.
	rebuild                = a_rebuild();
	rebuild.recovered_from = rebuild.session;
	EXPECT_FALSE(is_well_formed(rebuild));

	// A default-constructed record is the well-defined not-a-recovery.
	EXPECT_FALSE(is_well_formed(recovery{}));
}

} // namespace
