#include "market-data/l2_book.hpp"
#include "market-data/normalised.hpp"
#include "market-data/reconstructor.hpp"
#include "market-data/sequencer.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

using exchange::price;
using exchange::side;
using exchange::market_data::book_snapshot;
using exchange::market_data::depth_event;
using exchange::market_data::depth_reconstructor;
using exchange::market_data::reconstructor_options;
using exchange::market_data::sequence_action;
using exchange::market_data::timestamp;

namespace {

// One bid level changing at a single sequence number — enough to tell which
// events reached the book and in what order.
depth_event bid_at(std::uint64_t sequence, price price, exchange::quantity size) {
	return depth_event{{sequence, sequence}, timestamp{}, {{price, size}}, {}};
}

book_snapshot seed_of(std::uint64_t sequence) {
	return book_snapshot{sequence, timestamp{}, {{100, 1}}, {{200, 1}}};
}

// --------------------------------------------------------------------------
// The sync procedure end to end
// --------------------------------------------------------------------------

TEST(DepthReconstructor, StartsNeedingASnapshot) {
	const depth_reconstructor reconstructor;
	EXPECT_FALSE(reconstructor.live());
	EXPECT_TRUE(reconstructor.needs_snapshot());
	EXPECT_EQ(reconstructor.pending(), 0u);
}

TEST(DepthReconstructor, HoldsEventsUntilASnapshotArrives) {
	depth_reconstructor reconstructor;
	EXPECT_EQ(reconstructor.on_event(bid_at(5, 100, 5)),
			  sequence_action::buffer);
	EXPECT_EQ(reconstructor.on_event(bid_at(6, 101, 6)),
			  sequence_action::buffer);
	EXPECT_EQ(reconstructor.pending(), 2u);
	// Nothing reaches the book before it has a seed to build on.
	EXPECT_EQ(reconstructor.book().depth(side::bid), 0u);
}

TEST(DepthReconstructor, SnapshotDrainsTheBufferAndGoesLive) {
	depth_reconstructor reconstructor;
	reconstructor.on_event(bid_at(3, 100, 3)); // predates the snapshot
	reconstructor.on_event(bid_at(4, 101, 4)); // ditto
	reconstructor.on_event(bid_at(5, 102, 5)); // the bridging event
	reconstructor.on_event(bid_at(6, 103, 6));

	ASSERT_TRUE(reconstructor.on_snapshot(seed_of(4)));
	EXPECT_TRUE(reconstructor.live());
	EXPECT_EQ(reconstructor.pending(), 0u);

	const auto &book = reconstructor.book();
	EXPECT_EQ(book.volume_at_price(100, side::bid), 1); // the snapshot's own
	EXPECT_EQ(book.volume_at_price(101, side::bid), 0); // 3 and 4 were stale
	EXPECT_EQ(book.volume_at_price(102, side::bid), 5); // 5 and 6 replayed
	EXPECT_EQ(book.volume_at_price(103, side::bid), 6);
	EXPECT_EQ(reconstructor.last_sequence(), 6u);
	EXPECT_EQ(reconstructor.stats().discarded, 2u);
	EXPECT_EQ(reconstructor.stats().applied, 2u);
}

TEST(DepthReconstructor, AppliesEventsDirectlyOnceLive) {
	depth_reconstructor reconstructor;
	ASSERT_TRUE(reconstructor.on_snapshot(seed_of(10)));
	EXPECT_EQ(reconstructor.on_event(bid_at(11, 105, 7)),
			  sequence_action::apply);
	EXPECT_EQ(reconstructor.book().volume_at_price(105, side::bid), 7);
	EXPECT_EQ(reconstructor.pending(), 0u); // nothing is retained while live
}

TEST(DepthReconstructor, ASnapshotOlderThanTheBufferDoesNotGoLive) {
	depth_reconstructor reconstructor;
	reconstructor.on_event(bid_at(20, 100, 5));
	reconstructor.on_event(bid_at(21, 101, 5));

	// Nothing covers 11..19, so this snapshot cannot be bridged to the buffer.
	EXPECT_FALSE(reconstructor.on_snapshot(seed_of(10)));
	EXPECT_TRUE(reconstructor.needs_snapshot());
	// The book is cleared rather than left as a plausible-looking near-miss.
	EXPECT_EQ(reconstructor.book().depth(side::bid), 0u);
	// The un-bridged events are kept — a newer snapshot may still reach them.
	EXPECT_EQ(reconstructor.pending(), 2u);

	ASSERT_TRUE(reconstructor.on_snapshot(seed_of(19)));
	EXPECT_TRUE(reconstructor.live());
	EXPECT_EQ(reconstructor.book().volume_at_price(100, side::bid), 5);
	EXPECT_EQ(reconstructor.last_sequence(), 21u);
}

// --------------------------------------------------------------------------
// Gaps
// --------------------------------------------------------------------------

TEST(DepthReconstructor, AGapClearsTheBookAndDemandsASnapshot) {
	depth_reconstructor reconstructor;
	ASSERT_TRUE(reconstructor.on_snapshot(seed_of(10)));
	ASSERT_EQ(reconstructor.on_event(bid_at(11, 105, 7)),
			  sequence_action::apply);

	// 12 was lost. The book is now a replica of nothing in particular, so it
	// must not be left readable as if it were current.
	EXPECT_EQ(reconstructor.on_event(bid_at(13, 106, 8)),
			  sequence_action::gap);
	EXPECT_TRUE(reconstructor.needs_snapshot());
	EXPECT_EQ(reconstructor.book().depth(side::bid), 0u);
	EXPECT_EQ(reconstructor.book().depth(side::ask), 0u);
	EXPECT_EQ(reconstructor.stats().gaps, 1u);
	// The event that exposed the gap is kept: the next snapshot may bridge it.
	EXPECT_EQ(reconstructor.pending(), 1u);
}

TEST(DepthReconstructor, RecoversFromAGapOnTheNextSnapshot) {
	depth_reconstructor reconstructor;
	ASSERT_TRUE(reconstructor.on_snapshot(seed_of(10)));
	ASSERT_EQ(reconstructor.on_event(bid_at(11, 105, 7)),
			  sequence_action::apply);
	ASSERT_EQ(reconstructor.on_event(bid_at(13, 106, 8)), sequence_action::gap);
	ASSERT_EQ(reconstructor.on_event(bid_at(14, 107, 9)),
			  sequence_action::buffer);

	ASSERT_TRUE(reconstructor.on_snapshot(seed_of(12)));
	EXPECT_TRUE(reconstructor.live());
	const auto &book = reconstructor.book();
	EXPECT_EQ(book.volume_at_price(106, side::bid), 8); // 13 and 14 replayed
	EXPECT_EQ(book.volume_at_price(107, side::bid), 9);
	EXPECT_EQ(book.volume_at_price(105, side::bid), 0); // the stale book is gone
	EXPECT_EQ(book.best_ask(), std::optional<price>{200}); // reseeded from 12
	EXPECT_EQ(reconstructor.stats().gaps, 1u);
}

TEST(DepthReconstructor, InvalidateDropsTheReplicaWithoutBlamingTheFeed) {
	depth_reconstructor reconstructor;
	ASSERT_TRUE(reconstructor.on_snapshot(seed_of(10)));
	ASSERT_EQ(reconstructor.on_event(bid_at(11, 105, 7)),
			  sequence_action::apply);

	reconstructor.invalidate(); // e.g. the WebSocket dropped and reconnected
	EXPECT_TRUE(reconstructor.needs_snapshot());
	EXPECT_EQ(reconstructor.book().depth(side::bid), 0u);
	EXPECT_EQ(reconstructor.stats().gaps, 0u); // the sequence never broke
	EXPECT_EQ(reconstructor.on_event(bid_at(12, 106, 1)),
			  sequence_action::buffer);
}

// --------------------------------------------------------------------------
// The pending buffer is bounded
// --------------------------------------------------------------------------

TEST(DepthReconstructor, DropsTheOldestPendingEventsAtTheCap) {
	depth_reconstructor reconstructor{reconstructor_options{.max_pending = 2}};
	reconstructor.on_event(bid_at(1, 100, 1));
	reconstructor.on_event(bid_at(2, 101, 2));
	reconstructor.on_event(bid_at(3, 102, 3)); // evicts sequence 1

	EXPECT_EQ(reconstructor.pending(), 2u);
	EXPECT_EQ(reconstructor.dropped(), 1u);

	// Dropping from the front is safe: a snapshot that arrives this late covers
	// the evicted events anyway, so the buffer still bridges.
	ASSERT_TRUE(reconstructor.on_snapshot(seed_of(1)));
	EXPECT_EQ(reconstructor.book().volume_at_price(101, side::bid), 2);
	EXPECT_EQ(reconstructor.book().volume_at_price(102, side::bid), 3);
	EXPECT_EQ(reconstructor.last_sequence(), 3u);
}

TEST(DepthReconstructor, ZeroCapMeansUnbounded) {
	depth_reconstructor reconstructor{reconstructor_options{.max_pending = 0}};
	for (std::uint64_t sequence = 1; sequence <= 100; ++sequence)
		reconstructor.on_event(bid_at(sequence, 100 + sequence, 1));
	EXPECT_EQ(reconstructor.pending(), 100u);
	EXPECT_EQ(reconstructor.dropped(), 0u);
}

} // namespace
