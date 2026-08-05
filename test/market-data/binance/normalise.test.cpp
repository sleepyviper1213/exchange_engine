#include "market-data/binance/binance_depth.hpp"
#include "market-data/binance/normalise.hpp"
#include "market-data/l2_book.hpp"
#include "market-data/normalised.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>

using exchange::side_t;
using exchange::market_data::book_snapshot;
using exchange::market_data::depth_event;
using exchange::market_data::l2_book;
using exchange::market_data::sequence_range;
using exchange::market_data::timestamp;

namespace binance = exchange::market_data::binance;

// binance::normalise — U/u, milliseconds and scaled levels into neutral types.

namespace {

TEST(BinanceNormalise, UpperAndLowerUpdateIdsBecomeTheSequenceRange) {
	binance::DepthUpdate update;
	update.firstUpdateId = 390'497'796;
	update.finalUpdateId = 390'497'878;
	EXPECT_EQ(binance::sequence_of(update),
			  (sequence_range{390'497'796, 390'497'878}));
	EXPECT_EQ(normalise(update).sequence,
			  (sequence_range{390'497'796, 390'497'878}));
}

TEST(BinanceNormalise, SequenceOfReadsTheStreamingParsersMetaToo) {
	// The zero-copy path never builds a DepthUpdate, but still has to be
	// gap-checked — so the ids alone normalise on their own.
	binance::DepthUpdateMeta meta;
	meta.firstUpdateId = 10;
	meta.finalUpdateId = 12;
	EXPECT_EQ(binance::sequence_of(meta), (sequence_range{10, 12}));
}

TEST(BinanceNormalise, EventTimeConvertsFromMillisecondsToNanoseconds) {
	binance::DepthUpdate update;
	update.eventTime = 1'568'014'460'893; // Binance publishes E in ms
	EXPECT_EQ(normalise(update).event_time,
			  std::chrono::milliseconds{1'568'014'460'893});
	EXPECT_EQ(normalise(update).event_time.count(),
			  1'568'014'460'893'000'000LL);
}

TEST(BinanceNormalise, LevelsCarryOverScaledAndInOrder) {
	binance::DepthUpdate update;
	update.bids = {{15'345, 100}, {15'344, 250}};
	update.asks = {{15'350, 0}};

	const auto event = normalise(update);
	ASSERT_EQ(event.bids.size(), 2u);
	EXPECT_EQ(event.bids[0].price, 15'345u);
	EXPECT_EQ(event.bids[0].qty, 100);
	EXPECT_EQ(event.bids[1].price, 15'344u);
	ASSERT_EQ(event.asks.size(), 1u);
	EXPECT_EQ(event.asks[0].qty, 0); // a removal survives normalisation
}

TEST(BinanceNormalise, SnapshotLastUpdateIdBecomesTheSeedSequence) {
	binance::DepthSnapshot snapshot;
	snapshot.lastUpdateId = 1'027'024;
	snapshot.bids         = {{4, 431}};
	snapshot.asks         = {{4'000'000'200, 12}};

	const auto normalised = normalise(snapshot);
	EXPECT_EQ(normalised.sequence, 1'027'024u);
	ASSERT_EQ(normalised.bids.size(), 1u);
	EXPECT_EQ(normalised.bids[0].price, 4u);
	ASSERT_EQ(normalised.asks.size(), 1u);
	EXPECT_EQ(normalised.asks[0].price, 4'000'000'200u);
	// The REST payload carries no event time, and normalisation invents none.
	EXPECT_EQ(normalised.event_time, timestamp{});
}

TEST(BinanceNormalise, ANormalisedSnapshotSeedsABookDirectly) {
	binance::DepthSnapshot snapshot;
	snapshot.lastUpdateId = 7;
	snapshot.bids         = {{100, 5}, {99, 6}}; // Binance sends bids descending
	snapshot.asks         = {{101, 4}, {102, 3}}; // and asks ascending

	l2_book book;
	reset(book, normalise(snapshot));
	EXPECT_EQ(book.best_bid(), std::optional<exchange::price_t>{100});
	EXPECT_EQ(book.best_ask(), std::optional<exchange::price_t>{101});
	EXPECT_EQ(book.volume_at_price(99, side_t::bid), 6);
}

} // namespace
