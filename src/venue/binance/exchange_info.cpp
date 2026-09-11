#include "exchange_info.hpp"

#include "api_error.hpp"
#include "host.hpp"

#include <fmt/format.h>

#include <simdjson.h>
#include <string>

namespace exchange::venue::binance {
namespace {

/// @brief Find the named filter object in a symbol's `filters` array.
///
/// A linear scan, because the array is ordered by nothing in particular and
/// holds a dozen entries. Returns whether it was found rather than throwing:
/// a listing missing PRICE_FILTER is a malformed response, and the caller wants
/// to say which filter was absent.
[[nodiscard]] bool find_filter(const simdjson::dom::element &symbol_doc,
							   std::string_view type,
							   simdjson::dom::element &out) {
	simdjson::dom::array filters;
	if (symbol_doc["filters"].get_array().get(filters)) return false;
	for (simdjson::dom::element filter : filters) {
		std::string_view name;
		if (filter["filterType"].get_string().get(name)) continue;
		if (name == type) {
			out = filter;
			return true;
		}
	}
	return false;
}

/// @brief Read a string field out of a filter object.
[[nodiscard]] bool read_string(const simdjson::dom::element &doc,
							   std::string_view field, std::string &out) {
	std::string_view value;
	if (doc[field].get_string().get(value)) return false;
	out = std::string(value);
	return true;
}

/**
 * @brief @p increment with the venue's zero padding removed.
 *
 * Binance right-pads every number to eight decimals, so the grid arrives as
 * @c "0.01000000" where the increment is one hundredth. That form cannot be
 * used as-is: @c parse_exact_decimal requires a decimal to be *exactly*
 * representable at the scale it is given, so eight decimals against a scale of
 * two is refused as malformed - correctly, since silently dropping digits is
 * how a grid ends up wrong. Trimming here is not rounding: every digit removed
 * is a zero.
 *
 * @return @c "0.01" from @c "0.01000000", @c "1" from @c "1.00000000".
 */
[[nodiscard]] std::string trim_increment(std::string_view increment) {
	const std::size_t point = increment.find('.');
	if (point == std::string_view::npos) return std::string(increment);

	std::size_t last = increment.size();
	while (last > point && increment[last - 1] == '0') --last;
	// The fraction was all zeros, so the point has nothing left to separate.
	if (last == point + 1) last = point;
	return std::string(increment.substr(0, last));
}

/// @brief Pull the grid out of one `symbols` entry, already matched by name.
[[nodiscard]] std::expected<symbol_filters, std::string>
read_symbol_grid(const simdjson::dom::element &entry, std::string_view symbol) {
	symbol_filters out;
	out.symbol = std::string(symbol);
	if (!read_string(entry, "status", out.status))
		return std::unexpected(fmt::format("{} has no `status`", symbol));

	simdjson::dom::element price_filter;
	if (!find_filter(entry, "PRICE_FILTER", price_filter))
		return std::unexpected(fmt::format("{} has no PRICE_FILTER", symbol));
	std::string tick;
	if (!read_string(price_filter, "tickSize", tick))
		return std::unexpected(
			fmt::format("{}'s PRICE_FILTER has no tickSize", symbol));

	simdjson::dom::element lot_size;
	if (!find_filter(entry, "LOT_SIZE", lot_size))
		return std::unexpected(fmt::format("{} has no LOT_SIZE", symbol));
	std::string step;
	if (!read_string(lot_size, "stepSize", step))
		return std::unexpected(
			fmt::format("{}'s LOT_SIZE has no stepSize", symbol));

	// Decimals from the padded form and the text from the trimmed one - the two
	// agree, because trimming only ever removes zeros, and the trimmed text is
	// the only form parse_exact_decimal will accept at that scale.
	out.price_decimals = significant_decimals(tick);
	out.qty_decimals   = significant_decimals(step);
	out.tick_size      = trim_increment(tick);
	out.step_size      = trim_increment(step);

	// Optional, unlike the two above, and absence is a fact rather than a
	// failure: a listing genuinely may not have one, and a run that refused to
	// start over a missing floor would refuse to read a book it could read
	// perfectly well. `NOTIONAL` first because it is the current spelling;
	// `MIN_NOTIONAL` is the older one and some listings still carry only that.
	simdjson::dom::element notional;
	if (find_filter(entry, "NOTIONAL", notional) ||
		find_filter(entry, "MIN_NOTIONAL", notional)) {
		std::string floor_text;
		if (read_string(notional, "minNotional", floor_text))
			out.min_notional = trim_increment(floor_text);
	}
	return out;
}

} // namespace

int significant_decimals(std::string_view increment) noexcept {
	const std::size_t point = increment.find('.');
	if (point == std::string_view::npos) return 0;

	const std::string_view fraction = increment.substr(point + 1);
	// Walk back over the padding. Binance right-pads every number to eight
	// decimals, so "0.00100000" and "0.001" describe the same grid and must
	// produce the same answer.
	std::size_t last = fraction.size();
	while (last > 0 && fraction[last - 1] == '0') --last;
	return static_cast<int>(last);
}

std::expected<symbol_filters, std::string>
parse_exchange_info(std::string_view json, std::string_view symbol) {
	// An error envelope first: a refused request is a JSON object too, and
	// reporting it as "no symbols array" would hide what the venue actually
	// said. @see parse_api_error
	if (const auto refused = parse_api_error(json))
		return std::unexpected(
			refused->msg.empty()
				? fmt::format("venue refused exchangeInfo: code {}",
							  refused->code)
				: fmt::format("venue refused exchangeInfo: {} (code {})",
							  refused->msg,
							  refused->code));

	simdjson::dom::parser parser;
	simdjson::dom::element doc;
	if (const auto err = parser.parse(simdjson::padded_string(json)).get(doc))
		return std::unexpected(fmt::format("exchangeInfo is not JSON: {}",
										   simdjson::error_message(err)));

	simdjson::dom::array symbols;
	if (doc["symbols"].get_array().get(symbols))
		return std::unexpected("exchangeInfo has no `symbols` array");

	for (simdjson::dom::element entry : symbols) {
		std::string_view name;
		if (entry["symbol"].get_string().get(name)) continue;
		if (name != symbol) continue;
		return read_symbol_grid(entry, symbol);
	}

	// Not "no symbols": the array was there and this listing was not in it,
	// which for a request scoped to one symbol means the venue does not have
	// it.
	return std::unexpected(
		fmt::format("exchangeInfo does not describe {}", symbol));
}

http_endpoint exchange_info_endpoint(std::string_view symbol, environment env) {
	return {.host   = std::string(host_for(env).rest),
			.target = fmt::format("/api/v3/exchangeInfo?symbol={}", symbol)};
}

} // namespace exchange::venue::binance
