#include "depth_feed.hpp"

#include "binance_depth.hpp"
#include "depth_error.hpp"
#include "market-data/feed.hpp"
#include "market-data/normalised.hpp"
#include "normalise.hpp"

#include <cstddef>
#include <expected>
#include <string_view>
#include <utility>

namespace exchange::market_data::binance {
namespace {

/// Whether @p ch is line whitespace worth trimming before a frame is decoded.
/// `\r` above all: a capture written on one platform and replayed on another
/// carries CRLF, and simdjson would call the trailing carriage return a parse
/// error rather than what it is.
constexpr bool is_blank(char ch) noexcept {
	return ch == ' ' || ch == '\t' || ch == '\r';
}

/// Trim leading and trailing blanks from @p line.
constexpr std::string_view trimmed(std::string_view line) noexcept {
	while (!line.empty() && is_blank(line.front())) line.remove_prefix(1);
	while (!line.empty() && is_blank(line.back())) line.remove_suffix(1);
	return line;
}

/**
 * The static text a neutral @c feed_status carries for a venue parse failure.
 *
 * @c depth_parse_error::context is documented as always static - a field name,
 * simdjson's own message, or the numeric parse_error's - so it can be handed
 * across the seam as-is. When it is empty the category label stands in, which
 * is also static. Nothing here can view into the frame, which is what
 * @c feed_status::detail promises.
 */
constexpr std::string_view detail_of(const depth_parse_error &error) noexcept {
	return error.context.empty() ? message(error.code) : error.context;
}

} // namespace

jsonl_depth_feed::jsonl_depth_feed(std::string_view jsonl, int price_decimals,
								   int qty_decimals)
	: jsonl_(jsonl),
	  price_decimals_(price_decimals),
	  qty_decimals_(qty_decimals) {}

jsonl_depth_feed::jsonl_depth_feed(book_snapshot seed, std::string_view jsonl,
								   int price_decimals, int qty_decimals)
	: jsonl_(jsonl),
	  seed_(std::move(seed)),
	  price_decimals_(price_decimals),
	  qty_decimals_(qty_decimals) {}

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

	while (at_ < jsonl_.size()) {
		const std::size_t eol       = jsonl_.find('\n', at_);
		const std::string_view line = jsonl_.substr(
			at_,
			eol == std::string_view::npos ? std::string_view::npos : eol - at_);
		// Step over the line *before* anything can fail on it, so the feed is
		// resumable after a malformed frame. @see the note on next().
		at_ = eol == std::string_view::npos ? jsonl_.size() : eol + 1;
		++line_;

		const std::string_view frame = trimmed(line);
		if (frame.empty()) continue;

		auto decoded =
			parser_.parse_update(frame, price_decimals_, qty_decimals_);
		if (!decoded) {
			++malformed_;
			return std::unexpected(
				feed_status{.reason   = feed_stop::malformed,
							.detail   = detail_of(decoded.error()),
							.position = line_});
		}

		++frames_;
		// The rvalue overload: the decoded frame is wanted for nothing else, so
		// its level vectors move across instead of being copied.
		return feed_message{normalise(std::move(*decoded))};
	}

	return std::unexpected(
		feed_status{.reason = feed_stop::exhausted, .position = line_});
}

} // namespace exchange::market_data::binance
