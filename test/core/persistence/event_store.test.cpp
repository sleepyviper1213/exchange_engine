#include "core/persistence/event_store.hpp"
#include "core/persistence/persistence.fixture.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>
#include <vector>

// The directory, and the three things in it agreeing with each other. `record_log`
// and `manifest` are each correct in isolation and neither knows the other exists;
// what is left to get wrong is the relationship — that the manifest's `sequence`
// counts records in *this* journal, and that its `snapshot_id` names a file that
// is actually there. These suites are about that relationship.

using exchange::core::persistence::event_store;
using exchange::core::persistence::journal_path;
using exchange::core::persistence::manifest;
using exchange::core::persistence::manifest_path;
using exchange::core::persistence::save;
using exchange::core::persistence::snapshot_path;

namespace {

struct sample {
	std::uint64_t id;
	std::uint32_t kind;

	bool operator==(const sample &) const noexcept = default;
};

static_assert(std::is_trivially_copyable_v<sample>);

using store = event_store<sample>;

std::vector<sample> samples(std::uint64_t count, std::uint32_t kind = 0) {
	std::vector<sample> records;
	for (std::uint64_t i = 0; i < count; ++i)
		records.push_back({.id = i + 1, .kind = kind});
	return records;
}

/// @brief Stand in for a book snapshot, which persistence cannot write itself.
void write_snapshot(const std::filesystem::path &path) {
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << "state";
}

// A store that has never run is not an error state, and its checkpoint says so in
// the only way recovery can act on: no snapshot, replay from record zero.
TEST(EventStore, ANewStoreOpensWithAnEmptyCheckpoint) {
	const scratch_dir dir("store_new");
	auto opened = store::open(dir.file("venue"));
	ASSERT_TRUE(opened.has_value()) << opened.error();

	EXPECT_EQ(opened->checkpoint(), manifest{});
	EXPECT_EQ(opened->checkpoint().sequence, 0U);
	EXPECT_EQ(opened->checkpoint().snapshot_id, 0U);
	EXPECT_EQ(opened->journal().count(), 0U);
	EXPECT_TRUE(std::filesystem::is_directory(dir.file("venue")));
}

TEST(EventStore, OpeningCreatesTheDirectoryAndTheJournalInIt) {
	const scratch_dir dir("store_layout");
	const auto root = dir.file("venue");
	auto opened     = store::open(root);
	ASSERT_TRUE(opened.has_value()) << opened.error();

	ASSERT_TRUE(opened->journal().append(samples(2)));
	ASSERT_TRUE(opened->journal().sync());

	EXPECT_EQ(opened->root(), root);
	EXPECT_TRUE(std::filesystem::exists(journal_path(root)));
	// The journal's name has to be the same on the run that writes it and the one
	// that recovers from it, or a venue quietly starts a second journal and
	// forgets the first.
	EXPECT_EQ(opened->journal().path(), journal_path(root));
}

// Snapshot ids are padded so a directory listing comes out in snapshot order,
// which matters exactly when somebody is reading the directory by hand because
// recovery went wrong.
TEST(EventStore, SnapshotNamesSortLexicallyInSnapshotOrder) {
	const scratch_dir dir("store_names");
	const auto root = dir.file("venue");

	const auto first  = snapshot_path(root, 2);
	const auto second = snapshot_path(root, 10);
	EXPECT_LT(first.filename().string(), second.filename().string())
		<< first << " should sort before " << second;
	EXPECT_EQ(first.filename().string().size(),
			  second.filename().string().size());
}

// Zero means "no snapshot" in the manifest, so a real one must never be zero —
// otherwise a committed checkpoint is indistinguishable from a store that has
// never taken one.
TEST(EventStore, SnapshotIdsStartAtOneAndAdvanceWithEachCheckpoint) {
	const scratch_dir dir("store_ids");
	auto opened = store::open(dir.file("venue"));
	ASSERT_TRUE(opened.has_value()) << opened.error();

	EXPECT_EQ(opened->next_snapshot_id(), 1U);

	write_snapshot(opened->snapshot_path(1));
	ASSERT_TRUE(opened->commit(1, /*session=*/42).has_value());
	EXPECT_EQ(opened->next_snapshot_id(), 2U);

	write_snapshot(opened->snapshot_path(2));
	ASSERT_TRUE(opened->commit(2, 42).has_value());
	EXPECT_EQ(opened->next_snapshot_id(), 3U);
}

// The relationship the store exists to maintain: a committed checkpoint records
// how much of *this* journal the snapshot already accounts for.
TEST(EventStore, CommitRecordsTheJournalPositionTheSnapshotCovers) {
	const scratch_dir dir("store_commit");
	auto opened = store::open(dir.file("venue"));
	ASSERT_TRUE(opened.has_value()) << opened.error();

	ASSERT_TRUE(opened->journal().append(samples(7)));
	write_snapshot(opened->snapshot_path(1));
	ASSERT_TRUE(opened->commit(1, /*session=*/99).has_value());

	EXPECT_EQ(opened->checkpoint().snapshot_id, 1U);
	EXPECT_EQ(opened->checkpoint().sequence, 7U);
	EXPECT_EQ(opened->checkpoint().session, 99U);

	// Records after the checkpoint are the ones replay is responsible for, and
	// they do not move the committed sequence until the next commit.
	ASSERT_TRUE(opened->journal().append(samples(3)));
	EXPECT_EQ(opened->checkpoint().sequence, 7U);
	EXPECT_EQ(opened->journal().count(), 10U);
}

// A manifest naming a snapshot that was never finished is a recovery that cannot
// start, so it is refused at the one moment it is still preventable.
TEST(EventStore, CommittingASnapshotThatWasNeverWrittenIsRefused) {
	const scratch_dir dir("store_missing_snap");
	auto opened = store::open(dir.file("venue"));
	ASSERT_TRUE(opened.has_value()) << opened.error();

	const auto refused = opened->commit(1, 42);
	ASSERT_FALSE(refused.has_value());
	EXPECT_NE(refused.error().find("never written"), std::string::npos)
		<< refused.error();

	// And the checkpoint is untouched, so the store is still recoverable.
	EXPECT_EQ(opened->checkpoint(), manifest{});
	EXPECT_FALSE(std::filesystem::exists(manifest_path(dir.file("venue"))));
}

TEST(EventStore, CommittingSnapshotZeroIsRefused) {
	const scratch_dir dir("store_zero_snap");
	auto opened = store::open(dir.file("venue"));
	ASSERT_TRUE(opened.has_value()) << opened.error();
	EXPECT_FALSE(opened->commit(0, 42).has_value());
}

// Reopening is what a restart is. The checkpoint and the journal must both come
// back, and they must still agree.
TEST(EventStore, ReopeningRecoversTheCheckpointAndTheJournal) {
	const scratch_dir dir("store_reopen");
	const auto root = dir.file("venue");

	{
		auto opened = store::open(root);
		ASSERT_TRUE(opened.has_value()) << opened.error();
		ASSERT_TRUE(opened->journal().append(samples(4)));
		write_snapshot(opened->snapshot_path(1));
		ASSERT_TRUE(opened->commit(1, /*session=*/7).has_value());
		// Two more after the checkpoint: exactly what a replay must re-apply.
		ASSERT_TRUE(opened->journal().append(samples(2, /*kind=*/9)));
		ASSERT_TRUE(opened->journal().sync());
	}

	auto reopened = store::open(root);
	ASSERT_TRUE(reopened.has_value()) << reopened.error();
	EXPECT_EQ(reopened->checkpoint().snapshot_id, 1U);
	EXPECT_EQ(reopened->checkpoint().sequence, 4U);
	EXPECT_EQ(reopened->checkpoint().session, 7U);
	EXPECT_EQ(reopened->journal().count(), 6U);
	// And appending continues the same journal rather than starting another.
	ASSERT_TRUE(reopened->journal().append(samples(1)));
	EXPECT_EQ(reopened->journal().count(), 7U);
}

// A manifest that is present and unreadable is a different situation from one
// that is absent, and only the first is a failure: the pointer is there and
// cannot be followed, which no default can stand in for.
TEST(EventStore, AnUnreadableManifestFailsToOpenRatherThanDefaulting) {
	const scratch_dir dir("store_bad_manifest");
	const auto root = dir.file("venue");
	ASSERT_TRUE(store::open(root).has_value());

	std::ofstream out(manifest_path(root), std::ios::binary | std::ios::trunc);
	out << "this is not a manifest\n";
	out.close();

	EXPECT_FALSE(store::open(root).has_value());
}

// A crash between writing a snapshot and committing it leaves the file orphaned.
// Recovery must ignore it — the committed checkpoint is still the truth — and the
// next id must not reuse it.
TEST(EventStore, AnUncommittedSnapshotIsOrphanedRatherThanTrusted) {
	const scratch_dir dir("store_orphan");
	const auto root = dir.file("venue");

	{
		auto opened = store::open(root);
		ASSERT_TRUE(opened.has_value()) << opened.error();
		ASSERT_TRUE(opened->journal().append(samples(3)));
		write_snapshot(opened->snapshot_path(1));
		ASSERT_TRUE(opened->commit(1, 1).has_value());
		// ... and then the process died here, after the file, before the commit.
		write_snapshot(opened->snapshot_path(2));
	}

	auto reopened = store::open(root);
	ASSERT_TRUE(reopened.has_value()) << reopened.error();
	EXPECT_EQ(reopened->checkpoint().snapshot_id, 1U) << "trusted the orphan";
	EXPECT_TRUE(std::filesystem::exists(reopened->snapshot_path(2)))
		<< "the orphan should be left for an operator, not deleted";
	// Monotonic from the committed id, so the orphan's number is skipped rather
	// than handed out again.
	EXPECT_EQ(reopened->next_snapshot_id(), 2U);
}

TEST(EventStore, OpeningOverAFileRatherThanADirectoryFails) {
	const scratch_dir dir("store_not_dir");
	const auto path = dir.file("venue");
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << "not a directory";
	out.close();

	EXPECT_FALSE(store::open(path).has_value());
}

} // namespace
