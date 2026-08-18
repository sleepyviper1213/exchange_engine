#include "reconstructor.hpp"

#include "normalised.hpp"
#include "sequencer.hpp"

#include <utility>

namespace exchange::market_data {

void depth_reconstructor::retain(depth_event event) {
	if (options_.max_pending != 0 && pending_.size() >= options_.max_pending) {
		// Evict from the front: a snapshot that eventually arrives covers a
		// higher sequence than these, so they would be discarded on replay
		// anyway. Dropping the newest instead would tear a hole in the middle
		// of the buffer and guarantee a gap.
		pending_.pop_front();
		++dropped_;
	}
	pending_.emplace_back(std::move(event));
}

void depth_reconstructor::drop_replica() noexcept {
	sequencer_.invalidate();
	book_.clear();
}

bool depth_reconstructor::check_cross() noexcept {
	if (!book_.is_crossed()) return false;
	// Counted unconditionally: a venue where crossing is normal should still be
	// able to see how often it happens before arming the reaction.
	++crosses_;
	return options_.resync_on_cross;
}

sequence_action depth_reconstructor::on_event(depth_event event) {
	const auto action = sequencer_.observe(event.sequence);
	switch (action) {
	case sequence_action::buffer: retain(std::move(event)); break;
	case sequence_action::apply:
		apply(book_, event);
		// Checked here rather than inside apply(): one event's bids are all
		// written before its asks, so the book is legitimately crossed part-way
		// through an event that ends consistent.
		if (check_cross()) {
			drop_replica();
			pending_.clear();
			retain(std::move(event));
			return sequence_action::gap;
		}
		break;
	case sequence_action::discard: break;
	case sequence_action::gap:
		// observe() has already dropped the sequencer back to
		// awaiting_snapshot. The book is a replica of a sequence that no longer
		// exists, so it goes; the buffered events (if any) predate the gap and
		// are equally useless, but this event does not - it is the oldest thing
		// the next snapshot might bridge to.
		book_.clear();
		pending_.clear();
		retain(std::move(event));
		break;
	}
	return action;
}

bool depth_reconstructor::on_snapshot(book_snapshot snapshot) {
	// A snapshot that predates a live replica can only move it backwards: it
	// would overwrite the book with older depth and rewind the expected
	// sequence, while leaving is_alive() true for a consumer to trust. Two fetches
	// outstanding with the older one landing second is an ordinary way to get
	// here, not a pathological one, so it is refused rather than left for the
	// gap that some later event would eventually trip.
	if (is_alive() && snapshot.sequence <= sequencer_.last_sequence()) {
		++stale_snapshots_;
		snapshot_pending_ = false;
		// True because the replica *is* live and correct - just not thanks to
		// this snapshot. The caller asked whether it may read book(); it may.
		return true;
	}

	snapshot_pending_            = false;
	const sequence_t sequence = snapshot.sequence;
	reset(book_, snapshot);
	sequencer_.seed(sequence);

	// Replay what was buffered while the snapshot was in flight. Everything at
	// or below `sequence` is already baked into it and drops out here.
	while (!pending_.empty()) {
		const depth_event &event = pending_.front();
		const auto action        = sequencer_.observe(event.sequence);
		if (action == sequence_action::gap) {
			// The snapshot is older than this event and nothing covers the
			// numbers between them. Whatever was applied above came after the
			// hole, so the book is wrong; the un-bridged events stay buffered
			// for the newer snapshot the caller must now fetch.
			book_.clear();
			return false;
		}
		if (action == sequence_action::apply) apply(book_, event);
		pending_.pop_front();
	}

	// The seed itself can be inconsistent: a REST snapshot read while the venue
	// was mid-update comes back torn, and every sequence number in it looks
	// fine. This is the only signal that says otherwise.
	if (check_cross()) {
		drop_replica();
		return false;
	}
	return true;
}

void depth_reconstructor::invalidate() noexcept {
	snapshot_pending_ = false;
	drop_replica();
}

} // namespace exchange::market_data
