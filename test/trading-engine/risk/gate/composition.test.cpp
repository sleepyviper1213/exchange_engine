// The architectural claim, checked rather than asserted in prose: a gate is a
// command_sink, so it drops in between a strategy host and the gateway without
// either of them naming it.
//
// This is the only place in the tree that names strategy/ and risk/ together,
// and deliberately — neither module may depend on the other, so the conformance
// has nowhere to live but a test.

#include "gate.fixture.hpp"
#include "trading-engine/order_book/trade.hpp"
#include "trading-engine/orders/order.hpp"
#include "trading-engine/orders/types.hpp"
#include "trading-engine/risk/limits.hpp"
#include "trading-engine/strategy/command_writer.hpp"
#include "trading-engine/strategy/concepts.hpp"
#include "trading-engine/strategy/engine.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>

namespace {

using exchange::order_id_t;
using exchange::quantity_t;
using exchange::side_t;
using exchange::engine::trade;
using exchange::engine::orders::order;
using exchange::engine::risk::circuit_breaker;
using exchange::engine::risk::position_book;
using exchange::engine::risk::risk_gate;
using exchange::engine::risk::risk_limits;
using exchange::engine::strategy::command_writer;
using exchange::engine::strategy::compose;

using exchange::test::risk::permissive;
using exchange::test::risk::recording_sink;
using exchange::test::risk::SYMBOL;
using exchange::test::risk::test_gate;

// The claim itself. A failure here means a gate can no longer be handed to
// strategy_engine, whatever the tests below do.
static_assert(exchange::engine::strategy::command_sink<test_gate>,
			  "a risk gate must be usable wherever a partition is");
static_assert(
	exchange::engine::strategy::command_sink<risk_gate<recording_sink>>,
	"including with the default clock");

/// @brief Buys one lot at whatever just printed. Small enough that the test is
///        about the wiring rather than about the strategy.
class buy_the_print {
public:
	static constexpr std::size_t MAX_COMMANDS_PER_EVENT = 1;

	void on_trade(const trade &execution, command_writer &out) {
		out.place(order{.id    = next_id_++,
						.side  = side_t::bid,
						.price = execution.price,
						.qty   = 1});
	}

private:
	order_id_t next_id_ = 1;
};

TEST(RiskGateComposition, AHostWritesThroughTheGateAndReachesTheSink) {
	recording_sink sink;
	position_book positions{8};
	circuit_breaker breaker;
	risk_gate gate(sink, SYMBOL, permissive(), positions, breaker);
	auto host = compose(gate, SYMBOL, buy_the_print{});

	const std::array<trade, 2> prints{
		trade{.aggressor = 900, .resting = 901, .price = 100, .volume = 1},
		trade{.aggressor = 902, .resting = 903, .price = 101, .volume = 1}};
	ASSERT_EQ(host.on_trades(prints), 2U);
	ASSERT_TRUE(host.flush());

	ASSERT_EQ(sink.commands().size(), 2U);
	EXPECT_EQ(sink.commands()[0].as_place().price, 100U);
	EXPECT_EQ(sink.commands()[1].as_place().price, 101U);
	// The gate stamped nothing of its own; the writer's symbol survives.
	EXPECT_EQ(sink.commands()[0].symbol, SYMBOL);
	EXPECT_EQ(gate.passed(), 2U);
	EXPECT_EQ(gate.working_orders(), 2U);
}

TEST(RiskGateComposition, TheGateSilentlyDropsWhatTheHostShouldNotHaveSent) {
	// From the host's point of view the batch was accepted; the refused command
	// simply never reaches the book. That is the contract that keeps a host
	// from retrying an order that will be refused identically forever.
	recording_sink sink;
	position_book positions{8};
	circuit_breaker breaker;
	risk_limits limits        = permissive();
	limits.max_working_orders = 1;
	risk_gate gate(sink, SYMBOL, limits, positions, breaker);
	auto host = compose(gate, SYMBOL, buy_the_print{});

	const std::array<trade, 3> prints{
		trade{.aggressor = 900, .resting = 901, .price = 100, .volume = 1},
		trade{.aggressor = 902, .resting = 903, .price = 101, .volume = 1},
		trade{.aggressor = 904, .resting = 905, .price = 102, .volume = 1}};
	ASSERT_EQ(host.on_trades(prints), 3U);
	ASSERT_TRUE(host.flush());

	EXPECT_EQ(sink.commands().size(), 1U);
	EXPECT_EQ(gate.refused(), 2U);
	EXPECT_EQ(host.stalls(), 0U);
}

TEST(RiskGateComposition, TheHostSeesBackPressureThroughTheGateUnchanged) {
	recording_sink sink;
	position_book positions{8};
	circuit_breaker breaker;
	risk_gate gate(sink, SYMBOL, permissive(), positions, breaker);
	auto host = compose(gate, SYMBOL, buy_the_print{});

	sink.refuse(true);
	const std::array<trade, 1> print{
		trade{.aggressor = 900, .resting = 901, .price = 100, .volume = 1}};
	ASSERT_EQ(host.on_trades(print), 1U);
	EXPECT_FALSE(host.flush());
	EXPECT_EQ(host.stalls(), 1U);
	EXPECT_EQ(host.pending(), 1U);
	EXPECT_EQ(gate.working_orders(), 0U);

	// The host retries its intact batch and the gate behaves as though the
	// refused attempt never happened.
	sink.refuse(false);
	EXPECT_TRUE(host.flush());
	EXPECT_EQ(sink.commands().size(), 1U);
	EXPECT_EQ(gate.working_orders(), 1U);
}

TEST(RiskGateComposition, TwoGatesStackBecauseAGateIsAlsoASink) {
	// A per-strategy gate inside a per-desk one. Nothing special is needed: the
	// inner one's sink happens to be another gate.
	recording_sink sink;
	position_book desk_positions{8};
	position_book strategy_positions{8};
	circuit_breaker desk_breaker;
	circuit_breaker strategy_breaker;

	risk_limits desk   = permissive();
	desk.max_order_qty = 10;
	risk_gate desk_gate(sink, SYMBOL, desk, desk_positions, desk_breaker);

	risk_limits tighter   = permissive();
	tighter.max_order_qty = 1;
	risk_gate strategy_gate(desk_gate,
							SYMBOL,
							tighter,
							strategy_positions,
							strategy_breaker);

	const auto place = [&](order_id_t id, quantity_t qty) {
		return strategy_gate.submit(exchange::engine::event::command::place(
			exchange::test::risk::buy(id, 100, qty)));
	};

	EXPECT_TRUE(place(1, 1));
	EXPECT_EQ(sink.commands().size(), 1U);

	// Refused by the inner gate, so the outer one never sees it.
	EXPECT_TRUE(place(2, 5));
	EXPECT_EQ(sink.commands().size(), 1U);
	EXPECT_EQ(strategy_gate.refused(), 1U);
	EXPECT_EQ(desk_gate.refused(), 0U);
}

} // namespace
