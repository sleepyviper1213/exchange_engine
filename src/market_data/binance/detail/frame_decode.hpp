#pragma once
// The body every one of this venue's frame decoders has.
//
// `depth_frame_decoder::decode` and `trade_frame_decoder::decode` were the same
// fifteen lines twice - parse, count the failure and wrap it in a feed_status,
// count the success, normalise, stamp the arrival time - differing in three
// tokens: which parse method, which normalise overload, which payload type. So
// were their `detail_of` helpers, which differed only in the error type they
// took.
//
// Private to market_data/binance, and included only from its sources. It is a
// function template taking the two varying steps as callables rather than a
// CRTP base, for the reason `market_data::feed.hpp` gives for preferring a
// concept over a base class on this path: the decoders sit behind an exported
// ABI, and a shared base would have to put their parsers' surface into a public
// header and be exported per derived type to satisfy MSVC. The interface half
// of the unification is `market_data::frame_decoder`; this is the
// implementation half, and neither needs inheritance.
//
// @see detail/jsonl_frame.hpp, which templates read_optional_u64 on the error
// type for the same reason.

#include "core/chrono/ingress.hpp"
#include "market_data/feed.hpp" // feed_status, feed_stop

#include <cstdint>
#include <expected>
#include <string_view>
#include <type_traits>
#include <utility>

namespace exchange::market_data::binance::detail {

/**
 * @brief What to put in a @c feed_status for @p error.
 *
 * Every parse error this venue produces is a code plus an optional context
 * string, so the choice is the same one each time: the context where the
 * decoder had something specific to say, and the code's own label where it did
 * not.
 *
 * @tparam Error The decoder's parse-error aggregate. Templated rather than
 *         written per decoder, which is what the two copies were.
 * @return A view with static storage duration either way - @c message returns a
 *         generated label and @c context is set from a literal, so nothing here
 *         can view into the frame. That is what @c feed_status::detail
 *         promises.
 */
template <typename Error>
[[nodiscard]] constexpr std::string_view
detail_of(const Error &error) noexcept {
	return error.context.empty() ? message(error.code) : error.context;
}

/// @brief A decoder's running counts. One member instead of two, so a decoder
///        declares the pair once and @c decode_frame takes it whole.
struct decode_tally {
	std::uint64_t frames    = 0; ///< Frames decoded successfully.
	std::uint64_t malformed = 0; ///< Frames that failed to decode.
};

/**
 * @brief Parse one frame, count the outcome, and normalise what came back.
 *
 * @tparam Payload The neutral type @p normalise produces - @c depth_event,
 *         @c trade_print. Named explicitly at the call site rather than deduced
 *         because it is the decoder's whole output type and worth reading
 * there.
 * @param tally Advanced exactly once per call, on whichever side happened.
 * @param position Where in the source the frame was, for the failure report.
 * @param ingress When the transport received it.
 * @param parse Invoked with no arguments; returns
 *        @c std::expected<Venue, Error>. A lambda at the call site, which is
 *        what keeps the parser and the listing's precision out of this header.
 * @param normalise Invoked with the parsed venue message as an rvalue; returns
 *        @p Payload.
 * @return The normalised payload, or @c feed_stop::malformed with the
 *         decoder's own diagnostic.
 *
 * @note @p normalise is always handed an rvalue, so a payload whose levels can
 *       move across does, and one whose overload only takes a const reference
 *       binds to it harmlessly. The depth path is the first kind and the trade
 *       path the second.
 *
 * @note @p Payload must carry an @c ingress member; the assignment below is
 *       the check, since a payload without one is a compile error naming the
 *       line. (@c core::chrono::has_ingress is a predicate over a *stamp*, not
 *       a trait over a type, so it cannot express this.)
 *
 * @note The ingress stamp is applied here rather than passed into @p normalise:
 *       arrival time is not venue knowledge, and a venue adapter that had to
 *       carry it would acquire a parameter meaning nothing in its own
 *       vocabulary.
 */
template <typename Payload, typename Parse, typename Normalise>
[[nodiscard]] std::expected<Payload, feed_status>
decode_frame(decode_tally &tally, std::uint64_t position,
			 core::chrono::ingress_time ingress, Parse &&parse,
			 Normalise &&normalise) {
	auto decoded = std::forward<Parse>(parse)();
	if (!decoded) {
		++tally.malformed;
		return std::unexpected(feed_status{.reason = feed_stop::malformed,
										   .detail = detail_of(decoded.error()),
										   .position = position});
	}

	++tally.frames;
	Payload payload = std::forward<Normalise>(normalise)(std::move(*decoded));
	payload.ingress = ingress;
	return payload;
}

} // namespace exchange::market_data::binance::detail
