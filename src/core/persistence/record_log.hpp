#pragma once
// Append-only log of fixed-size records - the durable half of "deterministic is
// only worth it if you can recover".
//
// The engine is deterministic by construction: the same command stream always
// produces the same books and the same trades. That property is what makes a
// log of *inputs* sufficient - there is no need to record what happened, only
// what was asked, because replaying the asks reproduces the happening. This is
// that log.
//
// It knows nothing about commands, orders or books, and it must not: the
// dependency graph runs Event -> Execution -> Persistence -> Util, so naming a
// command here would point an edge back up it. The record type is a template
// parameter supplied by whoever is above, which costs nothing and is what lets
// the same file hold a command journal in one deployment and a book snapshot in
// the next.

#include "core/util/attributes.hpp"
#include "core/util/owned_file.hpp"
#ifndef __cpp_lib_start_lifetime_as
#include "core/util/start_lifetime_as.hpp"
#endif
#include "core_export.hpp" // CORE_EXPORT (generated)

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace exchange::core::persistence {

/**
 * @brief Force an already-written file's contents onto the device.
 *
 * The durability half of a write, for the files here that are not record logs -
 * a manifest, a snapshot written through an @c ofstream. It exists because
 * @c std::ofstream has no portable way to reach the file descriptor underneath
 * it, and so no way to do anything stronger than flushing into the operating
 * system's cache; surviving a machine crash needs the platform's file-sync call
 * on that descriptor, which is what this reopens the file to make.
 *
 * @param path An existing file, opened read/write for the duration.
 * @return @c false if it could not be opened or the sync failed.
 * @note Not for the record log's own writes - @c raw_record_log::sync does this
 *       on the handle it already holds, without the reopen.
 */
[[nodiscard]] CORE_EXPORT bool sync_file(const std::filesystem::path &path);

/**
 * @brief Bytes of preamble before the first record of every log.
 *
 * 64 rather than the 24 the fields actually need, so a later version can add
 * one without moving anything a reader already knows the offset of. A log is
 * the file you are least able to migrate - it is what you have left when the
 * process is gone - so the room is worth more here than the bytes are.
 */
inline constexpr std::size_t LOG_HEADER_SIZE = 64;

/// @brief Bytes of CRC32C following each record's payload on disk.
inline constexpr std::size_t RECORD_CHECKSUM_SIZE = sizeof(std::uint32_t);

/**
 * @brief The on-disk format this build writes, and the only one it reads.
 *
 * Bumped when the framing changes, never for a change to what a record *means*
 * - a record's own layout is guarded by the stride in the header instead, which
 * catches it without anyone having to remember to bump anything.
 */
inline constexpr std::uint32_t LOG_FORMAT_VERSION = 1;

/**
 * @brief The untyped half: a file of fixed-stride records, and nothing else.
 *
 * Split out from @c record_log so the file handling is compiled once into
 * @c core rather than once per record type. Every offset it computes is in
 * *records*, not bytes - the stride is fixed at construction and the arithmetic
 * belongs here rather than at each call site.
 *
 * @par The shape on disk: a header, then fixed-width framed records
 * A @c LOG_HEADER_SIZE preamble states a magic, the format version and the
 * record stride; every record after it is its payload followed by a CRC32C of
 * that payload (@c RECORD_CHECKSUM_SIZE bytes). The stride stays fixed, so
 * record @e n is a seek to <code>LOG_HEADER_SIZE + n * frame_stride</code>
 * rather than a walk from the start - which is the property
 * @c manifest::sequence leans on, since it counts records and is then used as
 * an offset.
 *
 * @par What each of the three parts catches
 * Different failures, and none of them decoration:
 *
 * - **The magic** separates a log from any other file handed to it. Without it,
 *   aiming recovery at the wrong path reads whatever was there as records.
 * - **The version and stride** catch a *layout* change, which is the sharp one:
 *   adding a field to a journalled record silently changes its @c sizeof, and
 *   every log written before that change then reads back as plausible nonsense.
 *   The header turns that into a refusal at open time, naming both strides.
 * - **The per-record checksum** catches corruption *within* a record - bit rot,
 *   a bad sector, a partially-flushed page that still lands on a record
 *   boundary. A fixed stride detects a torn *tail* by arithmetic and nothing
 *   else, so these were previously undetectable by construction. A record that
 *   fails its checksum poisons the log, and @c corrupt_record says which one.
 *
 * The failure the stride still absorbs by itself is a *partial* final record -
 * the process died between the write starting and finishing - which it detects
 * exactly, because a file whose size is not a whole number of frames has a torn
 * tail by arithmetic rather than by guess. @c open_for_append truncates it,
 * @c open_for_read ignores it.
 *
 * @warning A log written before @c LOG_FORMAT_VERSION existed has no header, so
 *          it is refused rather than read. That is the intended outcome - the
 *          alternative is reading its first record as a header and everything
 *          after it off by 64 bytes - but it does mean this is a breaking
 * change to files on disk, not just to this API. @see docs/recovery.md
 *
 * @warning Whether the file is portable is the *record type's* business, not
 *          this class's. This writes whatever @p T is as @p T's object
 *          representation, which for most types means the compiler's layout and
 *          the machine's byte order - so a reader elsewhere reads nonsense.
 * What this class contributes either way is the stride in the header, which
 *          refuses a build whose @c sizeof disagrees rather than misreading it.
 *          A type that needs more supplies its own defined layout and hands one
 *          over: @c event::journal_record is 40 bytes of fields at documented
 *          offsets in little-endian, so the journal is portable while the
 *          snapshot beside it - raw @c resting_record - is not, through exactly
 *          the same code here.
 */
class raw_record_log {
public:
	/**
	 * @brief Bytes one record occupies on disk, payload and checksum together.
	 * @param stride The payload's size.
	 */
	[[nodiscard]] static constexpr std::size_t
	frame_stride(std::size_t stride) noexcept {
		return stride + RECORD_CHECKSUM_SIZE;
	}

	/**
	 * @brief Open @p path for appending, creating it if absent.
	 * @param path The log file.
	 * @param stride Bytes per record payload; must be positive.
	 * @return The log, or why it could not be opened - which includes a file
	 *         whose header is absent, unrecognised, of another format version,
	 * or written for a different @p stride.
	 * @post The file begins with a valid header: written if the file was empty,
	 *       validated against @p stride if it was not.
	 * @post Any torn tail has been truncated away, so the file is a whole
	 *       number of frames and the next append lands on a boundary.
	 */
	[[nodiscard]] CORE_EXPORT static std::expected<raw_record_log, std::string>
	open_for_append(const std::filesystem::path &path, std::size_t stride);

	/**
	 * @brief Open @p path for reading. The file must exist.
	 * @return The log, or why it could not be opened. @copydetails
	 * open_for_append
	 * @note A torn tail is excluded from @c count rather than truncated -
	 *       a reader has no business editing the file it is recovering from,
	 *       and a second reader must see the same thing this one did.
	 */
	[[nodiscard]] CORE_EXPORT static std::expected<raw_record_log, std::string>
	open_for_read(const std::filesystem::path &path, std::size_t stride);

	raw_record_log(const raw_record_log &)            = delete;
	raw_record_log &operator=(const raw_record_log &) = delete;
	CORE_EXPORT raw_record_log(raw_record_log &&) noexcept;
	CORE_EXPORT raw_record_log &operator=(raw_record_log &&) noexcept;
	CORE_EXPORT ~raw_record_log();

	/**
	 * @brief Make room to frame @p records in one write, allocating if needed.
	 *
	 * @c append has to interleave each payload with its checksum somewhere
	 * before it can issue a single @c fwrite, and that somewhere is a buffer
	 * this owns. Calling this once, off the hot path, is what keeps @c append
	 * allocation-free for batches up to @p records - which matters because the
	 * caller on the matching path may not allocate at all.
	 *
	 * @param records The largest batch @c append will be given.
	 * @note Never shrinks, and never required: an @c append larger than the
	 * buffer frames the batch in as many chunks as it takes, at the cost of one
	 *       @c fwrite per chunk instead of one for the batch. It stays correct
	 * and allocation-free either way; only the write count changes.
	 */
	CORE_EXPORT void reserve(std::size_t records);

	/**
	 * @brief Append @p count records from @p data.
	 * @return @c false if the write failed; the log is then poisoned and every
	 *         later append fails too, because a log with a hole in it is worse
	 *         than one that stopped.
	 * @note Each payload is checksummed and framed on the way out. That is a
	 *       @c memcpy and a CRC32C per record against a buffer, not a syscall
	 * per record - the batch still leaves in one @c fwrite when @c reserve has
	 *       been called for it.
	 * @note Buffered. This does **not** make anything durable - see @c sync,
	 *       and the group-commit note on @c record_log.
	 */
	[[nodiscard]] CORE_EXPORT bool append(const void *data, std::size_t count);

	/**
	 * @brief Force everything appended so far all the way to the device.
	 *
	 * Both halves matter and they are different syscalls: @c fflush moves the C
	 * library's buffer into the operating system, and only the platform's
	 * file-sync call moves the operating system's cache onto the disk. Doing
	 * just the first survives a *process* crash and loses data to a *machine*
	 * crash, which is precisely the distinction a durability barrier exists to
	 * make, so both are done here.
	 *
	 * @return @c false if either step failed.
	 */
	[[nodiscard]] CORE_EXPORT bool sync();

	/// @brief Whole records currently in the file. A torn tail is not counted.
	[[nodiscard]] CORE_EXPORT std::uint64_t count() const noexcept;

	/**
	 * @brief Read up to @p count records starting at record @p from.
	 *
	 * @param[out] out Destination for <code>count * stride</code> bytes - the
	 *        payloads only. Checksums are verified and stripped on the way in,
	 * so a caller sees the same bytes it appended and never the framing.
	 * @return How many whole records were read, which is short at end of file
	 * and short at the first record that fails its checksum.
	 * @post A checksum failure has poisoned the log and set @c corrupt_record.
	 *       Records read *before* it are still returned, because they verified.
	 */
	[[nodiscard]] CORE_EXPORT std::size_t read_at(std::uint64_t from, void *out,
												  std::size_t count);

	/// @brief Whether every operation so far has succeeded.
	[[nodiscard]] CORE_EXPORT bool is_good() const noexcept;

	/**
	 * @brief Which record failed its checksum, if one has.
	 *
	 * An index rather than a flag, because the number is what an operator does
	 * something with: it says how far a replay got before the log stopped being
	 * trustworthy, and therefore whether the damage is in the tail a recovery
	 * can abandon or in the middle of history it cannot.
	 *
	 * @return The record's index, or empty if no checksum has failed. Only the
	 *         first is kept - once one record is wrong the log is poisoned and
	 *         nothing further is read, so there is no second to record.
	 */
	[[nodiscard]] CORE_EXPORT std::optional<std::uint64_t>
	corrupt_record() const noexcept;

	/// @brief The path this log was opened on.
	[[nodiscard]] CORE_EXPORT const std::filesystem::path &
	path() const noexcept;

private:
	raw_record_log(util::owned_file file, std::filesystem::path path,
				   std::size_t stride, std::uint64_t count);

	/// @brief Frame up to @p count records into @c frame_ and write them.
	/// @return Records written, short only on a failed write.
	std::size_t write_framed(const std::byte *payloads, std::size_t count);

	util::owned_file file_;
	std::filesystem::path path_;
	std::size_t stride_  = 0;
	std::uint64_t count_ = 0;
	bool good_           = true;

	/// @brief Where payloads and checksums are interleaved before a write, and
	///        where frames are verified after a read. One buffer for both: they
	///        never overlap, since a log is written or read by one thread at a
	///        time and neither operation reenters the other.
	std::vector<std::byte> frame_;

	/// @brief Index of the first record that failed its checksum. @see
	/// corrupt_record
	std::uint64_t corrupt_at_ = 0;
	bool corrupt_             = false;
};

/**
 * @brief A typed append-only log of @p T.
 *
 * A thin facade over @c raw_record_log: it supplies the stride, converts
 * records to and from bytes, and adds nothing else. All the reasoning about
 * framing, torn tails and portability is on @c raw_record_log and applies
 * unchanged.
 *
 * @tparam T The record. Must be trivially copyable - this writes the object
 *         representation, so a type with a pointer, a vtable or an owning
 * 		   member would be written as an address that means nothing on the way
 * 		   back.
 *
 * @par Group commit, and why @c append does not make anything durable
 * A durability barrier is a device round trip: hundreds of microseconds against
 * a matching path budgeted in nanoseconds. Syncing per record would put that on
 * the path once per command, which is not a slow engine, it is a broken one. So
 * @c append is a buffered write and @c sync is a separate call the caller
 * places, once per batch, at the point durability is actually required.
 *
 * For the engine that point is exact and worth stating: **before the batch's
 * trades are published**, never after. A trade seen by a client whose command
 * is not yet on disk is a trade the venue may forget it made, which is the one
 * ordering an event-sourced design must not get wrong.
 *
 * @code
 * auto journal = record_log<command>::open_for_append("journal.bin");
 * for (const command &cmd : batch) journal->append(cmd);  // buffered
 * journal->sync();                                        // durable
 * partition.flush();                                      // now publish
 * @endcode
 *
 * @par Threading
 * One log, one thread. It holds a buffered file handle and a record count, and
 * synchronises neither. In the engine that thread is the partition's consumer,
 * which is also the only thread that has commands to journal.
 */
template <class T>
class record_log {
public:
	static_assert(
		std::is_trivially_copyable_v<T>,
		"a journalled record is written as its object representation, "
		"so it must be trivially copyable");

	/// @brief Bytes one record's payload occupies - the record itself.
	static constexpr std::size_t STRIDE = sizeof(T);

	/// @brief Bytes one record occupies on disk, its checksum included.
	static constexpr std::size_t ONDISK_STRIDE =
		raw_record_log::frame_stride(STRIDE);

	/// @brief Open @p path for appending, creating it if absent.
	/// @copydetails raw_record_log::open_for_append
	[[nodiscard]] static std::expected<record_log, std::string>
	open_for_append(const std::filesystem::path &path) {
		return raw_record_log::open_for_append(path, STRIDE)
			.transform([](raw_record_log &&raw) {
				return record_log(std::move(raw));
			});
	}

	/// @brief Open @p path for reading. The file must exist.
	[[nodiscard]] static std::expected<record_log, std::string>
	open_for_read(const std::filesystem::path &path) {
		return raw_record_log::open_for_read(path, STRIDE)
			.transform([](raw_record_log &&raw) {
				return record_log(std::move(raw));
			});
	}

	/// @brief Make room to frame @p records in one write.
	/// @copydetails raw_record_log::reserve
	void reserve(std::size_t records) { raw_.reserve(records); }

	/// @brief Append one record. Buffered; see the class note on group commit.
	[[nodiscard]] bool append(const T &record) {
		return raw_.append(&record, 1);
	}

	/// @brief Append a whole batch in one write.
	[[nodiscard]] bool append(std::span<const T> records) {
		if (records.empty()) return raw_.is_good();
		return raw_.append(records.data(), records.size());
	}

	/// @brief The durability barrier. @see raw_record_log::sync
	[[nodiscard]] bool sync() { return raw_.sync(); }

	/// @brief Which record failed its checksum, if one has.
	/// @copydetails raw_record_log::corrupt_record
	[[nodiscard]] std::optional<std::uint64_t> corrupt_record() const noexcept {
		return raw_.corrupt_record();
	}

	/// @brief Records currently in the file, torn tail excluded.
	[[nodiscard]] std::uint64_t count() const noexcept { return raw_.count(); }

	/// @brief Read up to @c out.size() records starting at record @p from.
	/// @return How many were read; short at end of file.
	[[nodiscard]] std::size_t read_at(std::uint64_t from, std::span<T> out) {
		if (out.empty()) return 0;
		return raw_.read_at(from, out.data(), out.size());
	}

	/**
	 * @brief Read up to @p capacity records from @p from into raw @p storage.
	 *
	 * The read that works for a @p T with no default constructor - which is the
	 * one that matters, because @c event::command deliberately has none, so
	 * that a tagged union can never exist with a tag its payload does not
	 * match. A caller cannot hand over a @c span<T> it was unable to construct,
	 * so it hands over bytes instead and gets back a view of the records now
	 * living in them.
	 *
	 * @param from First record to read.
	 * @param storage At least <code>capacity * sizeof(T)</code> bytes, aligned
	 *        for @p T.
	 * @param capacity How many records @p storage has room for.
	 * @return A view of the records read, empty at end of file. Valid until
	 *         @p storage is reused.
	 */
	[[nodiscard]] std::span<const T>
	read_into(std::uint64_t from, void *storage EXCHANGE_LIFETIMEBOUND,
			  std::size_t capacity) {
		const std::size_t read = raw_.read_at(from, storage, capacity);
		if (read == 0) return {};
		// The bytes are a T's object representation by construction - this is
		// the same file the same type was written to - so beginning their
		// lifetimes here is the sanctioned reinterpretation rather than a bare
		// reinterpret_cast. @see core/util/start_lifetime_as.hpp
#ifdef __cpp_lib_start_lifetime_as
		const T *items = std::start_lifetime_as_array<T>(storage, read);
#else
		const T *items = util::start_lifetime_as_array<T>(storage, read);
#endif
		return {items, read};
	}

	/**
	 * @brief Everything from record @p from to the end, in order.
	 *
	 * The convenience a test or a small recovery wants, and the one place here
	 * that allocates: a vector sized from the record count. Anything reading a
	 * journal that might not fit in memory wants @c replay, which streams
	 * through a fixed buffer instead.
	 */
	[[nodiscard]] std::vector<T> read_from(std::uint64_t from) {
		std::vector<T> records;
		const std::uint64_t total = count();
		if (from >= total) return records;
		records.reserve(static_cast<std::size_t>(total - from));

		// A chunk at a time rather than one buffer the size of the log, and
		// *copied* into the vector rather than resized into: only the copy
		// constructor is needed that way, which trivial copyability already
		// guarantees, where a resize would have demanded a default constructor.
		constexpr std::size_t CHUNK = 64;
		alignas(T) std::array<std::byte, sizeof(T) * CHUNK> storage{};

		for (std::uint64_t at = from; at < total;) {
			const std::span<const T> batch =
				read_into(at, storage.data(), CHUNK);
			if (batch.empty()) break;
			records.append_range(batch);
			at += batch.size();
		}
		return records;
	}

	/// @brief Whether every operation so far has succeeded.
	[[nodiscard]] bool is_good() const noexcept { return raw_.is_good(); }

	/// @brief The path this log was opened on.
	[[nodiscard]] const std::filesystem::path &path() const noexcept {
		return raw_.path();
	}

private:
	explicit record_log(raw_record_log &&raw) noexcept : raw_(std::move(raw)) {}

	raw_record_log raw_;
};

} // namespace exchange::core::persistence
