#pragma once
// The whole managed-local-order-book procedure in one object: a book, the
// sequencer that guards it, and the buffer events wait in while unsynced.
//
// depth_sequencer answers "may I apply this?" but holds no data; l2_book holds
// the data but sequences nothing. Every consumer of a diff feed needs both plus
// the same buffer-and-replay bookkeeping between them, which is exactly the
// part that is easy to get subtly wrong. It lives here once.
//
// The caller's whole job becomes two calls and one branch:
//
//   if (recon.needs_snapshot()) fetch_snapshot();   // asynchronously
//   ...
//   recon.on_event(normalise(decoded_frame));       // per feed frame
//   recon.on_snapshot(normalise(rest_payload));     // when the fetch lands
//
// and reading recon.book() only while recon.live().

#include "fwd.hpp"
#include "l2_book.hpp"
#include "market_data_export.hpp"
#include "normalised.hpp"
#include "sequencer.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>

namespace exchange::market_data {

/// @brief Tunables for @c depth_reconstructor.
struct reconstructor_options {
	/**
	 * @brief Cap on events held while unsynced; 0 means unbounded.
	 *
	 * The buffer only grows while a snapshot is outstanding, so it is bounded
	 * in practice by the fetch latency — but a snapshot request that fails and
	 * is never retried would otherwise grow it without limit. At the cap the
	 * oldest event is dropped, which is safe: a snapshot fetched later covers a
	 * higher sequence number, so the events dropped first are the ones it would
	 * have discarded anyway.
	 */
	std::size_t max_pending = 4096;

	/**
	 * @brief Force a resync when a completed event or snapshot leaves the book
	 *        crossed (@c l2_book::crossed).
	 *
	 * On by default, for the same reason a sequence gap clears the book: a
	 * replica that is visibly wrong is worse than no replica, because
	 * @c live() is what a consumer trusts. A cross is the only wrongness
	 * detectable without a second data source.
	 *
	 * Turn it off for a venue whose feed legitimately publishes a locked or
	 * crossed book between events — the cost of a false positive is a REST
	 * snapshot fetch and a stall, which on a busy symbol is not cheap.
	 * @c crosses() keeps counting either way, so the check can be observed
	 * before it is armed.
	 */
	bool resync_on_cross = true;
};

/**
 * @brief A live L2 replica of one venue book, kept honest by gap detection.
 *
 * Owns an @c l2_book and only ever lets in-sequence events reach it. On a gap
 * the book is cleared rather than left silently wrong, and the reconstructor
 * goes back to buffering until the caller supplies a fresh snapshot — so
 * @c book() is either a correct replica or explicitly not live.
 *
 * @note Not thread-safe: one feed, one consuming thread, one reconstructor.
 */
class MARKET_DATA_EXPORT depth_reconstructor {
public:
	depth_reconstructor() = default;
	explicit depth_reconstructor(reconstructor_options options) noexcept
		: options_(options) {}

	/**
	 * @brief Feed one normalised diff event.
	 *
	 * Buffers it, applies it, drops it as stale, or reports a gap — see
	 * @c depth_sequencer::observe for the rule. On a gap the book is cleared,
	 * the buffer is emptied and this event starts a fresh one, because it may
	 * be bridged by the snapshot the caller is now obliged to fetch.
	 *
	 * @c gap is also returned when an applied event leaves the book crossed and
	 * @c reconstructor_options::resync_on_cross is set. The sequence was intact
	 * in that case, so it is not counted in @c stats().gaps — @c crosses() is.
	 * The two are the same instruction to the caller (this replica is dead,
	 * fetch a snapshot) arrived at by different evidence.
	 *
	 * @param event The decoded event; consumed.
	 * @return What was done with it. Anything but @c apply leaves @c book()
	 *         unchanged; @c gap additionally means it is no longer live.
	 */
	sequence_action on_event(depth_event event);

	/**
	 * @brief Seed (or re-seed) from a snapshot, then replay the buffer onto it.
	 *
	 * Buffered events wholly covered by the snapshot are dropped and the rest
	 * applied in order. If the snapshot is older than the buffer's oldest event
	 * — nothing bridges @c sequence + 1 — the book cannot be trusted, so it is
	 * cleared and the un-bridged events are kept for the next attempt.
	 *
	 * @par Snapshots that would move a live replica backwards
	 * Ignored, and reported as success. A snapshot only ever helps a replica
	 * that has fallen out of sequence; applied to a live one that has already
	 * moved past it, it would silently rewind both the book and the expected
	 * sequence while leaving @c live() true. That is reachable in ordinary
	 * operation — two fetches outstanding and the older one lands second — so
	 * it is refused here rather than left to be repaired by the gap that the
	 * next event would eventually trip. @c stale_snapshots() counts them.
	 *
	 * @param snapshot The full depth; consumed.
	 * @return @c true if the book is now live; @c false if a newer snapshot is
	 *         needed.
	 */
	bool on_snapshot(book_snapshot snapshot);

	/**
	 * @brief Note that a snapshot fetch is now in flight.
	 *
	 * Clears @c needs_snapshot until the fetch resolves, so a caller that polls
	 * it per event issues one request rather than one per event for the whole
	 * round trip. Purely advisory bookkeeping — the reconstructor performs no
	 * I/O and cannot observe the fetch itself.
	 */
	void snapshot_requested() noexcept { snapshot_pending_ = true; }

	/**
	 * @brief Note that the in-flight fetch failed, so another is needed.
	 *
	 * Without this a caller whose request errors would leave
	 * @c needs_snapshot false forever and the replica dead but silent.
	 */
	void snapshot_failed() noexcept { snapshot_pending_ = false; }

	/// @brief Declare the replica stale (transport reconnect, dropped frame).
	///        Clears the book; events buffer again until the next snapshot.
	/// @note Abandons any in-flight snapshot: an explicit invalidate means the
	///       caller decided the world changed underneath it, and a fetch issued
	///       before that decision is not evidence about the world after it.
	void invalidate() noexcept;

	/// @brief The replica. Meaningful only while @c live().
	[[nodiscard]] const l2_book &book() const noexcept { return book_; }

	/// @brief Whether the book is a seeded, in-sequence replica.
	[[nodiscard]] bool live() const noexcept { return sequencer_.streaming(); }

	/// @brief Whether the caller owes this reconstructor a snapshot *and* is
	///        not already fetching one. @see snapshot_requested
	[[nodiscard]] bool needs_snapshot() const noexcept {
		return !live() && !snapshot_pending_;
	}

	/// @brief Whether a fetch the caller announced has yet to resolve.
	[[nodiscard]] bool snapshot_in_flight() const noexcept {
		return snapshot_pending_;
	}

	/// @brief Events currently held waiting for a snapshot.
	[[nodiscard]] std::size_t pending() const noexcept {
		return pending_.size();
	}

	/// @brief Events discarded because the pending buffer hit its cap.
	[[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }

	/// @brief Completed events or snapshots that left the book crossed.
	///        Counted whether or not @c resync_on_cross acted on them.
	[[nodiscard]] std::uint64_t crosses() const noexcept { return crosses_; }

	/// @brief Snapshots ignored for predating a live replica.
	[[nodiscard]] std::uint64_t stale_snapshots() const noexcept {
		return stale_snapshots_;
	}

	/// @brief Last sequence number applied (or seeded).
	[[nodiscard]] std::uint64_t last_sequence() const noexcept {
		return sequencer_.last_sequence();
	}

	/// @brief Feed-health counters — @c gaps above all.
	[[nodiscard]] const sequencer_stats &stats() const noexcept {
		return sequencer_.stats();
	}

private:
	/// Retain @p event, evicting the oldest if that would exceed the cap.
	void retain(depth_event event);

	/// Tear the replica down and go back to buffering: what a gap, a failed
	/// bridge and a detected cross all reduce to.
	void drop_replica() noexcept;

	/// Count a cross if the book is crossed, and say whether to act on it.
	[[nodiscard]] bool check_cross() noexcept;

	l2_book book_;
	depth_sequencer sequencer_;
	/// Deque, not vector: the replay drains from the front and the cap evicts
	/// from the front, and neither should be an O(n) shift.
	std::deque<depth_event> pending_;
	reconstructor_options options_{};
	std::uint64_t dropped_         = 0;
	std::uint64_t crosses_         = 0;
	std::uint64_t stale_snapshots_ = 0;
	bool snapshot_pending_         = false;
};

} // namespace exchange::market_data
