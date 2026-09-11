#pragma once
// Binance's side of the tape seam: a JSONL capture, read as normalised prints.
//
// The mirror of depth_feed.hpp, and deliberately the smaller file - a tape has
// no snapshot to reconcile against and no sequence to repair, so everything
// depth_feed.hpp has to say about seeding and resync simply has no counterpart
// here. What is left is the same three-part split: the decoder, the reuse of
// its buffers, and the venue-to-neutral mapping, behind a single next().
//
// What is *not* here, exactly as with depth, is the socket. A live tape is this
// class with a WebSocket read where the line scan is; obtaining the bytes is
// transport's job. @see transport/websocket.hpp.

#include "binance_trade.hpp"          // trade_parser, trade_parse_error
#include "detail/frame_decode.hpp"    // detail::decode_tally - a member
#include "fwd.hpp"
#include "market_data/fwd.hpp"
#include "market_data/normalised.hpp" // trade_print
#include "market_data/trade_feed.hpp" // trade_pull, trade_feed
#include "market_data_export.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

namespace exchange::market_data::binance {

/**
 * @brief Turns one @c trade frame into a venue-neutral print.
 *
 * The venue-to-neutral step on its own, with the decoder's buffers held across
 * frames and the venue's parse errors already mapped onto @c feed_status.
 * Separate from the feeds that use it because the frames arrive by more than
 * one route - a line of a JSONL capture, a WebSocket message - and only the
 * route differs. Whoever obtains the bytes owns the route; this owns the
 * meaning. @see depth_frame_decoder, which draws the same line.
 *
 * @note Stateful (it amortises simdjson's buffers) and therefore not
 *       thread-safe: one decoder per consuming thread.
 */
class trade_frame_decoder {
public:
	/**
	 * @brief A decoder for one listing's precision.
	 * @param price_decimals Tick precision for the symbol.
	 * @param qty_decimals Step precision for the symbol.
	 */
	MARKET_DATA_EXPORT trade_frame_decoder(int price_decimals,
										   int qty_decimals);
	MARKET_DATA_EXPORT ~trade_frame_decoder();
	MARKET_DATA_EXPORT trade_frame_decoder(trade_frame_decoder &&) noexcept;
	MARKET_DATA_EXPORT trade_frame_decoder &
	operator=(trade_frame_decoder &&) noexcept;
	trade_frame_decoder(const trade_frame_decoder &)            = delete;
	trade_frame_decoder &operator=(const trade_frame_decoder &) = delete;

	/**
	 * @brief Decode @p frame into a neutral print.
	 * @param frame One @c trade JSON object. Borrowed; the returned print holds
	 *        no view into it - it is scalars only.
	 * @param position Where the frame came from - a line number, a frame index
	 *        - stamped into a failure so the caller need not re-attach it. 0
	 *        when the source has no position.
	 * @param ingress When this process took delivery of @p frame, copied onto
	 *        the returned print. Default-constructed - the "nobody stamped
	 *        this" value - for a source where arrival time means nothing.
	 * @return The print, or @c feed_stop::malformed with the decoder's own
	 *         message. The detail is static text, as @c feed_status requires.
	 *
	 * @par Why the stamp is passed in rather than read here
	 * The same reason @c depth_frame_decoder::decode states: this is not where
	 * the bytes arrived. Reading the clock here would fold the socket read and
	 * the coroutine resume into the decode cost, and leave whoever owns the
	 * socket unable to say otherwise.
	 */
	[[nodiscard]] MARKET_DATA_EXPORT std::expected<trade_print, feed_status>
	decode(std::string_view frame, std::uint64_t position = 0,
		   core::chrono::ingress_time ingress = {});

	/// @brief Frames decoded successfully.
	[[nodiscard]] MARKET_DATA_EXPORT std::uint64_t frames() const noexcept;

	/// @brief Frames that failed to decode.
	[[nodiscard]] MARKET_DATA_EXPORT std::uint64_t malformed() const noexcept;

private:
	/// Owned so its structural-index and input buffers amortise across frames -
	/// the reason this is a class rather than a free function.
	trade_parser parser_;
	/// The running counts, held together because decode_frame advances
	/// them together. @see detail::decode_tally
	detail::decode_tally tally_{};
	int price_decimals_ = 0;
	int qty_decimals_   = 0;
};

/**
 * @brief A @ref trade_feed over a JSONL capture of @c trade frames.
 *
 * One JSON object per line, as @c transport::ws::capture writes it. Frames are
 * decoded lazily, one per @c next(), through a @c trade_parser this feed owns -
 * so the whole tape is never materialised and simdjson's buffers are amortised
 * across it. That matters more here than on the depth side: a busy symbol
 * prints tens of thousands of trades a minute, and @c parse_binance_trades
 * would hold every one of them at once.
 *
 * @warning @p jsonl is borrowed, not copied: the buffer must outlive the feed.
 *
 * @note Stateful and not thread-safe: one capture, one consuming thread, one
 *       feed.
 */
class jsonl_trade_feed {
public:
	/**
	 * @brief Read @p jsonl as a tape.
	 * @param jsonl The capture; borrowed, must outlive the feed.
	 * @param price_decimals Tick precision for the symbol.
	 * @param qty_decimals Step precision for the symbol.
	 */
	MARKET_DATA_EXPORT jsonl_trade_feed(std::string_view jsonl,
										int price_decimals, int qty_decimals);

	MARKET_DATA_EXPORT ~jsonl_trade_feed();
	MARKET_DATA_EXPORT jsonl_trade_feed(jsonl_trade_feed &&) noexcept;
	MARKET_DATA_EXPORT jsonl_trade_feed &
	operator=(jsonl_trade_feed &&) noexcept;
	jsonl_trade_feed(const jsonl_trade_feed &)            = delete;
	jsonl_trade_feed &operator=(const jsonl_trade_feed &) = delete;

	/**
	 * @brief The next print: one decoded frame per call.
	 *
	 * Blank lines are skipped rather than reported - a trailing newline is not
	 * a malformed frame.
	 *
	 * @return The print, @c feed_stop::exhausted at the end of the capture, or
	 *         @c feed_stop::malformed naming the 1-based line that failed and
	 *         why.
	 * @note Resumable, as @ref trade_feed requires: the failing line has
	 *       already been stepped over when the status is returned, so a caller
	 *       that decides a corrupt frame is survivable simply pulls again. It
	 *       is not a free choice - the frame's trade id went with it, so a
	 *       consumer checking id continuity will read a gap it cannot repair,
	 *       there being no snapshot to resync a tape from.
	 * @note Prints come back with no ingress stamp, deliberately. A capture's
	 *       frames arrived when they were recorded, not when they are read
	 *       back, so stamping them here would report the replay loop's own
	 *       speed as a reaction time - a number that gets *better* the further
	 *       the harness drifts from the live path it stands in for.
	 *       @see core::chrono::ingress_clock
	 */
	[[nodiscard]] MARKET_DATA_EXPORT trade_pull next();

	/// @brief Lines consumed so far, 1-based - the position a
	///        @c feed_stop::malformed reports.
	[[nodiscard]] std::uint64_t line() const noexcept { return line_; }

	/// @brief Frames successfully decoded and handed over.
	[[nodiscard]] std::uint64_t frames() const noexcept {
		return decoder_.frames();
	}

	/// @brief Lines that failed to decode. Non-zero means the capture is
	///        damaged, and every one of them is a trade the tape can no longer
	///        account for.
	[[nodiscard]] std::uint64_t malformed() const noexcept {
		return decoder_.malformed();
	}

private:
	/// The venue-to-neutral half, shared with every other route a frame can
	/// arrive by. @see trade_frame_decoder
	trade_frame_decoder decoder_;
	/// Borrowed; see the class warning.
	std::string_view jsonl_;
	/// Offset of the next unread line in jsonl_.
	std::size_t at_     = 0;
	std::uint64_t line_ = 0;
};

static_assert(frame_decoder<trade_frame_decoder, trade_print>);
static_assert(trade_feed<jsonl_trade_feed>);

} // namespace exchange::market_data::binance
