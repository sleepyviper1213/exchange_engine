#include "book_snapshot.hpp"

#include "l2_book.hpp"
#include "orders/types.hpp"

namespace exchange::market_data {

void reset(l2_book &book, const book_snapshot &snapshot) {
	// load() replaces a side outright, so both sides together are a full reseed
	// - no clear() first, and nothing survives from the book's previous state.
	//
	// By reference, and no move: the book owns its cells for life and copies
	// the levels it keeps into them, so there is nothing here for the caller to
	// hand over. Taking the snapshot by value would move two vectors only to
	// read and drop them.
	book.load(side_t::bid, snapshot.bids);
	book.load(side_t::ask, snapshot.asks);
}

} // namespace exchange::market_data
