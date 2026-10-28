#include "manifest.hpp"

#include "core/util/flag.hpp"
#include "record_log.hpp"

#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>

#include <charconv>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace exchange::core::persistence {
namespace {

std::string describe(const std::filesystem::path &path, std::string_view what) {
	return fmt::format("{} {}", what, path.string());
}

/**
 * @brief The manifest's fields: one bit each, named by the key each is written
 *        under.
 *
 * @par Why the key is the enumerator's name
 * A manifest is meant to be read and edited by hand, so the spelling in the
 * file *is* the field's identity - @c save writing one spelling while @c load
 * looks for another is a file that round-trips through neither, and nothing
 * about it looks wrong. With the key as the magic_enum name, @c save writes
 * @c enum_name and @c load parses with @c enum_cast, so the two cannot
 * disagree; adding a field is an enumerator, a key and a @c case in
 * @c apply_line, and the compiler names the last two.
 *
 * @par Why bits at all
 * So @c load can tell a manifest that omitted a key from one that set it to
 * zero. Those are different files: zero is a meaningful value for all three,
 * and a missing key is a truncated manifest whose defaults would be read as
 * instructions.
 */
enum class manifest_field : std::uint8_t {
	SNAPSHOT = 1U << 0U,
	SEQUENCE = 1U << 1U,
	SESSION  = 1U << 2U,
};

EXCHANGE_ENABLE_FLAGS(manifest_field)

} // namespace
} // namespace exchange::core::persistence

template <>
struct magic_enum::customize::enum_range<
	exchange::core::persistence::manifest_field> {
	static constexpr bool is_flags = true; // NOLINT(*-identifier-naming)
};

template <>
constexpr magic_enum::customize::customize_t magic_enum::customize::enum_name(
	exchange::core::persistence::manifest_field value) noexcept {
	switch (value) {
		using enum exchange::core::persistence::manifest_field;
	case SNAPSHOT: return "snapshot_id";
	case SEQUENCE: return "sequence";
	case SESSION: return "session";
	}
	return default_tag;
}

namespace exchange::core::persistence {
namespace {

/**
 * @brief The set of fields one manifest supplied.
 *
 * A @c flag<> rather than a bare @c unsigned mask because the two questions
 * this code asks - "was this one field given" and "were all the required ones"
 * - read identically when both are an @c & against an integer, and one of them
 * is wrong. @c test and @c all_of say which is meant, and a single
 * @c manifest_field no longer converts to a number that could be compared
 * against the wrong thing.
 */
using manifest_fields = util::flag<manifest_field>;

/// @brief The fields that *instruct* a recovery, and so the ones that cannot be
///        defaulted.
///
/// @c SESSION is not among them on purpose: it says which session's state is
/// being continued, which a reader wants and a replay does not need, so its
/// absence costs a log line rather than a book. That split is what keeps this
/// format extensible - a field added later is optional by default, and an older
/// manifest goes on loading, which is the reason it is key=value and not three
/// positional numbers.
constexpr manifest_fields REQUIRED_MASK =
	manifest_field::SNAPSHOT | manifest_field::SEQUENCE;

/// @brief The required fields @p seen lacks, as @c " key" apiece - the tail of
///        the incomplete-manifest message, and empty when none are missing.
///        Built only on the failure path; the check itself stays one @c &.
std::string missing_fields(manifest_fields seen) {
	std::string missing;
	for (const manifest_field field : magic_enum::enum_values<manifest_field>())
		if (REQUIRED_MASK.test(field) && seen.none_of(field))
			missing += fmt::format(" {}", magic_enum::enum_name(field));
	return missing;
}

/// @brief Parse one @c key=value line into the field @p key names.
/// @param[in,out] seen Gains the bit for the field this line set.
/// @return @c false if the line is malformed, the key is unknown, or the key
///         has already been given a value.
bool apply_line(std::string_view line, manifest &into, manifest_fields &seen) {
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

	const std::optional<manifest_field> field =
		magic_enum::enum_cast<manifest_field>(key);
	if (!field) return false;

	// A repeated key is refused rather than last-one-wins. Two values for one
	// field is a file somebody edited and got wrong, and choosing one of them
	// is choosing which half of their intent to honour. Asked before the store
	// below, so a refused line leaves `into` as it found it.
	if (seen.test(*field)) return false;

	// The switch is what makes adding a manifest_field a compile error here
	// rather than a key that parses and lands nowhere.
	switch (*field) {
		using enum manifest_field;
	case SNAPSHOT: into.snapshot_id = parsed; break;
	case SEQUENCE: into.sequence = parsed; break;
	case SESSION: into.session = parsed; break;
	}
	seen.set(*field);
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
						   magic_enum::enum_name(manifest_field::SNAPSHOT),
						   current.snapshot_id,
						   magic_enum::enum_name(manifest_field::SEQUENCE),
						   current.sequence,
						   magic_enum::enum_name(manifest_field::SESSION),
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
	manifest_fields seen;
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

	// Both instructions, or neither. A default standing in for one of these is
	// the one way this file can be wrong without looking wrong, and it goes
	// wrong in both directions: a manifest holding only `snapshot_id` loads
	// that snapshot and replays the journal from record zero, so every PLACE
	// comes back DUPLICATE_ORDER_ID and every CANCEL a rejection sent to a
	// client who never asked - while one holding only `sequence` starts from an
	// empty book and skips every record before it. `save` writes them together
	// and cannot produce either, which leaves a hand edit or a truncating copy,
	// and this is the file people are meant to read and edit.
	if (!seen.all_of(REQUIRED_MASK))
		return std::unexpected(fmt::format("incomplete manifest {}: missing{}",
										   path.string(),
										   missing_fields(seen)));
	return current;
}

} // namespace exchange::core::persistence
