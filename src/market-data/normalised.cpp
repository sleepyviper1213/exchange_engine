#include "normalised.hpp"

#include "trading-engine/orders/types.hpp"
#include "l2_book.hpp"


namespace exchange::market_data {

void apply(l2_book &book, const depth_event &event) {
	for (const auto &[price, volume] : event.bids)
		book.set_level(side_t::bid, price, volume);
	for (const auto &[price, volume] : event.asks)
		book.set_level(side_t::ask, price, volume);
}

void reset(l2_book &book, const book_snapshot &snapshot) {
	// load() replaces a side outright, so both sides together are a full reseed
	// - no clear() first, and nothing survives from the book's previous state.
	//
	// By reference, and no move: the book owns its cells for life and copies the
	// levels it keeps into them, so there is nothing here for the caller to hand
	// over. Taking the snapshot by value would move two vectors only to read and
	// drop them.
	book.load(side_t::bid, snapshot.bids);
	book.load(side_t::ask, snapshot.asks);
}

} // namespace exchange::market_data
