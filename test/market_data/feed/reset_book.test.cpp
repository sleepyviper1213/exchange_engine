#include "market_data/book_snapshot.hpp"
#include "market_data/depth_event.hpp"
#include "market_data/l2_book.hpp"

#include <gtest/gtest.h>

using namespace exchange;
using exchange::side_t;
using exchange::market_data::book_snapshot;
using exchange::market_data::depth_event;
using exchange::market_data::l2_book;
using exchange::market_data::timestamp;

// reset - replace a book wholesale with a snapshot.

namespace {

TEST(ResetBook, InstallsBothSidesSortedBestFirst) {
	l2_book book;
	// Deliberately unsorted: a normalised snapshot makes no ordering promise,
	// because venues disagree about it.
	reset(book,
		  book_snapshot{.sequence   = 42,
						.event_time = timestamp{},
						.bids = {{at_scaled(99), 1 * units::scaled_size},
								 {at_scaled(101), 2 * units::scaled_size},
								 {at_scaled(100), 3 * units::scaled_size}},
						.asks = {{at_scaled(105), 1 * units::scaled_size},
								 {at_scaled(103), 2 * units::scaled_size},
								 {at_scaled(104), 3 * units::scaled_size}}});

	const auto &bids = book.bid_levels();
	ASSERT_EQ(bids.size(), 3u);
	EXPECT_EQ(bids[0].price, at_scaled(101)); // bids descending
	EXPECT_EQ(bids[1].price, at_scaled(100));
	EXPECT_EQ(bids[2].price, at_scaled(99));

	const auto &asks = book.ask_levels();
	ASSERT_EQ(asks.size(), 3u);
	EXPECT_EQ(asks[0].price, at_scaled(103)); // asks ascending
	EXPECT_EQ(asks[1].price, at_scaled(104));
	EXPECT_EQ(asks[2].price, at_scaled(105));
}

TEST(ResetBook, ReplacesEverythingThatWasThereBefore) {
	l2_book book;
	book.set_level(side_t::bid, at_scaled(50), 9 * units::scaled_size);
	book.set_level(side_t::ask, at_scaled(60), 9 * units::scaled_size);

	reset(book,
		  book_snapshot{.sequence   = 1,
						.event_time = timestamp{},
						.bids = {{at_scaled(100), 1 * units::scaled_size}}});
	EXPECT_EQ(book.volume_at_price(at_scaled(50), side_t::bid),
			  0 * units::scaled_size);      // gone, not merged
	EXPECT_EQ(book.depth(side_t::ask), 0u); // an empty side_t clears
	EXPECT_EQ(book.volume_at_price(at_scaled(100), side_t::bid),
			  1 * units::scaled_size);
}

TEST(ResetBook, DropsNonPositiveSizes) {
	l2_book book;
	reset(book,
		  book_snapshot{.sequence   = 1,
						.event_time = timestamp{},
						.bids = {{at_scaled(100), 5 * units::scaled_size},
								 {at_scaled(99), 0 * units::scaled_size}}});
	// A zero-size level is the same state as an absent one; it must not become
	// a cell the binary search then has to step over.
	EXPECT_EQ(book.depth(side_t::bid), 1u);
	EXPECT_EQ(book.volume_at_price(at_scaled(99), side_t::bid),
			  0 * units::scaled_size);
}

TEST(ResetBook, KeepsOnePricePerSide) {
	l2_book book;
	reset(book,
		  book_snapshot{.sequence   = 1,
						.event_time = timestamp{},
						.bids = {{at_scaled(100), 5 * units::scaled_size},
								 {at_scaled(100), 7 * units::scaled_size}}});
	// A duplicate price would break set_level's binary search; the first wins.
	EXPECT_EQ(book.depth(side_t::bid), 1u);
	EXPECT_EQ(book.volume_at_price(at_scaled(100), side_t::bid),
			  5 * units::scaled_size);
}

} // namespace
