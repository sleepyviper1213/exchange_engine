#pragma once
// The seam a second venue plugs into: "a source of normalised depth".
//
// normalised.hpp already says what a venue's data must be turned *into*, and
// binance/normalise.hpp does the turning. What was missing is the shape of the
// thing doing it, so every driver in the tree stopped one step short of
// venue-neutral: the CLI's replay, the backtest harness and the replay
// benchmark all spell the same four steps and name Binance in the middle of
// them.
//
//   slurp(path) -> binance::parse_binance_depth_updates -> binance::normalise
//               -> depth_reconstructor
//
// Only the middle two are venue knowledge, and only the last is what the driver
// is actually about. A feed is the first three collapsed behind one call, so a
// driver reads:
//
//   const feed_run result = drive(feed, reconstructor);
//
// and a second venue is a second class satisfying @ref depth_feed, changing
// nothing above it.
//
// --- what a feed is not ----------------------------------------------------
//
// It is not the steady-state hot path. A feed hands over depth_events, and
// building one materialises the frame's levels into vectors - unavoidable, and
// exactly what depth_reconstructor needs, because an event may have to be
// retained across a snapshot fetch. A consumer that only ever applies frames in
// sequence and never retains one should still use DepthParser::apply_update
// with sequence_of, which writes levels straight into the book and builds no
// event at all. This is the *managed* path: sequenced, gap-checked, resyncable,
// and priced accordingly.

#include "core/util/enum_string.hpp"
#include "fwd.hpp"
#include "normalised.hpp" // depth_event, book_snapshot

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

namespace exchange::market_data {

#define MARKET_DATA_FEED_STOP_LIST(X)                                          \
	X(exhausted, "end of feed")          /* normal: nothing left to read   */  \
	X(limited, "message limit reached")  /* normal: the caller's own bound */  \
	X(malformed, "malformed frame")      /* the source produced garbage    */  \
	X(unavailable, "source unavailable") /* the source could not be read   */

/**
 * @brief Why a feed stopped producing messages.
 *
 * Two of the four are not failures: ending is how a recording finishes, and
 * stopping at a bound is what the caller asked for. A driver that treated every
 * non-message as an error would report a clean replay as broken, which is why
 * @c feed_status::is_clean exists rather than a bare boolean return.
 */
enum class feed_stop : std::uint8_t {
	EXCHANGE_ENUM_VALUES(MARKET_DATA_FEED_STOP_LIST)
};

/// @brief What a @c feed_stop means, e.g. @c "malformed frame" - and with it
///        the fmt hook, so a feed_stop prints as that phrase.
EXCHANGE_ENUM_LABEL(feed_stop, describe, MARKET_DATA_FEED_STOP_LIST)

#undef MARKET_DATA_FEED_STOP_LIST

/**
 * @brief Why a feed stopped, and where.
 *
 * @c detail is always static text - a decoder's own message, or an offending
 * field name - never a view into the frame that failed, so it outlives the pull
 * that produced it. That is the contract
 * @c binance::depth_parse_error::context already carries, and it is here for
 * the same reason: a diagnostic that dangles is worse than none.
 */
struct feed_status {
	feed_stop reason = feed_stop::exhausted;
	/// @brief Static context - an offending field, a decoder's message - or
	///        empty, in which case @c reason's own label is the whole story.
	std::string_view detail;
	/// @brief Where in the source: a 1-based frame or line index, or the count
	///        of messages already yielded. 0 when the source has no position.
	std::uint64_t position = 0;

	/// @brief Whether the feed ended for an ordinary reason rather than a
	///        fault. @see feed_stop
	[[nodiscard]] constexpr bool is_clean() const noexcept {
		return reason == feed_stop::exhausted || reason == feed_stop::limited;
	}
};

/**
 * @brief One thing a feed hands over: a diff, or the snapshot that seeds one.
 *
 * One ordered stream rather than two sources, because the order between the two
 * *is* the managed-local-order-book procedure - a snapshot that arrives after
 * the events it should have preceded is a resync, and @c depth_reconstructor
 * can only tell the difference if it sees them in the order they arrived.
 * Splitting them into @c next_event() and @c next_snapshot() would hand that
 * ordering to the caller, which is exactly where the bug lives.
 *
 * The diff comes first in the alternative list so a default-constructed message
 * is an empty event: the common case, and the one no book is corrupted by.
 */
using feed_message = std::variant<depth_event, book_snapshot>;

/// @brief The result of one pull: a message, or why there was not one.
using feed_pull = std::expected<feed_message, feed_status>;

/**
 * @brief A source of normalised depth for one listing.
 *
 * @par Why a concept and not a base class
 * @c next() is called once per frame, which is the per-message path this
 * project reserves for static dispatch. The indirection would also buy a
 * flexibility nobody spends: the set of venues a deployment speaks is fixed
 * when it is built.
 *
 * @par Why pull and not push
 * A push feed owns the loop, so a consumer that wants to stop - the backtest's
 * @c --events bound, an operator's shutdown - has to signal that back through a
 * return value somebody will forget to check. Pulling puts the loop where the
 * decision is. It also makes a recording and a live socket the same shape: one
 * blocks inside @c next() and the other does not, and nothing above cares.
 *
 * @par The contract
 * - @c next() returns the next message, or a @c feed_status saying why not.
 * - @c feed_stop::exhausted means the source has genuinely ended; further pulls
 *   must keep saying so rather than resuming.
 * - Any other status must leave the feed @em resumable - the next pull
 *   continues after the frame that failed. A caller may then choose to carry
 *   on, and it is a real choice: a skipped frame is a sequence gap, which the
 *   sequencer will see and resync from. @c drive makes that choice for nobody.
 */
template <class F>
concept depth_feed = requires(F &feed) {
	{ feed.next() } -> std::same_as<feed_pull>;
};

/**
 * @brief Something that consumes a feed's two message kinds.
 *
 * @c depth_reconstructor satisfies this as written, which is the point: the
 * handler a driver is written against is the component that already exists, not
 * an interface it was made to implement. Return types are deliberately
 * unconstrained - the reconstructor answers a @c sequence_action and a @c bool,
 * a bridge answers something else again, and a driver that ignored both would
 * be no more general for having demanded them.
 */
template <class H>
concept feed_handler =
	requires(H &handler, depth_event event, book_snapshot snapshot) {
		handler.on_event(std::move(event));
		handler.on_snapshot(std::move(snapshot));
	};

/// @brief What one @c drive did, and why it stopped.
struct feed_run {
	std::uint64_t events    = 0; ///< Diffs handed to the handler.
	std::uint64_t snapshots = 0; ///< Snapshots handed to the handler.
	/// @brief Why the loop ended. Always set - a run always has a reason.
	feed_status stop{};

	/// @brief Whether the run ended by finishing rather than by failing.
	[[nodiscard]] constexpr bool is_clean() const noexcept {
		return stop.is_clean();
	}
};

/**
 * @brief Pull @p feed into @p handler until it ends, fails, or reaches
 *        @p max_events.
 *
 * @param feed The source; pulled from until it stops.
 * @param handler The consumer; @c depth_reconstructor is one.
 * @param max_events Stop after this many diff events, or 0 for no bound.
 *        Snapshots are not counted: they are the feed repairing itself rather
 *        than market activity, and "the first N events of this recording"
 *        should mean the same thing whether or not it had to resync.
 * @return The counts, and the reason it stopped.
 *
 * @note Stops at the first fault rather than skipping past it. Continuing has a
 *       consequence - the skipped frame is a sequence gap, so the replica dies
 *       and needs a snapshot - which makes it the caller's decision, and the
 *       caller acts on it by calling @c drive again on the same feed. @see
 *       depth_feed on why that is guaranteed to work.
 */
template <depth_feed Feed, feed_handler Handler>
feed_run drive(Feed &feed, Handler &handler, std::uint64_t max_events = 0) {
	feed_run run;
	for (;;) {
		if (max_events != 0 && run.events >= max_events) {
			run.stop = feed_status{.reason   = feed_stop::limited,
								   .position = run.events};
			return run;
		}

		feed_pull pulled = feed.next();
		if (!pulled) {
			run.stop = pulled.error();
			return run;
		}

		if (auto *event = std::get_if<depth_event>(&*pulled)) {
			handler.on_event(std::move(*event));
			++run.events;
		} else {
			handler.on_snapshot(std::move(std::get<book_snapshot>(*pulled)));
			++run.snapshots;
		}
	}
}

/**
 * @brief A feed over depth that is already decoded and already in memory.
 *
 * The venue-neutral half of offline replay: a corpus decoded once, then driven
 * through the same @c drive loop a live feed uses. Benchmarks want it because
 * decoding has to stay outside the timed region; tests want it because it is a
 * feed with no venue, no I/O and no failure mode behind it.
 *
 * @note Non-owning, and it copies each event on the way out. Both follow from
 *       replaying one corpus more than once: moving events out would consume
 *       it, and holding a second decoded copy costs more than the pull does. A
 *       caller timing reconstruction should account for that copy or drive the
 *       reconstructor directly - this replays a corpus, it does not measure the
 *       cost of one frame.
 */
class replay_feed {
public:
	replay_feed() = default;

	/// @brief Replay @p events, with no seeding snapshot.
	/// @param events The corpus; must outlive the feed.
	explicit replay_feed(std::span<const depth_event> events) noexcept
		: events_(events) {}

	/**
	 * @brief Replay @p events, handing over @p seed first.
	 * @param seed The snapshot to yield before any event; must outlive the
	 *        feed.
	 * @param events The corpus; must outlive the feed.
	 */
	replay_feed(const book_snapshot &seed,
				std::span<const depth_event> events) noexcept
		: seed_(&seed), events_(events) {}

	/// @brief The next message, or @c feed_stop::exhausted at the end.
	[[nodiscard]] feed_pull next() {
		if (seed_ != nullptr && !seeded_) {
			seeded_ = true;
			return feed_message{*seed_};
		}
		if (at_ >= events_.size())
			return std::unexpected(
				feed_status{.reason   = feed_stop::exhausted,
							.position = static_cast<std::uint64_t>(at_)});
		return feed_message{events_[at_++]};
	}

	/// @brief Start the corpus again, snapshot included.
	void rewind() noexcept {
		at_     = 0;
		seeded_ = false;
	}

	/// @brief Events yielded so far.
	[[nodiscard]] std::size_t position() const noexcept { return at_; }

private:
	const book_snapshot *seed_ = nullptr;
	std::span<const depth_event> events_;
	std::size_t at_ = 0;
	bool seeded_    = false;
};

static_assert(depth_feed<replay_feed>);

} // namespace exchange::market_data
