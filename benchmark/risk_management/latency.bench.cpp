// Per-command latency of the pre-trade gate, as a distribution.
//
// gate.bench.cpp answers "what does this cost on average". This file answers
// the question the budgets in docs/performance.md are actually written in: all
// three risk targets are *latency* budgets, and a mean cannot tell a path that
// is uniformly quick from one that is quick 99 times and terrible on the
// hundredth. Google Benchmark aggregates over repetitions of a whole loop, so a
// single slow call vanishes into the millions around it - hence the cycle
// counter in latency.fixture.hpp.
//
// Each family here maps onto exactly one budget:
//
//   LimitCheck      limit validation      < 50 ns
//   PositionApply   position calculation  < 100 ns
//   Submit          risk check            < 200 ns

#include "latency.fixture.hpp"
#include "risk.fixture.hpp"
#include "event/command.hpp"
#include "order_book/trade.hpp"
#include "orders/types.hpp"
#include "risk_management/hooks/system/circuit_breaker.hpp"
#include "risk_management/gate.hpp"
#include "risk_management/hooks/pre_trade/position.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <vector>

using exchange::order_id_t;
using exchange::side_t;
using exchange::bench::latency_sampler;
using exchange::bench::risk::armed;
using exchange::bench::risk::free_clock;
using exchange::bench::risk::limit_order;
using exchange::bench::risk::MARK;
using exchange::bench::risk::null_sink;
using exchange::bench::risk::SYMBOL;
using exchange::engine::trade;
using exchange::engine::event::command;
using exchange::risk::hooks::system::circuit_breaker;
using exchange::risk::hooks::pre_trade::position_book;
using exchange::risk::risk_gate;

namespace {

#if EXCHANGE_HAS_CYCLE_CLOCK

/// @brief Orders left resting while the timed ones come and go, so the ledger
///        probes against a realistic occupancy rather than an empty table.
constexpr order_id_t RESTING = 4000;

/**
 * @brief Limit validation alone: all ten rules, nothing mutated.
 *
 * Budget 50 ns. The distribution should be nearly degenerate - the rules are a
 * fixed sequence of compares over values already in L1, with one branch at the
 * end - so a wide p99 here would mean the state gather is missing cache, not
 * that some order is harder to check than another.
 */
void BM_GateLatency_LimitCheck(benchmark::State &state) {
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

	latency_sampler sampler;
	for (auto _ : state)
		sampler.sample(
			[&] { benchmark::DoNotOptimize(gate.inspect(cmd).bits()); });
	sampler.publish(state);
}

BENCHMARK(BM_GateLatency_LimitCheck);

/**
 * @brief One execution into the position book.
 *
 * Budget 100 ns. Three single-writer relaxed read-modify-writes on one cache
 * line - @see position_book on why these are not @c fetch_add.
 */
void BM_GateLatency_PositionApply(benchmark::State &state) {
	position_book positions{8};
	bool buy = true;

	latency_sampler sampler;
	for (auto _ : state) {
		sampler.sample([&] {
			positions.apply_fill(SYMBOL,
								 buy ? side_t::bid : side_t::ask,
								 MARK,
								 1);
			benchmark::ClobberMemory();
		});
		buy = !buy;
	}
	sampler.publish(state);
}

BENCHMARK(BM_GateLatency_PositionApply);

/// @brief Reading the position, which is what @c open_batch does once a batch.
void BM_GateLatency_PositionRead(benchmark::State &state) {
	position_book positions{8};
	positions.apply_fill(SYMBOL, side_t::bid, MARK, 500);
	positions.add_working(SYMBOL, side_t::ask, 150);

	latency_sampler sampler;
	for (auto _ : state)
		sampler.sample(
			[&] { benchmark::DoNotOptimize(positions.snapshot(SYMBOL)); });
	sampler.publish(state);
}

BENCHMARK(BM_GateLatency_PositionRead);

/**
 * @brief The whole inline path for one order: screen, reserve, deliver, commit.
 *
 * Budget 200 ns, and this is the pessimistic way to spend it - a batch of one,
 * so the order pays the clock read, the breaker load and the position read by
 * itself instead of sharing them with fifteen others. The real clock is used
 * rather than @c free_clock precisely because that cost is real at this batch
 * size.
 *
 * Retiring the order afterwards keeps the ledger at a steady occupancy, and is
 * left outside the timed region: it is the fill path, which has its own cost
 * and would otherwise average into a number that is supposed to be about
 * submission.
 */
void BM_GateLatency_Submit(benchmark::State &state) {
	null_sink sink;
	position_book positions{8};
	circuit_breaker breaker;
	risk_gate gate(sink, SYMBOL, armed(), positions, breaker, MARK);

	// Warm the ledger so an insert probes a table under load rather than one
	// that answers on the first slot every time.
	for (order_id_t id = 1; id <= RESTING; ++id)
		benchmark::DoNotOptimize(
			gate.submit(command::place(limit_order(id, 1))));

	order_id_t next = RESTING;
	latency_sampler sampler;
	for (auto _ : state) {
		const auto o        = limit_order(++next, 10);
		const command place = command::place(o);

		sampler.sample([&] { benchmark::DoNotOptimize(gate.submit(place)); });

		// Untimed, but still on Google Benchmark's clock - so the Time column
		// is a per-iteration average of submit *and* retire. The percentiles
		// are the answer here.
		gate.on_trade(trade{.aggressor = next,
							.resting   = 0,
							.price     = o.price,
							.volume    = o.qty});
	}
	sampler.publish(state);
}

BENCHMARK(BM_GateLatency_Submit);

/**
 * @brief The same single-order path with the clock read taken out.
 *
 * The difference against @c BM_GateLatency_Submit is the whole cost of
 * @c steady_clock::now(), tail included - and on Windows that is a
 * @c QueryPerformanceCounter, which has a far worse p99 than its mean suggests.
 * Splitting the two matters because the gate reads the clock *once per batch*:
 * whatever shows up here is what an order actually pays, and whatever the
 * difference is gets divided by the batch size in production.
 */
void BM_GateLatency_SubmitNoClock(benchmark::State &state) {
	null_sink sink;
	position_book positions{8};
	circuit_breaker breaker;
	risk_gate<null_sink, free_clock> gate(sink,
										  SYMBOL,
										  armed(),
										  positions,
										  breaker,
										  MARK);

	for (order_id_t id = 1; id <= RESTING; ++id)
		benchmark::DoNotOptimize(
			gate.submit(command::place(limit_order(id, 1))));

	order_id_t next = RESTING;
	latency_sampler sampler;
	for (auto _ : state) {
		const auto o        = limit_order(++next, 10);
		const command place = command::place(o);
		sampler.sample([&] { benchmark::DoNotOptimize(gate.submit(place)); });
		gate.on_trade(trade{.aggressor = next,
							.resting   = 0,
							.price     = o.price,
							.volume    = o.qty});
	}
	sampler.publish(state);
}

BENCHMARK(BM_GateLatency_SubmitNoClock);

/**
 * @brief The production shape: one @c submit_range carrying a whole batch.
 *
 * @c strategy_engine flushes @c EVENTS_PER_BATCH events at a time, so this -
 * not a batch of one - is the call that actually crosses the boundary. The
 * counters are the distribution of the *whole* batch, which is the right unit:
 * a strategy waits for the call to return, not for any one order inside it.
 * Divide by
 * @c state.range(0) for the per-order figure the budget is written in, and the
 * @c per_order_p99_ns counter does that for you.
 */
void BM_GateLatency_SubmitBatch(benchmark::State &state) {
	const auto batch_size = static_cast<std::size_t>(state.range(0));
	null_sink sink;
	position_book positions{8};
	circuit_breaker breaker;
	risk_gate gate(sink, SYMBOL, armed(), positions, breaker, MARK);

	for (order_id_t id = 1; id <= RESTING; ++id)
		benchmark::DoNotOptimize(
			gate.submit(command::place(limit_order(id, 1))));

	std::vector<command> batch;
	std::vector<trade> fills;
	batch.reserve(batch_size);
	fills.reserve(batch_size);
	for (std::size_t i = 0; i < batch_size; ++i) {
		const auto o = limit_order(RESTING + 1 + i, 10);
		batch.push_back(command::place(o));
		fills.push_back(trade{.aggressor = o.id,
							  .resting   = 0,
							  .price     = o.price,
							  .volume    = o.qty});
	}

	latency_sampler sampler;
	for (auto _ : state) {
		sampler.sample(
			[&] { benchmark::DoNotOptimize(gate.submit_range(batch)); });
		gate.on_trades(fills);
	}
	sampler.publish(state);
	state.counters["per_order_p99_ns"] =
		state.counters["p99_ns"] / static_cast<double>(batch_size);
	state.counters["per_order_p50_ns"] =
		state.counters["p50_ns"] / static_cast<double>(batch_size);
}
BENCHMARK(BM_GateLatency_SubmitBatch)
->Arg(16)->Arg(64);

#endif // EXCHANGE_HAS_CYCLE_CLOCK

} // namespace
