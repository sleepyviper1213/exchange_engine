#include "record_log.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>


#if defined(_WIN32)
#include <io.h>
#include <share.h>
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

/// @brief Open @p path with @p mode, allowing other handles on the same file.
///
/// The sharing is the point, and it is why this is not @c fopen_s. MSVC's
/// @c fopen_s opens for *exclusive* access by default, where POSIX @c fopen
/// does not - so a journal open for append could not be read by anything,
/// including the recovery check that wants to verify what it just wrote. @c
/// _fsopen with
/// @c _SH_DENYNO restores the POSIX behaviour, which is the one the rest of
/// this class documents.
std::FILE *open_shared(const std::filesystem::path &path, const char *mode) {
#if defined(_WIN32)
	return ::_fsopen(path.string().c_str(), mode, _SH_DENYNO);
#else
	return std::fopen(path.c_str(), mode);
#endif
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

} // namespace

bool sync_file(const std::filesystem::path &path) {
	// "r+b": the file must already exist and must not be truncated, and the
	// write half is required because Windows will not commit a read-only
	// handle.
	FILE *file = open_shared(path, "r+b");
	if (file == nullptr) return false;
	const bool ok = sync_to_device(file);
	(void)std::fclose(file);
	return ok;
}

void raw_record_log::file_closer::operator()(std::FILE *file) const noexcept {
	if (file != nullptr) (void)std::fclose(file);
}

raw_record_log::raw_record_log(std::FILE *file, std::filesystem::path path,
							   std::size_t stride, std::uint64_t count) noexcept
	: file_(file), path_(std::move(path)), stride_(stride), count_(count) {}

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

	const std::uint64_t whole = static_cast<std::uint64_t>(existing) / stride;
	const std::uintmax_t aligned = static_cast<std::uintmax_t>(whole) * stride;

	// A torn tail: the previous process died part-way through a write. Dropping
	// it is the only option that leaves the file appendable - an append after a
	// partial record would put every later record off its boundary, turning one
	// lost command into an unreadable log.
	if (aligned != existing) {
		std::filesystem::resize_file(path, aligned, ec);
		if (ec)
			return std::unexpected(
				describe(path, "cannot truncate a torn record", ec.message()));
	}

	// "a+b" rather than "ab": append-only mode makes every write go to the end
	// regardless of the file position, which is what this wants, and the read
	// half lets one handle serve read_at without reopening.
	FILE *file = open_shared(path, "a+b");
	if (file == nullptr)
		return std::unexpected(describe(path, "cannot open log", last_error()));

	return raw_record_log(file, path, stride, whole);
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

	std::FILE *file = open_shared(path, "rb");
	if (file == nullptr)
		return std::unexpected(describe(path, "cannot open log", last_error()));

	// Not truncated, only ignored: a reader is recovering *from* this file and
	// has no business editing it, and two readers must agree on what it holds.
	return raw_record_log(file,
						  path,
						  stride,
						  static_cast<std::uint64_t>(size) / stride);
}

bool raw_record_log::append(const void *data, std::size_t count) {
	if (!good_) return false;
	if (count == 0) return true;

	if (std::fwrite(data, stride_, count, file_.get()) != count) {
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

	const auto offset = static_cast<long long>(from * stride_);
#if defined(_WIN32)
	if (::_fseeki64(file_.get(), offset, SEEK_SET) != 0) {
#else
	if (::fseeko(file_.get(), static_cast<off_t>(offset), SEEK_SET) != 0) {
#endif
		good_ = false;
		return 0;
	}

	const std::size_t read = std::fread(out, stride_, wanted, file_.get());

	// Reading from a handle that is also the append handle leaves the position
	// where the read stopped; stdio requires a seek between a read and a write
	// on the same stream, and appending mode ignores the position anyway.
	(void)std::fseek(file_.get(), 0, SEEK_END);
	return read;
}

bool raw_record_log::is_good() const noexcept { return good_; }

const std::filesystem::path &raw_record_log::path() const noexcept {
	return path_;
}

} // namespace exchange::core::persistence
