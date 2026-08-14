#include "sequencer.hpp"

namespace exchange::market_data {

sequence_action depth_sequencer::observe(
	core::util::inclusive_range<sequence_t> sequence) noexcept {
	if (state_ == sync_state::awaiting_snapshot) {
		// Nothing to judge the event against yet. It is not necessarily stale —
		// the snapshot that follows may well need it — so it is the caller's to
		// keep.
		++stats_.buffered;
		return sequence_action::buffer;
	}

	// A frame whose range runs backwards cannot be placed in the sequence at
	// all. Treating it as a gap is the conservative reading: the book may
	// already have missed whatever the frame really covered.
	if (!sequence.is_ordered()) {
		++stats_.gaps;
		invalidate();
		return sequence_action::gap;
	}

	const sequence_t expected = expected_sequence();

	// Wholly behind us: the snapshot already included it, or the venue resent
	// it. Re-applying would write back sizes that have since moved.
	if (sequence.ends_before(expected)) {
		++stats_.discarded;
		return sequence_action::discard;
	}

	// Starts past what we are waiting for: the events covering [expected,
	// first) were lost and the book can no longer be reconciled by replay.
	if (sequence.begins_after(expected)) {
		++stats_.gaps;
		invalidate();
		return sequence_action::gap;
	}

	// Covers `expected`, so replay is unbroken. Starting strictly below it
	// means the event re-covers ids already applied; absolute level sizes make
	// that harmless, but a venue promising exact adjacency should never do it.
	if (sequence.begins_before(expected)) ++stats_.overlapped;

	last_sequence_ = sequence.last();
	++stats_.applied;
	return sequence_action::apply;
}

void depth_sequencer::seed(sequence_t snapshot_sequence) noexcept {
	last_sequence_ = snapshot_sequence;
	state_         = sync_state::streaming;
}

void depth_sequencer::invalidate() noexcept {
	// last_sequence_ is deliberately kept: it is the last number this replica
	// ever held, which is worth reporting even though nothing may be applied on
	// top of it until the next seed().
	state_ = sync_state::awaiting_snapshot;
}

[[nodiscard]] sync_state depth_sequencer::state() const noexcept {
	return state_;
}

[[nodiscard]] sequence_t depth_sequencer::last_sequence() const noexcept {
	return last_sequence_;
}

[[nodiscard]] sequence_t depth_sequencer::expected_sequence() const noexcept {
	return last_sequence_ + 1;
}

[[nodiscard]] const sequencer_stats &depth_sequencer::stats() const noexcept {
	return stats_;
}

[[nodiscard]] bool depth_sequencer::is_streaming() const noexcept {
	return state_ == sync_state::streaming;
}


} // namespace exchange::market_data
