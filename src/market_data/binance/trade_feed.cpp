#include "trade_feed.hpp"

#include "binance_trade.hpp"
#include "detail/frame_decode.hpp"
#include "market_data/binance/detail/jsonl_frame.hpp"
#include "market_data/feed.hpp"
#include "market_data/normalised.hpp"
#include "market_data/trade_feed.hpp"
#include "normalise.hpp"
#include "trade_error.hpp"

#include <cstddef>
#include <expected>
#include <string_view>
#include <utility>

namespace exchange::market_data::binance {

// --- trade_frame_decoder ---------------------------------------------------

trade_frame_decoder::trade_frame_decoder(int price_decimals, int qty_decimals)
	: price_decimals_(price_decimals), qty_decimals_(qty_decimals) {}

trade_frame_decoder::~trade_frame_decoder() = default;
trade_frame_decoder::trade_frame_decoder(trade_frame_decoder &&) noexcept =
	default;
trade_frame_decoder &
trade_frame_decoder::operator=(trade_frame_decoder &&) noexcept = default;

std::uint64_t trade_frame_decoder::frames() const noexcept {
	return tally_.frames;
}

std::uint64_t trade_frame_decoder::malformed() const noexcept {
	return tally_.malformed;
}

std::expected<trade_print, feed_status>
trade_frame_decoder::decode(std::string_view frame, std::uint64_t position,
							core::chrono::ingress_time ingress) {
	return detail::decode_frame<trade_print>(
		tally_,
		position,
		ingress,
		[&] {
			return parser_.parse_trade(frame, price_decimals_, qty_decimals_);
		},
		[](trade_message &&trade) { return normalise(trade); });
}

// --- jsonl_trade_feed ------------------------------------------------------

jsonl_trade_feed::jsonl_trade_feed(std::string_view jsonl, int price_decimals,
								   int qty_decimals)
	: decoder_(price_decimals, qty_decimals), jsonl_(jsonl) {}

jsonl_trade_feed::~jsonl_trade_feed()                            = default;
jsonl_trade_feed::jsonl_trade_feed(jsonl_trade_feed &&) noexcept = default;
jsonl_trade_feed &
jsonl_trade_feed::operator=(jsonl_trade_feed &&) noexcept = default;

trade_pull jsonl_trade_feed::next() {
	// @see detail::next_frame - the blank-skipping and the resumable
	// advance live there, which is why this is straight-line.
	const std::optional<std::string_view> frame =
		detail::next_frame(jsonl_, at_, line_);
	if (!frame)
		return std::unexpected(
			feed_status{.reason = feed_stop::exhausted, .position = line_});

	auto decoded = decoder_.decode(*frame, line_);
	if (!decoded) return std::unexpected(decoded.error());
	return *decoded;
}

} // namespace exchange::market_data::binance
