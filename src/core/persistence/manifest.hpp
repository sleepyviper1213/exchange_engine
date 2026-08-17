#pragma once
// The pointer recovery starts from: which snapshot to load, and how far into the
// journal it already accounts for.
//
// Everything else in this module is a bulk artefact — a journal of thousands of
// commands, a snapshot of thousands of orders. This is three numbers, and it is
// the only file whose loss makes the other two unusable: a directory of
// snapshots with no manifest is a set of candidate pasts with nothing saying
// which one is current, or where the journal picks up from it.
//
// That asymmetry is why this file is written the way it is — replaced
// atomically, and in text. @see manifest for both arguments.

#include "core_export.hpp" // CORE_EXPORT (generated)

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace exchange::core::persistence {

/**
 * @brief What a checkpoint was, in the three numbers recovery needs.
 *
 * @par Why text, when the journal beside it is raw bytes
 * Because the two have opposite failure requirements. The journal is written per
 * command on a path with a latency budget, so it is the host's object
 * representation and pays nothing to encode — and if a layout change makes an
 * old journal unreadable, that is survivable, because a journal you cannot read
 * costs you the tail of one session.
 *
 * A manifest you cannot read costs you *everything*: it is what names the
 * snapshot, so without it the snapshots are unidentifiable and the journal has
 * no starting offset. So it is the one artefact here that must stay readable
 * across a compiler change, an architecture change, a layout change, and an
 * operator with nothing but @c cat at three in the morning. Three lines of
 * @c key=value costs nothing to write once per checkpoint and cannot be
 * invalidated by any of those.
 *
 * @par Why it is replaced rather than written
 * A manifest overwritten in place has a window in which it is half the old
 * values and half the new — and a crash inside that window leaves recovery
 * pointing at a snapshot that does not exist, or at a journal offset from a
 * different checkpoint, which is worse than having no manifest at all. So
 * @c save writes a temporary beside it and renames over the top: rename is
 * atomic on both POSIX and Windows, so a reader sees either the whole previous
 * manifest or the whole new one and never a mixture. Emporia's
 * `ExchangeCoreCheckpointStore` does exactly this, and it is the one part of its
 * checkpointing worth copying.
 */
struct manifest {
	/// @brief Which snapshot this names. Recovery turns it into a filename.
	std::uint64_t snapshot_id = 0;

	/**
	 * @brief Journal records the snapshot already accounts for.
	 *
	 * The resume point, and the reason a snapshot is worth taking: replay starts
	 * at this record rather than at zero. It is a *count*, so it is also the
	 * index of the first record still to apply — the snapshot covers
	 * <code>[0, sequence)</code>.
	 *
	 * @note A count and not a stamped sequence number, because nothing stamps
	 *       commands yet (TODO.md #7). Position in the append-only log is the
	 *       order until that lands, and this is that position. When commands do
	 *       carry a sequence, this becomes the place the two are reconciled.
	 */
	std::uint64_t sequence = 0;

	/**
	 * @brief The session the checkpoint was taken in.
	 *
	 * Opaque here — @c persistence sits below the trading engine and must not
	 * name @c lifecycle::session_id_t — but not opaque to its reader: it is what
	 * lets a recovery say which session's state it is continuing rather than
	 * merely that it continued one.
	 */
	std::uint64_t session = 0;

	bool operator==(const manifest &) const noexcept = default;
};

/**
 * @brief Replace the manifest at @p path with @p current, atomically.
 *
 * @param path The manifest file. Its directory must exist.
 * @param current What to record.
 * @return Nothing on success, or why it failed.
 * @post Either @p path holds @p current in full, or it is unchanged. There is no
 *       state in which it holds part of it.
 *
 * @warning Atomic, but not fully durable on POSIX without one more step this
 *          does not take: the temporary's *contents* are synced before the
 *          rename, so the rename can never expose a half-written file, but the
 *          rename itself lives in the parent directory's metadata and making
 *          *that* survive a machine crash needs an fsync on the directory, which
 *          has no portable spelling and is a no-op on Windows. The window is a
 *          machine losing power between the rename and the filesystem's own
 *          commit; the previous manifest and its snapshot are what survive it,
 *          which is a stale recovery rather than a broken one.
 */
[[nodiscard]] CORE_EXPORT std::expected<void, std::string>
save(const std::filesystem::path &path, const manifest &current);

/**
 * @brief Read the manifest at @p path.
 * @return The manifest, or why it could not be read.
 * @note A missing file is an error rather than a default-constructed manifest:
 *       "no checkpoint has been taken" and "the checkpoint pointer is gone" are
 *       different situations and only the caller knows which one is expected, so
 *       distinguishing them is its business and not this function's.
 */
[[nodiscard]] CORE_EXPORT std::expected<manifest, std::string>
load(const std::filesystem::path &path);

} // namespace exchange::core::persistence
