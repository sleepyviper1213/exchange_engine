#pragma once
// The store: one directory holding a journal, a checkpoint pointer and the
// snapshots it points at.
//
// `record_log` is a file and `manifest` is three numbers. Neither of them knows
// the other exists, which is correct — and leaves somebody having to know that
// the manifest's `sequence` counts records in *that* journal, and that its
// `snapshot_id` names a file whose path is derived a particular way. That
// somebody is this. It is the difference between a log and a store: a log is
// appended to, a store is *recovered from*.
//
// Like everything else in this module it names no domain type. The record is a
// template parameter, because the dependency graph runs
// Event -> Execution -> Persistence -> Util and a journal of commands would
// otherwise point an edge back up it.

#include "core_export.hpp" // CORE_EXPORT (generated)
#include "manifest.hpp"
#include "record_log.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <utility>

namespace exchange::core::persistence {

/// @brief Create @p root and any missing parent, or say why it could not be.
[[nodiscard]] CORE_EXPORT std::expected<void, std::string>
ensure_directory(const std::filesystem::path &root);

/// @brief Where the command journal lives inside a store rooted at @p root.
[[nodiscard]] CORE_EXPORT std::filesystem::path
journal_path(const std::filesystem::path &root);

/// @brief Where the checkpoint pointer lives inside a store rooted at @p root.
[[nodiscard]] CORE_EXPORT std::filesystem::path
manifest_path(const std::filesystem::path &root);

/**
 * @brief Where snapshot @p id lives inside a store rooted at @p root.
 *
 * Zero-padded to the full width of a 64-bit decimal, so a directory listing
 * comes out in snapshot order without anyone having to sort it numerically. That
 * matters exactly once — when somebody is looking at the directory by hand
 * because recovery went wrong — which is when it matters most.
 */
[[nodiscard]] CORE_EXPORT std::filesystem::path
snapshot_path(const std::filesystem::path &root, std::uint64_t id);

/**
 * @brief A recoverable store of @p T: the journal, the checkpoint, the snapshots.
 *
 * @tparam T The journalled record. Trivially copyable, per @c record_log.
 *
 * @par The two flows it exists to make correct
 * **Recovery**, on startup: read @c checkpoint(), load the snapshot it names if
 * its @c snapshot_id is non-zero, then replay the journal from its @c sequence.
 * That last number is the whole point of the manifest — without it a recovery
 * either replays from zero (correct but unboundedly slow) or guesses (fast and
 * wrong).
 *
 * **Checkpointing**, periodically: write a snapshot to @c snapshot_path of a
 * fresh @c next_snapshot_id, sync it, then @c commit. The order is not
 * negotiable and @c commit is what enforces the half of it that it can: a
 * manifest naming a snapshot that was never finished sends recovery to a file
 * that does not exist, so @c commit refuses an id with no file behind it.
 *
 * @code
 * auto store = event_store<command>::open("var/venue");
 * // recovery
 * const std::uint64_t from = store->checkpoint().sequence;
 * // ... load store->snapshot_path(store->checkpoint().snapshot_id) if non-zero
 * replay(store->journal(), from, [&](const command &c) { return apply(c); });
 * // steady state
 * partition.attach_journal(&store->journal());
 * @endcode
 *
 * @par What it deliberately does not do
 * It does not take the snapshot, because a snapshot is book state and this
 * module has never heard of a book. It hands out the path and records that the
 * path was filled; writing it belongs to whoever owns the state. Nor does it
 * delete old snapshots — retention is a policy about disk and audit, not about
 * recovery, and a store that silently removed the file an operator was about to
 * inspect would be solving the wrong problem.
 *
 * @par Threading
 * One store, one thread — the same one that owns the journal, which in the
 * engine is a partition's consumer.
 */
template <class T>
class event_store {
public:
	/**
	 * @brief Open (creating if absent) the store rooted at @p root.
	 *
	 * @return The store, or why it could not be opened.
	 * @post The directory exists, the journal is open for appending with any torn
	 *       tail already truncated, and @c checkpoint() is loaded — or is a
	 *       default-constructed manifest when the store is new, which reads as
	 *       "no snapshot, replay from record zero" and is exactly right for one.
	 */
	[[nodiscard]] static std::expected<event_store, std::string>
	open(std::filesystem::path root) {
		if (auto made = ensure_directory(root); !made)
			return std::unexpected(std::move(made.error()));

		auto log = record_log<T>::open_for_append(journal_path(root));
		if (!log) return std::unexpected(std::move(log.error()));

		// A missing manifest is not a failure: it is what a store that has never
		// been checkpointed looks like, and the default it stands in for —
		// snapshot 0, sequence 0 — is the correct instruction for that case.
		// A manifest that exists and cannot be *parsed* is a different matter and
		// is reported, because it means the pointer is there and unreadable.
		manifest checkpoint;
		const std::filesystem::path pointer = manifest_path(root);
		if (std::filesystem::exists(pointer)) {
			auto loaded = load(pointer);
			if (!loaded) return std::unexpected(std::move(loaded.error()));
			checkpoint = *loaded;
		}

		return event_store(std::move(root), std::move(*log), checkpoint);
	}

	// Holds an open file handle and a record_log that is itself immovable-ish;
	// move is kept because open() returns by value, copy is not because two
	// stores appending to one journal is not a thing that can work.
	event_store(const event_store &)            = delete;
	event_store &operator=(const event_store &) = delete;
	event_store(event_store &&)                 = default;
	event_store &operator=(event_store &&)      = default;
	~event_store()                              = default;

	/// @brief The journal. Append to it; @c attach_journal takes its address.
	[[nodiscard]] record_log<T> &journal() noexcept { return journal_; }

	[[nodiscard]] const record_log<T> &journal() const noexcept {
		return journal_;
	}

	/// @brief What recovery starts from — the last committed checkpoint, or all
	///        zeroes when there has never been one.
	[[nodiscard]] const manifest &checkpoint() const noexcept {
		return checkpoint_;
	}

	/// @brief The store's directory.
	[[nodiscard]] const std::filesystem::path &root() const noexcept {
		return root_;
	}

	/// @brief Where snapshot @p id belongs.
	[[nodiscard]] std::filesystem::path
	snapshot_path(std::uint64_t id) const {
		return persistence::snapshot_path(root_, id);
	}

	/**
	 * @brief An id no snapshot in this store has used.
	 *
	 * One past the committed one, rather than a scan of the directory. Monotonic
	 * by construction, so an id is never reused even if an uncommitted snapshot
	 * file was left behind by a crash — that file is simply orphaned, and
	 * orphaning it is what keeps the committed one intact.
	 *
	 * @note Starts at 1, because zero is the manifest's "no snapshot" value and a
	 *       real snapshot must be distinguishable from the absence of one.
	 */
	[[nodiscard]] std::uint64_t next_snapshot_id() const noexcept {
		return checkpoint_.snapshot_id + 1;
	}

	/**
	 * @brief Record that snapshot @p snapshot_id covers the journal so far.
	 *
	 * @param snapshot_id The snapshot just written. Its file must exist.
	 * @param session The session the checkpoint was taken in, for the manifest.
	 * @return Nothing, or why the checkpoint could not be committed.
	 *
	 * @pre The snapshot file exists and has been synced. This checks the first
	 *      half — a manifest pointing at a file that is not there is a recovery
	 *      that cannot start, so it is worth one @c exists call to refuse — and
	 *      trusts the caller on the second, because "has this been synced" is not
	 *      a question the filesystem answers.
	 * @post On success the manifest names @p snapshot_id and the journal's current
	 *       record count, atomically. On failure the previous checkpoint is
	 *       untouched and still usable, which is the property that makes a failed
	 *       checkpoint survivable: it costs a longer replay, not a lost venue.
	 */
	[[nodiscard]] std::expected<void, std::string>
	commit(std::uint64_t snapshot_id, std::uint64_t session) {
		if (snapshot_id == 0)
			return std::unexpected(
				std::string("snapshot id 0 is reserved for 'no snapshot'"));

		const std::filesystem::path file = snapshot_path(snapshot_id);
		if (!std::filesystem::exists(file))
			return std::unexpected("cannot commit a checkpoint for a snapshot "
								   "that was never written: " +
								   file.string());

		// The journal must be durable before the manifest says a checkpoint
		// covers it, for the same reason a trade must not be published before its
		// command is durable: the manifest is a claim about what is on disk.
		if (!journal_.sync())
			return std::unexpected("cannot sync the journal before committing: " +
								   journal_.path().string());

		const manifest next{.snapshot_id = snapshot_id,
							.sequence    = journal_.count(),
							.session     = session};
		if (auto saved = save(manifest_path(root_), next); !saved)
			return std::unexpected(std::move(saved.error()));

		checkpoint_ = next;
		return {};
	}

private:
	event_store(std::filesystem::path root, record_log<T> &&journal,
				const manifest &checkpoint)
		: root_(std::move(root)),
		  journal_(std::move(journal)),
		  checkpoint_(checkpoint) {}

	std::filesystem::path root_;
	record_log<T> journal_;
	manifest checkpoint_;
};

} // namespace exchange::core::persistence
