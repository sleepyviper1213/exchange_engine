#include "strategy/backtest/depth_feed_bridge.hpp"

#include "trading-engine/order_book.hpp"
#include "trading-engine/symbol/symbol_spec.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <vector>

// The join between the two subsystems: a venue's published depth becoming this
// engine's resting liquidity. The property under test throughout is the bridge's
// one invariant — after any call, draining the emitted commands into an
// order_book leaves its aggregate depth equal to the replica. Everything else
// (gaps, resyncs, evictions) is a case that invariant has to survive.

using exchange::strategy::backtest::depth_feed_bridge;
using exchange::engine::order_book;
using exchange::engine::symbol_spec;
using exchange::market_data::book_snapshot;
using exchange::market_data::depth_event;
using exchange::market_data::sequence_action;
using exchange::market_data::sequence_t;
using exchange::price_t;
using exchange::quantity_t;
using exchange::side_t;

namespace {

/// @brief A listing whose tick and lot grids are both 1, at scale 0.
///
/// The feed's scaled values and the engine's ticks and lots then coincide, so
/// every case below can keep writing prices and sizes as plain integers while
/// still going through the real @c symbol_spec conversion the bridge performs.
/// A listing with a coarser grid is a different test — this one is about the
/// diff logic, not about rounding.
symbol_spec unit_spec(exchange::symbol_id_t id) {
	return symbol_spec{id, "TEST", 0, 0, 1, 1, 100};
}

/// @brief Apply every command to @p book, as a partition's drain would.
void drain(order_book &book,
		   const std::vector<depth_feed_bridge::command> &cmds) {
	using command = depth_feed_bridge::command;
	for (const command &cmd : cmds) {
		switch (cmd.type) {
		case command::Type::ADD: {
			const auto &lvl = cmd.as_level();
			book.add_order(lvl.side, lvl.price, lvl.volume);
			break;
		}
		case command::Type::REDUCE: {
			const auto &lvl = cmd.as_level();
			book.delete_order(lvl.side, lvl.price, lvl.volume);
			break;
		}
		case command::Type::PLACE:
		case command::Type::CANCEL: FAIL() << "the bridge emits depth only";
		}
	}
}

/// @brief The invariant: the engine's book agrees with the venue replica on
///        every price either of them quotes.
void expect_book_matches_replica(const order_book &book,
								 const depth_feed_bridge &bridge) {
	// The replica speaks the feed's scaled numbers and the book speaks ticks and
	// lots. Under unit_spec they are numerically the same, so the casts here are
	// the type system asking which side of the boundary each value came from
	// rather than a conversion doing any work.
	for (const auto &[price, qty] : bridge.replica().bid_levels())
		EXPECT_EQ(book.volume_at_price(static_cast<price_t>(price), side_t::bid),
				  qty)
			<< "bid @" << price;
	for (const auto &[price, qty] : bridge.replica().ask_levels())
		EXPECT_EQ(book.volume_at_price(static_cast<price_t>(price), side_t::ask),
				  qty)
			<< "ask @" << price;

	const auto as_ticks =
		[](std::optional<exchange::market_data::scaled_price_t> scaled) {
			return scaled.has_value()
					   ? std::optional<price_t>{static_cast<price_t>(*scaled)}
					   : std::nullopt;
		};
	EXPECT_EQ(book.best_bid(), as_ticks(bridge.replica().best_bid()));
	EXPECT_EQ(book.best_ask(), as_ticks(bridge.replica().best_ask()));
}

book_snapshot snapshot_at(sequence_t sequence) {
	return {.sequence = sequence,
			.bids     = {{.price = 100, .qty = 10}, {.price = 99, .qty = 5}},
			.asks     = {{.price = 101, .qty = 7}}};
}

depth_event event_over(sequence_t first, sequence_t last,
					   std::vector<exchange::market_data::book_level> bids,
					   std::vector<exchange::market_data::book_level> asks) {
	// Positional, not designated: inclusive_range declares a constructor now, so
	// it is no longer an aggregate and .first/.last do not name initialisers.
	return {.sequence = {first, last},
			.bids     = std::move(bids),
			.asks     = std::move(asks)};
}

} // namespace

TEST(DepthFeedBridge, EmitsNothingBeforeASnapshot) {
	const symbol_spec spec = unit_spec(1);
	depth_feed_bridge bridge(spec);
	std::vector<depth_feed_bridge::command> cmds;

	const auto action =
		bridge.on_event(event_over(5, 6, {{.price = 100, .qty = 3}}, {}), cmds);

	EXPECT_EQ(action, sequence_action::buffer);
	EXPECT_TRUE(cmds.empty());
	EXPECT_FALSE(bridge.live());
	EXPECT_TRUE(bridge.needs_snapshot());
}

TEST(DepthFeedBridge, ASnapshotSeedsTheBookWithAddCommands) {
	const symbol_spec spec = unit_spec(1);
	depth_feed_bridge bridge(spec);
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
	const symbol_spec spec = unit_spec(42);
	depth_feed_bridge bridge(spec);
	std::vector<depth_feed_bridge::command> cmds;

	ASSERT_TRUE(bridge.on_snapshot(snapshot_at(100), cmds));

	ASSERT_FALSE(cmds.empty());
	for (const auto &cmd : cmds) EXPECT_EQ(cmd.symbol, 42u);
	EXPECT_EQ(bridge.symbol(), 42u);
}

TEST(DepthFeedBridge, AnInSequenceDiffMovesTheBookByTheDelta) {
	const symbol_spec spec = unit_spec(1);
	depth_feed_bridge bridge(spec);
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
	const symbol_spec spec = unit_spec(1);
	depth_feed_bridge bridge(spec);
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
	const symbol_spec spec = unit_spec(1);
	depth_feed_bridge bridge(spec);
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
	const symbol_spec spec = unit_spec(1);
	depth_feed_bridge bridge(spec);
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
	const symbol_spec spec = unit_spec(1);
	depth_feed_bridge bridge(spec);
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
	const symbol_spec spec = unit_spec(1);
	depth_feed_bridge bridge(spec);
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
	const symbol_spec spec = unit_spec(1);
	depth_feed_bridge bridge(spec);
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
	const symbol_spec spec = unit_spec(1);
	depth_feed_bridge bridge(spec);
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
	const symbol_spec spec = unit_spec(1);
	depth_feed_bridge bridge(spec);
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

// --- depth an identified order took out of the book -------------------------
//
// The mirror is this class's model of what the engine holds in *anonymous*
// depth, and every command it emits is a delta against that model. A match that
// crosses into the seeded liquidity moves the book without moving the mirror,
// which is the one way the model can go wrong on its own. `consumed` is how it
// is told; these two cases are what it buys.

TEST(DepthFeedBridge, ConsumptionMakesTheNextDiffRestoreWhatWasTaken) {
	const symbol_spec spec = unit_spec(1);
	depth_feed_bridge bridge(spec);
	std::vector<depth_feed_bridge::command> cmds;
	order_book book;

	ASSERT_TRUE(bridge.on_snapshot(snapshot_at(100), cmds));
	drain(book, cmds);
	cmds.clear();

	// An order of ours crossed into the offer and took three of its seven lots.
	book.delete_order(side_t::ask, 101, 3); // what the match did to the book
	bridge.consumed(side_t::ask, 101, 3);   // what this class has to be told
	EXPECT_EQ(bridge.consumed_lots(), 3);
	EXPECT_EQ(book.volume_at_price(101, side_t::ask), 4);

	// The venue says nothing about 101 — it is still showing all seven — and
	// that silence is exactly the case the mirror has to get right.
	const auto action =
		bridge.on_event(event_over(101, 101, {{.price = 99, .qty = 4}}, {}), cmds);
	ASSERT_EQ(action, sequence_action::apply);
	drain(book, cmds);

	EXPECT_EQ(book.volume_at_price(101, side_t::ask), 7)
		<< "the venue still publishes seven, so the book must hold seven";
	expect_book_matches_replica(book, bridge);
}

TEST(DepthFeedBridge, ConsumingMoreThanWasSeededClampsToEmpty) {
	const symbol_spec spec = unit_spec(1);
	depth_feed_bridge bridge(spec);
	std::vector<depth_feed_bridge::command> cmds;

	ASSERT_TRUE(bridge.on_snapshot(snapshot_at(100), cmds));

	// More than the level held, and a level it never seeded at all. Neither is
	// this class's liquidity, and neither may take the mirror negative.
	bridge.consumed(side_t::ask, 101, 99);
	bridge.consumed(side_t::bid, 42, 5);
	EXPECT_EQ(bridge.mirror().volume_at_price(101, side_t::ask), 0);
	EXPECT_EQ(bridge.mirror().volume_at_price(42, side_t::bid), 0);
}
