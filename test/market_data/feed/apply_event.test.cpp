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

// apply - one normalised diff onto an l2_book, via absolute set_level writes.

namespace {

TEST(ApplyEvent, SetsAbsoluteSizesOnBothSides) {
	l2_book book;
	const depth_event event{.sequence   = {1, 1},
							.event_time = timestamp{},
							.bids = {{at_scaled(100), 5 * units::scaled_size}},
							.asks = {{at_scaled(101), 7 * units::scaled_size}}};
	apply(book, event);
	EXPECT_EQ(book.volume_at_price(at_scaled(100), side_t::bid),
			  5 * units::scaled_size);
	EXPECT_EQ(book.volume_at_price(at_scaled(101), side_t::ask),
			  7 * units::scaled_size);
}

TEST(ApplyEvent, ZeroSizeRemovesTheLevel) {
	l2_book book;
	apply(book,
		  depth_event{.sequence   = {1, 1},
					  .event_time = timestamp{},
					  .bids = {{at_scaled(100), 5 * units::scaled_size}}});
	ASSERT_EQ(book.depth(side_t::bid), 1u);
	// The diff primitive: a level published at size 0 is a removal.
	apply(book,
		  depth_event{.sequence   = {2, 2},
					  .event_time = timestamp{},
					  .bids = {{at_scaled(100), 0 * units::scaled_size}}});
	EXPECT_EQ(book.depth(side_t::bid), 0u);
}

TEST(ApplyEvent, ApplyingTheSameEventTwiceIsIdempotent) {
	l2_book book;
	const depth_event event{.sequence   = {1, 1},
							.event_time = timestamp{},
							.bids = {{at_scaled(100), 5 * units::scaled_size},
									 {at_scaled(99), 3 * units::scaled_size}}};
	apply(book, event);
	apply(book, event);
	EXPECT_EQ(book.depth(side_t::bid), 2u);
	EXPECT_EQ(book.volume_at_price(at_scaled(100), side_t::bid),
			  5 * units::scaled_size);
}

} // namespace
