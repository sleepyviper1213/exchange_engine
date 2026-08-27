#pragma once
// Shared scaffolding for the strategy suites: a sink that records instead of
// queueing, and the small readers that turn an emitted command back into the
// fields a test wants to assert on.

#include "event/command.hpp"
#include "order_book/order_state.hpp"
#include "order_book/outcome.hpp"
#include "order_book/trade.hpp"
#include "orders/types.hpp"

#include <cstddef>
#include <span>
#include <vector>


// The scalar vocabulary. Spelled out because these fixtures sit at global
// scope: nothing here is inside `exchange`, so nothing is inherited from it.
using exchange::order_id_t;
using exchange::price_t;
using exchange::quantity_t;

using exchange::engine::order_outcome;
using exchange::engine::order_state;
using exchange::engine::OrderStatus;
using exchange::engine::OutcomeType;
using exchange::engine::trade;
using exchange::engine::event::command;

/**
 * @brief A command_sink that keeps what it is given, and can be told to refuse.
 *
 * Refusal is the interesting half: it is how a full SPSC queue looks to a
 * strategy host, and the tests need it to prove the host keeps the batch rather
 * than dropping it.
 */
class recording_sink {
public:
	bool submit_range(std::span<const command> batch) {
		if (refusing_) {
			++refusals_;
			return false;
		}
		commands_.append_range(batch);
		++batches_;
		return true;
	}

	/// @brief Start (or stop) rejecting every batch, as a full queue would.
	void refuse(bool on) noexcept { refusing_ = on; }

	[[nodiscard]] const std::vector<command> &commands() const noexcept {
		return commands_;
	}

	[[nodiscard]] std::size_t size() const noexcept { return commands_.size(); }

	/// @brief How many separate submit_range calls landed - the batching story.
	[[nodiscard]] std::size_t batches() const noexcept { return batches_; }

	[[nodiscard]] std::size_t refusals() const noexcept { return refusals_; }

	void clear() noexcept {
		commands_.clear();
		batches_ = 0;
	}

private:
	std::vector<command> commands_;
	std::size_t batches_  = 0;
	std::size_t refusals_ = 0;
	bool refusing_        = false;
};

/// @brief The outcome the book emits when @p id's resting quantity is entirely
///        taken - the only outcome an iceberg replenishes on.
inline order_outcome filled(order_id_t id, quantity_t qty) {
	order_state state{qty};
	state.apply_fill(qty);
	return order_outcome::fill(id, state);
}

/// @brief A fill that leaves @p id resting with quantity still in front of the
///        market.
inline order_outcome partially_filled(order_id_t id, quantity_t qty,
									  quantity_t executed) {
	order_state state{qty};
	state.apply_fill(executed);
	return order_outcome::fill(id, state);
}

/// @brief One print, at @p price. The ids are noise for a trade observer: a
///        stop watches the tape, not who was on either side of it.
inline trade strategy_print(price_t price, quantity_t volume = 1) {
	return trade{.aggressor = 0,
				 .resting   = 0,
				 .price     = price,
				 .volume    = volume};
}
