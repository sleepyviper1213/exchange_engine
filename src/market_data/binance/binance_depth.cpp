#include "detail/depth_decode.hpp"

#include "depth_parse_error.hpp"
#include "depth_snapshot.hpp"
#include "depth_update.hpp"
#include "depth_update_meta.hpp"
#include "market_data/binance/detail/jsonl_frame.hpp"
#include "market_data/format.hpp" // fmt::formatter<depth_parse_error>
#include "parse_depth.hpp"
#include "parse_scaled.hpp"
#include "price_level.hpp"

#include <fmt/format.h>

#include <string>

namespace exchange::market_data::binance {

std::expected<std::int64_t, core::scaled::parse_error>
parse_scaled(std::string_view text, int decimals) {
	// The decimal-string -> scaled-integer conversion lives in the shared,
	// SIMD-accelerated parser module; this is a thin binance-namespace alias
	// that forwards its enum-typed error straight through - no std::string on
	// the parse path (render it with core::scaled::message only when
	// displaying).
	return core::scaled::parse_fixed_point(text, decimals);
}

std::string message(const depth_parse_error &error) {
	// One rendering, defined by the formatter (market_data/format.hpp); this
	// stays as the convenience spelling for callers that want an owned string.
	return fmt::format("{}", error);
}

std::expected<depth_snapshot, depth_parse_error>
parse_binance_depth(std::string_view json, int price_decimals,
					int qty_decimals) {
	using namespace simdjson;

	// simdjson On-Demand needs SIMDJSON_PADDING bytes past the end; copy into a
	// padded buffer. Fields are read in document order (lastUpdateId, bids,
	// asks) so On-Demand never rewinds.
	ondemand::parser parser;
	padded_string padded(json);
	ondemand::document doc;
	TRY_JSON(parser.iterate(padded).get(doc), invalid_json);
	return detail::snapshot_from_doc(doc, price_decimals, qty_decimals);
}

std::expected<depth_update, depth_parse_error>
parse_binance_depth_update(std::string_view json, int price_decimals,
						   int qty_decimals) {
	using namespace simdjson;

	// Same padded-buffer contract as parse_binance_depth. depthUpdate frames
	// arrive as {e,E,s,U,u,b,a}; operator[] tolerates the leading e/s we skip,
	// and we read the rest (E, U, u, b, a) in document order.
	ondemand::parser parser;
	padded_string padded(json);
	ondemand::document doc;
	TRY_JSON(parser.iterate(padded).get(doc), invalid_json);
	return detail::update_from_doc(doc, price_decimals, qty_decimals);
}

std::expected<depth_update_meta, depth_parse_error>
apply_binance_depth_update(l2_book &book, std::string_view json,
						   int price_decimals, int qty_decimals) {
	using namespace simdjson;

	// Same padded-buffer contract as parse_binance_depth_update, but the levels
	// are applied to the book as they are parsed rather than collected first.
	ondemand::parser parser;
	padded_string padded(json);
	ondemand::document doc;
	TRY_JSON(parser.iterate(padded).get(doc), invalid_json);
	return detail::stream_update_from_doc(book, doc, price_decimals, qty_decimals);
}

std::expected<std::vector<depth_update>, depth_parse_error>
parse_binance_depth_updates(std::string_view jsonl, int price_decimals,
							int qty_decimals) {
	std::vector<depth_update> updates;
	std::size_t line_no = 0;
	std::size_t pos     = 0;

	while (pos <= jsonl.size()) {
		const std::size_t nl = jsonl.find('\n', pos);
		const std::size_t end =
			nl == std::string_view::npos ? jsonl.size() : nl;
		std::string_view line = jsonl.substr(pos, end - pos);
		++line_no;

		// Trim a trailing '\r' so CRLF captures parse, and skip blank lines.
		if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
		const auto first = line.find_first_not_of(" \t");
		if (first != std::string_view::npos) {
			auto parsed =
				parse_binance_depth_update(line, price_decimals, qty_decimals);
			if (!parsed) {
				auto err = parsed.error();
				err.line = static_cast<std::uint32_t>(line_no);
				return std::unexpected(err);
			}
			updates.emplace_back(std::move(*parsed));
		}

		if (nl == std::string_view::npos) break;
		pos = nl + 1;
	}
	return updates;
}

#undef TRY_JSON
// Declared in parse_depth.hpp with the module's export macro, so it is
// defined in the translation unit that includes it - a definition that
// cannot see the exported declaration is not exported, and the link
// failure names the caller rather than this file.
void apply_depth_update(l2_book &book, const depth_update &update) {
	for (const auto &[price, volume] : update.bids)
		book.set_level(side_t::bid, price, volume);
	for (const auto &[price, volume] : update.asks)
		book.set_level(side_t::ask, price, volume);
}

} // namespace exchange::market_data::binance
