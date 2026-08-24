#pragma once
// Shared by the feed-seam suites: a feed with nothing behind it, a handler that
// only remembers, and the two message shapes to script them with.
//
// The stub feed is the point of the exercise as much as it is a test aid - if
// `depth_feed` could not be satisfied in twenty lines by something with no
// venue, no I/O and no state machine, the concept would be asking for too much.
//
// Every name here is at global scope, which is the convention (see testing.md)
// and which makes the whole of `order_test` one namespace: a name that collides
// with another fixture's is an ODR violation, not a compile error. Hence the
// `_feed_` in `recording_feed_handler` - `recording_handler` was already taken
// by test/trading-engine/event/event_dispatcher/, and the duplicate linked
// cleanly and then corrupted the heap, because the two classes' implicitly
// inline destructors mangle identically and the linker keeps one of them.

#include "market-data/feed.hpp"
#include "market-data/normalised.hpp"
#include "orders/types.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>
#include <vector>

using exchange::price_t;
using exchange::quantity_t;
using exchange::market_data::book_snapshot;
using exchange::market_data::depth_event;
using exchange::market_data::feed_message;
using exchange::market_data::feed_pull;
using exchange::market_data::feed_status;
using exchange::market_data::feed_stop;
using exchange::market_data::sequence_t;
using exchange::market_data::timestamp;

/// @brief One bid level changing at a single sequence number - enough to say
///        which event reached a handler, and in what order.
inline depth_event event_at(sequence_t sequence, price_t price = 100,
							quantity_t size = 1) {
	return depth_event{{sequence, sequence}, timestamp{}, {{price, size}}, {}};
}

/// @brief A two-sided seed covering everything up to @p sequence.
inline book_snapshot snapshot_at(sequence_t sequence) {
	return book_snapshot{sequence, timestamp{}, {{100, 1}}, {{200, 1}}};
}

/// @brief A feed that hands over exactly the pulls it was handed, in order,
///        then reports exhaustion for ever after.
///
/// Scripting the *pulls* rather than the messages is what lets a suite place a
/// fault mid-stream and check what a driver does either side of it - the one
/// thing a feed over a well-formed corpus can never show.
class scripted_feed {
public:
	scripted_feed() = default;

	explicit scripted_feed(std::vector<feed_pull> script)
		: script_(std::move(script)) {}

	[[nodiscard]] feed_pull next() {
		if (at_ >= script_.size())
			return std::unexpected(
				feed_status{.reason = feed_stop::exhausted, .position = at_});
		return script_[at_++];
	}

	/// @brief How many times a driver asked for a message.
	[[nodiscard]] std::size_t pulls() const noexcept { return at_; }

private:
	std::vector<feed_pull> script_;
	std::size_t at_ = 0;
};

static_assert(exchange::market_data::depth_feed<scripted_feed>);

/// @brief A handler that records what it was given and does nothing with it.
struct recording_feed_handler {
	std::vector<sequence_t> events;    ///< First sequence of each diff seen.
	std::vector<sequence_t> snapshots; ///< Sequence of each snapshot seen.

	void on_event(depth_event event) {
		events.push_back(event.sequence.first());
	}

	void on_snapshot(book_snapshot snapshot) {
		snapshots.push_back(snapshot.sequence);
	}
};

static_assert(exchange::market_data::feed_handler<recording_feed_handler>);

/// @brief Wrap @p event as a successful pull.
inline feed_pull pull_of(depth_event event) {
	return feed_pull{feed_message{std::move(event)}};
}

/// @brief Wrap @p snapshot as a successful pull.
inline feed_pull pull_of(book_snapshot snapshot) {
	return feed_pull{feed_message{std::move(snapshot)}};
}

/// @brief A failed pull with @p reason at @p position.
inline feed_pull pull_failing(feed_stop reason, std::uint64_t position = 0) {
	return feed_pull{
		std::unexpected(feed_status{.reason = reason, .position = position})};
}
