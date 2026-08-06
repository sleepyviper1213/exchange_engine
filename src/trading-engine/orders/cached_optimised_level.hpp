
#pragma once


#include "trading-engine/orders/types.hpp"

#include <cstdint>
#include <new>

namespace exchange::engine::experimental {
/// @brief Cache-line-aligned aggregate level, padded to avoid false sharing
///        between the hot read fields and the running statistics.
struct alignas(std::hardware_destructive_interference_size)
	cache_optimised_level {
	price_t price;
	quantity_t volume;
	uint32_t count;
	uint32_t timestamp; // compressed

	uint64_t total_volume;
	uint32_t avg_order_size;
};
} // namespace exchange::engine::experimental