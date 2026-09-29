#include "depth_parser.hpp"

#include "depth_parse_error.hpp"
#include "depth_snapshot.hpp"
#include "depth_update.hpp"
#include "depth_update_meta.hpp"
#include "detail/depth_decode.hpp" // the decode both entry points share

#include <memory>
#include <simdjson.h>


namespace exchange::market_data::binance {

/**
 * @brief Reusable parser state: a simdjson parser plus a padded input buffer,
 *        both amortised across successive frames.
 */
struct depth_parser::impl {
	simdjson::ondemand::parser json_parser;
	/// Reused input staging. simdjson::padded_string owns a buffer with the
	/// trailing padding On-Demand's over-read needs, but has no
	/// capacity-preserving assign - so we drive a grow-only policy by hand:
	/// reallocate only when a frame is larger than any seen so far, otherwise
	/// memcpy into the existing buffer. A steady feed thus does no per-frame
	/// allocation. Seeded non-empty so data() is never null on the empty-frame
	/// path.
	simdjson::padded_string storage{0uz};

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
		const simdjson::padded_string_view view(storage.data(),
												json.size(),
												storage.size() +
													simdjson::SIMDJSON_PADDING);
		simdjson::ondemand::document doc;
		TRY_JSON(json_parser.iterate(view).get(doc), invalid_json);
		return doc;
	}
};

depth_parser::depth_parser() : impl_(std::in_place) {}

depth_parser::~depth_parser() = default;

depth_parser::depth_parser(depth_parser &&) noexcept = default;

depth_parser &depth_parser::operator=(depth_parser &&) noexcept = default;

std::expected<depth_snapshot, depth_parse_error>
depth_parser::parse_snapshot(std::string_view json, int price_decimals,
							 int qty_decimals) {
	auto doc = impl_->iterate(json);
	if (!doc) return std::unexpected(doc.error());
	return detail::snapshot_from_doc(*doc, price_decimals, qty_decimals);
}

std::expected<depth_update, depth_parse_error>
depth_parser::parse_update(std::string_view json, int price_decimals,
						   int qty_decimals) {
	auto doc = impl_->iterate(json);
	if (!doc) return std::unexpected(doc.error());
	return detail::update_from_doc(*doc, price_decimals, qty_decimals);
}

std::expected<depth_update_meta, depth_parse_error>
depth_parser::apply_update(l2_book &book, std::string_view json,
						   int price_decimals, int qty_decimals) {
	auto doc = impl_->iterate(json);
	if (!doc) return std::unexpected(doc.error());
	return detail::stream_update_from_doc(book, *doc, price_decimals, qty_decimals);
}

} // namespace exchange::market_data::binance