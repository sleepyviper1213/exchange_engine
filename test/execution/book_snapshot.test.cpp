#include "core/persistence/persistence.fixture.hpp"
#include "execution/book_manager.hpp"
#include "execution/book_snapshot.hpp"
#include "order_book/order_book.hpp"
#include "order_book/resting_view.hpp"
#include "order_book/trade.hpp"
#include "orders/side.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

// A snapshot is only worth anything if what comes back is the book that went in -
// and "the book" means the queues, not just the quantities. Two books holding the
// same orders at the same prices in a different order within a level match the
// same flow differently, so every suite here checks priority and not only depth.
//
// The traversal and the restore are tested as a pair on purpose. Either one alone
// is unfalsifiable: a walk with no restore has nothing to compare against, and a
// restore with no walk cannot be shown to have rebuilt anything in particular.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::execution;
using namespace exchange::engine::orders;

namespace {

/// @brief Every resting order in @p book, in fill order.
std::vector<resting_view> contents(const order_book &book) {
	std::vector<resting_view> seen;
	book.for_each_resting([&](const resting_view &order) {
		seen.push_back(order);
	});
	return seen;
}

/// @brief Place @p qty at @p price on @p side, letting it match if it crosses.
void place(order_book &book, order_id_t id, side_t side, price_t price,
		   quantity_t qty) {
	std::vector<trade> trades;
	book.place_order(
		orders::order{.id = id, .side = side, .price = price, .qty = qty},
		trades);
}

/// @brief A book with two levels a side and several orders queued per level, so
///        that FIFO order is something a round trip can get wrong.
void fill_book(order_book &book) {
	place(book, 1, side_t::bid, at_tick(100), 5 * units::lot);
	place(book,
		  2,
		  side_t::bid,
		  at_tick(100),
		  3 * units::lot); // behind id 1 at the same price
	place(book, 3, side_t::bid, at_tick(100), 7 * units::lot); // behind id 2
	place(book, 4, side_t::bid, at_tick(99), 4 * units::lot);
	place(book, 5, side_t::ask, at_tick(101), 6 * units::lot);
	place(book, 6, side_t::ask, at_tick(101), 2 * units::lot); // behind id 5
	place(book, 7, side_t::ask, at_tick(102), 9 * units::lot);
}

TEST(BookSnapshot, AnEmptyBookWalksToNothing) {
	const order_book book;
	EXPECT_TRUE(contents(book).empty());
}

// The traversal order is the contract restore depends on, so it is pinned
// directly rather than only through a round trip.
TEST(BookSnapshot, TheWalkIsBidsThenAsksBestFirstOldestFirst) {
	order_book book;
	fill_book(book);

	const std::vector<resting_view> seen = contents(book);
	ASSERT_EQ(seen.size(), 7U);

	// Bids first, best price first, oldest first within the level.
	EXPECT_EQ(seen[0].side, side_t::bid);
	EXPECT_EQ(seen[0].price, at_tick(100));
	EXPECT_EQ(seen[0].id, 1U);
	EXPECT_EQ(seen[1].id, 2U);
	EXPECT_EQ(seen[2].id, 3U);
	EXPECT_EQ(seen[3].price, at_tick(99)); // the worse bid level comes after
	EXPECT_EQ(seen[3].id, 4U);

	// Then asks, again best (lowest) first.
	EXPECT_EQ(seen[4].side, side_t::ask);
	EXPECT_EQ(seen[4].price, at_tick(101));
	EXPECT_EQ(seen[4].id, 5U);
	EXPECT_EQ(seen[5].id, 6U);
	EXPECT_EQ(seen[6].price, at_tick(102));
	EXPECT_EQ(seen[6].id, 7U);
}

// A partially filled order must come back partially filled. Restoring it as a
// fresh order of its remaining quantity rests the right size and quietly rewrites
// the order's history, which is the failure this catches.
TEST(BookSnapshot, APartialFillsTradedQuantitySurvivesTheWalk) {
	order_book book;
	place(book, 1, side_t::ask, at_tick(100), 10 * units::lot);
	place(book,
		  2,
		  side_t::bid,
		  at_tick(100),
		  4 * units::lot); // takes 4 of id 1

	const std::vector<resting_view> seen = contents(book);
	ASSERT_EQ(seen.size(), 1U);
	EXPECT_EQ(seen[0].id, 1U);
	EXPECT_EQ(seen[0].state.remaining(), 6 * units::lot);
	EXPECT_EQ(seen[0].state.traded(), 4 * units::lot)
		<< "the fill history was lost";
	EXPECT_EQ(seen[0].state.quantity(), 10 * units::lot);
}

// The round trip, on one book: walk it, restore into an empty one, and the two
// must be indistinguishable - same orders, same levels, same queues.
TEST(BookSnapshot, RestoringAWalkRebuildsTheBookExactly) {
	order_book original;
	fill_book(original);
	place(original,
		  8,
		  side_t::bid,
		  at_tick(100),
		  20 * units::lot); // and a partial fill to carry
	place(original,
		  9,
		  side_t::ask,
		  at_tick(100),
		  5 * units::lot); // takes 5 of id 8's 20

	order_book restored;
	for (const resting_view &order : contents(original))
		EXPECT_TRUE(restored.restore_order(order));

	EXPECT_EQ(contents(restored), contents(original));
	EXPECT_EQ(restored.best_bid(), original.best_bid());
	EXPECT_EQ(restored.best_ask(), original.best_ask());
}

// Restoring must not match, even though a snapshot holds both sides of the book
// and they arrive one at a time. Through place_order the first bid would cross an
// already-resting ask; through restore_order nothing crosses at all.
TEST(BookSnapshot, RestoringNeverMatchesTheOrdersAgainstEachOther) {
	order_book original;
	place(original, 1, side_t::ask, at_tick(100), 5 * units::lot);
	place(original, 2, side_t::bid, at_tick(99), 5 * units::lot);

	order_book restored;
	// Deliberately worst case: the ask goes in first, so the bid that follows
	// would be the aggressor if this matched. It does not, so both rest.
	for (const resting_view &order : contents(original))
		ASSERT_TRUE(restored.restore_order(order));

	EXPECT_EQ(contents(restored).size(), 2U);
	EXPECT_EQ(restored.best_ask(), original.best_ask());
	EXPECT_EQ(restored.best_bid(), original.best_bid());
}

// A restored order is a real resting order, not a shadow of one: the index has to
// know about it or no cancel could ever reach it.
TEST(BookSnapshot, ARestoredOrderCanStillBeCancelled) {
	order_book original;
	place(original, 42, side_t::bid, at_tick(100), 5 * units::lot);

	order_book restored;
	for (const resting_view &order : contents(original))
		ASSERT_TRUE(restored.restore_order(order));

	std::vector<order_outcome> outcomes;
	restored.cancel_order(42, outcomes);
	ASSERT_EQ(outcomes.size(), 1U);
	EXPECT_EQ(outcomes[0].type, OutcomeType::CANCELLED);
	EXPECT_TRUE(contents(restored).empty());
}

TEST(BookSnapshot, RestoringADuplicateIdIsRefused) {
	order_book book;
	const resting_view order{.id    = 1,
							 .state = order_state{5 * units::lot},
							 .price = at_tick(100),
							 .side  = side_t::bid};
	EXPECT_TRUE(book.restore_order(order));
	EXPECT_FALSE(book.restore_order(order)) << "would orphan the first node";
	EXPECT_EQ(contents(book).size(), 1U);
}

TEST(BookSnapshot, RestoringAnOrderWithNothingLeftIsRefused) {
	order_book book;
	order_state spent{5 * units::lot};
	spent.apply_fill(5 * units::lot); // FILLED: nothing to rest
	EXPECT_FALSE(book.restore_order(
		{.id = 1, .state = spent, .price = at_tick(100), .side = side_t::bid}));
	EXPECT_TRUE(contents(book).empty());
}

// Now through a file, across every listing a manager carries - which is what
// recovery actually does.
TEST(BookSnapshot, AFileRoundTripRebuildsEveryListing) {
	const scratch_dir dir("book_snapshot_roundtrip");
	const auto path = dir.file("snapshot.bin");

	book_manager saved;
	fill_book(saved.create(1));
	place(saved.create(2), 20, side_t::ask, at_tick(200), 8 * units::lot);
	place(saved.create(2),
		  21,
		  side_t::ask,
		  at_tick(200),
		  4 * units::lot); // behind id 20
	place(saved.create(2), 22, side_t::bid, at_tick(150), 3 * units::lot);

	const auto written = save_snapshot(saved, path);
	ASSERT_TRUE(written.has_value()) << written.error();
	EXPECT_EQ(*written, 10U); // seven on listing 1, three on listing 2

	book_manager loaded;
	// The listings have to be registered first: which symbols a partition carries
	// is a deployment's decision, not a snapshot's to reinstate.
	loaded.create(1);
	loaded.create(2);
	const auto restored = load_snapshot(loaded, path);
	ASSERT_TRUE(restored.has_value()) << restored.error();
	EXPECT_EQ(*restored, *written);

	EXPECT_EQ(contents(*loaded.lookup(1)), contents(*saved.lookup(1)));
	EXPECT_EQ(contents(*loaded.lookup(2)), contents(*saved.lookup(2)));
}

// The file is a header and then a fixed-stride array of records, each carrying
// its own checksum - no length prefixes, no per-record encode step. Anything
// else means a serialisation crept in where the design says there is none, and
// the arithmetic has to stay exact because a record is located by index.
TEST(BookSnapshot, TheFileIsAHeaderThenAnArrayOfFramedRecords) {
	const scratch_dir dir("book_snapshot_stride");
	const auto path = dir.file("snapshot.bin");

	book_manager saved;
	fill_book(saved.create(1));
	const auto written = save_snapshot(saved, path);
	ASSERT_TRUE(written.has_value()) << written.error();

	EXPECT_EQ(std::filesystem::file_size(path),
			  exchange::core::persistence::LOG_HEADER_SIZE +
				  (*written * snapshot_log::ONDISK_STRIDE));
}

// A snapshot is a whole statement about one moment, so saving again replaces
// rather than appends. Appending would put the older snapshot's orders in front of
// the newer one's and a load would restore both, every colliding id being dropped.
TEST(BookSnapshot, SavingAgainReplacesTheSnapshotRatherThanAppending) {
	const scratch_dir dir("book_snapshot_replace");
	const auto path = dir.file("snapshot.bin");

	book_manager first;
	fill_book(first.create(1));
	ASSERT_TRUE(save_snapshot(first, path).has_value());

	book_manager second;
	place(second.create(1), 99, side_t::bid, at_tick(50), 1 * units::lot);
	const auto written = save_snapshot(second, path);
	ASSERT_TRUE(written.has_value()) << written.error();
	EXPECT_EQ(*written, 1U);

	book_manager loaded;
	loaded.create(1);
	const auto restored = load_snapshot(loaded, path);
	ASSERT_TRUE(restored.has_value()) << restored.error();
	EXPECT_EQ(*restored, 1U) << "the previous snapshot's orders came back too";
	EXPECT_EQ(contents(*loaded.lookup(1)).size(), 1U);
}

// A snapshot and a partition assignment that disagree is the same fault
// `misrouted` reports on the command path. Skipped and counted, never invented.
TEST(BookSnapshot, RecordsForAListingTheManagerLacksAreSkippedAndCounted) {
	const scratch_dir dir("book_snapshot_skip");
	const auto path = dir.file("snapshot.bin");

	book_manager saved;
	fill_book(saved.create(1));
	place(saved.create(2), 20, side_t::ask, at_tick(200), 8 * units::lot);
	ASSERT_TRUE(save_snapshot(saved, path).has_value());

	book_manager partial;
	partial.create(1); // listing 2 deliberately absent
	const auto report = load_snapshot_reporting(partial, path);
	ASSERT_TRUE(report.has_value()) << report.error();
	EXPECT_EQ(report->restored, 7U);
	EXPECT_EQ(report->skipped, 1U);
	EXPECT_EQ(contents(*partial.lookup(1)), contents(*saved.lookup(1)));
}

TEST(BookSnapshot, AnEmptyManagerSnapshotsToAnEmptyFile) {
	const scratch_dir dir("book_snapshot_empty");
	const auto path = dir.file("snapshot.bin");

	const book_manager empty;
	const auto written = save_snapshot(empty, path);
	ASSERT_TRUE(written.has_value()) << written.error();
	EXPECT_EQ(*written, 0U);

	book_manager loaded;
	const auto restored = load_snapshot(loaded, path);
	ASSERT_TRUE(restored.has_value()) << restored.error();
	EXPECT_EQ(*restored, 0U);
}

TEST(BookSnapshot, LoadingAMissingSnapshotFails) {
	const scratch_dir dir("book_snapshot_missing");
	book_manager books;
	books.create(1);
	EXPECT_FALSE(load_snapshot(books, dir.file("absent.bin")).has_value());
}

} // namespace
