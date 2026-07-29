#pragma once
#include "core/types.hpp"

#include <cstdint>
#include <new>

namespace exchange::engine {

/// @brief A stop order that becomes marketable once the tape trades through
///        @c trigger_price. Scaffold: not yet wired into the matching path.
struct StopOrder {
	order_id id;
	side side;
	price trigger_price;
	quantity volume;
};

/// @brief A plain resting limit order. Scaffold placeholder for the richer
///        order taxonomy the strategies build on.
struct LimitOrder {
	order_id id;
	side side;
	price price;
};

/// @brief Cache-line-aligned aggregate level, padded to avoid false sharing
///        between the hot read fields and the running statistics.
struct alignas(std::hardware_destructive_interference_size)
	cache_optimised_level {
	price price;
	quantity volume;
	uint32_t count;
	uint32_t timestamp;

	uint64_t total_volume;
	uint32_t avg_order_size;
};
} // namespace exchange::engine
