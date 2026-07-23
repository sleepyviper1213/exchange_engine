#pragma once
#include "order_book/side.hpp"
#include "order_book/types.hpp"

#include <memory>

namespace order_book {

using engine::OrderId;
using engine::Price;
using engine::Side;
using engine::Volume;


struct StopOrder {
	OrderId id;
	Side side;
	Price trigger_price;
	Volume volume; 
};

struct LimitOrder {
	OrderId id;
	Side side;
	Price price;
};

struct alignas(std::hardware_destructive_interference_size)
	cache_optimisied_level {
	Price price;
	Volume volume;
	uint32_t count;
	uint32_t timestamp;

	uint64_t total_volume;
	uint32_t avg_order_size;
};
} // namespace order_book