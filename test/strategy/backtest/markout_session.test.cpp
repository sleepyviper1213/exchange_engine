#include "backtest.fixture.hpp"
#include "strategy/backtest/markout_report.hpp"
#include "strategy/backtest/null_trader.hpp"
#include "strategy/backtest/session.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// The markout wiring, through the real harness rather than against the recorder
// directly. What the unit suites cannot see is whether the session hands over
// the right side, the right stamp and the right midpoint - three things that
// are individually plausible and jointly easy to get wrong.
//
// The case that matters is the last one: a resting bid that gets filled because
// the offer came down through it, and then the market keeps going. That is
// adverse selection, and the curve has to show it as a loss.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::strategy::backtest;
using exchange::market_data::book_level;

namespace {

/// @brief A trader that places one order on demand. @see session.test.cpp,
///        which has the fuller version; this only needs to place.
struct markout_trader {
	session::gate_type *sink = nullptr;
	std::vector<command> pending{};

	void place(order_id_t id, side_t side, price_t price, quantity_t qty) {
		pending.push_back(command::place(orders::order{.id        = id,
													   .symbol_id = 0,
													   .side      = side,
													   .price     = price,
													   .qty       = qty}));
	}

	bool flush() {
		if (pending.empty()) return true;
		if (!sink->submit_range(pending)) return false;
		pending.clear();
		return true;
	}

	// Counted and discarded: this suite reads the curve, not the prints.
	std::size_t on_trades(std::span<const trade> batch) { return batch.size(); }

	std::size_t on_outcomes(std::span<const order_outcome> batch) {
		return batch.size();
	}
};

static_assert(trader<markout_trader>);

session_options markout_session_options() {
	return session_options{.markout = true};
}

} // namespace

TEST(MarkoutSession, ReportsNothingWhenTheOptionIsOff) {
	const symbol_spec spec = unit_listing();
	session run(spec);
	null_trader idle;

	ASSERT_TRUE(
		run.on_snapshot(seed(10,
							 std::to_array<book_level>({level(99, 50)}),
							 std::to_array<book_level>({level(102, 50)})),
						idle));
	run.finish(idle);

	// Six default horizons exist on the recorder, but nothing was ever fed to
	// it, so every bucket is empty rather than absent.
	const markout_report out = run.markout();
	for (std::size_t i = 0; i < out.count; ++i)
		EXPECT_EQ(resolved_fills(out.horizons[i]), 0U);
}

TEST(MarkoutSession, APassiveBuyPickedOffByAFallingMarketScoresNegative) {
	const symbol_spec spec = unit_listing();
	session run(spec, markout_session_options());
	markout_trader actor{.sink = &run.sink()};

	// Book is 99 / 102.
	ASSERT_TRUE(
		run.on_snapshot(seed(10,
							 std::to_array<book_level>({level(99, 50)}),
							 std::to_array<book_level>({level(102, 50)})),
						actor));

	// Five lots resting at 101, inside the spread. Exactly five, so the fill
	// below leaves nothing behind - a residue would be crossed again by the
	// collapse and the case would be measuring two fills, not one.
	actor.place(1, side_t::bid, 101, 5);
	run.on_event(diff(11, 1000, {}, {}), actor);
	ASSERT_EQ(total_fills(run.result()), 0U);

	// t = 2µs: the offer comes down through us and we buy five at 101.
	run.on_event(
		diff(12,
			 2000,
			 {},
			 std::to_array<book_level>({level(102, 0), level(100, 5)})),
		actor);
	ASSERT_EQ(run.result().passive_fills, 1U) << "the setup must fill us";
	ASSERT_EQ(run.result().passive_lots, 5);

	// t = 5ms: the market goes. 90 / 92, so the mid is 91 - ten ticks below
	// what we paid.
	run.on_event(
		diff(13,
			 5'000'000,
			 std::to_array<book_level>({level(99, 0), level(90, 50)}),
			 std::to_array<book_level>({level(100, 0), level(92, 50)})),
		actor);

	// t = 20ms, quiet. The 10ms horizon fell at 10.002ms, between this frame
	// and the last, so it resolves against the 5ms frame - the last mid at or
	// before the deadline. This frame exists only to carry time past it.
	run.on_event(diff(14, 20'000'000, {}, {}), actor);
	run.finish(actor);

	const markout_report out = run.markout();
	ASSERT_GT(out.count, 0U);

	// Only the 10ms horizon is inside the recording; the rest outlive it.
	const markout_report::bucket &at_10ms = out.horizons[0];
	ASSERT_EQ(resolved_fills(at_10ms), 1U);
	EXPECT_EQ(at_10ms.passive_fills, 1U);
	EXPECT_EQ(at_10ms.passive_lots, 5);
	// Bought at 101, mid at the deadline 91. Ten ticks against us, twenty
	// half-ticks, five lots: -100. A sign error reads +100, and a strategy
	// being picked off would look profitable.
	EXPECT_EQ(at_10ms.passive_half_tick_lots, -100);
	EXPECT_EQ(passive_markout_per_lot(at_10ms), -20);

	EXPECT_GT(out.horizons[1].unresolved, 0U)
		<< "50ms is past the end of a 20ms recording";
}

TEST(MarkoutSession, ARestingBuyTheMarketRunsAwayFromScoresPositive) {
	const symbol_spec spec = unit_listing();
	session run(spec, markout_session_options());
	markout_trader actor{.sink = &run.sink()};

	ASSERT_TRUE(
		run.on_snapshot(seed(10,
							 std::to_array<book_level>({level(99, 50)}),
							 std::to_array<book_level>({level(102, 50)})),
						actor));
	actor.place(1, side_t::bid, 101, 5);
	run.on_event(diff(11, 1000, {}, {}), actor);

	run.on_event(
		diff(12,
			 2000,
			 {},
			 std::to_array<book_level>({level(102, 0), level(100, 5)})),
		actor);
	ASSERT_EQ(run.result().passive_fills, 1U);

	// Same shape, opposite move: 110 / 112, mid 111.
	run.on_event(
		diff(13,
			 5'000'000,
			 std::to_array<book_level>({level(99, 0), level(110, 50)}),
			 std::to_array<book_level>({level(100, 0), level(112, 50)})),
		actor);
	run.on_event(diff(14, 20'000'000, {}, {}), actor);
	run.finish(actor);

	const markout_report out              = run.markout();
	const markout_report::bucket &at_10ms = out.horizons[0];
	ASSERT_EQ(resolved_fills(at_10ms), 1U);
	// Bought at 101, mid rose to 111: ten ticks our way.
	EXPECT_EQ(at_10ms.passive_half_tick_lots, 100);
}

TEST(MarkoutSession, TheCurveIsStampedInMarketTimeNotWallTime) {
	const symbol_spec spec = unit_listing();
	session run(spec, markout_session_options());
	markout_trader actor{.sink = &run.sink()};

	ASSERT_TRUE(
		run.on_snapshot(seed(10,
							 std::to_array<book_level>({level(99, 50)}),
							 std::to_array<book_level>({level(102, 50)})),
						actor));
	actor.place(1, side_t::bid, 101, 10);
	run.on_event(diff(11, 1000, {}, {}), actor);
	run.on_event(
		diff(12,
			 2000,
			 {},
			 std::to_array<book_level>({level(102, 0), level(100, 5)})),
		actor);
	ASSERT_EQ(run.result().passive_fills, 1U);
	run.finish(actor);

	// The recording stops 2µs after it started. Wall time spent running this
	// test is milliseconds, so a wall clock would have retired every horizon;
	// market time has barely moved and the long ones must still be open.
	const markout_report out = run.markout();
	EXPECT_GT(out.horizons[out.count - 1].unresolved, 0U)
		<< "the longest horizon cannot have resolved in 2µs of market time";
}
