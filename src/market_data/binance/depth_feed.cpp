#include "depth_feed.hpp"

#include "binance_depth.hpp"
#include "depth_error.hpp"
#include "detail/frame_decode.hpp"
#include "market_data/binance/detail/jsonl_frame.hpp"
#include "market_data/feed.hpp"
#include "market_data/normalised.hpp"
#include "normalise.hpp"

#include <cstddef>
#include <expected>
#include <string_view>
#include <utility>

namespace exchange::market_data::binance {

// --- depth_frame_decoder ---------------------------------------------------

depth_frame_decoder::depth_frame_decoder(int price_decimals, int qty_decimals)
	: price_decimals_(price_decimals), qty_decimals_(qty_decimals) {}

depth_frame_decoder::~depth_frame_decoder() = default;
depth_frame_decoder::depth_frame_decoder(depth_frame_decoder &&) noexcept =
	default;
depth_frame_decoder &
depth_frame_decoder::operator=(depth_frame_decoder &&) noexcept = default;

std::uint64_t depth_frame_decoder::frames() const noexcept {
	return tally_.frames;
}

std::uint64_t depth_frame_decoder::malformed() const noexcept {
	return tally_.malformed;
}

std::expected<depth_event, feed_status>
depth_frame_decoder::decode(std::string_view frame, std::uint64_t position,
							core::chrono::ingress_time ingress) {
	return detail::decode_frame<depth_event>(
		tally_,
		position,
		ingress,
		[&] {
			return parser_.parse_update(frame, price_decimals_, qty_decimals_);
		},
		[](depth_update &&update) { return normalise(std::move(update)); });
}

// --- jsonl_depth_feed ------------------------------------------------------

jsonl_depth_feed::jsonl_depth_feed(std::string_view jsonl, int price_decimals,
								   int qty_decimals)
	: decoder_(price_decimals, qty_decimals), jsonl_(jsonl) {}

jsonl_depth_feed::jsonl_depth_feed(book_snapshot seed, std::string_view jsonl,
								   int price_decimals, int qty_decimals)
	: decoder_(price_decimals, qty_decimals),
	  jsonl_(jsonl),
	  seed_(std::move(seed)) {}

jsonl_depth_feed::~jsonl_depth_feed()                            = default;
jsonl_depth_feed::jsonl_depth_feed(jsonl_depth_feed &&) noexcept = default;
jsonl_depth_feed &
jsonl_depth_feed::operator=(jsonl_depth_feed &&) noexcept = default;

feed_pull jsonl_depth_feed::next() {
	if (seed_.has_value()) {
		book_snapshot seed = std::move(*seed_);
		seed_.reset();
		return feed_message{std::move(seed)};
	}

	// next_frame skips blank lines and steps past a line before it can be
	// failed on, so there is no loop left here. @see detail::next_frame
	const std::optional<std::string_view> frame =
		detail::next_frame(jsonl_, at_, line_);
	if (!frame)
		return std::unexpected(
			feed_status{.reason = feed_stop::exhausted, .position = line_});

	auto decoded = decoder_.decode(*frame, line_);
	if (!decoded) return std::unexpected(decoded.error());
	return feed_message{std::move(*decoded)};
}

} // namespace exchange::market_data::binance
