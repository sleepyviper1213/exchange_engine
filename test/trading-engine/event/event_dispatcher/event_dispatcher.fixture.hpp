#pragma once
// The two ends every event_dispatcher suite needs: somewhere for events to come
// from, and somewhere for them to go.
//
// Both are deliberately dumber than the real thing. `scripted_source` hands out
// a script in chunks so a suite can decide exactly which events land in one
// pump, which is the only way to test the run-cutting without going through a
// matching engine. `recording_handler` writes down what it was given and can
// refuse part of it, which is the whole of the back-pressure contract.

#include "trading-engine/event/engine_event.hpp"
#include "trading-engine/order_book/outcome.hpp"
#include "trading-engine/order_book/trade.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

// A fixture at global scope cannot see these for free. @see testing.md
using exchange::order_id_t;
using exchange::quantity_t;
using exchange::symbol_id_t;
using exchange::engine::order_outcome;
using exchange::engine::trade;
using exchange::engine::event::engine_event;

/// @brief A trade distinguishable by @p aggressor alone, so a suite can assert
/// on identity without spelling four fields.
inline trade print(order_id_t aggressor) {
	return {.aggressor = aggressor, .resting = 99, .price = 100, .volume = 1};
}

/// @brief An event source that yields a fixed script, at most @c chunk per
/// call.
///
/// Not a queue: nothing is published into it while a suite runs, which is
/// exactly what makes a pump's boundaries predictable. @see event_source
class scripted_source {
public:
	explicit scripted_source(std::vector<engine_event> script,
							 std::size_t chunk = 64)
		: script_(std::move(script)), chunk_(chunk) {}

	std::size_t receive(std::span<engine_event> out) noexcept {
		const std::size_t count =
			std::min({chunk_, out.size(), script_.size() - cursor_});
		std::copy_n(script_.begin() + static_cast<std::ptrdiff_t>(cursor_),
					count,
					out.begin());
		cursor_ += count;
		++calls_;
		return count;
	}

	/// @brief How many times the dispatcher asked for events. A stalled pump
	/// 	   must not ask, which is what this counts.
	[[nodiscard]] std::size_t calls() const noexcept { return calls_; }

	[[nodiscard]] std::size_t remaining() const noexcept {
		return script_.size() - cursor_;
	}

private:
	std::vector<engine_event> script_;
	std::size_t chunk_;
	std::size_t cursor_ = 0;
	std::size_t calls_  = 0;
};

/// @brief One delivery, flattened so a whole session can be compared as a list.
struct delivery {
	symbol_id_t symbol;
	bool is_trade;
	order_id_t id; ///< the trade's aggressor, or the outcome's order id

	bool operator==(const delivery &) const noexcept = default;
};

/**
 * @brief Records every event it is handed, and optionally refuses some of them.
 *
 * @c take is the cap on how much of one span it will accept: the default of
 * @c npos accepts everything (the risk-gate shape, which cannot refuse), and a
 * finite value is a host whose command buffer filled part-way through a batch.
 */
class recording_handler {
public:
	explicit recording_handler(std::size_t take = static_cast<std::size_t>(-1))
		: take_(take) {}

	std::size_t on_trades(symbol_id_t symbol, std::span<const trade> trades) {
		const std::size_t count = std::min(take_, trades.size());
		for (std::size_t i = 0; i < count; ++i)
			seen_.emplace_back(symbol, true, trades[i].aggressor);
		trade_spans_.push_back(trades.size());
		return count;
	}

	std::size_t on_outcomes(symbol_id_t symbol,
							std::span<const order_outcome> outcomes) {
		const std::size_t count = std::min(take_, outcomes.size());
		for (std::size_t i = 0; i < count; ++i)
			seen_.emplace_back(symbol, false, outcomes[i].id);
		outcome_spans_.push_back(outcomes.size());
		return count;
	}

	/// @brief Start accepting everything again — how a suite resolves a stall.
	void unblock() noexcept { take_ = static_cast<std::size_t>(-1); }

	void set_take(std::size_t take) noexcept { take_ = take; }

	[[nodiscard]] const std::vector<delivery> &seen() const noexcept {
		return seen_;
	}

	/// @brief The size of each trade span it was handed, in order. A suite
	/// 	   asserts on this to prove runs were coalesced rather than
	///		   delivered one at a time — the amortisation the span interface
	///		   exists for.
	[[nodiscard]] const std::vector<std::size_t> &trade_spans() const noexcept {
		return trade_spans_;
	}

	[[nodiscard]] const std::vector<std::size_t> &
	outcome_spans() const noexcept {
		return outcome_spans_;
	}

private:
	std::size_t take_;
	std::vector<delivery> seen_;
	std::vector<std::size_t> trade_spans_;
	std::vector<std::size_t> outcome_spans_;
};
