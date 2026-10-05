#include "market_data/binance/normalise.hpp"

#include "core/util/inclusive_range.hpp"
#include "market_data/binance/depth_snapshot.hpp"
#include "market_data/binance/depth_update.hpp"
#include "market_data/binance/depth_update_meta.hpp"
#include "market_data/book_snapshot.hpp"
#include "market_data/depth_event.hpp"
#include "market_data/fwd.hpp"
#include "market_data/l2_book.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>

using namespace exchange;
using exchange::side_t;
using exchange::market_data::book_snapshot;
using exchange::market_data::depth_event;
using exchange::market_data::l2_book;
using exchange::market_data::sequence_t;
using exchange::market_data::timestamp;

namespace binance = exchange::market_data::binance;

// binance::normalise - U/u, milliseconds and scaled levels into neutral types.

namespace {
using range = exchange::core::util::inclusive_range<sequence_t>;

TEST(BinanceNormalise, UpperAndLowerUpdateIdsBecomeTheSequenceRange) {
	binance::depth_update update;
	update.firstUpdateId = 390'497'796;
	update.finalUpdateId = 390'497'878;
	EXPECT_EQ(binance::sequence_of(update), (range{390'497'796, 390'497'878}));
	EXPECT_EQ(normalise(update).sequence, (range{390'497'796, 390'497'878}));
}

TEST(BinanceNormalise, SequenceOfReadsTheStreamingParsersMetaToo) {
	// The zero-copy path never builds a depth_update, but still has to be
	// gap-checked - so the ids alone normalise on their own.
	binance::depth_update_meta meta;
	meta.firstUpdateId = 10;
	meta.finalUpdateId = 12;
	EXPECT_EQ(binance::sequence_of(meta), (range{10, 12}));
}

TEST(BinanceNormalise, EventTimeConvertsFromMillisecondsToNanoseconds) {
	binance::depth_update update;
	update.eventTime = 1'568'014'460'893; // Binance publishes E in ms
	EXPECT_EQ(normalise(update).event_time,
			  std::chrono::milliseconds{1'568'014'460'893});
	EXPECT_EQ(normalise(update).event_time.count(),
			  1'568'014'460'893'000'000LL);
}

TEST(BinanceNormalise, LevelsCarryOverScaledAndInOrder) {
	binance::depth_update update;
	update.bids = {{at_scaled(15345), 100 * units::scaled_size},
				   {at_scaled(15344), 250 * units::scaled_size}};
	update.asks = {{at_scaled(15350), 0 * units::scaled_size}};

	const auto event = normalise(update);
	ASSERT_EQ(event.bids.size(), 2u);
	EXPECT_EQ(event.bids[0].price, at_scaled(15345));
	EXPECT_EQ(event.bids[0].qty, 100 * units::scaled_size);
	EXPECT_EQ(event.bids[1].price, at_scaled(15344));
	ASSERT_EQ(event.asks.size(), 1u);
	EXPECT_EQ(event.asks[0].qty,
			  0 * units::scaled_size); // a removal survives normalisation
}

TEST(BinanceNormalise, SnapshotLastUpdateIdBecomesTheSeedSequence) {
	binance::depth_snapshot snapshot;
	snapshot.lastUpdateId = 1'027'024;
	snapshot.bids         = {{at_scaled(4), 431 * units::scaled_size}};
	snapshot.asks = {{at_scaled(4'000'000'200), 12 * units::scaled_size}};

	const auto normalised = normalise(snapshot);
	EXPECT_EQ(normalised.sequence, 1'027'024u);
	ASSERT_EQ(normalised.bids.size(), 1u);
	EXPECT_EQ(normalised.bids[0].price, at_scaled(4));
	ASSERT_EQ(normalised.asks.size(), 1u);
	EXPECT_EQ(normalised.asks[0].price, at_scaled(4'000'000'200));
	// The REST payload carries no event time, and normalisation invents none.
	EXPECT_EQ(normalised.event_time, timestamp{});
}

TEST(BinanceNormalise, ANormalisedSnapshotSeedsABookDirectly) {
	binance::depth_snapshot snapshot;
	snapshot.lastUpdateId = 7;
	snapshot.bids = {{at_scaled(100), 5 * units::scaled_size},
					 {at_scaled(99),
					  6 * units::scaled_size}}; // Binance sends bids descending
	snapshot.asks = {
		{at_scaled(101), 4 * units::scaled_size},
		{at_scaled(102), 3 * units::scaled_size}}; // and asks ascending

	l2_book book;
	reset(book, normalise(snapshot));
	EXPECT_EQ(book.best_bid(), std::optional<scaled_price_t>{at_scaled(100)});
	EXPECT_EQ(book.best_ask(), std::optional<scaled_price_t>{at_scaled(101)});
	EXPECT_EQ(book.volume_at_price(at_scaled(99), side_t::bid),
			  6 * units::scaled_size);
}

} // namespace
