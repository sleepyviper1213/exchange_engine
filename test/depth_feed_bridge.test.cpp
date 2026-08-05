#include "app/depth_feed_bridge.hpp"

#include "trading-engine/order_book.hpp"

#include <gtest/gtest.h>

#include <vector>

// The join between the two subsystems: a venue's published depth becoming this
// engine's resting liquidity. The property under test throughout is the bridge's
// one invariant — after any call, draining the emitted commands into an
// order_book leaves its aggregate depth equal to the replica. Everything else
// (gaps, resyncs, evictions) is a case that invariant has to survive.

using exchange::app::depth_feed_bridge;
using exchange::engine::order_book;
using exchange::market_data::book_snapshot;
using exchange::market_data::depth_event;
using exchange::market_data::sequence_action;
using exchange::price_t;
using exchange::quantity_t;
using exchange::side_t;

namespace {

/// @brief Apply every command to @p book, as a partition's drain would.
void drain(order_book &book,
		   const std::vector<depth_feed_bridge::command> &cmds) {
	using command = depth_feed_bridge::command;
	for (const command &cmd : cmds) {
		switch (cmd.type) {
		case command::Type::ADD:
			book.add_order(cmd.level.side, cmd.level.price, cmd.level.volume);
			break;
		case command::Type::REDUCE:
			book.delete_order(cmd.level.side, cmd.level.price,
							  cmd.level.volume);
			break;
		case command::Type::PLACE:
		case command::Type::CANCEL: FAIL() << "the bridge emits depth only";
		}
	}
}

/// @brief The invariant: the engine's book agrees with the venue replica on
///        every price either of them quotes.
void expect_book_matches_replica(const order_book &book,
								 const depth_feed_bridge &bridge) {
	for (const auto &[price, qty] : bridge.replica().bid_levels())
		EXPECT_EQ(book.volume_at_price(price, side_t::bid), qty)
			<< "bid @" << price;
	for (const auto &[price, qty] : bridge.replica().ask_levels())
		EXPECT_EQ(book.volume_at_price(price, side_t::ask), qty)
			<< "ask @" << price;
	EXPECT_EQ(book.best_bid(), bridge.replica().best_bid());
	EXPECT_EQ(book.best_ask(), bridge.replica().best_ask());
}

book_snapshot snapshot_at(std::uint64_t sequence) {
	return {.sequence = sequence,
			.bids     = {{.price = 100, .qty = 10}, {.price = 99, .qty = 5}},
			.asks     = {{.price = 101, .qty = 7}}};
}

depth_event event_over(std::uint64_t first, std::uint64_t last,
					   std::vector<exchange::market_data::book_level> bids,
					   std::vector<exchange::market_data::book_level> asks) {
	return {.sequence = {.first = first, .last = last},
			.bids     = std::move(bids),
			.asks     = std::move(asks)};
}

} // namespace

TEST(DepthFeedBridge, EmitsNothingBeforeASnapshot) {
	depth_feed_bridge bridge(1);
	std::vector<depth_feed_bridge::command> cmds;

	const auto action =
		bridge.on_event(event_over(5, 6, {{.price = 100, .qty = 3}}, {}), cmds);

	EXPECT_EQ(action, sequence_action::buffer);
	EXPECT_TRUE(cmds.empty());
	EXPECT_FALSE(bridge.live());
	EXPECT_TRUE(bridge.needs_snapshot());
}

TEST(DepthFeedBridge, ASnapshotSeedsTheBookWithAddCommands) {
	depth_feed_bridge bridge(1);
	std::vector<depth_feed_bridge::command> cmds;
	order_book book;

	ASSERT_TRUE(bridge.on_snapshot(snapshot_at(100), cmds));
	drain(book, cmds);

	EXPECT_TRUE(bridge.live());
	EXPECT_EQ(cmds.size(), 3u); // two bids, one ask
	expect_book_matches_replica(book, bridge);
	EXPECT_EQ(book.volume_at_price(100, side_t::bid), 10);
	EXPECT_EQ(book.volume_at_price(101, side_t::ask), 7);
}

// Every emitted command names the listing, so the dispatcher can route it
// without knowing anything about market data.
TEST(DepthFeedBridge, EveryCommandCarriesTheSymbol) {
	depth_feed_bridge bridge(42);
	std::vector<depth_feed_bridge::command> cmds;

	ASSERT_TRUE(bridge.on_snapshot(snapshot_at(100), cmds));

	ASSERT_FALSE(cmds.empty());
	for (const auto &cmd : cmds) EXPECT_EQ(cmd.symbol, 42u);
	EXPECT_EQ(bridge.symbol(), 42u);
}

TEST(DepthFeedBridge, AnInSequenceDiffMovesTheBookByTheDelta) {
	depth_feed_bridge bridge(1);
	std::vector<depth_feed_bridge::command> cmds;
	order_book book;

	ASSERT_TRUE(bridge.on_snapshot(snapshot_at(100), cmds));
	drain(book, cmds);
	cmds.clear();

	// 100 grows 10 -> 14, 101 shrinks 7 -> 2, 98 is new.
	const auto action = bridge.on_event(
		event_over(101, 101, {{.price = 100, .qty = 14}, {.price = 98, .qty = 4}},
				   {{.price = 101, .qty = 2}}),
		cmds);
	drain(book, cmds);

	EXPECT_EQ(action, sequence_action::apply);
	expect_book_matches_replica(book, bridge);
	EXPECT_EQ(book.volume_at_price(100, side_t::bid), 14);
	EXPECT_EQ(book.volume_at_price(98, side_t::bid), 4);
	EXPECT_EQ(book.volume_at_price(101, side_t::ask), 2);
}

// A zero size is the wire's way of deleting a price, and the engine's book has
// to lose the level rather than keep quoting it.
TEST(DepthFeedBridge, AZeroSizeRemovesTheLevelFromTheEngineBook) {
	depth_feed_bridge bridge(1);
	std::vector<depth_feed_bridge::command> cmds;
	order_book book;

	ASSERT_TRUE(bridge.on_snapshot(snapshot_at(100), cmds));
	drain(book, cmds);
	cmds.clear();

	bridge.on_event(event_over(101, 101, {{.price = 99, .qty = 0}}, {}), cmds);
	drain(book, cmds);

	EXPECT_EQ(book.volume_at_price(99, side_t::bid), 0);
	EXPECT_EQ(book.best_bid(), 100u);
	expect_book_matches_replica(book, bridge);
}

TEST(DepthFeedBridge, AnEventTheSnapshotAlreadyCoversChangesNothing) {
	depth_feed_bridge bridge(1);
	std::vector<depth_feed_bridge::command> cmds;
	order_book book;

	ASSERT_TRUE(bridge.on_snapshot(snapshot_at(100), cmds));
	drain(book, cmds);
	cmds.clear();

	const auto action = bridge.on_event(
		event_over(90, 95, {{.price = 100, .qty = 999}}, {}), cmds);

	EXPECT_EQ(action, sequence_action::discard);
	EXPECT_TRUE(cmds.empty());
	EXPECT_EQ(book.volume_at_price(100, side_t::bid), 10);
}

// The one that matters. When the replica dies, liquidity seeded from it stops
// being evidence about the venue, and matching against it would be matching
// against the past — so the depth has to be withdrawn, not left behind.
TEST(DepthFeedBridge, AGapWithdrawsEveryLevelItHadSeeded) {
	depth_feed_bridge bridge(1);
	std::vector<depth_feed_bridge::command> cmds;
	order_book book;

	ASSERT_TRUE(bridge.on_snapshot(snapshot_at(100), cmds));
	drain(book, cmds);
	cmds.clear();
	ASSERT_TRUE(book.best_bid().has_value());

	// Expected 101; 105 means events were lost.
	const auto action = bridge.on_event(
		event_over(105, 106, {{.price = 100, .qty = 12}}, {}), cmds);
	drain(book, cmds);

	EXPECT_EQ(action, sequence_action::gap);
	EXPECT_FALSE(bridge.live());
	EXPECT_FALSE(book.best_bid().has_value());
	EXPECT_FALSE(book.best_ask().has_value());
	EXPECT_EQ(book.volume_at_price(100, side_t::bid), 0);
	EXPECT_EQ(bridge.reconstructor().stats().gaps, 1u);
}

TEST(DepthFeedBridge, AFreshSnapshotAfterAGapReseedsTheEngineBook) {
	depth_feed_bridge bridge(1);
	std::vector<depth_feed_bridge::command> cmds;
	order_book book;

	ASSERT_TRUE(bridge.on_snapshot(snapshot_at(100), cmds));
	drain(book, cmds);
	cmds.clear();
	bridge.on_event(event_over(105, 106, {{.price = 100, .qty = 12}}, {}), cmds);
	drain(book, cmds);
	cmds.clear();
	ASSERT_FALSE(bridge.live());

	ASSERT_TRUE(bridge.on_snapshot(snapshot_at(200), cmds));
	drain(book, cmds);

	EXPECT_TRUE(bridge.live());
	expect_book_matches_replica(book, bridge);
	EXPECT_EQ(book.volume_at_price(100, side_t::bid), 10);
}

TEST(DepthFeedBridge, InvalidateWithdrawsTheDepthAndAsksForASnapshot) {
	depth_feed_bridge bridge(1);
	std::vector<depth_feed_bridge::command> cmds;
	order_book book;

	ASSERT_TRUE(bridge.on_snapshot(snapshot_at(100), cmds));
	drain(book, cmds);
	cmds.clear();

	bridge.invalidate(cmds);
	drain(book, cmds);

	EXPECT_FALSE(bridge.live());
	EXPECT_TRUE(bridge.needs_snapshot());
	EXPECT_FALSE(book.best_bid().has_value());
	// Not a sequence gap: the numbers never said anything was wrong.
	EXPECT_EQ(bridge.reconstructor().stats().gaps, 0u);
}

// A snapshot that lands after events have been buffered replays them onto the
// seeded book. Deriving commands from each event alone would miss that replay
// entirely, which is why the bridge diffs book states instead.
TEST(DepthFeedBridge, BufferedEventsReplayedByASnapshotReachTheEngineBook) {
	depth_feed_bridge bridge(1);
	std::vector<depth_feed_bridge::command> cmds;
	order_book book;

	// Buffered while unsynced, so nothing is emitted for them yet.
	bridge.on_event(event_over(101, 101, {{.price = 100, .qty = 20}}, {}), cmds);
	bridge.on_event(event_over(102, 102, {}, {{.price = 101, .qty = 3}}), cmds);
	ASSERT_TRUE(cmds.empty());

	ASSERT_TRUE(bridge.on_snapshot(snapshot_at(100), cmds));
	drain(book, cmds);

	EXPECT_TRUE(bridge.live());
	expect_book_matches_replica(book, bridge);
	// The snapshot said 10 and 7; the replayed diffs moved them to 20 and 3.
	EXPECT_EQ(book.volume_at_price(100, side_t::bid), 20);
	EXPECT_EQ(book.volume_at_price(101, side_t::ask), 3);
}

TEST(DepthFeedBridge, MirrorTracksTheReplicaAfterEveryCall) {
	depth_feed_bridge bridge(1);
	std::vector<depth_feed_bridge::command> cmds;

	const auto agrees = [&bridge] {
		EXPECT_EQ(bridge.mirror().depth(side_t::bid),
				  bridge.replica().depth(side_t::bid));
		EXPECT_EQ(bridge.mirror().depth(side_t::ask),
				  bridge.replica().depth(side_t::ask));
		EXPECT_EQ(bridge.mirror().best_bid(), bridge.replica().best_bid());
		EXPECT_EQ(bridge.mirror().best_ask(), bridge.replica().best_ask());
	};

	ASSERT_TRUE(bridge.on_snapshot(snapshot_at(100), cmds));
	agrees();
	bridge.on_event(event_over(101, 101, {{.price = 97, .qty = 8}}, {}), cmds);
	agrees();
	bridge.on_event(event_over(999, 999, {}, {}), cmds); // gap
	agrees();
	ASSERT_TRUE(bridge.on_snapshot(snapshot_at(2000), cmds));
	agrees();
}

// Re-applying the same in-sequence state must not emit churn: the diff is
// against what the engine already holds, not against the event.
TEST(DepthFeedBridge, AnEventThatChangesNothingEmitsNothing) {
	depth_feed_bridge bridge(1);
	std::vector<depth_feed_bridge::command> cmds;

	ASSERT_TRUE(bridge.on_snapshot(snapshot_at(100), cmds));
	cmds.clear();

	// Restates sizes the book already has.
	const auto action = bridge.on_event(
		event_over(101, 101, {{.price = 100, .qty = 10}}, {{.price = 101, .qty = 7}}),
		cmds);

	EXPECT_EQ(action, sequence_action::apply);
	EXPECT_TRUE(cmds.empty());
}
