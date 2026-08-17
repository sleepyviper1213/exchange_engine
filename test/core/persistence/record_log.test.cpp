#include "core/persistence/persistence.fixture.hpp"
#include "core/persistence/record_log.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

// The durable log everything in TODO.md #6 is built on. Two properties carry it,
// and both are only interesting when something went wrong: what was appended
// comes back byte-identical, and a file left half-written by a crash is readable
// up to the last whole record rather than not at all.
//
// The torn-tail suites simulate that crash the only way a test can — by writing a
// partial record into the file directly — because the real cause is the process
// dying between two syscalls, which cannot be arranged from inside it.

using exchange::core::persistence::raw_record_log;
using exchange::core::persistence::record_log;

namespace {

/// @brief A record with padding in it, on purpose: 12 bytes of members in a
///        16-byte type, so a round trip that compared object representations
///        rather than members would be reading uninitialised padding.
struct sample {
	std::uint64_t id;
	std::uint16_t kind;
	std::uint16_t flags;

	bool operator==(const sample &) const noexcept = default;
};

static_assert(std::is_trivially_copyable_v<sample>);

std::vector<sample> samples(std::uint64_t count) {
	std::vector<sample> records;
	for (std::uint64_t i = 0; i < count; ++i)
		records.push_back({.id    = i + 1,
						   .kind  = static_cast<std::uint16_t>(i % 7),
						   .flags = static_cast<std::uint16_t>(i % 3)});
	return records;
}

/// @brief Append @p bytes raw, bypassing the log — the only way to produce the
///        half-written record a crash would leave.
void append_raw_bytes(const std::filesystem::path &path, std::size_t bytes) {
	std::FILE *file = nullptr;
#if defined(_WIN32)
	static_cast<void>(::fopen_s(&file, path.string().c_str(), "ab"));
#else
	file = std::fopen(path.c_str(), "ab");
#endif
	ASSERT_NE(file, nullptr);
	const std::vector<char> junk(bytes, '\x7f');
	ASSERT_EQ(std::fwrite(junk.data(), 1, bytes, file), bytes);
	static_cast<void>(std::fclose(file));
}

TEST(RecordLog, AFreshLogIsEmpty) {
	const scratch_dir dir("fresh");
	auto log = record_log<sample>::open_for_append(dir.file("journal.bin"));
	ASSERT_TRUE(log.has_value()) << log.error();
	EXPECT_EQ(log->count(), 0U);
	EXPECT_TRUE(log->good());
}

TEST(RecordLog, WhatWasAppendedComesBackIdentical) {
	const scratch_dir dir("roundtrip");
	const std::vector<sample> written = samples(64);

	auto log = record_log<sample>::open_for_append(dir.file("journal.bin"));
	ASSERT_TRUE(log.has_value()) << log.error();
	ASSERT_TRUE(log->append(written));
	ASSERT_TRUE(log->sync());
	EXPECT_EQ(log->count(), written.size());

	auto reader = record_log<sample>::open_for_read(dir.file("journal.bin"));
	ASSERT_TRUE(reader.has_value()) << reader.error();
	EXPECT_EQ(reader->count(), written.size());
	EXPECT_EQ(reader->read_from(0), written);
}

// The read a replay actually makes: everything after the point a snapshot
// already covers.
TEST(RecordLog, ReadingFromAnOffsetReturnsTheTail) {
	const scratch_dir dir("offset");
	const std::vector<sample> written = samples(10);

	auto log = record_log<sample>::open_for_append(dir.file("journal.bin"));
	ASSERT_TRUE(log.has_value()) << log.error();
	ASSERT_TRUE(log->append(written));
	ASSERT_TRUE(log->sync());

	const std::vector<sample> tail = log->read_from(6);
	ASSERT_EQ(tail.size(), 4U);
	EXPECT_EQ(tail.front(), written[6]);
	EXPECT_EQ(tail.back(), written.back());

	// Past the end is empty, not an error: a snapshot taken at the very tail of
	// the log has nothing to replay, which is a successful recovery.
	EXPECT_TRUE(log->read_from(written.size()).empty());
	EXPECT_TRUE(log->read_from(written.size() + 5).empty());
}

TEST(RecordLog, AReadIsShortRatherThanFailingAtTheEnd) {
	const scratch_dir dir("short_read");
	auto log = record_log<sample>::open_for_append(dir.file("journal.bin"));
	ASSERT_TRUE(log.has_value()) << log.error();
	ASSERT_TRUE(log->append(samples(3)));
	ASSERT_TRUE(log->sync());

	std::vector<sample> out(10);
	EXPECT_EQ(log->read_at(0, out), 3U);
	EXPECT_EQ(log->read_at(2, out), 1U);
	EXPECT_EQ(log->read_at(3, out), 0U);
}

// Reopening is what a restart does, so the count has to carry across it — a log
// that restarted its numbering would make a manifest's sequence meaningless.
TEST(RecordLog, ReopeningForAppendContinuesTheExistingLog) {
	const scratch_dir dir("reopen");
	const auto path = dir.file("journal.bin");
	const std::vector<sample> first = samples(5);

	{
		auto log = record_log<sample>::open_for_append(path);
		ASSERT_TRUE(log.has_value()) << log.error();
		ASSERT_TRUE(log->append(first));
		ASSERT_TRUE(log->sync());
	}

	auto reopened = record_log<sample>::open_for_append(path);
	ASSERT_TRUE(reopened.has_value()) << reopened.error();
	EXPECT_EQ(reopened->count(), first.size());

	const sample extra{.id = 99, .kind = 1, .flags = 2};
	ASSERT_TRUE(reopened->append(extra));
	ASSERT_TRUE(reopened->sync());
	EXPECT_EQ(reopened->count(), first.size() + 1);

	const std::vector<sample> all = reopened->read_from(0);
	ASSERT_EQ(all.size(), first.size() + 1);
	EXPECT_EQ(all.front(), first.front());
	EXPECT_EQ(all.back(), extra);
}

// The crash case, and the reason the log has a fixed stride at all: a file whose
// size is not a whole number of records has a torn tail by arithmetic, so the
// last whole record is recoverable without guessing where it ended.
TEST(RecordLog, ATornTailIsTruncatedWhenOpenedForAppend) {
	const scratch_dir dir("torn_append");
	const auto path = dir.file("journal.bin");
	const std::vector<sample> written = samples(4);

	{
		auto log = record_log<sample>::open_for_append(path);
		ASSERT_TRUE(log.has_value()) << log.error();
		ASSERT_TRUE(log->append(written));
		ASSERT_TRUE(log->sync());
	}
	// The process died here, part-way through record five.
	append_raw_bytes(path, sizeof(sample) / 2);
	ASSERT_EQ(std::filesystem::file_size(path),
			  written.size() * sizeof(sample) + sizeof(sample) / 2);

	auto reopened = record_log<sample>::open_for_append(path);
	ASSERT_TRUE(reopened.has_value()) << reopened.error();
	EXPECT_EQ(reopened->count(), written.size());
	EXPECT_EQ(std::filesystem::file_size(path), written.size() * sizeof(sample));

	// And the log is appendable again: the next record lands on a boundary, so
	// everything still reads back in order.
	const sample extra{.id = 42, .kind = 3, .flags = 1};
	ASSERT_TRUE(reopened->append(extra));
	ASSERT_TRUE(reopened->sync());
	const std::vector<sample> all = reopened->read_from(0);
	ASSERT_EQ(all.size(), written.size() + 1);
	EXPECT_EQ(all.back(), extra);
}

// A reader must not edit the file it is recovering from, so it ignores the torn
// tail instead of removing it — and two readers then see the same thing.
TEST(RecordLog, ATornTailIsIgnoredButKeptWhenOpenedForRead) {
	const scratch_dir dir("torn_read");
	const auto path = dir.file("journal.bin");
	const std::vector<sample> written = samples(4);

	{
		auto log = record_log<sample>::open_for_append(path);
		ASSERT_TRUE(log.has_value()) << log.error();
		ASSERT_TRUE(log->append(written));
		ASSERT_TRUE(log->sync());
	}
	append_raw_bytes(path, sizeof(sample) / 2);
	const auto torn_size = std::filesystem::file_size(path);

	auto reader = record_log<sample>::open_for_read(path);
	ASSERT_TRUE(reader.has_value()) << reader.error();
	EXPECT_EQ(reader->count(), written.size());
	EXPECT_EQ(reader->read_from(0), written);
	EXPECT_EQ(std::filesystem::file_size(path), torn_size) << "a reader wrote";
}

TEST(RecordLog, OpeningAMissingLogForReadFailsAndNamesIt) {
	const scratch_dir dir("missing");
	const auto path = dir.file("absent.bin");
	auto reader     = record_log<sample>::open_for_read(path);
	ASSERT_FALSE(reader.has_value());
	// Recovery failures are read by whoever is trying to get a venue back up, so
	// the message has to say which file.
	EXPECT_NE(reader.error().find(path.filename().string()), std::string::npos)
		<< reader.error();
}

TEST(RecordLog, AppendingNothingIsANoOpAndLeavesTheLogGood) {
	const scratch_dir dir("empty_append");
	auto log = record_log<sample>::open_for_append(dir.file("journal.bin"));
	ASSERT_TRUE(log.has_value()) << log.error();
	EXPECT_TRUE(log->append(std::span<const sample>{}));
	EXPECT_EQ(log->count(), 0U);
	EXPECT_TRUE(log->good());
}

// A batch append and a loop of single appends must produce the same file: the
// batch path is one fwrite of many records and the single path is many of one,
// and a stride bug would show up as a difference between them.
TEST(RecordLog, ABatchAppendMatchesRecordByRecordAppends) {
	const scratch_dir dir("batch");
	const std::vector<sample> written = samples(16);

	auto batched = record_log<sample>::open_for_append(dir.file("batch.bin"));
	ASSERT_TRUE(batched.has_value()) << batched.error();
	ASSERT_TRUE(batched->append(written));
	ASSERT_TRUE(batched->sync());

	auto singles = record_log<sample>::open_for_append(dir.file("single.bin"));
	ASSERT_TRUE(singles.has_value()) << singles.error();
	for (const sample &record : written) ASSERT_TRUE(singles->append(record));
	ASSERT_TRUE(singles->sync());

	EXPECT_EQ(batched->count(), singles->count());
	EXPECT_EQ(batched->read_from(0), singles->read_from(0));
	EXPECT_EQ(std::filesystem::file_size(dir.file("batch.bin")),
			  std::filesystem::file_size(dir.file("single.bin")));
}

// The stride is the record's size, so the file is exactly an array on disk.
// Anything else means an encode step crept in.
TEST(RecordLog, TheFileIsExactlyAnArrayOfRecords) {
	const scratch_dir dir("stride");
	static_assert(record_log<sample>::STRIDE == sizeof(sample));

	auto log = record_log<sample>::open_for_append(dir.file("journal.bin"));
	ASSERT_TRUE(log.has_value()) << log.error();
	ASSERT_TRUE(log->append(samples(7)));
	ASSERT_TRUE(log->sync());
	EXPECT_EQ(std::filesystem::file_size(dir.file("journal.bin")),
			  7U * sizeof(sample));
}

TEST(RecordLog, AZeroStrideIsRefused) {
	const scratch_dir dir("stride_zero");
	auto log = raw_record_log::open_for_append(dir.file("journal.bin"), 0);
	EXPECT_FALSE(log.has_value());
}

} // namespace
