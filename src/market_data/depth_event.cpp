#include "depth_event.hpp"

#include "l2_book.hpp"
#include "orders/types.hpp"

namespace exchange::market_data {

void apply(l2_book &book, const depth_event &event) {
	for (const auto &[price, volume] : event.bids)
		book.set_level(side_t::bid, price, volume);
	
	for (const auto &[price, volume] : event.asks)
		book.set_level(side_t::ask, price, volume);
}

} // namespace exchange::market_data
