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

sequence_action depth_reconstructor::on_event(depth_event event) {
	const auto action = sequencer_.observe(event.sequence);
	switch (action) {
	case sequence_action::buffer: retain(std::move(event)); break;
	case sequence_action::apply: apply(book_, event); break;
	case sequence_action::discard: break;
	case sequence_action::gap:
		// observe() has already dropped the sequencer back to
		// awaiting_snapshot. The book is a replica of a sequence that no longer
		// exists, so it goes; the buffered events (if any) predate the gap and
		// are equally useless, but this event does not — it is the oldest thing
		// the next snapshot might bridge to.
		book_.clear();
		pending_.clear();
		retain(std::move(event));
		break;
	}
	return action;
}

bool depth_reconstructor::on_snapshot(book_snapshot snapshot) {
	const std::uint64_t sequence = snapshot.sequence;
	reset(book_, std::move(snapshot));
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
	return true;
}

void depth_reconstructor::invalidate() noexcept {
	sequencer_.invalidate();
	book_.clear();
}

} // namespace exchange::market_data
