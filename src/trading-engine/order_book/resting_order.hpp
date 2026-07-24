#pragma once

#include "types.hpp"

namespace order_book {

/// @brief A resting order living in the pool; FIFO links come from Node<T>.
class RestingOrder {
public:
	RestingOrder() = default;
	TRADING_ENGINE_EXPORT RestingOrder(OrderId id, Volume volume) noexcept;

	[[nodiscard]] TRADING_ENGINE_EXPORT OrderId id() const noexcept;
	[[nodiscard]] TRADING_ENGINE_EXPORT Volume volume() const noexcept;
	[[nodiscard]] TRADING_ENGINE_EXPORT bool has_quantity() const noexcept;
	TRADING_ENGINE_EXPORT void decrease_volume_by(Volume amount) noexcept;
private:
	OrderId id_;
	Volume volume_;
};

} // namespace order_book