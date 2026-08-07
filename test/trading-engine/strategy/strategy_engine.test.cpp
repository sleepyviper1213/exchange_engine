#include "strategy.fixture.hpp"
#include "trading-engine/order_book/outcome.hpp"
#include "trading-engine/order_book/trade.hpp"
#include "trading-engine/orders/types.hpp"
#include "trading-engine/strategy/command_writer.hpp"
#include "trading-engine/strategy/concepts.hpp"
#include "trading-engine/strategy/engine.hpp"
#include "trading-engine/strategy/iceberg.hpp"
#include "trading-engine/strategy/stop.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::strategy;
using exchange::test::strategy::print;
using exchange::test::strategy::recording_sink;

namespace {

constexpr symbol_id_t SYMBOL = 7;

/// @brief Emits one CANCEL per trade, so a test can count events by commands.
struct trade_echo {
	static constexpr std::size_t MAX_COMMANDS_PER_EVENT = 1;
	std::size_t seen                                    = 0;

	void on_trade(const Trade &t, command_writer &out) {
		++seen;
		out.cancel(t.price);
	}
};

/// @brief Subscribed to trades but emits nothing — proves an event costs no
///        command, and therefore no flush.
struct trade_watcher {
	static constexpr std::size_t MAX_COMMANDS_PER_EVENT = 1;
	std::size_t seen                                    = 0;

	void on_trade(const Trade & /*t*/, command_writer & /*out*/) { ++seen; }
};

struct outcome_echo {
	static constexpr std::size_t MAX_COMMANDS_PER_EVENT = 1;
	std::size_t seen                                    = 0;

	void on_outcome(const OrderOutcome &o, command_writer &out) {
		++seen;
		out.cancel(o.id);
	}
};

struct clock_echo {
	static constexpr std::size_t MAX_COMMANDS_PER_EVENT = 1;
	std::uint64_t last                                  = 0;

	void on_clock(std::uint64_t now_ns, command_writer &out) {
		last = now_ns;
		out.cancel(now_ns);
	}
};

std::vector<Trade> prints(std::size_t n) {
	std::vector<Trade> out;
	out.reserve(n);
	for (std::size_t i = 0; i < n; ++i)
		out.push_back(print(static_cast<price_t>(i + 1)));
	return out;
}

// ---------------------------------------------------------------------------
// Compile-time composition. None of this generates code; that is the point.
// ---------------------------------------------------------------------------

TEST(StrategyEngine, SubscriptionFlagsFollowTheComposition) {
	using both = strategy_engine<recording_sink, trade_echo, outcome_echo>;
	static_assert(both::OBSERVES_TRADES);
	static_assert(both::OBSERVES_OUTCOMES);
	static_assert(!both::OBSERVES_CLOCK);

	using trades_only = strategy_engine<recording_sink, trade_echo>;
	static_assert(trades_only::OBSERVES_TRADES);
	static_assert(!trades_only::OBSERVES_OUTCOMES);

	using clocked_only = strategy_engine<recording_sink, clock_echo>;
	static_assert(!clocked_only::OBSERVES_TRADES);
	static_assert(!clocked_only::OBSERVES_OUTCOMES);
	static_assert(clocked_only::OBSERVES_CLOCK);
	SUCCEED();
}

TEST(StrategyEngine, RealStrategiesSubscribeToExactlyOneStreamEach) {
	// Iceberg is driven by what became of its own orders; stop by the tape.
	// Neither wants the other's stream, and the host must not hand it over.
	static_assert(outcome_observer<iceberg<4>>);
	static_assert(!trade_observer<iceberg<4>>);
	static_assert(trade_observer<stop<4>>);
	static_assert(!outcome_observer<stop<4>>);
	static_assert(!clocked<iceberg<4>> && !clocked<stop<4>>);

	using host = strategy_engine<recording_sink, iceberg<4>, stop<4>>;
	static_assert(host::OBSERVES_TRADES && host::OBSERVES_OUTCOMES);
	static_assert(!host::OBSERVES_CLOCK);
	SUCCEED();
}

TEST(StrategyEngine, BufferIsSizedByTheSumOfTheDeclaredBounds) {
	// One outcome reaches at most one iceberg parent; one print can trigger
	// every armed stop. 1 + 4.
	using host = strategy_engine<recording_sink, iceberg<4>, stop<4>>;
	static_assert(host::COMMANDS_PER_EVENT == 5);
	static_assert(host::CAPACITY == 5 * host::EVENTS_PER_BATCH);

	using lean = strategy_engine<recording_sink, iceberg<4>>;
	static_assert(lean::COMMANDS_PER_EVENT == 1);
	static_assert(lean::CAPACITY == host::EVENTS_PER_BATCH);
	SUCCEED();
}

// A composition is pinned to its thread; the batch cursor points into the
// batch's own storage, so relocating one would leave it aimed at the corpse.
TEST(StrategyEngine, IsNeitherCopyableNorMovable) {
	using host = strategy_engine<recording_sink, trade_echo>;
	static_assert(!std::is_copy_constructible_v<host>);
	static_assert(!std::is_move_constructible_v<host>);
	static_assert(!std::is_copy_assignable_v<host>);
	SUCCEED();
}

// ---------------------------------------------------------------------------
// Fan-out
// ---------------------------------------------------------------------------

TEST(StrategyEngine, DeliversEachEventToEveryStrategySubscribedToIt) {
	recording_sink sink;
	auto host =
		compose(sink, SYMBOL, trade_echo{}, trade_watcher{}, outcome_echo{});

	const auto tape = prints(3);
	EXPECT_EQ(host.on_trades(tape), 3U);

	EXPECT_EQ(host.get<trade_echo>().seen, 3U);
	EXPECT_EQ(host.get<trade_watcher>().seen, 3U);
	// The outcome-driven strategy saw nothing: it is not on that stream.
	EXPECT_EQ(host.get<outcome_echo>().seen, 0U);
}

TEST(StrategyEngine, FeedingAStreamNobodySubscribedToIsANoOp) {
	recording_sink sink;
	auto host = compose(sink, SYMBOL, outcome_echo{});

	// Not merely harmless — on_trades does not walk the span at all, because
	// OBSERVES_TRADES is false and the loop is not instantiated.
	const auto tape = prints(100);
	EXPECT_EQ(host.on_trades(tape), 100U);
	EXPECT_EQ(host.pending(), 0U);
	EXPECT_EQ(sink.size(), 0U);
}

TEST(StrategyEngine, ClockReachesOnlyClockedStrategies) {
	recording_sink sink;
	auto host = compose(sink, SYMBOL, clock_echo{}, trade_echo{});

	EXPECT_TRUE(host.on_clock(1234));

	EXPECT_EQ(host.get<clock_echo>().last, 1234U);
	EXPECT_EQ(host.get<trade_echo>().seen, 0U);
	EXPECT_EQ(host.pending(), 1U);
}

TEST(StrategyEngine, ClockOnAnUnclockedCompositionSucceedsAndDoesNothing) {
	recording_sink sink;
	auto host = compose(sink, SYMBOL, trade_echo{});

	EXPECT_TRUE(host.on_clock(999));
	EXPECT_EQ(host.pending(), 0U);
}

TEST(StrategyEngine, StampsItsSymbolOnEverythingItPublishes) {
	recording_sink sink;
	auto host = compose(sink, SYMBOL, trade_echo{});

	EXPECT_EQ(host.symbol(), SYMBOL);
	EXPECT_EQ(host.on_trades(prints(2)), 2U);
	ASSERT_TRUE(host.flush());

	ASSERT_EQ(sink.size(), 2U);
	for (const auto &cmd : sink.commands()) EXPECT_EQ(cmd.symbol, SYMBOL);
}

// ---------------------------------------------------------------------------
// Batching and the watermark
// ---------------------------------------------------------------------------

TEST(StrategyEngine, HoldsCommandsUntilTheBufferFillsThenSubmitsOneBatch) {
	recording_sink sink;
	auto host                      = compose(sink, SYMBOL, trade_echo{});
	constexpr std::size_t capacity = decltype(host)::CAPACITY;

	// Exactly a bufferful: written, but nothing published yet.
	EXPECT_EQ(host.on_trades(prints(capacity)), capacity);
	EXPECT_EQ(host.pending(), capacity);
	EXPECT_EQ(sink.batches(), 0U);

	// One more event has nowhere to go, so the watermark flushes first.
	EXPECT_EQ(host.on_trades(prints(1)), 1U);
	EXPECT_EQ(sink.batches(), 1U);
	EXPECT_EQ(sink.size(), capacity);
	EXPECT_EQ(host.pending(), 1U);
}

TEST(StrategyEngine, FlushPublishesTheRemainderAndCountsIt) {
	recording_sink sink;
	auto host = compose(sink, SYMBOL, trade_echo{});

	EXPECT_EQ(host.on_trades(prints(3)), 3U);
	ASSERT_TRUE(host.flush());

	EXPECT_EQ(sink.size(), 3U);
	EXPECT_EQ(sink.batches(), 1U);
	EXPECT_EQ(host.pending(), 0U);
	EXPECT_EQ(host.submitted(), 3U);
	EXPECT_EQ(host.batches(), 1U);
}

TEST(StrategyEngine, FlushingNothingSucceedsWithoutTouchingTheSink) {
	recording_sink sink;
	auto host = compose(sink, SYMBOL, trade_echo{});

	EXPECT_TRUE(host.flush());
	EXPECT_TRUE(host.flush());
	EXPECT_EQ(sink.batches(), 0U);
	EXPECT_EQ(host.batches(), 0U);
}

TEST(StrategyEngine, EventsThatEmitNothingNeverReachTheSink) {
	recording_sink sink;
	auto host = compose(sink, SYMBOL, trade_watcher{});

	EXPECT_EQ(host.on_trades(prints(500)), 500U);
	ASSERT_TRUE(host.flush());

	EXPECT_EQ(host.get<trade_watcher>().seen, 500U);
	EXPECT_EQ(sink.batches(), 0U);
	EXPECT_EQ(host.submitted(), 0U);
}

// ---------------------------------------------------------------------------
// Back-pressure
// ---------------------------------------------------------------------------

TEST(StrategyEngine, StopsAtTheEventItCouldNotMakeRoomFor) {
	recording_sink sink;
	auto host                      = compose(sink, SYMBOL, trade_echo{});
	constexpr std::size_t capacity = decltype(host)::CAPACITY;

	sink.refuse(true);
	const auto tape            = prints(capacity + 5);
	const std::size_t consumed = host.on_trades(tape);

	// It filled the buffer, then declined to run past what it could hold.
	EXPECT_EQ(consumed, capacity);
	EXPECT_EQ(host.pending(), capacity);
	EXPECT_EQ(sink.size(), 0U);
	EXPECT_GE(host.stalls(), 1U);
	EXPECT_EQ(host.get<trade_echo>().seen, capacity);
}

TEST(StrategyEngine, KeepsTheRefusedBatchAndDeliversItOnRetry) {
	recording_sink sink;
	auto host = compose(sink, SYMBOL, trade_echo{});

	EXPECT_EQ(host.on_trades(prints(4)), 4U);
	sink.refuse(true);
	EXPECT_FALSE(host.flush());
	EXPECT_EQ(host.pending(), 4U);
	EXPECT_EQ(sink.size(), 0U);

	sink.refuse(false);
	EXPECT_TRUE(host.flush());
	EXPECT_EQ(sink.size(), 4U);
	EXPECT_EQ(host.pending(), 0U);
}

// The documented recovery loop, run against a sink that clears after one
// refusal: every event is consumed exactly once and every command arrives.
TEST(StrategyEngine,
	 ResumingFromTheConsumedCountLosesNothingAndRepeatsNothing) {
	recording_sink sink;
	auto host                      = compose(sink, SYMBOL, trade_echo{});
	constexpr std::size_t capacity = decltype(host)::CAPACITY;

	const auto tape = prints((capacity * 2) + 3);
	sink.refuse(true);

	std::span<const Trade> left{tape};
	std::size_t rounds = 0;
	while (!left.empty()) {
		left = left.subspan(host.on_trades(left));
		if (!left.empty()) {
			sink.refuse(false); // the consumer caught up
			EXPECT_TRUE(host.flush());
		}
		ASSERT_LT(++rounds, 10U) << "recovery loop is not converging";
	}
	ASSERT_TRUE(host.flush());

	EXPECT_EQ(sink.size(), tape.size());
	EXPECT_EQ(host.get<trade_echo>().seen, tape.size());
	// Commands come back in tape order, and each print produced exactly one.
	for (std::size_t i = 0; i < tape.size(); ++i)
		EXPECT_EQ(sink.commands()[i].as_cancel(), tape[i].price) << "at " << i;
}

TEST(StrategyEngine, ReserveIsWhatMakesRoomForACallerDrivenWrite) {
	recording_sink sink;
	auto host                      = compose(sink, SYMBOL, trade_echo{});
	constexpr std::size_t capacity = decltype(host)::CAPACITY;

	EXPECT_EQ(host.on_trades(prints(capacity)), capacity);
	ASSERT_EQ(host.pending(), capacity);

	// The buffer is full, so reserving flushes it and leaves room.
	ASSERT_TRUE(host.reserve());
	EXPECT_EQ(host.pending(), 0U);
	host.writer().cancel(1234);
	EXPECT_EQ(host.pending(), 1U);
}

TEST(StrategyEngine, NthAndGetReachTheSameStrategy) {
	recording_sink sink;
	auto host = compose(sink, SYMBOL, trade_echo{}, outcome_echo{});

	host.nth<0>().seen = 11;
	EXPECT_EQ(host.get<trade_echo>().seen, 11U);
	host.get<outcome_echo>().seen = 22;
	EXPECT_EQ(host.nth<1>().seen, 22U);
}

} // namespace
