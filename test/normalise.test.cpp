#include "market-data/binance/binance_depth.hpp"
#include "market-data/binance/normalise.hpp"
#include "market-data/l2_book.hpp"
#include "market-data/normalised.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>

using exchange::side;
using exchange::market_data::book_snapshot;
using exchange::market_data::depth_event;
using exchange::market_data::l2_book;
using exchange::market_data::sequence_range;
using exchange::market_data::timestamp;

namespace binance = exchange::market_data::binance;

namespace {

// --------------------------------------------------------------------------
// sequence_range — the one sequencing primitive every venue maps onto
// --------------------------------------------------------------------------

TEST(SequenceRange, CoversItsClosedInterval) {
	constexpr sequence_range range{10, 12};
	EXPECT_FALSE(range.covers(9));
	EXPECT_TRUE(range.covers(10)); // both ends are inclusive
	EXPECT_TRUE(range.covers(11));
	EXPECT_TRUE(range.covers(12));
	EXPECT_FALSE(range.covers(13));
}

TEST(SequenceRange, ASingleNumberIsADegenerateRange) {
	constexpr sequence_range range{7, 7};
	EXPECT_TRUE(range.ordered());
	EXPECT_TRUE(range.covers(7));
}

TEST(SequenceRange, ABackwardsRangeIsNotOrdered) {
	EXPECT_TRUE((sequence_range{4, 9}).ordered());
	EXPECT_FALSE((sequence_range{9, 4}).ordered());
	// A backwards range covers nothing, so a caller that skips the check still
	// cannot conclude anything from it.
	EXPECT_FALSE((sequence_range{9, 4}).covers(6));
}

TEST(SequenceRange, ComparesFieldwise) {
	EXPECT_EQ((sequence_range{1, 2}), (sequence_range{1, 2}));
	EXPECT_NE((sequence_range{1, 2}), (sequence_range{1, 3}));
}

// --------------------------------------------------------------------------
// apply / reset — normalised data reaching the book
// --------------------------------------------------------------------------

TEST(ApplyEvent, SetsAbsoluteSizesOnBothSides) {
	l2_book book;
	const depth_event event{{1, 1}, timestamp{}, {{100, 5}}, {{101, 7}}};
	apply(book, event);
	EXPECT_EQ(book.volume_at_price(100, side::bid), 5);
	EXPECT_EQ(book.volume_at_price(101, side::ask), 7);
}

TEST(ApplyEvent, ZeroSizeRemovesTheLevel) {
	l2_book book;
	apply(book, depth_event{{1, 1}, timestamp{}, {{100, 5}}, {}});
	ASSERT_EQ(book.depth(side::bid), 1u);
	// The diff primitive: a level published at size 0 is a removal.
	apply(book, depth_event{{2, 2}, timestamp{}, {{100, 0}}, {}});
	EXPECT_EQ(book.depth(side::bid), 0u);
}

TEST(ApplyEvent, ApplyingTheSameEventTwiceIsIdempotent) {
	l2_book book;
	const depth_event event{{1, 1}, timestamp{}, {{100, 5}, {99, 3}}, {}};
	apply(book, event);
	apply(book, event);
	EXPECT_EQ(book.depth(side::bid), 2u);
	EXPECT_EQ(book.volume_at_price(100, side::bid), 5);
}

TEST(ResetBook, InstallsBothSidesSortedBestFirst) {
	l2_book book;
	// Deliberately unsorted: a normalised snapshot makes no ordering promise,
	// because venues disagree about it.
	reset(book,
		  book_snapshot{42, timestamp{}, {{99, 1}, {101, 2}, {100, 3}},
						{{105, 1}, {103, 2}, {104, 3}}});

	const auto &bids = book.levels(side::bid);
	ASSERT_EQ(bids.size(), 3u);
	EXPECT_EQ(bids[0].price, 101u); // bids descending
	EXPECT_EQ(bids[1].price, 100u);
	EXPECT_EQ(bids[2].price, 99u);

	const auto &asks = book.levels(side::ask);
	ASSERT_EQ(asks.size(), 3u);
	EXPECT_EQ(asks[0].price, 103u); // asks ascending
	EXPECT_EQ(asks[1].price, 104u);
	EXPECT_EQ(asks[2].price, 105u);
}

TEST(ResetBook, ReplacesEverythingThatWasThereBefore) {
	l2_book book;
	book.set_level(side::bid, 50, 9);
	book.set_level(side::ask, 60, 9);

	reset(book, book_snapshot{1, timestamp{}, {{100, 1}}, {}});
	EXPECT_EQ(book.volume_at_price(50, side::bid), 0); // gone, not merged
	EXPECT_EQ(book.depth(side::ask), 0u);              // an empty side clears
	EXPECT_EQ(book.volume_at_price(100, side::bid), 1);
}

TEST(ResetBook, DropsNonPositiveSizes) {
	l2_book book;
	reset(book, book_snapshot{1, timestamp{}, {{100, 5}, {99, 0}}, {}});
	// A zero-size level is the same state as an absent one; it must not become
	// a cell the binary search then has to step over.
	EXPECT_EQ(book.depth(side::bid), 1u);
	EXPECT_EQ(book.volume_at_price(99, side::bid), 0);
}

TEST(ResetBook, KeepsOnePricePerSide) {
	l2_book book;
	reset(book, book_snapshot{1, timestamp{}, {{100, 5}, {100, 7}}, {}});
	// A duplicate price would break set_level's binary search; the first wins.
	EXPECT_EQ(book.depth(side::bid), 1u);
	EXPECT_EQ(book.volume_at_price(100, side::bid), 5);
}

TEST(LoadSide, LeavesTheOtherSideAlone) {
	l2_book book;
	book.set_level(side::ask, 200, 4);
	book.load(side::bid, {{100, 1}, {101, 2}});
	EXPECT_EQ(book.depth(side::bid), 2u);
	EXPECT_EQ(book.volume_at_price(200, side::ask), 4);
}

TEST(LoadSide, LeavesTheSideUsableBySetLevel) {
	l2_book book;
	book.load(side::bid, {{99, 1}, {101, 2}, {100, 3}});
	// The loaded side must satisfy the sorted invariant set_level assumes.
	book.set_level(side::bid, 100, 8); // existing price -> overwrite
	book.set_level(side::bid, 102, 4); // new best -> inserted at the front
	EXPECT_EQ(book.volume_at_price(100, side::bid), 8);
	EXPECT_EQ(book.best_bid(), std::optional<exchange::price>{102});
	EXPECT_EQ(book.depth(side::bid), 4u);
}

// --------------------------------------------------------------------------
// binance -> neutral: the venue's vocabulary stops at the adapter
// --------------------------------------------------------------------------

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
	EXPECT_EQ(book.best_bid(), std::optional<exchange::price>{100});
	EXPECT_EQ(book.best_ask(), std::optional<exchange::price>{101});
	EXPECT_EQ(book.volume_at_price(99, side::bid), 6);
}

} // namespace
