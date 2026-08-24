#include "record_log.hpp"

#include <crc32c/crc32c.h>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <bit> // bit_cast, byteswap, endian - the header's byte order
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>


#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace exchange::core::persistence {
namespace {

/// @brief The C library's reason for the last failure, as text.
///
/// @c errno rather than @c std::filesystem's error codes because every call
/// below is a stdio call, and stdio reports through errno. Mixing the two would
/// give two vocabularies for one kind of failure.
std::string last_error() { return std::generic_category().message(errno); }

/// @brief One shape for every failure here: what went wrong, on which file, and
///        the platform's reason. A recovery failure is read by whoever is
///        trying to get a venue back up, so the path is never the part left
///        out.
std::string describe(const std::filesystem::path &path, std::string_view what,
					 const std::string &why) {
	return fmt::format("{} {}: {}", what, path.string(), why);
}

/// @brief Move the OS's cache for @p file onto the device.
///
/// The half @c fflush does not do. Split by platform because there is no
/// portable spelling: POSIX has @c fsync on a file descriptor, Windows has
/// @c _commit on the CRT's equivalent. Both take the descriptor underneath the
/// @c FILE*, which is why this log is built on stdio rather than @c fstream -
/// an @c std::ofstream has no portable way to reach the descriptor, and so no
/// way to offer a durability barrier that survives losing the machine.
bool sync_to_device(std::FILE *file) noexcept {
#if defined(_WIN32)
	const int descriptor = ::_fileno(file);
	if (descriptor < 0) return false;
	return ::_commit(descriptor) == 0;
#else
	const int descriptor = ::fileno(file);
	if (descriptor < 0) return false;
	return ::fsync(descriptor) == 0;
#endif
}

// ---------------------------------------------------------------- the header

/**
 * @brief What every log file starts with, so a reader can tell that it is one.
 *
 * The two control bytes are deliberate and borrowed from PNG: 0x1A is a DOS
 * end-of-file and 0x0A a bare newline, so a file that has been through a
 * text-mode handle or a line-ending conversion no longer matches. That is worth
 * catching explicitly, because it is the corruption most likely to leave a file
 * that still *looks* plausible.
 */
constexpr std::array<std::byte, 8> MAGIC = {std::byte{'E'},
											std::byte{'X'},
											std::byte{'L'},
											std::byte{'O'},
											std::byte{'G'},
											std::byte{0x1A},
											std::byte{0x0A},
											std::byte{0x00}};

// Field offsets within the header. Spelled out rather than derived from a
// struct because this is a file format: the numbers are the contract, and a
// compiler is not entitled to an opinion about them.
constexpr std::size_t OFFSET_MAGIC          = 0;
constexpr std::size_t OFFSET_VERSION        = 8;
constexpr std::size_t OFFSET_PAYLOAD_STRIDE = 12;
constexpr std::size_t OFFSET_FRAME_STRIDE   = 16;
constexpr std::size_t OFFSET_HEADER_SUM     = 20;
/// @brief Bytes the header's own checksum covers - everything before it.
constexpr std::size_t HEADER_SUMMED = OFFSET_HEADER_SUM;

/// @brief Store @p value at @p at, little-endian.
///
/// Explicit rather than a @c memcpy of the object representation, because the
/// header is the one part of the file that has to be readable by a host that
/// does not share this one's byte order - it is what tells such a host that it
/// cannot read the records. A header written in native order could not say so.
void store_le32(std::byte *at, std::uint32_t value) noexcept {
	if constexpr (std::endian::native == std::endian::big)
		value = std::byteswap(value);

	auto bytes = std::bit_cast<std::array<std::byte, 4>>(value);

	std::ranges::copy(bytes, at);
}

/// @brief Load a little-endian @c uint32_t from @p at.
///
/// @c memcpy rather than a cast through a pointer to @c uint32_t or to an array
/// type: there is no object of either type at @p at, so dereferencing such a
/// pointer is undefined however reliably it happens to work. This is the
/// spelling that is both defined and free - every compiler here folds it to one
/// unaligned load.
[[nodiscard]] std::uint32_t load_le32(const std::byte *at) noexcept {
	std::uint32_t value = 0;
	std::memcpy(&value, at, sizeof value);
	if constexpr (std::endian::native == std::endian::big)
		return std::byteswap(value);
	return value;
}

/// @brief CRC32C of @p bytes bytes at @p at.
///
/// One place where the byte-pointer conversion happens, rather than at each of
/// the call sites. @c std::byte and @c std::uint8_t are both byte types, and
/// reading an object's representation through one is exactly what they are for,
/// so this is defined behaviour rather than the reinterpretation the project's
/// style rules are about - those are about looking at a byte buffer as some
/// *other* type, which is what @c start_lifetime_as exists for.
[[nodiscard]] std::uint32_t checksum(const std::byte *at,
									 std::size_t bytes) noexcept {
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
	return ::crc32c::Crc32c(reinterpret_cast<const std::uint8_t *>(at), bytes);
}

/// @brief The header a fresh log of @p stride gets.
[[nodiscard]] std::array<std::byte, LOG_HEADER_SIZE>
build_header(std::size_t stride) noexcept {
	// Zeroed, so every reserved byte is zero rather than whatever was on the
	// stack - a reserved field only stays usable if it starts out predictable.
	std::array<std::byte, LOG_HEADER_SIZE> header{};
	std::ranges::copy(MAGIC, header.begin() + OFFSET_MAGIC);
	store_le32(header.data() + OFFSET_VERSION, LOG_FORMAT_VERSION);
	store_le32(header.data() + OFFSET_PAYLOAD_STRIDE,
			   static_cast<std::uint32_t>(stride));
	store_le32(
		header.data() + OFFSET_FRAME_STRIDE,
		static_cast<std::uint32_t>(raw_record_log::frame_stride(stride)));
	store_le32(header.data() + OFFSET_HEADER_SUM,
			   checksum(header.data(), HEADER_SUMMED));
	return header;
}

/**
 * @brief Whether @p header describes a log of @p stride that this build can
 * read.
 *
 * Every check reports what was expected against what was found. A recovery that
 * refuses to start is read by somebody who has to decide what to do next, and
 * "unsupported format version 2, this build reads 1" tells them; "bad header"
 * does not.
 */
[[nodiscard]] std::expected<void, std::string>
validate_header(std::span<const std::byte, LOG_HEADER_SIZE> header,
				std::size_t stride) {
	if (!std::ranges::equal(header.first(MAGIC.size()), MAGIC))
		return std::unexpected(
			std::string("not a record log: the file does not start with one's "
						"magic. A log written before the format carried a "
						"header reads this way, and cannot be upgraded in "
						"place"));

	if (const std::uint32_t sum = load_le32(header.data() + OFFSET_HEADER_SUM);
		sum != checksum(header.data(), HEADER_SUMMED))
		return std::unexpected(fmt::format(
			"the log header is corrupt: checksum {:#010x} does not match the "
			"{:#010x} its own fields produce",
			sum,
			checksum(header.data(), HEADER_SUMMED)));

	if (const std::uint32_t version = load_le32(header.data() + OFFSET_VERSION);
		version != LOG_FORMAT_VERSION)
		return std::unexpected(fmt::format(
			"unsupported log format version {}, this build reads {}",
			version,
			LOG_FORMAT_VERSION));

	// The check that catches a record type whose layout moved. It is the whole
	// reason the stride is in the file rather than assumed from the caller.
	if (const std::uint32_t written =
			load_le32(header.data() + OFFSET_PAYLOAD_STRIDE);
		written != stride)
		return std::unexpected(fmt::format(
			"the log holds {}-byte records but this build reads {}-byte ones - "
			"the record type's layout changed, so the file cannot be read back "
			"into it",
			written,
			stride));

	if (const std::uint32_t frame =
			load_le32(header.data() + OFFSET_FRAME_STRIDE);
		frame != raw_record_log::frame_stride(stride))
		return std::unexpected(
			fmt::format("the log frames records at {} bytes but this build "
						"frames them at {}",
						frame,
						raw_record_log::frame_stride(stride)));

	return {};
}

/// @brief Read the header off @p path without disturbing anything.
[[nodiscard]] std::expected<std::array<std::byte, LOG_HEADER_SIZE>, std::string>
read_header(const std::filesystem::path &path) {
	const util::owned_file file = util::open_shared(path, "rb");
	if (file == nullptr)
		return std::unexpected(describe(path, "cannot open log", last_error()));

	std::array<std::byte, LOG_HEADER_SIZE> header{};
	if (std::fread(header.data(), 1, header.size(), file.get()) !=
		header.size())
		return std::unexpected(
			describe(path, "cannot read the log header", last_error()));
	return header;
}

/**
 * @brief Validate an existing log's header and trim any torn tail off it.
 *
 * @param path The log, known to be at least @c LOG_HEADER_SIZE bytes.
 * @param size Its size, taken before anything opened it.
 * @param stride The payload stride the caller expects.
 * @return How many whole records it holds, or why it cannot be opened.
 *
 * @note The order is not interchangeable: the header is validated *before*
 *       anything is truncated, because a torn tail is only a torn tail under a
 *       known stride. Trimming first would damage a file this build is about to
 *       discover it cannot read.
 */
[[nodiscard]] std::expected<std::uint64_t, std::string>
adopt_existing(const std::filesystem::path &path, std::uintmax_t size,
			   std::size_t stride) {
	auto header = read_header(path);
	if (!header) return std::unexpected(std::move(header.error()));
	if (const auto ok = validate_header(*header, stride); !ok)
		return std::unexpected(describe(path, "cannot open log", ok.error()));

	const std::size_t frame = raw_record_log::frame_stride(stride);
	const auto whole =
		static_cast<std::uint64_t>((size - LOG_HEADER_SIZE) / frame);
	const std::uintmax_t aligned =
		LOG_HEADER_SIZE + (static_cast<std::uintmax_t>(whole) * frame);

	// A torn tail: the previous process died part-way through a write. Dropping
	// it is the only option that leaves the file appendable - an append after a
	// partial record would put every later record off its boundary, turning one
	// lost command into an unreadable log.
	if (aligned != size) {
		std::error_code ec;
		std::filesystem::resize_file(path, aligned, ec);
		if (ec)
			return std::unexpected(
				describe(path, "cannot truncate a torn record", ec.message()));
	}
	return whole;
}

/**
 * @brief Empty a file too short to be a log, so a fresh header can be written.
 *
 * A file of fewer than @c LOG_HEADER_SIZE bytes cannot hold a record, so there
 * is nothing in it to lose - which is what makes discarding it safe rather than
 * destructive. A crash between creating the file and writing its header lands
 * here, and so does an absent file, which needs nothing done.
 */
[[nodiscard]] std::expected<void, std::string>
discard_stub(const std::filesystem::path &path, std::uintmax_t size) {
	if (size == 0) return {};

	std::error_code ec;
	std::filesystem::resize_file(path, 0, ec);
	if (ec)
		return std::unexpected(
			describe(path, "cannot discard a torn log header", ec.message()));
	return {};
}

/**
 * @brief Write a fresh header to @p file and get it into the file.
 *
 * Flushed here rather than left to the first @c sync, because until the header
 * is written the file is not a log: a crash in between would leave one that has
 * to be discarded rather than one that is merely empty.
 */
[[nodiscard]] std::expected<void, std::string>
write_header(const std::filesystem::path &path, std::FILE *file,
			 std::size_t stride) {
	const auto header = build_header(stride);
	if (std::fwrite(header.data(), 1, header.size(), file) != header.size() ||
		std::fflush(file) != 0)
		return std::unexpected(
			describe(path, "cannot write the log header", last_error()));
	return {};
}

/// @brief How many records the buffer is grown to hold when nobody said.
///
/// Enough that a single append is one write for any batch a test or a tool
/// produces, small enough that a log nobody reserved costs a few kilobytes. The
/// engine's partition reserves its queue capacity instead. @see
/// raw_record_log::reserve
constexpr std::size_t DEFAULT_FRAME_RECORDS = 64;

} // namespace

bool sync_file(const std::filesystem::path &path) {
	// "r+b": the file must already exist and must not be truncated, and the
	// write half is required because Windows will not commit a read-only
	// handle.
	// Owned rather than closed by hand: the early return below used to sit
	// between an open and its fclose, which is the shape that leaks a handle
	// the day somebody adds a second one.
	const util::owned_file file = util::open_shared(path, "r+b");
	if (file == nullptr) return false;
	return sync_to_device(file.get());
}

raw_record_log::raw_record_log(util::owned_file file,
							   std::filesystem::path path, std::size_t stride,
							   std::uint64_t count)
	: file_(std::move(file)),
	  path_(std::move(path)),
	  stride_(stride),
	  count_(count),
	  frame_(frame_stride(stride) * DEFAULT_FRAME_RECORDS) {}

raw_record_log::raw_record_log(raw_record_log &&) noexcept            = default;
raw_record_log &raw_record_log::operator=(raw_record_log &&) noexcept = default;
raw_record_log::~raw_record_log()                                     = default;

std::expected<raw_record_log, std::string>
raw_record_log::open_for_append(const std::filesystem::path &path,
								std::size_t stride) {
	if (stride == 0)
		return std::unexpected(std::string("record stride must be positive"));

	// The size is taken before opening, because the truncation below has to
	// happen while nothing is buffered against the file.
	std::error_code ec;
	const auto existing = std::filesystem::exists(path, ec)
							  ? std::filesystem::file_size(path, ec)
							  : std::uintmax_t{0};
	if (ec)
		return std::unexpected(describe(path, "cannot size log", ec.message()));

	// Three shapes a path can be in, and each wants something different: an
	// existing log is validated and trimmed, a stub too short to be one is
	// discarded, and an absent file becomes a fresh log below.
	const auto whole = (existing >= LOG_HEADER_SIZE)
						   ? adopt_existing(path, existing, stride)
						   : discard_stub(path, existing).transform([] {
								 return std::uint64_t{0};
							 });
	if (!whole) return std::unexpected(whole.error());

	// "a+b" rather than "ab": append-only mode makes every write go to the end
	// regardless of the file position, which is what this wants, and the read
	// half lets one handle serve read_at without reopening.
	util::owned_file file = util::open_shared(path, "a+b");
	if (file == nullptr)
		return std::unexpected(describe(path, "cannot open log", last_error()));

	if (existing < LOG_HEADER_SIZE)
		if (const auto written = write_header(path, file.get(), stride);
			!written)
			return std::unexpected(written.error());

	return raw_record_log(std::move(file), path, stride, *whole);
}

std::expected<raw_record_log, std::string>
raw_record_log::open_for_read(const std::filesystem::path &path,
							  std::size_t stride) {
	if (stride == 0)
		return std::unexpected(std::string("record stride must be positive"));

	std::error_code ec;
	const auto size = std::filesystem::file_size(path, ec);
	if (ec)
		return std::unexpected(describe(path, "cannot size log", ec.message()));

	if (size < LOG_HEADER_SIZE)
		return std::unexpected(describe(
			path,
			"cannot open log",
			fmt::format("the file is {} bytes, too short to hold a {}-byte "
						"header, so it is not a record log",
						size,
						LOG_HEADER_SIZE)));

	auto header = read_header(path);
	if (!header) return std::unexpected(std::move(header.error()));
	if (const auto ok = validate_header(*header, stride); !ok)
		return std::unexpected(describe(path, "cannot open log", ok.error()));

	util::owned_file file = util::open_shared(path, "rb");
	if (file == nullptr)
		return std::unexpected(describe(path, "cannot open log", last_error()));

	// Not truncated, only ignored: a reader is recovering *from* this file and
	// has no business editing it, and two readers must agree on what it holds.
	return raw_record_log(std::move(file),
						  path,
						  stride,
						  static_cast<std::uint64_t>(size - LOG_HEADER_SIZE) /
							  frame_stride(stride));
}

void raw_record_log::reserve(std::size_t records) {
	const std::size_t bytes = frame_stride(stride_) * records;
	if (bytes > frame_.size()) frame_.resize(bytes);
}

std::size_t raw_record_log::write_framed(const std::byte *payloads,
										 std::size_t count) {
	const std::size_t frame    = frame_stride(stride_);
	const std::size_t capacity = frame_.size() / frame;

	// Chunked against the buffer rather than sized to the batch, so this never
	// allocates however much it is handed. A caller that reserved for its worst
	// case takes one turn of this loop and one fwrite; one that did not takes
	// several of each, which is slower and still correct.
	std::size_t done = 0;
	while (done < count) {
		const std::size_t chunk = std::min(capacity, count - done);
		for (std::size_t i = 0; i < chunk; ++i) {
			const std::byte *payload = payloads + ((done + i) * stride_);
			std::byte *out           = frame_.data() + (i * frame);
			std::memcpy(out, payload, stride_);
			store_le32(out + stride_, checksum(payload, stride_));
		}
		if (std::fwrite(frame_.data(), frame, chunk, file_.get()) != chunk)
			return done;
		done += chunk;
	}
	return done;
}

bool raw_record_log::append(const void *data, std::size_t count) {
	if (!good_) return false;
	if (count == 0) return true;

	const auto *payloads = static_cast<const std::byte *>(data);
	if (write_framed(payloads, count) != count) {
		// Poisoned rather than retried. A short write has already put some
		// fraction of a record in the file, so the next append would continue
		// from a boundary that is not one; refusing everything afterwards keeps
		// the damage to the tail, where the size check will find it.
		good_ = false;
		return false;
	}
	count_ += count;
	return true;
}

bool raw_record_log::sync() {
	if (!good_) return false;
	if (std::fflush(file_.get()) != 0) {
		good_ = false;
		return false;
	}
	if (!sync_to_device(file_.get())) {
		good_ = false;
		return false;
	}
	return true;
}

std::uint64_t raw_record_log::count() const noexcept { return count_; }

std::size_t raw_record_log::read_at(std::uint64_t from, void *out,
									std::size_t count) {
	if (!good_ || count == 0 || from >= count_) return 0;

	// Everything the caller asked for that actually exists. Clamping here
	// rather than failing keeps "read to the end" from having to compute the
	// end first.
	const std::uint64_t available = count_ - from;
	const auto wanted =
		static_cast<std::size_t>(std::min<std::uint64_t>(available, count));

	// A log open for append has its position at the end and, on some platforms,
	// pending writes in the buffer; both are settled by flushing before
	// seeking.
	if (std::fflush(file_.get()) != 0) {
		good_ = false;
		return 0;
	}

	const std::size_t frame = frame_stride(stride_);
	const auto offset =
		static_cast<long long>(LOG_HEADER_SIZE + (from * frame));
#if defined(_WIN32)
	if (::_fseeki64(file_.get(), offset, SEEK_SET) != 0) {
#else
	if (::fseeko(file_.get(), static_cast<off_t>(offset), SEEK_SET) != 0) {
#endif
		good_ = false;
		return 0;
	}

	auto *destination          = static_cast<std::byte *>(out);
	const std::size_t capacity = frame_.size() / frame;
	std::size_t done           = 0;

	while (done < wanted) {
		const std::size_t chunk = std::min(capacity, wanted - done);
		const std::size_t got =
			std::fread(frame_.data(), frame, chunk, file_.get());

		for (std::size_t i = 0; i < got; ++i) {
			const std::byte *framed      = frame_.data() + (i * frame);
			const std::uint32_t recorded = load_le32(framed + stride_);
			const std::uint32_t actual   = checksum(framed, stride_);

			// The failure a fixed stride cannot see. Everything up to here
			// verified and is handed back - it is genuine, and a recovery that
			// threw away a readable prefix because the record after it was
			// rotten would be discarding history it could still have replayed.
			// The log stops there, though: past a record that cannot be
			// trusted, position is all that is left and position is exactly
			// what a corrupt record makes meaningless.
			if (recorded != actual) {
				corrupt_    = true;
				corrupt_at_ = from + done + i;
				good_       = false;
				(void)std::fseek(file_.get(), 0, SEEK_END);
				return done + i;
			}
			std::memcpy(destination + ((done + i) * stride_), framed, stride_);
		}
		done += got;

		// A short read here is always an anomaly, and that is worth spelling
		// out because it is the opposite of what a short read usually means.
		// `wanted` was clamped to records this log knows it holds, and the
		// flush above put the buffered ones in the file, so every record up to
		// `wanted` exists by construction. Coming back with fewer therefore
		// means the file lost them: a device error, or something truncating it
		// underneath the reader.
		//
		// Either way the log stops being trustworthy, so it is poisoned rather
		// than allowed to pass for an end of journal - a replay that mistook
		// one for the other would report a clean recovery having stopped half
		// way through, or spin forever on an offset that will never yield. The
		// records actually read are still returned, because they are genuine;
		// it is the *next* read that refuses.
		//
		// Deliberately not gated on ferror. That distinguishes a device error
		// from a truncation, and the distinction does not matter here: both are
		// the file failing to hold what this log was told it holds, and only
		// one of them sets the error indicator.
		if (got < chunk) {
			good_ = false;
			break;
		}
	}

	// Reading from a handle that is also the append handle leaves the position
	// where the read stopped; stdio requires a seek between a read and a write
	// on the same stream, and appending mode ignores the position anyway.
	(void)std::fseek(file_.get(), 0, SEEK_END);
	return done;
}

bool raw_record_log::is_good() const noexcept { return good_; }

std::optional<std::uint64_t> raw_record_log::corrupt_record() const noexcept {
	if (!corrupt_) return std::nullopt;
	return corrupt_at_;
}

const std::filesystem::path &raw_record_log::path() const noexcept {
	return path_;
}

} // namespace exchange::core::persistence
