#pragma once
// The second seam: "a source of normalised trade prints".
//
// feed.hpp does this for depth. It is not reused here, and the reason is the
// whole point of the file.
//
// --- why the tape is not a third feed_message alternative ------------------
//
// The obvious move is to widen `feed_message` to
// `variant<depth_event, book_snapshot, trade_print>` and be done. It is wrong
// twice over.
//
// The ordering argument that puts a diff and a snapshot in one variant does not
// extend to a print. Those two share a stream *because the order between them
// is the managed-local-order-book procedure* - a snapshot arriving after the
// events it should have preceded is a resync, and depth_reconstructor can only
// tell the difference if it sees both in arrival order. A trade print has no
// such relationship with either: it does not seed the book, does not repair it,
// and applying one to an l2_book is not a thing that means anything. Putting it
// in the same variant would claim a sequencing relationship that does not
// exist.
//
// And it would break every existing consumer to do it. `feed_handler` requires
// on_event and on_snapshot; a third alternative makes every handler in the tree
// - depth_reconstructor included - incomplete, and `drive` would have to decide
// what to do with a message its handler cannot take. The cost lands on the
// depth path, which gained nothing.
//
// So: a parallel seam, deliberately narrow, reusing feed_status and feed_stop
// because *those* really are venue- and message-neutral. "end of feed" and
// "malformed frame" mean the same thing on a tape as on a book.

#include "feed.hpp"       // feed_status, feed_stop, is_clean
#include "fwd.hpp"
#include "normalised.hpp" // trade_print

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>

namespace exchange::market_data {

/// @brief The result of one pull from a tape: a print, or why there was not
///        one. @see feed_pull, which is the depth half of the same idea.
using trade_pull = std::expected<trade_print, feed_status>;

/**
 * @brief A source of normalised trade prints for one listing.
 *
 * Pull-shaped and statically dispatched for the reasons @ref depth_feed spells
 * out, which apply here unchanged. The contract is the same one, with the same
 * resumability requirement: any status other than @c feed_stop::exhausted must
 * leave the feed able to continue after the frame that failed.
 *
 * @note No type can satisfy both this and @ref depth_feed - @c next() would
 *       have to return two different types - which is what lets @c drive be
 *       overloaded on the two without ever being ambiguous.
 */
template <class F>
concept trade_feed = requires(F &feed) {
	{ feed.next() } -> std::same_as<trade_pull>;
};

/**
 * @brief Something that consumes trade prints.
 *
 * One method, because a tape has one message kind. The return type is
 * unconstrained for the reason @ref feed_handler gives: a consumer that wants
 * to answer something is not made more general by being forced to.
 */
template <class H>
concept trade_handler = requires(H &handler, trade_print print) {
	handler.on_trade(std::move(print));
};

/// @brief What one tape @c drive did, and why it stopped.
struct trade_run {
	std::uint64_t trades = 0; ///< Prints handed to the handler.
	/// @brief Why the loop ended. Always set - a run always has a reason.
	feed_status stop{};
};

/// @brief Whether the run ended by finishing rather than by failing.
[[nodiscard]] constexpr bool is_clean(const trade_run &run) noexcept {
	return is_clean(run.stop);
}

/**
 * @brief Pull @p feed into @p handler until it ends, fails, or reaches
 *        @p max_trades.
 *
 * @param feed The source of prints.
 * @param handler The consumer.
 * @param max_trades Stop after this many prints, or 0 for no bound.
 * @return The count, and the reason it stopped.
 *
 * @note Overloads the depth @c drive rather than taking a new name. The two are
 *       never ambiguous: their @c Feed parameters are constrained by concepts
 *       no single type can satisfy at once (@see trade_feed), so exactly one
 *       candidate ever survives constraint checking. A handler that implements
 *       all three callbacks - a session consuming both feeds - is therefore
 *       still unambiguous, because it is the feed that decides.
 * @note Stops at the first fault rather than skipping it, and for the same
 *       reason @c drive(Feed &, Handler &, std::uint64_t) does: continuing past
 *       a malformed print loses a trade id, which is the tape's only
 *       gap-detection signal. The caller resumes by calling again.
 */
template <trade_feed Feed, trade_handler Handler>
trade_run drive(Feed &feed, Handler &handler, std::uint64_t max_trades = 0) {
	trade_run run;
	for (;;) {
		if (max_trades != 0 && run.trades >= max_trades) {
			run.stop = feed_status{.reason   = feed_stop::limited,
								   .position = run.trades};
			return run;
		}

		trade_pull pulled = feed.next();
		if (!pulled) {
			run.stop = pulled.error();
			return run;
		}

		handler.on_trade(std::move(*pulled));
		++run.trades;
	}
}

/**
 * @brief A tape that is already decoded and already in memory.
 *
 * The venue-neutral half of offline tape replay, and the counterpart of
 * @ref replay_feed: a corpus decoded once, then driven repeatedly through the
 * same loop a live tape uses. Benchmarks want it because decoding has to stay
 * outside the timed region; tests want it because it is a feed with no venue,
 * no I/O and no failure mode behind it.
 *
 * @note Non-owning, and it copies each print on the way out - both for the
 *       reasons @ref replay_feed states. A @c trade_print is a handful of
 *       scalars, so the copy is a register shuffle rather than an allocation.
 */
class replay_trade_feed {
public:
	replay_trade_feed() = default;

	/// @brief Replay @p prints.
	/// @param prints The corpus; must outlive the feed.
	explicit replay_trade_feed(std::span<const trade_print> prints) noexcept
		: prints_(prints) {}

	/// @brief The next print, or @c feed_stop::exhausted at the end.
	[[nodiscard]] trade_pull next() {
		if (at_ >= prints_.size())
			return std::unexpected(
				feed_status{.reason   = feed_stop::exhausted,
							.position = static_cast<std::uint64_t>(at_)});
		return prints_[at_++];
	}

	/// @brief Start the corpus again.
	void rewind() noexcept { at_ = 0; }

	/// @brief Prints yielded so far.
	[[nodiscard]] std::size_t position() const noexcept { return at_; }

private:
	std::span<const trade_print> prints_;
	std::size_t at_ = 0;
};

static_assert(trade_feed<replay_trade_feed>);

} // namespace exchange::market_data
