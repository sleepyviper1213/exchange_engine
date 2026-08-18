#include "trading-engine/execution/book_manager.hpp"

#include <gtest/gtest.h>

// Who owns a listing's book. The properties worth pinning are the ones the
// matching engine assumes without checking: that a book's address never moves
// once handed out, and that a symbol nobody registered answers null rather than
// an empty book.

using namespace exchange::engine;
using namespace exchange::engine::execution;
using namespace exchange;


TEST(BookManager, StartsEmptyAndFindsNothing) {
	book_manager books;
	EXPECT_TRUE(books.empty());
	EXPECT_EQ(books.size(), 0u);
	EXPECT_EQ(books.lookup(0), nullptr);
	EXPECT_EQ(books.lookup(7), nullptr);
	EXPECT_FALSE(books.contains(7));
}

TEST(BookManager, CreateThenLookupFindsTheSameBook) {
	book_manager books;
	order_book &created = books.create(3);

	EXPECT_EQ(books.lookup(3), &created);
	EXPECT_TRUE(books.contains(3));
	EXPECT_EQ(books.size(), 1u);
	EXPECT_FALSE(books.empty());
}

// A symbol this partition does not carry must read as absent, not as an empty
// book - otherwise a misroute becomes an order silently accepted onto a book
// nobody will ever read.
TEST(BookManager, LookupOfAnUnregisteredSymbolIsNullNotAFreshBook) {
	book_manager books;
	books.create(3);

	EXPECT_EQ(books.lookup(4), nullptr);   // inside the slot vector, unoccupied
	EXPECT_EQ(books.lookup(999), nullptr); // past its end
	EXPECT_EQ(books.size(), 1u);           // neither lookup created anything
}

// Replacing a live book would drop every order resting on it with nothing
// emitted to say so, so a second create returns the book already there.
TEST(BookManager, CreateIsIdempotentAndKeepsRestingOrders) {
	book_manager books;
	order_book &first = books.create(3);
	first.add_order(side_t::bid, 100, 10);

	order_book &again = books.create(3);

	EXPECT_EQ(&again, &first);
	EXPECT_EQ(books.size(), 1u);
	EXPECT_EQ(again.volume_at_price(100, side_t::bid), 10);
}

// The engine takes a reference from lookup and keeps using it. Registering more
// listings grows the slot vector, and that must not move the books.
TEST(BookManager, BookAddressesSurviveLaterRegistrations) {
	book_manager books;
	order_book &first = books.create(0);
	first.add_order(side_t::bid, 100, 10);

	for (symbol_id_t symbol = 1; symbol <= 64; ++symbol) books.create(symbol);

	EXPECT_EQ(books.lookup(0), &first);
	EXPECT_EQ(first.volume_at_price(100, side_t::bid), 10);
	EXPECT_EQ(books.size(), 65u);
}

TEST(BookManager, RemoveDropsTheBookAndReportsWhetherOneWasThere) {
	book_manager books;
	books.create(3);

	EXPECT_TRUE(books.remove(3));
	EXPECT_FALSE(books.contains(3));
	EXPECT_EQ(books.lookup(3), nullptr);
	EXPECT_EQ(books.size(), 0u);

	EXPECT_FALSE(books.remove(3));   // already gone
	EXPECT_FALSE(books.remove(999)); // never existed
}

// A hole is not a book: size counts listings, not slots.
TEST(BookManager, SizeCountsLiveBooksNotSlots) {
	book_manager books;
	books.create(0);
	books.create(10); // leaves slots 1..9 empty
	EXPECT_EQ(books.size(), 2u);

	books.remove(0);
	EXPECT_EQ(books.size(), 1u);
	EXPECT_TRUE(books.contains(10));
}

TEST(BookManager, ClearDropsEveryBookAndLeavesTheManagerReusable) {
	book_manager books;
	books.create(1);
	books.create(2);

	books.clear();

	EXPECT_TRUE(books.empty());
	EXPECT_EQ(books.lookup(1), nullptr);

	order_book &rebuilt = books.create(1);
	rebuilt.add_order(side_t::ask, 101, 5);
	EXPECT_EQ(books.size(), 1u);
	EXPECT_EQ(rebuilt.volume_at_price(101, side_t::ask), 5);
}
