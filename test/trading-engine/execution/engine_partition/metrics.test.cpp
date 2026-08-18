#include "trading-engine/execution/engine_partition.hpp"

#include <gtest/gtest.h>

#include <vector>

// engine_partition's optional partition_metrics: given one, drain() and
// flush() keep it in step with what the partition actually did; given none
// (the default), nothing about behaviour changes - a metrics pointer must be
// strictly additive.

using namespace exchange::engine;
using namespace exchange::engine::orders;
using namespace exchange;
using namespace exchange::engine::event;
using namespace exchange::engine::execution;

namespace {

TEST(EnginePartitionMetrics, NoMetricsGivenMeansNothingIsRecordedOrCrashed) {
	engine_partition<256> partition(nullptr);
	EXPECT_EQ(partition.metrics(), nullptr);
	partition.listing(0);

	ASSERT_TRUE(partition.submit(
		command::place({.id = 1, .side = side_t::bid, .price = 99, .qty = 5})));
	EXPECT_EQ(partition.drain_and_flush(), 1U);
}

TEST(EnginePartitionMetrics, DrainCountsEveryCommandApplied) {
	partition_metrics metrics;
	engine_partition<256> partition(nullptr,
									{},
									book_manager::DEFAULT_BOOK_CAPACITY,
									order_manager::DEFAULT_CAPACITY,
									&metrics);
	ASSERT_EQ(partition.metrics(), &metrics);
	partition.listing(0);

	ASSERT_TRUE(partition.submit(
		command::place({.id = 1, .side = side_t::bid, .price = 99, .qty = 5})));
	ASSERT_TRUE(partition.submit(
		command::place({.id = 2, .side = side_t::bid, .price = 98, .qty = 3})));
	EXPECT_EQ(partition.drain_and_flush(), 2U);

	EXPECT_EQ(metrics.commands_processed.load(), 2U);
	// One observation for the one drain() call above, whatever its duration.
	EXPECT_EQ(metrics.drain_latency_ns.read().total, 1U);
}

TEST(EnginePartitionMetrics, FlushCountsTradesPublishedNotJustQueued) {
	partition_metrics metrics;
	engine_partition<256> partition(nullptr,
									{},
									book_manager::DEFAULT_BOOK_CAPACITY,
									order_manager::DEFAULT_CAPACITY,
									&metrics);
	partition.listing(0);

	// A resting ask, then a bid that crosses it: one trade.
	ASSERT_TRUE(partition.submit(command::place(
		{.id = 1, .side = side_t::ask, .price = 100, .qty = 10})));
	ASSERT_TRUE(partition.submit(command::place(
		{.id = 2, .side = side_t::bid, .price = 100, .qty = 4})));
	EXPECT_EQ(partition.drain_and_flush(), 2U);

	EXPECT_EQ(metrics.trades_emitted.load(), 1U);
}

TEST(EnginePartitionMetrics, MisroutesCounterAgreesWithMisroutedAccessor) {
	partition_metrics metrics;
	engine_partition<256> partition(nullptr,
									{},
									book_manager::DEFAULT_BOOK_CAPACITY,
									order_manager::DEFAULT_CAPACITY,
									&metrics);
	partition.listing(1); // symbol 0 below is never registered

	ASSERT_TRUE(partition.submit(command::place(
		{.id = 1, .symbol_id = 0, .side = side_t::bid, .price = 99, .qty = 5})));
	EXPECT_EQ(partition.drain(), 1U);

	EXPECT_EQ(partition.misrouted(), 1U);
	EXPECT_EQ(metrics.misroutes.load(), 1U);
}

TEST(EnginePartitionMetrics, FlushWithNoTradesLeavesTradesEmittedUnchanged) {
	partition_metrics metrics;
	engine_partition<256> partition(nullptr,
									{},
									book_manager::DEFAULT_BOOK_CAPACITY,
									order_manager::DEFAULT_CAPACITY,
									&metrics);
	partition.listing(0);

	// Rests with nothing to cross - no trade.
	ASSERT_TRUE(partition.submit(
		command::place({.id = 1, .side = side_t::bid, .price = 99, .qty = 5})));
	EXPECT_EQ(partition.drain_and_flush(), 1U);

	EXPECT_EQ(metrics.trades_emitted.load(), 0U);
}

} // namespace
