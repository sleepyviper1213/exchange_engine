#include "binance_depth.hpp"

#include "market-data/parser/fixed_point.hpp"
#include "trading-engine/order_book/order_book.hpp"

#include <fmt/format.h>

#include <concepts>
#include <cstring>
#include <memory>
#include <optional>
#include <simdjson.h>
#include <string>
#include <utility>

/// @brief On a simdjson error from @p expr, bail out of the enclosing function
/// with a @c depth_parse_error of category @p category carrying simdjson's
/// (static) message. @c err is scoped to the generated block, so repeated use in
/// one function is fine. Local to this TU; @c \#undef'd at end of file.
#define TRY_JSON(expr, category)                                               \
	do {                                                                       \
		if (const auto err = (expr))                                           \
			return std::unexpected(depth_parse_error{                          \
				depth_error::category, simdjson::error_message(err)});         \
	} while (false)

namespace exchange::market_data::binance {
namespace {
/// Iterate a bids/asks array of ["price","qty"] string pairs, scaling each pair
/// and handing (price, volume) to @p on_level. Single pass, no copy of the raw
/// decimal strings — @c parse_scaled is pure and never touches the iterator.
///
/// A shape/numeric failure must not abandon the iterator mid-stream: that would
/// leave the reused parser's depth bookkeeping inconsistent and trip its debug
/// assertions. So the first bad level is recorded, iteration still runs to
/// completion, and the error is returned only once no iterator is in flight.
/// @note @p on_level may already have fired for earlier levels when an error is
///       returned; callers needing all-or-nothing must buffer (see parse_levels).
template <class OnLevel>
	requires std::invocable<OnLevel, Price, Volume>
std::expected<void, depth_parse_error>
for_each_level(simdjson::ondemand::value array_value, int price_decimals,
               int qty_decimals, OnLevel on_level) {
	using namespace simdjson;

	ondemand::array array;
	TRY_JSON(array_value.get_array().get(array), invalid_json);

	std::optional<depth_parse_error> deferred; // first bad level, if any
	for (auto element : array) {
		ondemand::array pair;
		TRY_JSON(element.get_array().get(pair), invalid_json);

		// Drain the pair's fields even after a prior failure, so the array
		// iterator stays consistent for the reused parser.
		std::string_view fields[2];
		int count = 0;
		for (auto field : pair) {
			std::string_view sv;
			TRY_JSON(field.get_string().get(sv), invalid_json);
			if (count < 2) fields[count] = sv;
			++count;
		}

		if (deferred) continue; // already failed — keep draining the array
		if (count != 2) {
			deferred = depth_parse_error{depth_error::malformed_level};
			continue;
		}
		// Parse straight to parse_error; its message() is a static view, safe to
		// carry as context past this call.
		auto price = parser::parse_fixed_point(fields[0], price_decimals);
		if (!price) {
			deferred = depth_parse_error{depth_error::bad_number,
			                             parser::message(price.error())};
			continue;
		}
		auto qty = parser::parse_fixed_point(fields[1], qty_decimals);
		if (!qty) {
			deferred = depth_parse_error{depth_error::bad_number,
			                             parser::message(qty.error())};
			continue;
		}
		on_level(static_cast<Price>(*price), static_cast<Volume>(*qty));
	}
	if (deferred) return std::unexpected(*deferred);
	return {};
}

/// Read a bids/asks array into a vector of scaled levels. All-or-nothing: on
/// error the half-built vector is discarded, so the caller never sees partial
/// data.
std::expected<std::vector<PriceLevel>, depth_parse_error>
parse_levels(const simdjson::ondemand::value &array_value, int price_decimals,
             int qty_decimals) {
	std::vector<PriceLevel> levels;
	auto applied = for_each_level(
		array_value,
		price_decimals,
		qty_decimals,
		[&](Price price, Volume volume) {
			levels.emplace_back(price, volume);
		});
	if (!applied) return std::unexpected(applied.error());
	return levels;
}

/// Read a bids/asks array and apply each scaled level straight to @p book on
/// @p side via set_level — no intermediate vector. @warning Not atomic: on a
/// malformed level, the levels before it are already applied (see
/// for_each_level).
std::expected<void, depth_parse_error>
stream_levels(order_book &book, Side side,
              simdjson::ondemand::value array_value, int price_decimals,
              int qty_decimals) {
	return for_each_level(
		array_value,
		price_decimals,
		qty_decimals,
		[&](Price price, Volume volume) {
			book.set_level(side, price, volume);
		});
}

/**
 * @brief Read and scale the two level arrays of a depth document.
 *
 * The caller must have consumed any scalar fields first, since simdjson
 * On-Demand walks the document in order and does not rewind.
 * @param doc An already-iterated depth document.
 * @param bid_key Field name of the bids array (@c "bids" or @c "b").
 * @param ask_key Field name of the asks array (@c "asks" or @c "a").
 * @param price_decimals Tick precision to scale prices by.
 * @param qty_decimals Step precision to scale quantities by.
 * @return A {bids, asks} pair of scaled levels, or an error message
 *         prefixed with the offending field name.
 */
std::expected<std::pair<std::vector<PriceLevel>, std::vector<PriceLevel> >,
	depth_parse_error>
parse_sides(simdjson::ondemand::document &doc, std::string_view bid_key,
            std::string_view ask_key, int price_decimals, int qty_decimals) {
	using namespace simdjson;

	ondemand::value bids_value;
	if (const auto err = doc[bid_key].get(bids_value))
		return std::unexpected(
			depth_parse_error{depth_error::missing_field, bid_key});
	auto bids = parse_levels(bids_value, price_decimals, qty_decimals);
	if (!bids) return std::unexpected(bids.error());

	ondemand::value asks_value;
	if (const auto err = doc[ask_key].get(asks_value))
		return std::unexpected(
			depth_parse_error{depth_error::missing_field, ask_key});
	auto asks = parse_levels(asks_value, price_decimals, qty_decimals);
	if (!asks) return std::unexpected(asks.error());

	return std::pair{std::move(*bids), std::move(*asks)};
}

/**
 * @brief Stream both level arrays of a depthUpdate straight into @p book.
 *
 * Mirrors @c parse_sides but applies each level via @c set_level instead of
 * collecting into vectors. @warning Not atomic (see @c stream_levels).
 */
std::expected<void, depth_parse_error>
stream_sides(order_book &book, simdjson::ondemand::document &doc,
             std::string_view bid_key, std::string_view ask_key,
             int price_decimals, int qty_decimals) {
	using namespace simdjson;

	ondemand::value bids_value;
	if (const auto err = doc[bid_key].get(bids_value))
		return std::unexpected(
			depth_parse_error{depth_error::missing_field, bid_key});
	if (auto r = stream_levels(book,
	                           Side::BID,
	                           bids_value,
	                           price_decimals,
	                           qty_decimals);
		!r)
		return std::unexpected(r.error());

	ondemand::value asks_value;
	if (const auto err = doc[ask_key].get(asks_value))
		return std::unexpected(
			depth_parse_error{depth_error::missing_field, ask_key});
	if (auto r = stream_levels(book,
	                           Side::ASK,
	                           asks_value,
	                           price_decimals,
	                           qty_decimals);
		!r)
		return std::unexpected(r.error());

	return {};
}

/// Read an optional unsigned scalar field, returning @p fallback when it is
/// absent or holds another type.
///
/// Those two cases leave the iterator usable, so the caller takes the fallback
/// and reads on. Any other error is a structural fault: On-Demand parses lazily,
/// so a document that survived @c iterate() can still turn out to be garbage
/// here, and simdjson has already abandoned the iterator by the time it reports
/// it. Querying such a document again trips its depth assertions, so the error
/// is propagated and parsing stops.
std::expected<std::uint64_t, depth_parse_error>
read_optional_u64(simdjson::ondemand::document &doc, std::string_view key,
                  std::uint64_t fallback = 0) {
	using namespace simdjson;

	std::uint64_t value = fallback;
	const auto err      = doc[key].get(value);
	if (!err || err == NO_SUCH_FIELD || err == INCORRECT_TYPE) return value;
	return std::unexpected(
		depth_parse_error{depth_error::invalid_json, error_message(err)});
}

/**
 * @brief Build a DepthSnapshot from an already-iterated depth document.
 *
 * Shared by the one-shot free function and the reusable DepthParser so the
 * field-order contract (lastUpdateId, then bids, then asks) lives in one place.
 */
std::expected<DepthSnapshot, depth_parse_error>
snapshot_from_doc(simdjson::ondemand::document &doc, int price_decimals,
                  int qty_decimals) {
	DepthSnapshot snapshot;
	auto last = read_optional_u64(doc, "lastUpdateId");
	if (!last) return std::unexpected(std::move(last.error()));
	snapshot.lastUpdateId = *last;

	auto sides = parse_sides(doc, "bids", "asks", price_decimals, qty_decimals);
	if (!sides) return std::unexpected(sides.error());

	snapshot.bids = std::move(sides->first);
	snapshot.asks = std::move(sides->second);
	return snapshot;
}

/**
 * @brief Build a DepthUpdate from an already-iterated depthUpdate document.
 *
 * Shared by the one-shot free function and the reusable DepthParser. Fields
 * are read in document order (E, U, u, then b, a) so On-Demand never rewinds.
 */
std::expected<DepthUpdate, depth_parse_error>
update_from_doc(simdjson::ondemand::document &doc, int price_decimals,
                int qty_decimals) {
	DepthUpdate update;
	auto event = read_optional_u64(doc, "E");
	if (!event) return std::unexpected(std::move(event.error()));
	update.eventTime = *event;
	auto first       = read_optional_u64(doc, "U");
	if (!first) return std::unexpected(std::move(first.error()));
	update.firstUpdateId = *first;
	auto last            = read_optional_u64(doc, "u");
	if (!last) return std::unexpected(std::move(last.error()));
	update.finalUpdateId = *last;

	auto sides = parse_sides(doc, "b", "a", price_decimals, qty_decimals);
	if (!sides) return std::unexpected(sides.error());

	update.bids = std::move(sides->first);
	update.asks = std::move(sides->second);
	return update;
}

/**
 * @brief Read a depthUpdate document's ids/time and stream its @c b / @c a
 *        levels straight into @p book.
 *
 * The streaming counterpart of @c update_from_doc: the scalar fields are read
 * in the same document order (E, U, u, then b, a) so On-Demand never rewinds,
 * but the levels go to the book via @c set_level instead of into vectors.
 */
std::expected<DepthUpdateMeta, depth_parse_error>
stream_update_from_doc(order_book &book, simdjson::ondemand::document &doc,
                       int price_decimals, int qty_decimals) {
	DepthUpdateMeta meta;
	auto event = read_optional_u64(doc, "E");
	if (!event) return std::unexpected(std::move(event.error()));
	meta.eventTime = *event;
	auto first     = read_optional_u64(doc, "U");
	if (!first) return std::unexpected(std::move(first.error()));
	meta.firstUpdateId = *first;
	auto last          = read_optional_u64(doc, "u");
	if (!last) return std::unexpected(std::move(last.error()));
	meta.finalUpdateId = *last;

	if (auto r = stream_sides(book, doc, "b", "a", price_decimals, qty_decimals)
		;
		!r)
		return std::unexpected(std::move(r.error()));
	return meta;
}
} // namespace

std::expected<std::int64_t, parser::parse_error>
parse_scaled(std::string_view text, int decimals) {
	// The decimal-string -> scaled-integer conversion lives in the shared,
	// SIMD-accelerated parser module; this is a thin binance-namespace alias that
	// forwards its enum-typed error straight through — no std::string on the
	// parse path (render it with parser::message only when displaying).
	return parser::parse_fixed_point(text, decimals);
}

std::string message(const depth_parse_error &error) {
	const std::string_view category = message(error.code);
	if (error.line && !error.context.empty())
		return fmt::format("line {}: {}: {}",
		                   error.line,
		                   error.context,
		                   category);
	if (error.line) return fmt::format("line {}: {}", error.line, category);
	if (!error.context.empty())
		return fmt::format("{}: {}", error.context, category);
	return std::string(category);
}

std::expected<DepthSnapshot, depth_parse_error>
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
	return snapshot_from_doc(doc, price_decimals, qty_decimals);
}

std::expected<DepthUpdate, depth_parse_error>
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
	return update_from_doc(doc, price_decimals, qty_decimals);
}

std::expected<DepthUpdateMeta, depth_parse_error>
apply_binance_depth_update(order_book &book, std::string_view json,
                           int price_decimals, int qty_decimals) {
	using namespace simdjson;

	// Same padded-buffer contract as parse_binance_depth_update, but the levels
	// are applied to the book as they are parsed rather than collected first.
	ondemand::parser parser;
	padded_string padded(json);
	ondemand::document doc;
	TRY_JSON(parser.iterate(padded).get(doc), invalid_json);
	return stream_update_from_doc(book, doc, price_decimals, qty_decimals);
}

std::expected<std::vector<DepthUpdate>, depth_parse_error>
parse_binance_depth_updates(std::string_view jsonl, int price_decimals,
                            int qty_decimals) {
	std::vector<DepthUpdate> updates;
	std::size_t line_no = 0;
	std::size_t pos     = 0;

	while (pos <= jsonl.size()) {
		const std::size_t nl  = jsonl.find('\n', pos);
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

/**
 * @brief Reusable parser state: a simdjson parser plus a padded input buffer,
 *        both amortised across successive frames.
 */
struct DepthParser::Impl {
	simdjson::ondemand::parser json_parser;
	/// Reused input staging. simdjson::padded_string owns a buffer with the
	/// trailing padding On-Demand's over-read needs, but has no
	/// capacity-preserving assign — so we drive a grow-only policy by hand:
	/// reallocate only when a frame is larger than any seen so far, otherwise
	/// memcpy into the existing buffer. A steady feed thus does no per-frame
	/// allocation. Seeded non-empty so data() is never null on the empty-frame
	/// path.
	simdjson::padded_string storage{std::size_t{0}};

	/**
	 * @brief Stage @p json in the reused padded buffer and begin iteration.
	 * @param json The raw frame to iterate.
	 * @return The iterating document, or an error message on failure.
	 */
	std::expected<simdjson::ondemand::document, depth_parse_error>
	iterate(std::string_view json) {
		if (json.size() > storage.size())
			storage = simdjson::padded_string(json.size());
		if (!json.empty())
			std::memcpy(storage.data(), json.data(), json.size());
		// storage owns storage.size() + SIMDJSON_PADDING readable bytes; claim
		// exactly the padding On-Demand requires past this (possibly shorter)
		// frame.
		const simdjson::padded_string_view view(
			storage.data(),
			json.size(),
			storage.size() + simdjson::SIMDJSON_PADDING);
		simdjson::ondemand::document doc;
		TRY_JSON(json_parser.iterate(view).get(doc), invalid_json);
		return doc;
	}
};

DepthParser::DepthParser() : impl_(std::make_unique<Impl>()) {}

DepthParser::~DepthParser() = default;

DepthParser::DepthParser(DepthParser &&) noexcept = default;

DepthParser &DepthParser::operator=(DepthParser &&) noexcept = default;

std::expected<DepthSnapshot, depth_parse_error>
DepthParser::parse_snapshot(std::string_view json, int price_decimals,
                            int qty_decimals) {
	auto doc = impl_->iterate(json);
	if (!doc) return std::unexpected(doc.error());
	return snapshot_from_doc(*doc, price_decimals, qty_decimals);
}

std::expected<DepthUpdate, depth_parse_error>
DepthParser::parse_update(std::string_view json, int price_decimals,
                          int qty_decimals) {
	auto doc = impl_->iterate(json);
	if (!doc) return std::unexpected(doc.error());
	return update_from_doc(*doc, price_decimals, qty_decimals);
}

std::expected<DepthUpdateMeta, depth_parse_error>
DepthParser::apply_update(order_book &book, std::string_view json,
                          int price_decimals, int qty_decimals) {
	auto doc = impl_->iterate(json);
	if (!doc) return std::unexpected(std::move(doc.error()));
	return stream_update_from_doc(book, *doc, price_decimals, qty_decimals);
}

void apply_depth_update(order_book &book, const DepthUpdate &update) {
	for (const auto &[price, volume] : update.bids)
		book.set_level(Side::BID, price, volume);
	for (const auto &[price, volume] : update.asks)
		book.set_level(Side::ASK, price, volume);
}

#undef TRY_JSON
} // namespace market_data::binance