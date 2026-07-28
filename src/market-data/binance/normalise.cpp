#include "normalise.hpp"

#include "binance_depth.hpp"
#include "market-data/normalised.hpp"

#include <chrono>
#include <cstdint>
#include <vector>

namespace exchange::market_data::binance {
namespace {

/// Binance stamps events in milliseconds since the Unix epoch; the neutral feed
/// speaks nanoseconds, so the unit conversion happens once, here.
timestamp to_timestamp(std::uint64_t event_time_ms) noexcept {
	return std::chrono::duration_cast<timestamp>(
		std::chrono::milliseconds(static_cast<std::int64_t>(event_time_ms)));
}

/// Copy a decoded side into neutral levels. The two structs are the same shape
/// — a price and an absolute size, both already scaled by the decoder — so this
/// is a field-wise copy and never a re-interpretation.
std::vector<book_level> to_levels(const std::vector<PriceLevel> &levels) {
	std::vector<book_level> normalised;
	normalised.reserve(levels.size());
	for (const auto &[price, volume] : levels)
		normalised.emplace_back(price, volume);
	return normalised;
}

} // namespace

sequence_range sequence_of(const DepthUpdateMeta &meta) noexcept {
	return sequence_range{meta.firstUpdateId, meta.finalUpdateId};
}

sequence_range sequence_of(const DepthUpdate &update) noexcept {
	return sequence_range{update.firstUpdateId, update.finalUpdateId};
}

depth_event normalise(const DepthUpdate &update) {
	return depth_event{sequence_of(update),
					   to_timestamp(update.eventTime),
					   to_levels(update.bids),
					   to_levels(update.asks)};
}

book_snapshot normalise(const DepthSnapshot &snapshot) {
	return book_snapshot{snapshot.lastUpdateId,
						 timestamp{},
						 to_levels(snapshot.bids),
						 to_levels(snapshot.asks)};
}

} // namespace exchange::market_data::binance
