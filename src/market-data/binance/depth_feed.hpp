#pragma once
// Binance's side of the feed seam: a JSONL capture, read as normalised depth.
//
// This is the whole of what market-data/feed.hpp asked for, filled in for one
// venue: the decoder, the reuse of its buffers, the venue-to-neutral mapping
// and the venue-to-neutral *error* mapping, behind a single next(). Everything
// above it - drive(), depth_reconstructor, the backtest harness - never names
// Binance again.
//
// The offline direction is deliberately the one built first. transport/ already
// records a `<symbol>@depth` stream to JSONL and reads it back, so a capture is
// the input the tree actually has, and a live feed is the same class with a
// socket where the line scan is. What is *not* here is that socket: a live feed
// also has to fetch REST snapshots on demand, which is I/O this module does not
// do and a coroutine this module does not own. @see transport/websocket.hpp.

#include "binance_depth.hpp"    // DepthParser, depth_parse_error
#include "fwd.hpp"
#include "market-data/feed.hpp" // feed_pull, depth_feed
#include "market-data/fwd.hpp"
#include "market-data/normalised.hpp"
#include "market_data_export.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace exchange::market_data::binance {

/**
 * @brief Turns one @c depthUpdate frame into a venue-neutral event.
 *
 * The venue-to-neutral step on its own, with the decoder's buffers held across
 * frames and the venue's parse errors already mapped onto @c feed_status. It is
 * separate from the feeds that use it because the frames arrive by more than
 * one route - a line of a JSONL capture, a WebSocket message - and only the
 * route differs. Whoever obtains the bytes owns the route; this owns the
 * meaning.
 *
 * @note Stateful (it amortises simdjson's buffers) and therefore not
 *       thread-safe: one decoder per consuming thread.
 */
class depth_frame_decoder {
public:
	/**
	 * @brief A decoder for one listing's precision.
	 * @param price_decimals Tick precision for the symbol.
	 * @param qty_decimals Step precision for the symbol.
	 */
	MARKET_DATA_EXPORT depth_frame_decoder(int price_decimals,
										   int qty_decimals);
	MARKET_DATA_EXPORT ~depth_frame_decoder();
	MARKET_DATA_EXPORT depth_frame_decoder(depth_frame_decoder &&) noexcept;
	MARKET_DATA_EXPORT depth_frame_decoder &
	operator=(depth_frame_decoder &&) noexcept;
	depth_frame_decoder(const depth_frame_decoder &)            = delete;
	depth_frame_decoder &operator=(const depth_frame_decoder &) = delete;

	/**
	 * @brief Decode @p frame into a neutral event.
	 * @param frame One @c depthUpdate JSON object. Borrowed; the returned event
	 *        owns its own levels and does not view into it.
	 * @param position Where the frame came from - a line number, a frame index
	 * - stamped into a failure so the caller need not re-attach it. 0 when the
	 * source has no position.
	 * @return The event, or @c feed_stop::malformed with the decoder's own
	 *         message. The detail is static text, as @c feed_status requires.
	 */
	[[nodiscard]] MARKET_DATA_EXPORT std::expected<depth_event, feed_status>
	decode(std::string_view frame, std::uint64_t position = 0);

	/// @brief Frames decoded successfully.
	[[nodiscard]] MARKET_DATA_EXPORT std::uint64_t frames() const noexcept;

	/// @brief Frames that failed to decode.
	[[nodiscard]] MARKET_DATA_EXPORT std::uint64_t malformed() const noexcept;

private:
	/// Owned so its structural-index and input buffers amortise across frames -
	/// the reason this is a class rather than a free function.
	DepthParser parser_;
	std::uint64_t frames_    = 0;
	std::uint64_t malformed_ = 0;
	int price_decimals_      = 0;
	int qty_decimals_        = 0;
};

/**
 * @brief A @ref depth_feed over a JSONL capture of @c depthUpdate frames.
 *
 * One JSON object per line, as @c transport::ws::capture writes it. Frames are
 * decoded lazily, one per @c next(), through a @c DepthParser this feed owns -
 * so the whole capture is never materialised and simdjson's buffers are
 * amortised across it.
 *
 * @par Why lazily, when parse_binance_depth_updates already exists
 * That function returns @c std::vector<DepthUpdate> for the whole file, which
 * is fine for a test corpus and wrong for a recording: a session's capture runs
 * to gigabytes, the decoded form is larger than the text, and none of it is
 * needed twice. Reading frame by frame also makes offline and live the same
 * shape, which is the point of the abstraction - a live feed could not
 * materialise anything even if it wanted to.
 *
 * @par The seed
 * A diff feed is meaningless without the snapshot it is replayed onto, so the
 * feed can carry one and hands it over first. It is taken already normalised
 * rather than as REST JSON, because the composition root usually has to read
 * that payload for its own reasons - deriving a listing's reference price from
 * the midpoint, say - and parsing it twice to satisfy an interface would be a
 * poor trade. @c normalise(DepthSnapshot) is the one call that bridges them.
 *
 * @warning @p jsonl is borrowed, not copied: the buffer must outlive the feed.
 *          That matches the free parse functions, which take a
 *          @c std::string_view for the same reason - a capture is read once
 *          into one string and nothing gains by copying it again.
 *
 * @note Stateful and not thread-safe: one capture, one consuming thread, one
 *       feed. @see DepthParser.
 */
class jsonl_depth_feed {
public:
	/**
	 * @brief Read @p jsonl as a diff feed, with no seeding snapshot.
	 *
	 * A feed with no seed never brings a reconstructor live on its own - every
	 * event buffers until a snapshot arrives from somewhere. That is a
	 * legitimate configuration (the caller may be about to fetch one) and it is
	 * also the shape of a capture replayed for its frames rather than its book.
	 * @param jsonl The capture; borrowed, must outlive the feed.
	 * @param price_decimals Tick precision for the symbol.
	 * @param qty_decimals Step precision for the symbol.
	 */
	MARKET_DATA_EXPORT jsonl_depth_feed(std::string_view jsonl,
										int price_decimals, int qty_decimals);

	/**
	 * @brief Read @p jsonl as a diff feed seeded by @p seed.
	 * @param seed The snapshot to hand over before any event; consumed.
	 * @param jsonl The capture; borrowed, must outlive the feed.
	 * @param price_decimals Tick precision for the symbol.
	 * @param qty_decimals Step precision for the symbol.
	 */
	MARKET_DATA_EXPORT jsonl_depth_feed(book_snapshot seed,
										std::string_view jsonl,
										int price_decimals, int qty_decimals);

	MARKET_DATA_EXPORT ~jsonl_depth_feed();
	MARKET_DATA_EXPORT jsonl_depth_feed(jsonl_depth_feed &&) noexcept;
	MARKET_DATA_EXPORT jsonl_depth_feed &
	operator=(jsonl_depth_feed &&) noexcept;
	jsonl_depth_feed(const jsonl_depth_feed &)            = delete;
	jsonl_depth_feed &operator=(const jsonl_depth_feed &) = delete;

	/**
	 * @brief The next message: the seed if it has not been handed over, then
	 *        one decoded frame per call.
	 *
	 * Blank lines are skipped rather than reported - a trailing newline is not
	 * a malformed frame.
	 *
	 * @return The message, @c feed_stop::exhausted at the end of the capture,
	 *         or @c feed_stop::malformed naming the 1-based line that failed
	 *         and why.
	 * @note Resumable, as @ref depth_feed requires: the line that failed has
	 *       already been stepped over when the status is returned, so a caller
	 *       that decides a corrupt frame is survivable simply pulls again. It
	 *       is not free to make that decision lightly - the frame's sequence
	 *       numbers went with it, so the sequencer will read a gap and the
	 *       replica will need a fresh snapshot.
	 */
	[[nodiscard]] MARKET_DATA_EXPORT feed_pull next();

	/// @brief Lines consumed so far, 1-based - the position a
	///        @c feed_stop::malformed reports.
	[[nodiscard]] std::uint64_t line() const noexcept { return line_; }

	/// @brief Frames successfully decoded and handed over.
	[[nodiscard]] std::uint64_t frames() const noexcept {
		return decoder_.frames();
	}

	/// @brief Lines that failed to decode. Non-zero means the capture is
	///        damaged, and every one of them is a sequence gap the replica had
	///        to be rebuilt from.
	[[nodiscard]] std::uint64_t malformed() const noexcept {
		return decoder_.malformed();
	}

	/// @brief Whether the seeding snapshot is still to be handed over.
	[[nodiscard]] bool has_pending_seed() const noexcept {
		return seed_.has_value();
	}

private:
	/// The venue-to-neutral half, shared with every other route a frame can
	/// arrive by. @see depth_frame_decoder
	depth_frame_decoder decoder_;
	/// Borrowed; see the class warning.
	std::string_view jsonl_;
	/// Handed over by the first next(), then cleared.
	std::optional<book_snapshot> seed_;
	/// Offset of the next unread line in jsonl_.
	std::size_t at_     = 0;
	std::uint64_t line_ = 0;
};

static_assert(depth_feed<jsonl_depth_feed>);

} // namespace exchange::market_data::binance
