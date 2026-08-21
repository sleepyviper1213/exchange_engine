#include "core/persistence/persistence.fixture.hpp"
#include "core/persistence/manifest.hpp"
#include "core/util/slurp.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

// The one file whose loss makes every other artefact here unusable, so the
// suites are about it never being half-written and never being unreadable: a
// replace that a crash can only leave as the old value or the new one, a text
// format an operator can read without a tool, and a refusal to guess at anything
// malformed.

using exchange::core::persistence::load;
using exchange::core::persistence::manifest;
using exchange::core::persistence::save;
using exchange::core::util::slurp;

namespace {

void write_text(const std::filesystem::path &path, const std::string &text) {
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << text;
}

TEST(Manifest, WhatWasSavedIsWhatLoads) {
	const scratch_dir dir("roundtrip");
	const manifest written{.snapshot_id = 7, .sequence = 1234, .session = 99};

	ASSERT_TRUE(save(dir.file("manifest"), written).has_value());
	const auto read = load(dir.file("manifest"));
	ASSERT_TRUE(read.has_value()) << read.error();
	EXPECT_EQ(*read, written);
}

// The format is part of the contract, not an implementation detail: it exists to
// be read during an incident by whatever is to hand.
TEST(Manifest, IsPlainTextAnOperatorCanRead) {
	const scratch_dir dir("text");
	ASSERT_TRUE(
		save(dir.file("manifest"), {.snapshot_id = 7, .sequence = 12, .session = 3})
			.has_value());

	EXPECT_EQ(slurp(dir.file("manifest")),
			  "snapshot_id=7\nsequence=12\nsession=3\n");
}

// Every field defaults to zero, which is the "nothing has happened yet" reading:
// snapshot zero, no journal records covered, no session.
TEST(Manifest, AZeroedManifestRoundTrips) {
	const scratch_dir dir("zero");
	ASSERT_TRUE(save(dir.file("manifest"), manifest{}).has_value());
	const auto read = load(dir.file("manifest"));
	ASSERT_TRUE(read.has_value()) << read.error();
	EXPECT_EQ(*read, manifest{});
}

// The point of the whole exercise: a second save is a replace, so a reader sees
// one whole manifest or the other and never a mixture. What a test can check
// without crashing the process mid-write is that the replacement is complete and
// leaves no debris behind.
TEST(Manifest, SavingAgainReplacesTheWholeFile) {
	const scratch_dir dir("replace");
	const auto path = dir.file("manifest");

	ASSERT_TRUE(
		save(path, {.snapshot_id = 1, .sequence = 10, .session = 5}).has_value());
	ASSERT_TRUE(
		save(path, {.snapshot_id = 2, .sequence = 20, .session = 6}).has_value());

	const auto read = load(path);
	ASSERT_TRUE(read.has_value()) << read.error();
	EXPECT_EQ(read->snapshot_id, 2U);
	EXPECT_EQ(read->sequence, 20U);
	EXPECT_EQ(read->session, 6U);

	// A shorter manifest overwriting a longer one must not leave the tail of the
	// old one behind, which is exactly what an in-place write would do.
	EXPECT_EQ(slurp(path), "snapshot_id=2\nsequence=20\nsession=6\n");

	// And the staging file is gone: a rename moved it rather than copying it.
	std::filesystem::path staging = path;
	staging += ".tmp";
	EXPECT_FALSE(std::filesystem::exists(staging));
}

TEST(Manifest, LoadingAMissingManifestFailsAndNamesIt) {
	const scratch_dir dir("missing");
	const auto path  = dir.file("manifest");
	const auto read  = load(path);
	ASSERT_FALSE(read.has_value());
	EXPECT_NE(read.error().find(path.filename().string()), std::string::npos)
		<< read.error();
}

// A manifest is the last file that should guess what somebody meant: a partly
// numeric value is a typo, not a number.
TEST(Manifest, ATrailingNonNumberIsRefusedRatherThanTruncated) {
	const scratch_dir dir("garbage");
	const auto path = dir.file("manifest");
	write_text(path, "snapshot_id=12x\nsequence=1\nsession=1\n");
	EXPECT_FALSE(load(path).has_value());
}

TEST(Manifest, AnUnknownKeyIsRefused) {
	const scratch_dir dir("unknown_key");
	const auto path = dir.file("manifest");
	write_text(path, "snapshot_id=1\nwhat_is_this=2\n");
	EXPECT_FALSE(load(path).has_value());
}

TEST(Manifest, ALineWithNoSeparatorIsRefused) {
	const scratch_dir dir("no_sep");
	const auto path = dir.file("manifest");
	write_text(path, "snapshot_id=1\njust some words\n");
	EXPECT_FALSE(load(path).has_value());
}

// A manifest written on Windows must read on POSIX and the other way round -
// this is the one leniency the format has, and it is deliberate.
TEST(Manifest, CarriageReturnsAreToleratedAcrossPlatforms) {
	const scratch_dir dir("crlf");
	const auto path = dir.file("manifest");
	write_text(path, "snapshot_id=4\r\nsequence=5\r\nsession=6\r\n");

	const auto read = load(path);
	ASSERT_TRUE(read.has_value()) << read.error();
	EXPECT_EQ(read->snapshot_id, 4U);
	EXPECT_EQ(read->sequence, 5U);
	EXPECT_EQ(read->session, 6U);
}

// A descriptive field the writer left out keeps its default rather than failing:
// a manifest gaining a field must not make every older one unreadable, which is
// the whole reason it is key=value and not three positional numbers.
TEST(Manifest, AMissingDescriptiveFieldKeepsItsDefault) {
	const scratch_dir dir("partial");
	const auto path = dir.file("manifest");
	write_text(path, "snapshot_id=4\nsequence=77\n");

	const auto read = load(path);
	ASSERT_TRUE(read.has_value()) << read.error();
	EXPECT_EQ(read->sequence, 77U);
	EXPECT_EQ(read->snapshot_id, 4U);
	EXPECT_EQ(read->session, 0U) << "session describes a recovery rather than "
									"instructing one";
}

// But not the two that instruct one. Either alone is a wrong instruction that
// reads as a valid file: snapshot_id without sequence replays a journal the
// snapshot already contains, and sequence without snapshot_id starts from an
// empty book and skips everything before it.
TEST(Manifest, AManifestMissingAnInstructionIsRefused) {
	const scratch_dir dir("incomplete");

	const auto no_sequence = dir.file("no_sequence");
	write_text(no_sequence, "snapshot_id=4\nsession=1\n");
	const auto first = load(no_sequence);
	ASSERT_FALSE(first.has_value());
	EXPECT_NE(first.error().find("sequence"), std::string::npos) << first.error();

	const auto no_snapshot = dir.file("no_snapshot");
	write_text(no_snapshot, "sequence=77\nsession=1\n");
	const auto second = load(no_snapshot);
	ASSERT_FALSE(second.has_value());
	EXPECT_NE(second.error().find("snapshot_id"), std::string::npos)
		<< second.error();
}

// A key given twice is refused rather than last-one-wins: two values for one
// field is a file somebody edited and got wrong, and picking one of them is
// picking which half of their intent to honour.
TEST(Manifest, ARepeatedKeyIsRefused) {
	const scratch_dir dir("repeated");
	const auto path = dir.file("manifest");
	write_text(path, "snapshot_id=1\nsequence=2\nsequence=3\n");
	EXPECT_FALSE(load(path).has_value());
}

TEST(Manifest, SavingIntoAMissingDirectoryFails) {
	const scratch_dir dir("no_dir");
	const auto path = dir.file("nowhere") / "manifest";
	EXPECT_FALSE(save(path, manifest{}).has_value());
}

} // namespace
