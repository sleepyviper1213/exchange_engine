// What the pre-trade gate costs, split along the boundaries a latency budget is
// actually stated in.
//
// Three families, because "the risk check" is three different questions:
//
//   Limits*    the branchless arithmetic alone — every limit rule, no state
//              touched. This is the number a "limit validation" budget means.
//   Position*  a fill applied and a position read, which is the shared,
//              cross-thread half and the only place atomics appear.
//   Gate*      the whole inline path: screen, reserve in the ledger, hand to
//   the
//              sink, commit. This is what an order actually pays.
//
// The Gate family is reported both per batch and per command, and the gap
// between them is the point: the clock read, the breaker load and the position
// loads are hoisted once per batch, so a gate fed one command at a time pays
// for them on every order and a gate fed a strategy host's batch does not.

#include "trading-engine/event/command.hpp"
#include "trading-engine/order_book/trade.hpp"
#include "trading-engine/orders/order.hpp"
#include "trading-engine/orders/types.hpp"
#include "trading-engine/risk.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

using exchange::order_id_t;
using exchange::price_t;
using exchange::quantity_t;
using exchange::side_t;
using exchange::symbol_id_t;
using exchange::engine::trade;
using exchange::engine::event::command;
using exchange::engine::orders::order;
using exchange::engine::risk::circuit_breaker;
using exchange::engine::risk::position_book;
using exchange::engine::risk::rate_limiter;
using exchange::engine::risk::risk_gate;
using exchange::engine::risk::risk_limits;
using exchange::engine::risk::working_ledger;

namespace {

constexpr symbol_id_t SYMBOL = 1;
constexpr price_t MARK       = 10000;

/// @brief A sink that accepts and forgets. The gate is what is being measured,
///        so the thing downstream of it must not appear in the number.
struct null_sink {
	std::size_t count = 0;

	bool submit_range(std::span<const command> batch) noexcept {
		count += batch.size();
		return true;
	}
};

/**
 * @brief A clock that costs nothing, so a per-command number is about the
 * check.
 *
 * @c steady_clock::now() is a @c QueryPerformanceCounter on Windows and lands
 * around 20–30 ns — several times the whole per-command budget. It is read once
 * per batch, so in production it amortises to nothing; leaving it in a
 * per-command microbenchmark would measure the clock rather than the gate. The
 * @c GateBatch family below uses the real clock so the amortisation is visible
 * rather than assumed.
 */
struct free_clock {
	std::uint64_t ns = 0;

	[[nodiscard]] std::uint64_t now_ns() const noexcept { return ns; }
};

/// @brief Limits with every rule armed at a level nothing in these benchmarks
///        breaches. A rule set to "unlimited" would still be evaluated — the
///        checks do not short-circuit — but arming them keeps the comparands
///        realistic.
[[nodiscard]] risk_limits armed() {
	return risk_limits{.max_order_qty         = 10000,
					   .max_order_notional    = 1'000'000'000,
					   .max_position_lots     = 1'000'000,
					   .max_exposure_notional = 100'000'000'000LL,
					   .max_working_orders    = 1U << 16U,
					   .price_band_bps        = 500,
					   .max_messages_per_window =
						   std::numeric_limits<std::uint32_t>::max()};
}

[[nodiscard]] order limit_order(order_id_t id, quantity_t qty) {
	return {.id        = id,
			.symbol_id = SYMBOL,
			.side      = (id & 1U) != 0 ? side_t::bid : side_t::ask,
			.price     = MARK + static_cast<price_t>(id % 16U),
			.qty       = qty};
}

// --- limit validation ------------------------------------------------------

/**
 * @brief Every arithmetic rule, on an order that passes all of them.
 *
 * The worst case rather than the best: because the mask is built without
 * short-circuiting, an order that breaks the first rule costs exactly as much
 * as one that breaks none. That is the property being measured — there is no
 * fast path to fall into and no branch to mispredict.
 */
void BM_LimitsInspectPass(benchmark::State &state) {
	null_sink sink;
	position_book positions{8};
	circuit_breaker breaker;
	risk_gate<null_sink, free_clock> gate(sink,
										  SYMBOL,
										  armed(),
										  positions,
										  breaker,
										  MARK);
	const command cmd = command::place(limit_order(1, 10));

	for (auto _ : state) benchmark::DoNotOptimize(gate.inspect(cmd).bits());
	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_LimitsInspectPass);

/// @brief The same rules on an order that breaks three of them at once, to show
///        the cost does not depend on the answer.
void BM_LimitsInspectBreach(benchmark::State &state) {
	null_sink sink;
	position_book positions{8};
	circuit_breaker breaker;
	risk_gate<null_sink, free_clock> gate(sink,
										  SYMBOL,
										  armed(),
										  positions,
										  breaker,
										  MARK);
	// Oversize, over-notional and far outside the band.
	const command cmd = command::place(order{.id        = 1,
											 .symbol_id = SYMBOL,
											 .side      = side_t::bid,
											 .price     = MARK * 10,
											 .qty       = 900'000});

	for (auto _ : state) benchmark::DoNotOptimize(gate.inspect(cmd).bits());
	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_LimitsInspectBreach);

// --- position calculation --------------------------------------------------

/// @brief One execution applied: three single-writer counters moved, no lock
///        prefix. @see position_book on why this is not @c fetch_add.
void BM_PositionApplyFill(benchmark::State &state) {
	position_book positions{8};
	bool buy = true;
	for (auto _ : state) {
		positions.apply_fill(SYMBOL, buy ? side_t::bid : side_t::ask, MARK, 1);
		buy = !buy;
		benchmark::ClobberMemory();
	}
	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_PositionApplyFill);

/// @brief The read the gate does at the start of every batch.
void BM_PositionRead(benchmark::State &state) {
	position_book positions{8};
	positions.apply_fill(SYMBOL, side_t::bid, MARK, 500);
	positions.add_working(SYMBOL, side_t::bid, 200);
	positions.add_working(SYMBOL, side_t::ask, 150);

	for (auto _ : state) {
		benchmark::DoNotOptimize(positions.net_lots(SYMBOL));
		benchmark::DoNotOptimize(positions.working_lots(SYMBOL, side_t::bid));
		benchmark::DoNotOptimize(positions.working_lots(SYMBOL, side_t::ask));
	}
	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_PositionRead);

/// @brief All six counters plus the gross-exposure arithmetic — what a
///        dashboard or a firm-wide aggregator reads.
void BM_PositionSnapshot(benchmark::State &state) {
	position_book positions{8};
	positions.apply_fill(SYMBOL, side_t::bid, MARK, 500);
	positions.add_working(SYMBOL, side_t::ask, 150);

	for (auto _ : state)
		benchmark::DoNotOptimize(positions.snapshot(SYMBOL).gross_lots());
	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_PositionSnapshot);

// --- the supporting structures ---------------------------------------------

/// @brief The fixed-window rate check: a shift, a compare and an AND.
void BM_RateHeadroom(benchmark::State &state) {
	const rate_limiter limiter{1'000'000};
	std::uint64_t now = 0;
	for (auto _ : state) {
		now += 1000;
		benchmark::DoNotOptimize(limiter.headroom(now));
	}
	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_RateHeadroom);

/// @brief One insert and one retire in the working-order ledger — the part of
///        the check that is a hash probe rather than arithmetic.
void BM_LedgerInsertRetire(benchmark::State &state) {
	working_ledger ledger{1U << 14U};
	// Warm to a realistic occupancy; an empty table probes once every time and
	// would flatter the number.
	for (order_id_t id = 1; id <= 8000; ++id)
		benchmark::DoNotOptimize(ledger.insert(id, side_t::bid, MARK, 1));

	order_id_t next = 1'000'000;
	for (auto _ : state) {
		benchmark::DoNotOptimize(ledger.insert(++next, side_t::bid, MARK, 1));
		benchmark::DoNotOptimize(ledger.retire(next).has_value());
	}
	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_LedgerInsertRetire);

// --- the whole gate --------------------------------------------------------

/**
 * @brief The full inline path, one command at a time, with the clock excluded.
 *
 * Screen, ledger insert, deliver, commit, and — because a real strategy's
 * orders do not accumulate forever — the retirement that a fill would cause,
 * applied through @c on_trade so the ledger and the position both move. One
 * iteration is therefore an order's whole round trip through the gate, not just
 * its entry.
 */
void BM_GateSubmitAndFill(benchmark::State &state) {
	null_sink sink;
	position_book positions{8};
	circuit_breaker breaker;
	risk_gate<null_sink, free_clock> gate(sink,
										  SYMBOL,
										  armed(),
										  positions,
										  breaker,
										  MARK);
	order_id_t next = 0;

	for (auto _ : state) {
		const order o = limit_order(++next, 10);
		benchmark::DoNotOptimize(gate.submit(command::place(o)));
		gate.on_trade(trade{.aggressor = next,
							.resting   = 0,
							.price     = o.price,
							.volume    = o.qty});
	}
	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_GateSubmitAndFill);

/**
 * @brief The gate as a strategy host actually drives it: whole batches, real
 *        clock, nothing paused.
 *
 * @c strategy_engine accumulates @c EVENTS_PER_BATCH events before it flushes,
 * so a batch is the unit that really crosses this boundary. Sweeping the batch
 * size against a *real* @c steady_clock is what makes the amortisation visible:
 * the clock, the breaker load and the three position loads are read once per
 * @c submit_range whatever its length, so the per-item cost should fall steeply
 * from one and then flatten onto the marginal per-command work.
 *
 * @par Why nothing is paused
 * @c PauseTiming / @c ResumeTiming cost around a microsecond per pair — three
 * orders of magnitude above what is being measured, and enough to swamp the
 * whole sweep. So the batch is built once and *reused*: feeding the fills back
 * through @c on_trades retires every id, which returns the ledger and the
 * working totals to where the iteration found them and lets the same commands
 * go round again. One iteration is therefore B orders' complete round trip,
 * timed end to end, which is also what @c BM_GateSubmitAndFill measures at B
 * = 1.
 *
 * The sides alternate by id parity, so the net position oscillates about zero
 * instead of walking into the position limit over a long run.
 */
void BM_GateSubmitBatch(benchmark::State &state) {
	const auto batch_size = static_cast<std::size_t>(state.range(0));
	null_sink sink;
	position_book positions{8};
	circuit_breaker breaker;
	risk_gate gate(sink, SYMBOL, armed(), positions, breaker, MARK);

	std::vector<command> batch;
	std::vector<trade> fills;
	batch.reserve(batch_size);
	fills.reserve(batch_size);
	for (order_id_t id = 1; id <= batch_size; ++id) {
		const order o = limit_order(id, 10);
		batch.push_back(command::place(o));
		fills.push_back(trade{.aggressor = id,
							  .resting   = 0,
							  .price     = o.price,
							  .volume    = o.qty});
	}

	for (auto _ : state) {
		benchmark::DoNotOptimize(gate.submit_range(batch));
		gate.on_trades(fills);
	}
	state.SetItemsProcessed(state.iterations() *
							static_cast<std::int64_t>(batch_size));
}
BENCHMARK(BM_GateSubmitBatch)
->Arg(1)->Arg(4)->Arg(16)->Arg(64)->Arg(256);

} // namespace
