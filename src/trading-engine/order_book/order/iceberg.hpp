#pragma once
#include "core/types.hpp"

namespace exchange::engine {

/// @brief Resting iceberg order: only @c visible is exposed to the book, while
///        @c remaining_amount is drawn down from the hidden reserve and the
///        visible slice replenished as it fills.
struct IcebergOrder {
	OrderId id;
	Side side;
	Price price;
	Volume total_amount;
	Volume visible;
	Volume remaining_amount;
};

} // namespace exchange::engine
