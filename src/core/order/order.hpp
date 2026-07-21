#pragma once
#include "engine/side.hpp"
#include "engine/types.hpp"

namespace core::order {

using engine::OrderId;
using engine::Price;
using engine::Side;
using engine::Volume;

struct IcebergOrder {
	OrderId id;
	Side side;
	Price price;
	Volume total_amount;
	Volume visible;
	Volume remaining_amount;
};

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

} // namespace core::order