#include "binance_trade.hpp"

#include "core/scaled/fixed_point.hpp"
#include "market_data/binance/detail/jsonl_frame.hpp"
#include "market_data/format.hpp" // fmt::formatter<trade_parse_error>
#include "market_data/types.hpp"

#include <fmt/format.h>

#include <cstring>
#include <memory>
#include <simdjson.h>
#include <string>
#include <utility>


/// @brief On a simdjson error from @p expr, bail out of the enclosing function
/// with a @c trade_parse_error of category @p category carrying simdjson's
/// (static) message. Local to this TU; undefined at end of file.
#ifndef TRY_JSON
#define TRY_JSON(expr, category)                                               \
	do {                                                                       \
		if (const auto err = (expr))                                           \
			return std::unexpected(                                            \
				trade_parse_error{trade_error::category,                       \
								  simdjson::error_message(err)});              \
	} while (false)
#endif
using namespace exchange::core::scaled;

namespace exchange::market_data::binance {
namespace {
/// Read an optional unsigned scalar, returning @p fallback when it is absent or
/// holds another type. Any other simdjson error is structural - On-Demand has
/// already abandoned the iterator by the time it reports one, and querying the
/// document again trips its depth assertions - so it is propagated and parsing
/// stops. @see the same helper in binance_depth.cpp.
/// @see detail::read_optional_u64, which both decoders share.
[[nodiscard]] std::expected<std::uint64_t, trade_parse_error>
read_trade_optional_u64(simdjson::ondemand::document &doc, std::string_view key,
						std::uint64_t fallback = 0) {
	return detail::read_optional_u64<trade_parse_error>(
		doc,
		key,
		trade_error::invalid_json,
		fallback);
}

/// Read a required unsigned scalar. Absent or mistyped is @c missing_field
/// naming the key, which is a string literal and so satisfies the static-text
/// contract on @c trade_parse_error::context.
std::expected<std::uint64_t, trade_parse_error>
read_u64(simdjson::ondemand::document &doc, std::string_view key) {
	using namespace simdjson;

	std::uint64_t value = 0;
	const auto err      = doc[key].get(value);
	if (!err) return value;
	if (err == NO_SUCH_FIELD || err == INCORRECT_TYPE)
		return std::unexpected(
			trade_parse_error{trade_error::missing_field, key});
	return std::unexpected(
		trade_parse_error{trade_error::invalid_json, error_message(err)});
}

/// Read a required boolean. Binance spells the maker flag as a JSON bool, so a
/// frame carrying anything else there is malformed rather than merely odd.
std::expected<bool, trade_parse_error>
read_bool(simdjson::ondemand::document &doc, std::string_view key) {
	using namespace simdjson;

	bool value     = false;
	const auto err = doc[key].get(value);
	if (!err) return value;
	if (err == NO_SUCH_FIELD || err == INCORRECT_TYPE)
		return std::unexpected(
			trade_parse_error{trade_error::missing_field, key});
	return std::unexpected(
		trade_parse_error{trade_error::invalid_json, error_message(err)});
}

/// Read a required decimal-string field and scale it to an integer by
/// 10^@p decimals. No floating point: this is the same fixed-point path every
/// level of every depth frame already goes through.
std::expected<std::int64_t, trade_parse_error>
read_scaled(simdjson::ondemand::document &doc, std::string_view key,
			int decimals) {
	using namespace simdjson;

	std::string_view text;
	const auto err = doc[key].get_string().get(text);
	if (err == NO_SUCH_FIELD || err == INCORRECT_TYPE)
		return std::unexpected(
			trade_parse_error{trade_error::missing_field, key});
	if (err)
		return std::unexpected(
			trade_parse_error{trade_error::invalid_json, error_message(err)});

	// parse_error::message() is a static view, safe to carry as context past
	// this call - the decimal text itself would not be.
	const auto parsed = core::scaled::parse_fixed_point(text, decimals);
	if (!parsed)
		return std::unexpected(
			trade_parse_error{trade_error::bad_number,
							  core::scaled::message(parsed.error())});
	return *parsed;
}

/**
 * @brief Build a trade_message from an already-iterated @c trade document.
 *
 * Shared by the one-shot free function and the reusable @c trade_parser so the
 * field-order contract lives in one place. Fields are read in wire order
 * (E, t, p, q, T, m) because On-Demand walks the document forwards and does not
 * rewind - reading them in any other order works only by accident of buffering.
 */
std::expected<trade_message, trade_parse_error>
trade_from_doc(simdjson::ondemand::document &doc, int price_decimals,
			   int qty_decimals) {
	trade_message trade;

	// E is tolerated as absent: a hand-written corpus that only cares about the
	// execution time should not have to carry the send time as well.
	auto event = read_trade_optional_u64(doc, "E");
	if (!event) return std::unexpected(event.error());
	trade.event_time = *event;

	auto id = read_u64(doc, "t");
	if (!id) return std::unexpected(id.error());
	trade.trade_id = *id;

	auto price = read_scaled(doc, "p", price_decimals);
	if (!price) return std::unexpected(price.error());
	trade.price = static_cast<scaled_price_t>(*price);

	auto qty = read_scaled(doc, "q", qty_decimals);
	if (!qty) return std::unexpected(qty.error());
	trade.qty = static_cast<scaled_qty_t>(*qty);

	// T falls back to E rather than to zero. The two are equal on Binance spot
	// in the overwhelming majority of frames, and a print stamped 1970 is far
	// worse than one stamped a millisecond early: a feed clock driven off it
	// would jump back fifty years, and every interval measured against it after
	// that is nonsense.
	auto executed = read_trade_optional_u64(doc, "T", trade.event_time);
	if (!executed) return std::unexpected(executed.error());
	trade.trade_time = *executed;

	auto maker = read_bool(doc, "m");
	if (!maker) return std::unexpected(maker.error());
	trade.buyer_is_maker = *maker;

	return trade;
}

} // namespace

std::string message(const trade_parse_error &error) {
	// One rendering, defined by the formatter (market_data/format.hpp); this is
	// the convenience spelling for callers that want an owned string.
	return fmt::format("{}", error);
}

std::expected<trade_message, trade_parse_error>
parse_binance_trade(std::string_view json, int price_decimals,
					int qty_decimals) {
	using namespace simdjson;

	// simdjson On-Demand needs SIMDJSON_PADDING bytes past the end; copy into a
	// padded buffer. The leading e/s fields are skipped by operator[], which
	// may only move forwards - hence the wire-order reads in trade_from_doc.
	ondemand::parser parser;
	padded_string padded(json);
	ondemand::document doc;
	TRY_JSON(parser.iterate(padded).get(doc), invalid_json);
	return trade_from_doc(doc, price_decimals, qty_decimals);
}

std::expected<std::vector<trade_message>, trade_parse_error>
parse_binance_trades(std::string_view jsonl, int price_decimals,
					 int qty_decimals) {
	std::vector<trade_message> trades;
	// One parser for the whole file rather than one per line: the buffers
	// amortise across frames, which is the entire reason trade_parser exists.
	trade_parser parser;
	std::size_t line_no = 0;
	std::size_t pos     = 0;

	while (pos <= jsonl.size()) {
		const std::size_t nl = jsonl.find('\n', pos);
		const std::size_t end =
			nl == std::string_view::npos ? jsonl.size() : nl;
		std::string_view line = jsonl.substr(pos, end - pos);
		++line_no;

		// Trim a trailing carriage return so a CRLF capture parses, and skip
		// blank lines.
		if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
		const auto first = line.find_first_not_of(" \t");
		if (first != std::string_view::npos) {
			auto parsed =
				parser.parse_trade(line, price_decimals, qty_decimals);
			if (!parsed) {
				auto err = parsed.error();
				err.line = static_cast<std::uint32_t>(line_no);
				return std::unexpected(err);
			}
			trades.push_back(*parsed);
		}

		if (nl == std::string_view::npos) break;
		pos = nl + 1;
	}
	return trades;
}

/**
 * @brief Reusable parser state: a simdjson parser plus a padded input buffer,
 *        both amortised across successive frames. @see depth_parser::impl,
 *        which this mirrors - including the grow-only buffer policy.
 */
struct trade_parser::impl {
	simdjson::ondemand::parser json_parser;
	/// Reused input staging. padded_string has no capacity-preserving assign,
	/// so the grow-only policy is driven by hand: reallocate only for a frame
	/// larger than any seen so far, otherwise memcpy into the existing buffer.
	/// Seeded non-empty so data() is never null on the empty-frame path.
	simdjson::padded_string storage{0uz};

	/**
	 * @brief Stage @p json in the reused padded buffer and begin iteration.
	 * @param json The raw frame to iterate.
	 * @return The iterating document, or the failure that stopped it.
	 */
	std::expected<simdjson::ondemand::document, trade_parse_error>
	iterate(std::string_view json) {
		if (json.size() > storage.size())
			storage = simdjson::padded_string(json.size());
		if (!json.empty())
			std::memcpy(storage.data(), json.data(), json.size());
		// storage owns storage.size() + SIMDJSON_PADDING readable bytes; claim
		// exactly the padding On-Demand requires past this (possibly shorter)
		// frame.
		const simdjson::padded_string_view view(storage.data(),
												json.size(),
												storage.size() +
													simdjson::SIMDJSON_PADDING);
		simdjson::ondemand::document doc;
		TRY_JSON(json_parser.iterate(view).get(doc), invalid_json);
		return doc;
	}
};

trade_parser::trade_parser() : impl_(std::in_place) {}

trade_parser::~trade_parser() = default;

trade_parser::trade_parser(trade_parser &&) noexcept = default;

trade_parser &trade_parser::operator=(trade_parser &&) noexcept = default;

std::expected<trade_message, trade_parse_error>
trade_parser::parse_trade(std::string_view json, int price_decimals,
						  int qty_decimals) {
	auto doc = impl_->iterate(json);
	if (!doc) return std::unexpected(doc.error());
	return trade_from_doc(*doc, price_decimals, qty_decimals);
}

} // namespace exchange::market_data::binance

#undef TRY_JSON
