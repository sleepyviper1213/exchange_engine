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

#include "core/util/owned_file.hpp"
#include "core/util/start_lifetime_as.hpp"
#include "core_export.hpp" // CORE_EXPORT (generated)

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <memory>
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
 * @brief The untyped half: a file of fixed-stride records, and nothing else.
 *
 * Split out from @c record_log so the file handling is compiled once into
 * @c core rather than once per record type. Every offset it computes is in
 * *records*, not bytes - the stride is fixed at construction and the arithmetic
 * belongs here rather than at each call site.
 *
 * @par Why there is no framing, no length prefix and no checksum
 * Because a fixed-width trivially copyable record makes the file an array on
 * disk, and an array needs no framing to be walked. That is not laziness, it is
 * the property being bought: appending is a @c fwrite of the object
 * representation with no encode step, and reading record @e n is a seek to
 * <code>n * stride</code> rather than a walk from the start.
 *
 * The one failure a fixed stride cannot absorb is a *partial* final record -
 * the process died between the write starting and finishing - and that one it
 * detects exactly, because a file whose size is not a whole number of records
 * has a torn tail by arithmetic rather than by guess. @c open_for_append
 * truncates it, @c open_for_read ignores it.
 *
 * @warning What it deliberately does not detect is corruption *within* a
 * record. Bit rot, a bad sector, a partially-flushed page that still lands on a
 *          record boundary - all read back as a valid record holding wrong
 *          values. A checksum per record would catch those and is the right
 *          thing to add the day this log outlives the machine that wrote it. It
 *          is not here because the log's job today is recovery on the same host
 *          after a process crash, which is exactly the failure the size check
 *          already covers.
 *
 * @warning The file holds the host's object representation, so it is **not**
 *          portable across architectures or across a change to the record's
 *          layout. It is a recovery log, not an interchange format; a reader
 *          built for a different endianness or a different @c sizeof will read
 *          nonsense with no complaint. The type's own @c static_assert on its
 *          size (@c order_record has one) is what makes a layout change loud.
 */
class raw_record_log {
public:
	/**
	 * @brief Open @p path for appending, creating it if absent.
	 * @param path The log file.
	 * @param stride Bytes per record; must be positive.
	 * @return The log, or why it could not be opened.
	 * @post Any torn tail has been truncated away, so the file is a whole
	 *       number of records and the next append lands on a boundary.
	 */
	[[nodiscard]] CORE_EXPORT static std::expected<raw_record_log, std::string>
	open_for_append(const std::filesystem::path &path, std::size_t stride);

	/**
	 * @brief Open @p path for reading. The file must exist.
	 * @return The log, or why it could not be opened.
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
	 * @brief Append @p count records from @p data.
	 * @return @c false if the write failed; the log is then poisoned and every
	 *         later append fails too, because a log with a hole in it is worse
	 *         than one that stopped.
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
	 * @param[out] out Destination for <code>count * stride</code> bytes.
	 * @return How many whole records were read, which is short at end of file.
	 */
	[[nodiscard]] CORE_EXPORT std::size_t read_at(std::uint64_t from, void *out,
												  std::size_t count);

	/// @brief Whether every operation so far has succeeded.
	[[nodiscard]] CORE_EXPORT bool is_good() const noexcept;

	/// @brief The path this log was opened on.
	[[nodiscard]] CORE_EXPORT const std::filesystem::path &
	path() const noexcept;

private:
	raw_record_log(util::owned_file file, std::filesystem::path path,
				   std::size_t stride, std::uint64_t count) noexcept;

	util::owned_file file_;
	std::filesystem::path path_;
	std::size_t stride_  = 0;
	std::uint64_t count_ = 0;
	bool good_           = true;
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

	/// @brief Bytes one record occupies on disk.
	static constexpr std::size_t STRIDE = sizeof(T);

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
	read_into(std::uint64_t from, void *storage, std::size_t capacity) {
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
