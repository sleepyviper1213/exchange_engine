#include "manifest.hpp"

#include "record_log.hpp"

#include <fmt/format.h>

#include <charconv>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace exchange::core::persistence {
namespace {

/// @brief The keys, spelled once. A manifest is read by hand often enough that
///        the names matter, and written in exactly one place so they cannot
///        drift from what @c load looks for.
constexpr std::string_view SNAPSHOT_KEY = "snapshot_id";
constexpr std::string_view SEQUENCE_KEY = "sequence";
constexpr std::string_view SESSION_KEY  = "session";

std::string describe(const std::filesystem::path &path, std::string_view what) {
	return fmt::format("{} {}", what, path.string());
}

/// @brief One bit per field, so @c load can tell a manifest that omitted a key
///        from one that set it to zero. Those are different files: zero is a
///        meaningful value for all three, and a missing key is a truncated
///        manifest whose defaults would be read as instructions.
enum : unsigned {
	SAW_SNAPSHOT = 1U << 0U,
	SAW_SEQUENCE = 1U << 1U,
	SAW_SESSION  = 1U << 2U,
	// The two that *instruct* a recovery, and so the two that cannot be
	// defaulted. `session` is not among them on purpose: it says which session's
	// state is being continued, which a reader wants and a replay does not need,
	// so its absence costs a log line rather than a book. That split is what
	// keeps this format extensible - a field added later is optional by default,
	// and an older manifest goes on loading, which is the reason it is key=value
	// and not three positional numbers.
	SAW_REQUIRED = SAW_SNAPSHOT | SAW_SEQUENCE,
};

/// @brief Parse one @c key=value line into the field @p key names.
/// @param[in,out] seen Gains the bit for the field this line set.
/// @return @c false if the line is malformed, the key is unknown, or the key has
///         already been given a value.
bool apply_line(std::string_view line, manifest &into, unsigned &seen) {
	const std::size_t split = line.find('=');
	if (split == std::string_view::npos) return false;

	const std::string_view key   = line.substr(0, split);
	const std::string_view value = line.substr(split + 1);

	std::uint64_t parsed  = 0;
	const auto *const end = value.data() + value.size();
	const auto [stop, ec] = std::from_chars(value.data(), end, parsed);
	// The whole value or none of it: "12x" is a typo, not the number twelve,
	// and a manifest is the last file that should guess what an operator meant.
	if (ec != std::errc{} || stop != end) return false;

	// A repeated key is refused rather than last-one-wins. Two values for one
	// field is a file somebody edited and got wrong, and choosing one of them is
	// choosing which half of their intent to honour.
	unsigned bit = 0;
	if (key == SNAPSHOT_KEY) {
		bit              = SAW_SNAPSHOT;
		into.snapshot_id = parsed;
	} else if (key == SEQUENCE_KEY) {
		bit           = SAW_SEQUENCE;
		into.sequence = parsed;
	} else if (key == SESSION_KEY) {
		bit          = SAW_SESSION;
		into.session = parsed;
	} else {
		return false;
	}
	if ((seen & bit) != 0) return false;
	seen |= bit;
	return true;
}

} // namespace

std::expected<void, std::string> save(const std::filesystem::path &path,
									  const manifest &current) {
	// Beside the target, not in the system temp: a rename is only atomic within
	// one filesystem, and a temp directory is routinely on another one, where
	// the rename silently degrades into a copy - which is exactly the
	// non-atomic write this function exists to avoid.
	std::filesystem::path staging = path;
	staging += ".tmp";

	{
		std::ofstream out(staging, std::ios::binary | std::ios::trunc);
		if (!out) return std::unexpected(describe(staging, "cannot write"));
		out << fmt::format("{}={}\n{}={}\n{}={}\n",
						   SNAPSHOT_KEY,
						   current.snapshot_id,
						   SEQUENCE_KEY,
						   current.sequence,
						   SESSION_KEY,
						   current.session);
		out.flush();
		if (!out) return std::unexpected(describe(staging, "cannot write"));
	}

	// The contents reach the device before the rename publishes them. Without
	// this the rename could expose a file the operating system has not written
	// yet, which is the half-written manifest the whole dance is avoiding.
	if (!sync_file(staging))
		return std::unexpected(describe(staging, "cannot sync"));

	std::error_code ec;
	std::filesystem::rename(staging, path, ec);
	if (ec) {
		std::error_code ignored;
		std::filesystem::remove(staging, ignored);
		return std::unexpected(
			fmt::format("cannot replace {}: {}", path.string(), ec.message()));
	}
	return {};
}

std::expected<manifest, std::string> load(const std::filesystem::path &path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) return std::unexpected(describe(path, "cannot read"));

	manifest current;
	unsigned seen = 0;
	std::string line;
	while (std::getline(in, line)) {
		// Tolerated so a manifest written on one platform reads on the other;
		// nothing else about the format is lenient.
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty()) continue;
		if (!apply_line(line, current, seen))
			return std::unexpected(
				fmt::format("malformed manifest {}: {}", path.string(), line));
	}

	// Both instructions, or neither. A default standing in for one of these is the
	// one way this file can be wrong without looking wrong, and it goes wrong in
	// both directions: a manifest holding only `snapshot_id` loads that snapshot
	// and replays the journal from record zero, so every PLACE comes back
	// DUPLICATE_ORDER_ID and every CANCEL a rejection sent to a client who never
	// asked - while one holding only `sequence` starts from an empty book and
	// skips every record before it. `save` writes them together and cannot
	// produce either, which leaves a hand edit or a truncating copy, and this is
	// the file people are meant to read and edit.
	if ((seen & SAW_REQUIRED) != SAW_REQUIRED)
		return std::unexpected(
			fmt::format("incomplete manifest {}: missing{}{}",
						path.string(),
						(seen & SAW_SNAPSHOT) == 0 ? " snapshot_id" : "",
						(seen & SAW_SEQUENCE) == 0 ? " sequence" : ""));
	return current;
}

} // namespace exchange::core::persistence
