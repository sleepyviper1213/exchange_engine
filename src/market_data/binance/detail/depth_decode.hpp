#pragma once
// The simdjson decoding shared by the two depth entry points.
//
// `parse_depth.hpp`'s one-shot functions and `depth_parser`'s reusable one run
// exactly the same decode; only the ownership of the parser differs. This used
// to live in an anonymous namespace inside binance_depth.cpp, which was fine
// while both callers were in that one translation unit and stopped being fine
// the moment depth_parser moved into its own.
//
// Internal to market_data/binance: `detail`, not exported, and no part of the
// module's surface. Everything is `inline` rather than `static` so the two
// translation units share one definition instead of each carrying a private
// copy of a 270-line decoder.

#include "core/scaled/fixed_point.hpp"
#include "core/util/function_ref.hpp"
#include "market_data/binance/depth_parse_error.hpp"
#include "market_data/binance/depth_snapshot.hpp"
#include "market_data/binance/depth_update.hpp"
#include "market_data/binance/depth_update_meta.hpp"
#include "market_data/binance/detail/jsonl_frame.hpp" // read_optional_u64
#include "market_data/l2_book.hpp"
#include "market_data/types.hpp"
#include "market_data/binance/parse_scaled.hpp"
#include "market_data/binance/price_level.hpp"

#include <concepts>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <simdjson.h>
#include <string>
#include <utility>
#include <vector>

/// @brief On a simdjson error from @p expr, bail out of the enclosing function
/// with a @c depth_parse_error of category @p category carrying simdjson's
/// (static) message. @c err is scoped to the generated block, so repeated use
/// in one function is fine. Local to this TU; @c \#undef'd at end of file.
#ifndef TRY_JSON
#define TRY_JSON(expr, category)                                               \
	do {                                                                       \
		if (const auto err = (expr))                                           \
			return std::unexpected(                                            \
				depth_parse_error{depth_error::category,                       \
								  simdjson::error_message(err)});              \
	} while (false)
#endif

namespace exchange::market_data::binance::detail {

/// Iterate a bids/asks array of ["price","qty"] string pairs, scaling each pair
/// and handing (price, qty) to @p on_level. Single pass, no copy of the raw
/// decimal strings - @c parse_scaled is pure and never touches the iterator.
///
/// A shape/numeric failure must not abandon the iterator mid-stream: that would
/// leave the reused parser's depth bookkeeping inconsistent and trip its debug
/// assertions. So the first bad level is recorded, iteration still runs to
/// completion, and the error is returned only once no iterator is in flight.
/// @note @p on_level may already have fired for earlier levels when an error is
///       returned; callers needing all-or-nothing must buffer (see
///       parse_levels).
inline std::expected<void, depth_parse_error> for_each_level(
	simdjson::ondemand::value array_value, int price_decimals, int qty_decimals,
	core::util::function_ref<void(scaled_price_t, scaled_qty_t) const>
		on_level) {
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

		if (deferred) continue; // already failed - keep draining the array
		if (count != 2) {
			deferred = depth_parse_error{depth_error::malformed_level};
			continue;
		}
		// Parse straight to parse_error; its message() is a static view, safe
		// to carry as context past this call.
		auto price = core::scaled::parse_fixed_point(fields[0], price_decimals);
		if (!price) {
			deferred = depth_parse_error{depth_error::bad_number,
										 core::scaled::message(price.error())};
			continue;
		}
		auto qty = core::scaled::parse_fixed_point(fields[1], qty_decimals);
		if (!qty) {
			deferred = depth_parse_error{depth_error::bad_number,
										 core::scaled::message(qty.error())};
			continue;
		}
		on_level(static_cast<scaled_price_t>(*price),
				 static_cast<scaled_qty_t>(*qty));
	}
	if (deferred) return std::unexpected(*deferred);
	return {};
}

/// Read a bids/asks array into a vector of scaled levels. All-or-nothing: on
/// error the half-built vector is discarded, so the caller never sees partial
/// data.
inline std::expected<std::vector<PriceLevel>, depth_parse_error>
parse_levels(const simdjson::ondemand::value &array_value, int price_decimals,
			 int qty_decimals) {
	std::vector<PriceLevel> levels;
	auto applied =
		for_each_level(array_value,
					   price_decimals,
					   qty_decimals,
					   [&](scaled_price_t price, scaled_qty_t volume) {
						   levels.emplace_back(price, volume);
					   });
	if (!applied) return std::unexpected(applied.error());
	return levels;
}

/// Read a bids/asks array and apply each scaled level straight to @p book on
/// @p side via set_level - no intermediate vector. @warning Not atomic: on a
/// malformed level, the levels before it are already applied (see
/// for_each_level).
inline std::expected<void, depth_parse_error>
stream_levels(l2_book &book, side_t side, simdjson::ondemand::value array_value,
			  int price_decimals, int qty_decimals) {
	return for_each_level(array_value,
						  price_decimals,
						  qty_decimals,
						  [&](scaled_price_t price, scaled_qty_t volume) {
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
inline std::expected<std::pair<std::vector<PriceLevel>, std::vector<PriceLevel>>,
			  depth_parse_error>
parse_sides(simdjson::ondemand::document &doc, std::string_view bid_key,
			std::string_view ask_key, int price_decimals, int qty_decimals) {
	using namespace simdjson;

	ondemand::value bids_value;
	if (doc[bid_key].get(bids_value))
		return std::unexpected(
			depth_parse_error{depth_error::missing_field, bid_key});
	auto bids = parse_levels(bids_value, price_decimals, qty_decimals);
	if (!bids) return std::unexpected(bids.error());

	ondemand::value asks_value;
	if (doc[ask_key].get(asks_value))
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
inline std::expected<void, depth_parse_error>
stream_sides(l2_book &book, simdjson::ondemand::document &doc,
			 std::string_view bid_key, std::string_view ask_key,
			 int price_decimals, int qty_decimals) {
	using namespace simdjson;

	ondemand::value bids_value;
	if (doc[bid_key].get(bids_value))
		return std::unexpected(
			depth_parse_error{depth_error::missing_field, bid_key});
	if (auto r = stream_levels(book,
							   side_t::bid,
							   bids_value,
							   price_decimals,
							   qty_decimals);
		!r)
		return std::unexpected(r.error());

	ondemand::value asks_value;
	if (doc[ask_key].get(asks_value))
		return std::unexpected(
			depth_parse_error{depth_error::missing_field, ask_key});
	if (auto r = stream_levels(book,
							   side_t::ask,
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
/// and reads on. Any other error is a structural fault: On-Demand parses
/// lazily, so a document that survived @c iterate() can still turn out to be
/// garbage here, and simdjson has already abandoned the iterator by the time it
/// reports it. Querying such a document again trips its depth assertions, so
/// the error is propagated and parsing stops.
/// @see the read_optional_u64 template in jsonl_frame.hpp, which both decoders share.
[[nodiscard]] inline std::expected<std::uint64_t, depth_parse_error>
depth_optional_u64(simdjson::ondemand::document &doc, std::string_view key,
				  std::uint64_t fallback = 0) {
	return detail::read_optional_u64<depth_parse_error>(
		doc,
		key,
		depth_error::invalid_json,
		fallback);
}

/**
 * @brief Build a depth_snapshot from an already-iterated depth document.
 *
 * Shared by the one-shot free function and the reusable depth_parser so the
 * field-order contract (lastUpdateId, then bids, then asks) lives in one place.
 */
inline std::expected<depth_snapshot, depth_parse_error>
snapshot_from_doc(simdjson::ondemand::document &doc, int price_decimals,
				  int qty_decimals) {
	depth_snapshot snapshot;
	auto last = depth_optional_u64(doc, "lastUpdateId");
	if (!last) return std::unexpected(last.error());
	snapshot.lastUpdateId = *last;

	auto sides = parse_sides(doc, "bids", "asks", price_decimals, qty_decimals);
	if (!sides) return std::unexpected(sides.error());

	snapshot.bids = std::move(sides->first);
	snapshot.asks = std::move(sides->second);
	return snapshot;
}

/**
 * @brief Build a depth_update from an already-iterated depthUpdate document.
 *
 * Shared by the one-shot free function and the reusable depth_parser. Fields
 * are read in document order (E, U, u, then b, a) so On-Demand never rewinds.
 */
inline std::expected<depth_update, depth_parse_error>
update_from_doc(simdjson::ondemand::document &doc, int price_decimals,
				int qty_decimals) {
	depth_update update;
	auto event = depth_optional_u64(doc, "E");
	if (!event) return std::unexpected(event.error());
	update.eventTime = *event;
	auto first       = depth_optional_u64(doc, "U");
	if (!first) return std::unexpected(first.error());
	update.firstUpdateId = *first;
	auto last            = depth_optional_u64(doc, "u");
	if (!last) return std::unexpected(last.error());
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
inline std::expected<depth_update_meta, depth_parse_error>
stream_update_from_doc(l2_book &book, simdjson::ondemand::document &doc,
					   int price_decimals, int qty_decimals) {
	depth_update_meta meta;
	auto event = depth_optional_u64(doc, "E");
	if (!event) return std::unexpected(event.error());
	meta.eventTime = *event;
	auto first     = depth_optional_u64(doc, "U");
	if (!first) return std::unexpected(first.error());
	meta.firstUpdateId = *first;
	auto last          = depth_optional_u64(doc, "u");
	if (!last) return std::unexpected(last.error());
	meta.finalUpdateId = *last;

	if (auto r =
			stream_sides(book, doc, "b", "a", price_decimals, qty_decimals);
		!r)
		return std::unexpected(r.error());
	return meta;
}

} // namespace exchange::market_data::binance::detail
