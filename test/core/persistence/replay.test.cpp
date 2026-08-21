#include "core/persistence/replay.hpp"
#include "core/util/function_ref.hpp"

#include "core/persistence/event_store.hpp"
#include "core/persistence/persistence.fixture.hpp"
#include "core/persistence/record_log.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <fstream>
#include <type_traits>
#include <vector>

// The driver, and the two things a hand-written read-and-apply loop gets wrong.
//
// It must stream, so a journal larger than memory replays in bounded space -
// and it must stop on the first refusal and say where, because the applier
// recovery actually uses is `engine_partition::submit`, whose queue is bounded
// and which therefore will refuse. A loop that ignored the refusal would drop
// commands in the middle of the one operation that must not drop any; a caller
// that ignored where it stopped would apply the accepted prefix twice, which
// for a journal of *commands* is not idempotent.

using exchange::core::persistence::event_store;
using exchange::core::persistence::record_log;
using exchange::core::persistence::replay;
using exchange::core::persistence::replay_result;
using exchange::core::util::function_ref;

namespace {



/// @brief Replay, and insist the journal was readable.
///
/// Every suite below is about what a replay does with records it *can* read, so
/// the unwrap lives here rather than in a dozen assertions. A journal that
/// cannot be read is a different question, and has its own test at the bottom.
replay_result must_replay(record_log<persistence_sample> &log, std::uint64_t from,
						  function_ref<bool(const persistence_sample &) const> apply) {
	auto done = replay(log, from, apply);
	EXPECT_TRUE(done.has_value()) << done.error();
	return done.value_or(replay_result{});
}

replay_result must_replay(record_log<persistence_sample> &log,
						  function_ref<bool(const persistence_sample &) const> apply) {
	return must_replay(log, 0, apply);
}


/// @brief A journal at @p path holding @p count records.
record_log<persistence_sample> journal_of(const std::filesystem::path &path,
							  std::uint64_t count) {
	auto log = record_log<persistence_sample>::open_for_append(path);
	EXPECT_TRUE(log.has_value());
	EXPECT_TRUE(log->append(persistence_samples(count)));
	EXPECT_TRUE(log->sync());
	return std::move(*log);
}

TEST(Replay, AnEmptyJournalIsCompleteWithNothingApplied) {
	const scratch_dir dir("replay_empty");
	auto log = journal_of(dir.file("journal.bin"), 0);

	std::vector<persistence_sample> seen;
	const replay_result done = must_replay(log, [&](const persistence_sample &r) {
		seen.push_back(r);
		return true;
	});

	EXPECT_EQ(done.applied, 0U);
	EXPECT_EQ(done.next, 0U);
	EXPECT_TRUE(done.complete);
	EXPECT_TRUE(seen.empty());
}

TEST(Replay, EveryRecordArrivesOnceAndInOrder) {
	const scratch_dir dir("replay_all");
	auto log = journal_of(dir.file("journal.bin"), 200);

	std::vector<persistence_sample> seen;
	const replay_result done = must_replay(log, [&](const persistence_sample &r) {
		seen.push_back(r);
		return true;
	});

	EXPECT_EQ(done.applied, 200U);
	EXPECT_EQ(done.next, 200U);
	EXPECT_TRUE(done.complete);
	// 200 crosses the internal chunk boundary several times, which is the case
	// a streaming reader can get wrong by dropping or repeating a chunk's edge.
	EXPECT_EQ(seen, persistence_samples(200));
}

// The read a recovery makes: everything the snapshot does not already cover.
TEST(Replay, ReplayingFromAnOffsetSkipsWhatTheSnapshotCovers) {
	const scratch_dir dir("replay_offset");
	auto log = journal_of(dir.file("journal.bin"), 10);

	std::vector<persistence_sample> seen;
	const replay_result done = must_replay(log, 6, [&](const persistence_sample &r) {
		seen.push_back(r);
		return true;
	});

	EXPECT_EQ(done.applied, 4U);
	EXPECT_EQ(done.next, 10U);
	EXPECT_TRUE(done.complete);
	ASSERT_EQ(seen.size(), 4U);
	EXPECT_EQ(seen.front().id, 7U);
	EXPECT_EQ(seen.back().id, 10U);
}

// A snapshot taken at the very tail leaves nothing to replay, which is a
// successful recovery rather than an empty one.
//
// Past the end reads the same way, and that is deliberate: this driver takes the
// offset on trust, because a log cannot know which offsets belong to it. The
// store is what refuses a checkpoint its journal cannot support - on the way in,
// at commit, and on the way back out, at open. @see EventStore
TEST(Replay, AnOffsetAtOrPastTheEndIsCompleteImmediately) {
	const scratch_dir dir("replay_past_end");
	auto log = journal_of(dir.file("journal.bin"), 5);

	const auto refuse = [](const persistence_sample &) { return false; };
	EXPECT_EQ(must_replay(log, 5, refuse), (replay_result{0, 5, true}));
	EXPECT_EQ(must_replay(log, 99, refuse), (replay_result{0, 99, true}));
}

// The property the whole return type exists for: a refusal stops the replay at
// exactly the record that was refused, and says so.
TEST(Replay, ARefusalStopsAtTheRecordItRefused) {
	const scratch_dir dir("replay_refuse");
	auto log = journal_of(dir.file("journal.bin"), 10);

	std::vector<persistence_sample> seen;
	const replay_result done = must_replay(log, [&](const persistence_sample &r) {
		if (r.id == 4) return false;
		seen.push_back(r);
		return true;
	});

	EXPECT_EQ(done.applied, 3U);
	EXPECT_EQ(done.next, 3U) << "resume must point at the refused record";
	EXPECT_FALSE(done.complete);
	ASSERT_EQ(seen.size(), 3U);
	EXPECT_EQ(seen.back().id, 3U);
}

// And resuming from `next` delivers the remainder exactly once - the loop a
// recovery driving a bounded queue actually writes.
TEST(Replay, ResumingFromNextDeliversTheRemainderExactlyOnce) {
	const scratch_dir dir("replay_resume");
	auto log = journal_of(dir.file("journal.bin"), 50);

	std::vector<persistence_sample> seen;
	// Accept three at a time, then refuse - a stand-in for a queue with room
	// for three that is drained between attempts.
	int budget       = 3;
	std::uint64_t at = 0;
	for (;;) {
		const replay_result step = must_replay(log, at, [&](const persistence_sample &r) {
			if (budget == 0) return false;
			--budget;
			seen.push_back(r);
			return true;
		});
		at                       = step.next;
		if (step.complete) break;
		budget = 3; // the consumer drained; carry on from `at`
	}

	EXPECT_EQ(at, 50U);
	EXPECT_EQ(seen, persistence_samples(50)) << "a resumed replay must not repeat or skip";
}

// A refusal on the very first record makes no progress, which must be reported
// as such rather than as completion - otherwise a caller loops forever or gives
// up.
TEST(Replay, RefusingEverythingMakesNoProgressAndSaysSo) {
	const scratch_dir dir("replay_refuse_all");
	auto log = journal_of(dir.file("journal.bin"), 5);

	const replay_result done =
		must_replay(log, [](const persistence_sample &) { return false; });
	EXPECT_EQ(done.applied, 0U);
	EXPECT_EQ(done.next, 0U);
	EXPECT_FALSE(done.complete);
}

// The whole point, end to end: write, checkpoint, "crash", reopen, and replay
// only the tail the checkpoint does not cover.
TEST(Replay, AStoreRecoversByReplayingOnlyWhatTheCheckpointDoesNotCover) {
	const scratch_dir dir("replay_recover");
	const auto root = dir.file("venue");

	{
		auto opened = event_store<persistence_sample>::open(root);
		ASSERT_TRUE(opened.has_value()) << opened.error();
		ASSERT_TRUE(opened->journal().append(persistence_samples(6)));

		// The snapshot stands in for book state, which persistence cannot
		// write.
		std::ofstream out(opened->snapshot_path(1),
						  std::ios::binary | std::ios::trunc);
		out << "state";
		out.close();
		ASSERT_TRUE(
			opened->commit(1, /*sequence=*/6, /*session=*/5).has_value());

		// Four more after the checkpoint, then the process dies.
		auto more = persistence_samples(10);
		ASSERT_TRUE(
			opened->journal().append(std::span<const persistence_sample>(more).subspan(6)));
		ASSERT_TRUE(opened->journal().sync());
	}

	auto reopened = event_store<persistence_sample>::open(root);
	ASSERT_TRUE(reopened.has_value()) << reopened.error();
	ASSERT_EQ(reopened->checkpoint().sequence, 6U);
	ASSERT_EQ(reopened->journal().count(), 10U);

	std::vector<persistence_sample> replayed;
	const replay_result done = must_replay(reopened->journal(),
									  reopened->checkpoint().sequence,
									  [&](const persistence_sample &r) {
										  replayed.push_back(r);
										  return true;
									  });

	EXPECT_TRUE(done.complete);
	EXPECT_EQ(done.applied, 4U) << "the snapshot already covered the first six";
	ASSERT_EQ(replayed.size(), 4U);
	EXPECT_EQ(replayed.front().id, 7U);
	EXPECT_EQ(replayed.back().id, 10U);
}

// A journal that stops being readable is not an end of journal, and conflating
// the two is how the resume loop above becomes an infinite one: `complete` would
// stay false, the applier would never be the thing refusing, and the caller would
// re-read the same offset forever. So it comes back as an error instead.
TEST(Replay, AnUnreadableJournalIsAnErrorRatherThanAnEarlyEnd) {
	const scratch_dir dir("replay_unreadable");
	const auto path = dir.file("journal.bin");
	{
		auto seeded = journal_of(path, 10);
		EXPECT_EQ(seeded.count(), 10U);
	}

	auto log = record_log<persistence_sample>::open_for_read(path);
	ASSERT_TRUE(log.has_value()) << log.error();
	ASSERT_EQ(log->count(), 10U);

	// Poisoned the only way a test can reach: a write to a read handle fails, and
	// a failed operation poisons the log, so every later read refuses too. The
	// real cause is a device error, which cannot be arranged from inside the
	// process - but it arrives at `replay` in exactly this shape.
	EXPECT_FALSE(log->append(persistence_sample{.id = 1, .kind = 0}));

	std::uint64_t seen = 0;
	const auto done    = replay(*log, [&](const persistence_sample &) {
        ++seen;
        return true;
    });

	ASSERT_FALSE(done.has_value()) << "an unreadable journal replayed cleanly";
	// The message names the record and the file, because whoever reads it is
	// trying to get a venue back up.
	EXPECT_NE(done.error().find("cannot read record 0"), std::string::npos)
		<< done.error();
	EXPECT_NE(done.error().find(path.filename().string()), std::string::npos)
		<< done.error();
	EXPECT_EQ(seen, 0U);
}

} // namespace
