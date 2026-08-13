#include "strategy/backtest/session.hpp"

#include "backtest.fixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// The harness end to end. These are the cases that would catch a wiring
// mistake between the four things the session joins — the depth bridge, the
// risk gate, the matching engine and the fill model — none of which any of the
// component suites can see on its own.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::strategy::backtest;

namespace {

/// @brief A trader the test drives by hand: it emits whatever was queued into
///        it and keeps everything it was told.
///
/// Deliberately not a @c market_observer — a case that wants an order placed at
/// a particular point in the recording says so at that point, which reads far
/// better in a test than a callback that has to work out where it is.
struct scripted_trader {
	session::gate_type *sink;
	std::vector<command> pending;
	std::vector<trade> prints;
	std::vector<order_outcome> records;

	void place(order_id_t id, side_t side, price_t price, quantity_t qty) {
		pending.push_back(command::place(orders::order{.id        = id,
													   .symbol_id = 0,
													   .side      = side,
													   .price     = price,
													   .qty       = qty}));
	}

	std::size_t on_trades(std::span<const trade> batch) {
		prints.insert(prints.end(), batch.begin(), batch.end());
		return batch.size();
	}

	std::size_t on_outcomes(std::span<const order_outcome> batch) {
		records.insert(records.end(), batch.begin(), batch.end());
		return batch.size();
	}

	bool flush() {
		if (pending.empty()) return true;
		if (!sink->submit_range(pending)) return false;
		pending.clear();
		return true;
	}

	[[nodiscard]] std::size_t count(OutcomeType type) const {
		std::size_t n = 0;
		for (const order_outcome &record : records)
			n += static_cast<std::size_t>(record.type == type);
		return n;
	}
};

static_assert(trader<scripted_trader>);

/// @brief The bridge's invariant, asserted against the real engine book: every
///        price the venue publishes rests in the matching book at the same
///        size.
void expect_book_matches_replica(const session &run) {
	for (const auto &[price, qty] : run.replica().bid_levels())
		EXPECT_EQ(run.book().volume_at_price(static_cast<price_t>(price),
											 side_t::bid),
				  qty)
			<< "bid @" << price;
	for (const auto &[price, qty] : run.replica().ask_levels())
		EXPECT_EQ(run.book().volume_at_price(static_cast<price_t>(price),
											 side_t::ask),
				  qty)
			<< "ask @" << price;
}

} // namespace

// --- the harness on its own -------------------------------------------------

TEST(BacktestSession, SeedsTheEngineBookFromASnapshot) {
	const symbol_spec spec = unit_listing();
	session run(spec);
	null_trader idle;

	ASSERT_TRUE(
		run.on_snapshot(seed(10, {level(99, 50)}, {level(102, 50)}), idle));
	EXPECT_TRUE(run.live());
	expect_book_matches_replica(run);
	EXPECT_EQ(run.result().depth_commands, 2U) << "one ADD per side";
}

TEST(BacktestSession, KeepsTheEngineBookEqualToTheReplicaAcrossDiffs) {
	const symbol_spec spec = unit_listing();
	session run(spec);
	null_trader idle;

	ASSERT_TRUE(
		run.on_snapshot(seed(10, {level(99, 50)}, {level(102, 50)}), idle));
	run.on_event(diff(11, 1000, {level(99, 30), level(98, 20)}, {}), idle);
	expect_book_matches_replica(run);
	run.on_event(diff(12, 2000, {}, {level(102, 0), level(103, 40)}), idle);
	expect_book_matches_replica(run);

	run.finish(idle);
	const report &result = run.result();
	EXPECT_EQ(result.events_applied, 2U);
	EXPECT_EQ(result.fills(), 0U) << "no order flow, so nothing can trade";
	EXPECT_TRUE(result.clean());
}

TEST(BacktestSession, TakesMarketTimeFromTheRecording) {
	const symbol_spec spec = unit_listing();
	session run(spec);
	null_trader idle;

	ASSERT_TRUE(
		run.on_snapshot(seed(10, {level(99, 50)}, {level(102, 50)}), idle));
	run.on_event(diff(11, 1000, {}, {}), idle);
	run.on_event(diff(12, 5500, {}, {}), idle);
	run.finish(idle);

	EXPECT_EQ(run.clock().now_ns(), 5500U);
	EXPECT_EQ(run.result().covered_ns(), 4500U);
	EXPECT_EQ(run.result().clock_regressions, 0U);
}

// --- the passive path -------------------------------------------------------

TEST(BacktestSession, RestsAQuoteInsideTheSpreadWithoutFillingIt) {
	const symbol_spec spec = unit_listing();
	session run(spec);
	scripted_trader actor{.sink = &run.sink()};

	ASSERT_TRUE(
		run.on_snapshot(seed(10, {level(99, 50)}, {level(102, 50)}), actor));
	actor.place(1, side_t::bid, 101, 10);
	run.on_event(diff(11, 1000, {}, {}), actor);
	run.finish(actor);

	EXPECT_EQ(actor.count(OutcomeType::ACCEPTED), 1U);
	EXPECT_EQ(run.result().fills(), 0U)
		<< "the venue never offered below 101, so nothing traded";
	EXPECT_EQ(run.book().volume_at_price(101, side_t::bid), 10)
		<< "the quote is resting in the matching book";
}

TEST(BacktestSession, FillsARestingQuoteWhenTheVenueTradesThroughIt) {
	const symbol_spec spec = unit_listing();
	session run(spec);
	scripted_trader actor{.sink = &run.sink()};

	ASSERT_TRUE(
		run.on_snapshot(seed(10, {level(99, 50)}, {level(102, 50)}), actor));
	actor.place(1, side_t::bid, 101, 10);
	run.on_event(diff(11, 1000, {}, {}), actor);
	ASSERT_EQ(run.result().fills(), 0U);

	// The offer comes down through our bid, five lots deep.
	run.on_event(diff(12, 2000, {}, {level(102, 0), level(100, 5)}), actor);
	run.finish(actor);

	const report &result = run.result();
	EXPECT_EQ(result.passive_fills, 1U);
	EXPECT_EQ(result.passive_lots, 5);
	EXPECT_EQ(result.aggressive_fills, 0U);
	EXPECT_EQ(result.injected_aggressors, 1U);
	EXPECT_EQ(result.net_lots, 5) << "we are long what we bought";

	ASSERT_EQ(actor.prints.size(), 1U);
	EXPECT_EQ(actor.prints[0].aggressor, 0U) << "the venue came to us";
	EXPECT_EQ(actor.prints[0].resting, 1U);
	EXPECT_EQ(actor.prints[0].price, 101U)
		<< "a passive order fills at its own limit, not at the venue's";
	EXPECT_EQ(actor.prints[0].volume, 5);
	EXPECT_EQ(run.book().volume_at_price(101, side_t::bid), 5)
		<< "the unfilled half of the quote is still resting";
}

TEST(BacktestSession, MarksToTheVenueMidpointRatherThanToItsOwnLastFill) {
	const symbol_spec spec = unit_listing();
	session run(spec);
	scripted_trader actor{.sink = &run.sink()};

	ASSERT_TRUE(
		run.on_snapshot(seed(10, {level(99, 50)}, {level(102, 50)}), actor));
	actor.place(1, side_t::bid, 101, 10);
	run.on_event(diff(11, 1000, {}, {}), actor);
	run.on_event(diff(12, 2000, {}, {level(102, 0), level(100, 5)}), actor);
	run.finish(actor);

	// Market is 99 / 100, so the mid floors to 99. We bought 5 at 101.
	const report &result = run.result();
	EXPECT_EQ(result.mark, 99U);
	EXPECT_EQ(result.pnl_tick_lots, 5 * 99 - 5 * 101)
		<< "valued where the market is, not where we traded";
}

// --- the aggressive path, and the assumption it rests on --------------------

TEST(BacktestSession, CrossesTheVenuesPublishedDepthForAnAggressiveOrder) {
	const symbol_spec spec = unit_listing();
	session run(spec);
	scripted_trader actor{.sink = &run.sink()};

	ASSERT_TRUE(
		run.on_snapshot(seed(10, {level(99, 50)}, {level(102, 50)}), actor));
	actor.place(1, side_t::bid, 102, 10); // marketable against the offer
	run.on_event(diff(11, 1000, {}, {}), actor);

	const report &result = run.result();
	EXPECT_EQ(result.aggressive_fills, 1U);
	EXPECT_EQ(result.aggressive_lots, 10);
	EXPECT_EQ(result.passive_fills, 0U);
	EXPECT_EQ(result.depth_consumed_lots, 10);

	ASSERT_EQ(actor.prints.size(), 1U);
	EXPECT_EQ(actor.prints[0].aggressor, 1U) << "we went to the venue";
	EXPECT_EQ(actor.prints[0].resting, 0U);
	EXPECT_EQ(actor.prints[0].price, 102U);
	EXPECT_EQ(run.book().volume_at_price(102, side_t::ask), 40)
		<< "the depth we took is gone until the venue restates it";
}

// The other half of the no-market-impact assumption, and the reason
// depth_feed_bridge::consumed exists: without it the mirror still believes the
// engine holds 50 at 102, the next diff computes its delta from that, and the
// book stays ten lots short of what the venue publishes for the rest of the
// run.
TEST(BacktestSession, RestoresDepthAnAggressiveOrderConsumedOnTheNextDiff) {
	const symbol_spec spec = unit_listing();
	session run(spec);
	scripted_trader actor{.sink = &run.sink()};

	ASSERT_TRUE(
		run.on_snapshot(seed(10, {level(99, 50)}, {level(102, 50)}), actor));
	actor.place(1, side_t::bid, 102, 10);
	run.on_event(diff(11, 1000, {}, {}), actor);
	ASSERT_EQ(run.book().volume_at_price(102, side_t::ask), 40);
	ASSERT_EQ(run.bridge().consumed_lots(), 10);

	// The venue says nothing about 102 — it is still showing the same 50 — and
	// that silence is exactly the case the mirror has to get right.
	run.on_event(diff(12, 2000, {level(98, 5)}, {}), actor);
	run.finish(actor);

	EXPECT_EQ(run.book().volume_at_price(102, side_t::ask), 50);
	expect_book_matches_replica(run);
	EXPECT_TRUE(run.result().clean());
}

// --- the gate is in the path, not beside it ---------------------------------

TEST(BacktestSession, RefusesAnOrderThatBreachesTheConfiguredLimits) {
	const symbol_spec spec = unit_listing();
	session_options options;
	options.limits.max_order_qty = 5;
	session run(spec, options);
	scripted_trader actor{.sink = &run.sink()};

	ASSERT_TRUE(
		run.on_snapshot(seed(10, {level(99, 50)}, {level(102, 50)}), actor));
	actor.place(1, side_t::bid, 101, 50); // ten times the limit
	run.on_event(diff(11, 1000, {}, {}), actor);
	run.finish(actor);

	EXPECT_EQ(run.result().risk_refusals, 1U);
	EXPECT_EQ(actor.count(OutcomeType::REJECTED), 1U)
		<< "the refusal reaches the trader on the same stream as a fill would";
	EXPECT_EQ(run.book().volume_at_price(101, side_t::bid), 0)
		<< "and never reached a book";
}

// --- a gap is a real event, not a hiccup ------------------------------------

TEST(BacktestSession, WithdrawsSeededLiquidityWhenTheFeedGaps) {
	const symbol_spec spec = unit_listing();
	session run(spec);
	null_trader idle;

	ASSERT_TRUE(
		run.on_snapshot(seed(10, {level(99, 50)}, {level(102, 50)}), idle));
	ASSERT_EQ(run.book().volume_at_price(99, side_t::bid), 50);

	// Sequence 20 when 11 was expected: the replica is dead and the liquidity
	// it seeded is no longer evidence about the venue.
	EXPECT_EQ(run.on_event(diff(20, 1000, {}, {}), idle),
			  market_data::sequence_action::gap);
	run.finish(idle);

	EXPECT_FALSE(run.live());
	EXPECT_EQ(run.book().volume_at_price(99, side_t::bid), 0);
	EXPECT_EQ(run.book().volume_at_price(102, side_t::ask), 0);
	EXPECT_EQ(run.result().gaps, 1U);
	EXPECT_FALSE(run.result().clean()) << "a gapped run is not a clean replay";
}
