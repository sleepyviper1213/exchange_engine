#pragma once
// The journal's on-disk record: a command's fields at defined offsets, in
// defined byte order.
//
// `record_log` writes whatever type it is given as that type's object
// representation, which is fast and correct and not portable: the layout is the
// compiler's to choose and the byte order is the machine's. That is the right
// trade for a snapshot, which is read back by the process that wrote it or not
// at all. It is the wrong one for a *journal*, which is the artefact a venue
// keeps: an audit trail read years later, a stream shipped to a standby, a
// history replayed by a build that is not this one.
//
// So the journal gets an explicit record and this is it. Two properties follow,
// and the second is the surprising one:
//
//   - It is portable. Every field is at a documented offset in little-endian,
//     so a reader on another architecture gets values rather than nonsense.
//   - It is *smaller*. `sizeof(command)` is 48, of which 8 bytes are padding the
//     compiler inserted to align a union; writing the fields directly needs 40.
//     A defined layout is not a tax here, it is an 8-byte-per-record saving.
//
// It lives in event/ rather than in persistence/ because it names `command`, and
// the dependency graph runs Event -> ... -> Persistence. Persistence still knows
// nothing about commands: it is handed a 40-byte trivially copyable record and
// frames it like any other.

#include "command.hpp"
#include "event_export.hpp" // EVENT_EXPORT (generated)
#include "fwd.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <type_traits>

namespace exchange::engine::event {

/**
 * @brief One command as it is written to the journal: 40 bytes, fixed layout.
 *
 * Deliberately opaque - a byte array rather than a struct of fields. A struct
 * would put the layout back in the compiler's hands, which is the entire thing
 * this type exists to take away from it; and a reader who wants a field wants a
 * @c command, which is what @c decode returns.
 *
 * @par The layout
 * Offsets are part of the format. Several are shared between arms, because the
 * tag says which of them is meaningful - that is what a union is, spelled where
 * it can be seen:
 *
 * | Offset | Size | Field | Meaningful for |
 * |---:|---:|---|---|
 * | 0 | 1 | tag (@c command_type) | every command |
 * | 1 | 4 | symbol | every command |
 * | 5 | 8 | order id / cancelled id | PLACE, CANCEL |
 * | 13 | 4 | order's own symbol_id | PLACE |
 * | 17 | 1 | side | PLACE, ADD, REDUCE |
 * | 18 | 1 | order type | PLACE |
 * | 19 | 1 | time in force | PLACE |
 * | 20 | 4 | price | PLACE, ADD, REDUCE |
 * | 24 | 4 | stop price | PLACE |
 * | 28 | 4 | quantity / level volume | PLACE, ADD, REDUCE |
 * | 32 | 8 | timestamp | PLACE |
 *
 * PLACE is the widest arm and uses all forty bytes, which is what sets the size.
 * Bytes an arm does not use are written as zero, so encoding the same command
 * twice gives the same bytes - a property worth having free, since a record is
 * checksummed and may be compared.
 *
 * @note Trivially copyable and fixed-size, so
 *       @c core::persistence::record_log takes it unchanged and frames it with a
 *       CRC32C exactly as it frames anything else. On disk a record is therefore
 *       44 bytes: these 40 plus the checksum.
 */
struct journal_record {
	/// @brief Bytes one record occupies. Part of the format, not an
	/// implementation detail - see the layout table.
	static constexpr std::size_t SIZE = 40;

	std::array<std::byte, SIZE> bytes{};

	bool operator==(const journal_record &) const noexcept = default;
};

static_assert(std::is_trivially_copyable_v<journal_record>,
			  "a journalled record is written as its object representation");
static_assert(sizeof(journal_record) == journal_record::SIZE,
			  "a journal_record must be exactly its documented layout, with no "
			  "padding of the compiler's own");
static_assert(sizeof(journal_record) < sizeof(command),
			  "the point of a defined layout here is that it is smaller than the "
			  "padded object representation it replaces");

/**
 * @brief Write @p cmd into its on-disk form.
 *
 * Total, and @c noexcept: every representable @c command encodes, because a
 * command can only be built through a factory that matches its tag to its
 * payload. There is no failure mode to report.
 */
[[nodiscard]] EVENT_EXPORT journal_record encode(const command &cmd) noexcept;

/**
 * @brief Read @p record back into a command, or say why it is not one.
 *
 * @return The command, or a description of what was wrong with the bytes.
 *
 * @par Why this can fail when @c encode cannot
 * Because the bytes may not have come from @c encode. A record log validates its
 * framing and its checksum, which together make the bytes *the bytes that were
 * written*; nothing about that makes them a valid command. A record from a newer
 * build, a hand-edited file, or a genuinely novel corruption that still
 * checksums can carry a tag or an enumerator this build has no meaning for, and
 * the honest answer is to refuse it rather than to construct a command that was
 * never asked for.
 *
 * @warning The validation is not decoration in one specific case: @c side_t is
 *          an enum with a @c bool underlying type, so a byte outside {0, 1} cast
 *          to it is undefined behaviour rather than a strange value. That byte is
 *          checked before the cast, not after.
 */
[[nodiscard]] EVENT_EXPORT std::expected<command, std::string>
decode(const journal_record &record) noexcept;

} // namespace exchange::engine::event
