#include "core/persistence/record_log.hpp"

#include "core/persistence/persistence.fixture.hpp"
#include "core/persistence/replay.hpp"
#include "core/util/owned_file.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>


// The durable log everything in TODO.md #6 is built on. Four properties carry
// it, and every one of them is only interesting when something went wrong: what
// was appended comes back byte-identical, a file left half-written by a crash
// is readable up to the last whole record rather than not at all, a file that
// is not one of these logs is refused rather than read, and a record whose
// bytes changed under the log is caught rather than replayed.
//
// The torn-tail suites simulate that crash the only way a test can - by writing
// a partial record into the file directly - because the real cause is the
// process dying between two syscalls, which cannot be arranged from inside it.
// The corruption suites do the same thing for bit rot, by editing a byte in
// place: a real bad sector cannot be arranged either, and lands identically.

using exchange::core::persistence::LOG_HEADER_SIZE;
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
	records.reserve(count);
	for (std::uint64_t i = 0; i < count; ++i)
		records.emplace_back(i + 1,
							 static_cast<std::uint16_t>(i % 7),
							 static_cast<std::uint16_t>(i % 3));
	return records;
}

/// @brief Bytes a log of @p records occupies: the header, then framed records.
///
/// Spelled as a helper so a change to the framing is one edit here rather than
/// a hunt through every suite that knows how big a file should be.
[[nodiscard]] std::uintmax_t log_size(std::uint64_t records) {
	return LOG_HEADER_SIZE + (records * record_log<sample>::ONDISK_STRIDE);
}

/// @brief Flip every bit of the byte at @p offset, in place.
///
/// What bit rot, a bad sector or a half-written page looks like from inside the
/// process: the file is still exactly the right length, so nothing about its
/// size gives the damage away. Before the per-record checksum this was
/// undetectable by construction, which is what these suites exist to pin.
void corrupt_byte_at(const std::filesystem::path &path, std::uintmax_t offset) {
	const exchange::core::util::owned_file file =
		exchange::core::util::open_shared(path, "r+b");
	ASSERT_NE(file, nullptr);
	ASSERT_EQ(std::fseek(file.get(), static_cast<long>(offset), SEEK_SET), 0);
	const int byte = std::fgetc(file.get());
	ASSERT_NE(byte, EOF);
	ASSERT_EQ(std::fseek(file.get(), static_cast<long>(offset), SEEK_SET), 0);
	ASSERT_NE(std::fputc(byte ^ 0xFF, file.get()), EOF);
}

/// @brief Append @p bytes raw, bypassing the log - the only way to produce the
///        half-written record a crash would leave.
void append_raw_bytes(const std::filesystem::path &path, std::size_t bytes) {
	const exchange::core::util::owned_file file =
		exchange::core::util::open_shared(path, "ab");
	ASSERT_NE(file, nullptr);
	const std::vector<char> junk(bytes, '');
	ASSERT_EQ(std::fwrite(junk.data(), 1, bytes, file.get()), bytes);
}

TEST(RecordLog, AFreshLogIsEmpty) {
	const scratch_dir dir("fresh");
	auto log = record_log<sample>::open_for_append(dir.file("journal.bin"));
	ASSERT_TRUE(log.has_value()) << log.error();
	EXPECT_EQ(log->count(), 0U);
	EXPECT_TRUE(log->is_good());
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

// Reopening is what a restart does, so the count has to carry across it - a log
// that restarted its numbering would make a manifest's sequence meaningless.
TEST(RecordLog, ReopeningForAppendContinuesTheExistingLog) {
	const scratch_dir dir("reopen");
	const auto path                 = dir.file("journal.bin");
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

// The crash case, and the reason the log has a fixed stride at all: a file
// whose size is not a whole number of records has a torn tail by arithmetic, so
// the last whole record is recoverable without guessing where it ended.
TEST(RecordLog, ATornTailIsTruncatedWhenOpenedForAppend) {
	const scratch_dir dir("torn_append");
	const auto path                   = dir.file("journal.bin");
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
			  log_size(written.size()) + sizeof(sample) / 2);

	auto reopened = record_log<sample>::open_for_append(path);
	ASSERT_TRUE(reopened.has_value()) << reopened.error();
	EXPECT_EQ(reopened->count(), written.size());
	EXPECT_EQ(std::filesystem::file_size(path), log_size(written.size()));

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
// tail instead of removing it - and two readers then see the same thing.
TEST(RecordLog, ATornTailIsIgnoredButKeptWhenOpenedForRead) {
	const scratch_dir dir("torn_read");
	const auto path                   = dir.file("journal.bin");
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

// A log is opened knowing how many records the file holds, so a read that comes
// back short means the file lost them. That is the one failure a fixed-stride
// log cannot detect by arithmetic - the size check catches a torn *tail*, not a
// file that shrank after it was measured - so it is caught here instead, and
// the log stops being trustworthy rather than reporting a clean end of journal.
//
// Truncation is how this test reaches the state; a device read error reaches it
// identically and cannot be arranged from inside a process.
TEST(RecordLog, AFileThatShrinksUnderAReaderPoisonsTheLog) {
	const scratch_dir dir("shrunk");
	const auto path = dir.file("journal.bin");

	{
		auto log = record_log<sample>::open_for_append(path);
		ASSERT_TRUE(log.has_value()) << log.error();
		ASSERT_TRUE(log->append(samples(10)));
		ASSERT_TRUE(log->sync());
	}

	auto reader = record_log<sample>::open_for_read(path);
	ASSERT_TRUE(reader.has_value()) << reader.error();
	ASSERT_EQ(reader->count(), 10U);
	ASSERT_TRUE(reader->is_good());

	// Something else takes the file down to three records while this reader
	// holds its count of ten.
	std::error_code ec;
	std::filesystem::resize_file(path, log_size(3), ec);
	ASSERT_FALSE(ec) << ec.message();

	// The three that survived are genuine and still come back...
	std::vector<sample> out(10);
	EXPECT_EQ(reader->read_at(0, out), 3U);
	// ...and the log refuses everything afterwards, because it can no longer
	// promise that what it reports is what it holds.
	EXPECT_FALSE(reader->is_good())
		<< "a file that lost records read as healthy";
	EXPECT_EQ(reader->read_at(0, out), 0U);
	EXPECT_TRUE(reader->read_from(0).empty());
}

// The same situation one layer up. Worth being exact about what this pins and
// what it does not: it passes with or without the poison above, because
// `replay` has its own reason to refuse - an empty batch below the record count
// it was given. So this covers the driver's contract (a journal that stops
// yielding is an error, not an end) rather than the log's, and the two are
// tested separately on purpose. Without either, a caller resuming from `next`
// re-reads the same offset forever.
TEST(RecordLog, AShrunkJournalMakesAReplayFailRatherThanFinish) {
	const scratch_dir dir("shrunk_replay");
	const auto path = dir.file("journal.bin");

	{
		auto log = record_log<sample>::open_for_append(path);
		ASSERT_TRUE(log.has_value()) << log.error();
		ASSERT_TRUE(log->append(samples(200)));
		ASSERT_TRUE(log->sync());
	}

	auto reader = record_log<sample>::open_for_read(path);
	ASSERT_TRUE(reader.has_value()) << reader.error();
	std::error_code ec;
	std::filesystem::resize_file(path, log_size(100), ec);
	ASSERT_FALSE(ec) << ec.message();

	std::uint64_t seen = 0;
	const auto done =
		exchange::core::persistence::replay(*reader, [&](const sample &) {
			++seen;
			return true;
		});

	ASSERT_FALSE(done.has_value()) << "replayed a truncated journal cleanly";
	EXPECT_NE(done.error().find("cannot read record"), std::string::npos)
		<< done.error();
	// It delivered what survived before it gave up, rather than throwing the
	// readable prefix away.
	EXPECT_GT(seen, 0U);
	EXPECT_LT(seen, 200U);
}

TEST(RecordLog, OpeningAMissingLogForReadFailsAndNamesIt) {
	const scratch_dir dir("missing");
	const auto path = dir.file("absent.bin");
	auto reader     = record_log<sample>::open_for_read(path);
	ASSERT_FALSE(reader.has_value());
	// Recovery failures are read by whoever is trying to get a venue back up,
	// so the message has to say which file.
	EXPECT_NE(reader.error().find(path.filename().string()), std::string::npos)
		<< reader.error();
}

TEST(RecordLog, AppendingNothingIsANoOpAndLeavesTheLogGood) {
	const scratch_dir dir("empty_append");
	auto log = record_log<sample>::open_for_append(dir.file("journal.bin"));
	ASSERT_TRUE(log.has_value()) << log.error();
	EXPECT_TRUE(log->append(std::span<const sample>{}));
	EXPECT_EQ(log->count(), 0U);
	EXPECT_TRUE(log->is_good());
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

// The file is a header and then a fixed-stride array of framed records, and the
// arithmetic has to be exact because `manifest::sequence` counts records and is
// then used as an offset. Anything else means the framing drifted.
TEST(RecordLog, TheFileIsAHeaderThenAnArrayOfFramedRecords) {
	const scratch_dir dir("stride");
	static_assert(record_log<sample>::STRIDE == sizeof(sample));
	static_assert(record_log<sample>::ONDISK_STRIDE == sizeof(sample) + 4,
				  "a frame is a payload and its 32-bit checksum");

	auto log = record_log<sample>::open_for_append(dir.file("journal.bin"));
	ASSERT_TRUE(log.has_value()) << log.error();

	// A log with no records is still a log: the header is written at open, so
	// the file is already the format before anything is appended to it.
	ASSERT_TRUE(log->sync());
	EXPECT_EQ(std::filesystem::file_size(dir.file("journal.bin")), log_size(0));

	ASSERT_TRUE(log->append(samples(7)));
	ASSERT_TRUE(log->sync());
	EXPECT_EQ(std::filesystem::file_size(dir.file("journal.bin")), log_size(7));
}

TEST(RecordLog, AZeroStrideIsRefused) {
	const scratch_dir dir("stride_zero");
	auto log = raw_record_log::open_for_append(dir.file("journal.bin"), 0);
	EXPECT_FALSE(log.has_value());
}

// A file that is not one of these logs must be refused rather than read as
// records. Before the header there was nothing to refuse it *with*: any file at
// all was a valid log of however many records its length divided into, so
// pointing recovery at the wrong path replayed whatever was there.
TEST(RecordLog, AFileThatIsNotALogIsRefused) {
	const scratch_dir dir("not_a_log");
	const auto path = dir.file("something_else.bin");

	// Long enough to hold a header, so it is only the magic that rejects it.
	append_raw_bytes(path, LOG_HEADER_SIZE * 2);

	auto reader = record_log<sample>::open_for_read(path);
	ASSERT_FALSE(reader.has_value()) << "read a foreign file as a log";
	EXPECT_NE(reader.error().find("magic"), std::string::npos)
		<< reader.error();

	// And it is not silently converted into one by opening it for append, which
	// would destroy whatever the file actually was.
	auto writer = record_log<sample>::open_for_append(path);
	ASSERT_FALSE(writer.has_value()) << "adopted a foreign file as a log";
	EXPECT_EQ(std::filesystem::file_size(path), LOG_HEADER_SIZE * 2)
		<< "a refused file was modified anyway"; 
}

// A log shorter than a header cannot hold a record, so there is nothing to lose
// by discarding it - which is what makes a crash between creating the file and
// writing its header recoverable rather than fatal.
TEST(RecordLog, AStubTooShortToHoldAHeaderIsStartedOver) {
	const scratch_dir dir("stub");
	const auto path = dir.file("journal.bin");
	append_raw_bytes(path, LOG_HEADER_SIZE / 2);

	auto log = record_log<sample>::open_for_append(path);
	ASSERT_TRUE(log.has_value()) << log.error();
	EXPECT_EQ(log->count(), 0U);
	ASSERT_TRUE(log->append(samples(2)));
	ASSERT_TRUE(log->sync());
	EXPECT_EQ(log->read_from(0), samples(2));
}

// The sharp one. A record type that gains a field changes the stride, and every
// log written before that change then reads back as plausible nonsense: the
// records are the wrong width, so every field of every one of them is wrong
// while the file still divides evenly and reports a sensible count. The stride
// in the header is what turns that into a refusal.
TEST(RecordLog, ALogWrittenForADifferentStrideIsRefused) {
	const scratch_dir dir("stride_change");
	const auto path = dir.file("journal.bin");

	{
		auto log = record_log<sample>::open_for_append(path);
		ASSERT_TRUE(log.has_value()) << log.error();
		ASSERT_TRUE(log->append(samples(8)));
		ASSERT_TRUE(log->sync());
	}

	// The same file, read back by a build whose record grew by eight bytes.
	auto grown =
		raw_record_log::open_for_read(path,
									  sizeof(sample) + sizeof(std::uint64_t));
	ASSERT_FALSE(grown.has_value()) << "read a log back into the wrong record";
	EXPECT_NE(grown.error().find("layout changed"), std::string::npos)
		<< grown.error();
}

// Corruption *within* a record - the failure the fixed stride was documented as
// unable to see, because the file is still exactly the right length. This is
// the suite that would have to fail if the checksum were removed.
TEST(RecordLog, ACorruptedRecordIsCaughtAndNamed) {
	const scratch_dir dir("corrupt");
	const auto path                   = dir.file("journal.bin");
	const std::vector<sample> written = samples(10);

	{
		auto log = record_log<sample>::open_for_append(path);
		ASSERT_TRUE(log.has_value()) << log.error();
		ASSERT_TRUE(log->append(written));
		ASSERT_TRUE(log->sync());
	}

	// A bit rots inside record six, leaving the file's length untouched.
	constexpr std::uint64_t VICTIM = 6;
	corrupt_byte_at(path,
					LOG_HEADER_SIZE +
						(VICTIM * record_log<sample>::ONDISK_STRIDE));
	ASSERT_EQ(std::filesystem::file_size(path), log_size(written.size()))
		<< "the corruption changed the file's length, so the size check would "
		   "have caught it and this suite would prove nothing";

	auto reader = record_log<sample>::open_for_read(path);
	ASSERT_TRUE(reader.has_value()) << reader.error();
	EXPECT_EQ(reader->count(), written.size());
	ASSERT_FALSE(reader->corrupt_record().has_value());

	// The prefix is genuine and still comes back - a recovery that threw away
	// history it could have replayed would be the wrong answer to one bad
	// record.
	const std::vector<sample> readable = reader->read_from(0);
	ASSERT_EQ(readable.size(), VICTIM);
	EXPECT_TRUE(std::equal(readable.begin(), readable.end(), written.begin()));

	// And the log says exactly where it stopped trusting itself, because that
	// is what decides whether the damage is in a tail recovery can abandon.
	EXPECT_FALSE(reader->is_good());
	ASSERT_TRUE(reader->corrupt_record().has_value());
	EXPECT_EQ(*reader->corrupt_record(), VICTIM);
}

// Damage to the checksum rather than the payload has to be caught too: the two
// are indistinguishable from inside the log, and treating a bad checksum over
// good bytes as readable would make the check optional in practice.
TEST(RecordLog, ACorruptedChecksumIsAlsoCaught) {
	const scratch_dir dir("corrupt_sum");
	const auto path = dir.file("journal.bin");

	{
		auto log = record_log<sample>::open_for_append(path);
		ASSERT_TRUE(log.has_value()) << log.error();
		ASSERT_TRUE(log->append(samples(4)));
		ASSERT_TRUE(log->sync());
	}

	// The last byte of record two's frame is the top byte of its checksum.
	corrupt_byte_at(path,
					LOG_HEADER_SIZE + (3 * record_log<sample>::ONDISK_STRIDE) -
						1);

	auto reader = record_log<sample>::open_for_read(path);
	ASSERT_TRUE(reader.has_value()) << reader.error();
	EXPECT_EQ(reader->read_from(0).size(), 2U);
	ASSERT_TRUE(reader->corrupt_record().has_value());
	EXPECT_EQ(*reader->corrupt_record(), 2U);
}

// A corrupt header is a different failure from a corrupt record and has to be
// refused at open, not at the first read: every offset a reader would compute
// comes from the header, so a reader that trusted a damaged one would be
// reading the wrong bytes and checksumming them against the wrong expectation.
TEST(RecordLog, ACorruptedHeaderIsRefusedAtOpen) {
	const scratch_dir dir("corrupt_header");
	const auto path = dir.file("journal.bin");

	{
		auto log = record_log<sample>::open_for_append(path);
		ASSERT_TRUE(log.has_value()) << log.error();
		ASSERT_TRUE(log->append(samples(4)));
		ASSERT_TRUE(log->sync());
	}

	// The stride field, past the magic - so the magic still matches and it is
	// the header's own checksum that has to notice.
	corrupt_byte_at(path, 12);

	auto reader = record_log<sample>::open_for_read(path);
	ASSERT_FALSE(reader.has_value()) << "opened a log on a corrupt header";
	EXPECT_NE(reader.error().find("corrupt"), std::string::npos)
		<< reader.error();
}

// Reserving is a performance affordance, not a semantic one: it decides how
// many fwrites a batch takes and nothing else. A batch larger than the
// reservation still has to come back identical, because that is the path a
// caller who never reserved takes on every append.
TEST(RecordLog, ReservingChangesNothingAboutWhatIsWritten) {
	const scratch_dir dir("reserve");
	const std::vector<sample> written = samples(300);

	auto reserved =
		record_log<sample>::open_for_append(dir.file("reserved.bin"));
	ASSERT_TRUE(reserved.has_value()) << reserved.error();
	reserved->reserve(written.size());
	ASSERT_TRUE(reserved->append(written));
	ASSERT_TRUE(reserved->sync());

	// No reservation at all, so this batch is framed in several chunks.
	auto chunked = record_log<sample>::open_for_append(dir.file("chunked.bin"));
	ASSERT_TRUE(chunked.has_value()) << chunked.error();
	ASSERT_TRUE(chunked->append(written));
	ASSERT_TRUE(chunked->sync());

	EXPECT_EQ(reserved->read_from(0), written);
	EXPECT_EQ(chunked->read_from(0), written);
	EXPECT_EQ(std::filesystem::file_size(dir.file("reserved.bin")),
			  std::filesystem::file_size(dir.file("chunked.bin")));
}

} // namespace
