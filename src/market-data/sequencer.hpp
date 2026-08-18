#pragma once
// Gap detection for a diff feed: does this event follow the last one applied?
//
// A diff feed is only meaningful in order. Each event states which sequence
// numbers it covers, and a local book is a correct replica exactly while every
// number since the seeding snapshot has been applied once, in order. Miss one -
// a dropped frame, a reconnect, a snapshot fetched too late - and the book is
// silently wrong: absolute level sizes mean a hole leaves no trace, the book
// just quietly stops matching the venue's. Detecting that is this file's whole
// job.
//
// The state machine is the managed-local-order-book procedure venues document,
// with the venue's spelling normalised away (see normalised.hpp):
//
//   1. subscribe first, and buffer events while unsynced   -> `buffer`
//   2. fetch a snapshot; it covers everything up to S      -> seed(S)
//   3. drop buffered events wholly at or below S           -> `discard`
//   4. the first event to apply must cover S + 1           -> `apply` / `gap`
//   5. thereafter each event must resume where the last    -> `apply` / `gap`
//      one ended
//
// Steps 4 and 5 are the same question - "does this event cover the number I am
// waiting for?" - so this implements one rule, not two. Step 4's failure mode
// (the snapshot is older than the oldest buffered event, leaving a hole between
// them) is a gap like any other and resolves the same way: fetch a newer
// snapshot.
//
// This class is pure bookkeeping over sequence ranges; it never sees a level
// and owns no book. @c depth_reconstructor pairs it with the buffer and the
// book to give the whole procedure.

#include "core/util/inclusive_range.hpp"
#include "fwd.hpp"
#include "sequencer_state.hpp" // IWYU pragma: export

#include <cstdint>

namespace exchange::market_data {

/**
 * @brief Running counts of what the sequencer decided - feed-health telemetry.
 *
 * @c gaps is the number that matters: it counts resyncs forced by a broken
 * sequence, so a non-zero and growing value means the feed (or the consumer
 * keeping up with it) is losing data.
 */
struct sequencer_stats {
	std::uint64_t applied    = 0; ///< Events that advanced the sequence.
	std::uint64_t discarded  = 0; ///< Events already covered by the snapshot.
	std::uint64_t buffered   = 0; ///< Events seen while unsynced.
	std::uint64_t overlapped = 0; ///< Applied events that re-covered old ids.
	std::uint64_t gaps       = 0; ///< Discontinuities that forced a resync.
};

/**
 * @brief Tracks a diff feed's sequence numbers and reports every discontinuity.
 *
 * Feed each event's @ref inclusive_range to @c observe() and act on what it
 * returns; call @c seed() when a snapshot arrives. The sequencer holds one
 * number (the next sequence it expects) and a state, so it is cheap enough to
 * sit on the per-event path.
 *
 * @note Not thread-safe, and not meant to be: one feed is one sequence, so one
 *       consuming thread owns one sequencer.
 */
class depth_sequencer {
public:
	/**
	 * @brief Sequence one event and say what to do with it.
	 *
	 * While @c awaiting_snapshot every event is @c buffer - there is no
	 * reference point to judge it against yet, and it may well be needed once a
	 * snapshot arrives. While @c streaming, with @c e the expected sequence:
	 *
	 * | condition                    | result    | meaning                    |
	 * |------------------------------|-----------|----------------------------|
	 * | @c last < @c e               | @c discard| wholly seen already        |
	 * | @c first <= @c e <= @c last  | @c apply  | resumes the sequence       |
	 * | @c first > @c e              | @c gap    | events were lost           |
	 * | @c first > @c last           | @c gap    | unorderable frame          |
	 *
	 * An event that covers @c e while also re-covering ids below it is applied
	 * (absolute sizes make the overlap harmless) and counted in
	 * @c sequencer_stats::overlapped - a venue that guarantees exact adjacency
	 * should never produce one, so it is worth watching.
	 *
	 * Returning @c gap also invalidates the sequencer: it drops back to
	 * @c awaiting_snapshot, so this event and every later one buffer until the
	 * caller supplies a fresh snapshot. The caller must treat its book as stale
	 * and discard it - the sequencer cannot do that for it.
	 * @param sequence The range the event covers.
	 * @return What the caller must do with the event.
	 */
	[[nodiscard]] MARKET_DATA_EXPORT sequence_action
	observe(core::util::inclusive_range<sequence_t> sequence) noexcept;

	/**
	 * @brief Seed from a snapshot covering everything up to @p
	 * snapshot_sequence.
	 *
	 * Puts the sequencer in @c streaming, expecting @p snapshot_sequence + 1
	 * next. Legal at any time: re-seeding a live feed from a fresh snapshot is
	 * how a gap is repaired, and how a periodic re-sync works.
	 * @param snapshot_sequence The snapshot's last covered sequence number.
	 */
	MARKET_DATA_EXPORT void seed(sequence_t snapshot_sequence) noexcept;

	/**
	 * @brief Declare the local book stale and require a new snapshot.
	 *
	 * For discontinuities the sequencer cannot see - a transport reconnect, a
	 * decode failure that dropped a frame, a consumer that fell behind. Not
	 * counted as a gap: the sequence numbers never said anything was wrong.
	 */
	MARKET_DATA_EXPORT void invalidate() noexcept;

	/// @brief Whether the book is seeded and in sequence.
	[[nodiscard]] MARKET_DATA_EXPORT sync_state state() const noexcept;

	/// @brief Whether events currently apply straight to the book.
	[[nodiscard]] MARKET_DATA_EXPORT bool is_streaming() const noexcept;

	/// @brief Last sequence number applied (or seeded); 0 before the first
	/// seed.
	[[nodiscard]] MARKET_DATA_EXPORT sequence_t last_sequence() const noexcept;

	/// @brief The sequence number the next event must cover. Meaningful only
	///        while @c streaming.
	[[nodiscard]] MARKET_DATA_EXPORT sequence_t expected_sequence() const noexcept;

	/// @brief Running counts of the decisions made so far.
	[[nodiscard]] MARKET_DATA_EXPORT const sequencer_stats &stats() const noexcept;

private:
	sequencer_stats stats_{};
	sequence_t last_sequence_ = 0;
	sync_state state_         = sync_state::awaiting_snapshot;
};

} // namespace exchange::market_data
