#pragma once

#include <cstdint>

// The number system market data speaks, kept deliberately separate from the one
// the engine matches on.
//
// A venue publishes decimal text; this module turns it into an integer scaled by
// 10^decimals and stores that - "153.45" at scale 2 is 15345, at scale 8 it is
// 15'345'000'000. The engine stores something else entirely: the same price
// divided by the listing's tick, which is a far smaller number (exchange::price_t,
// 32 bits). Both used to be one typedef, and the shared name hid the fact that a
// scaled price at scale 8 is around 10^12 while a tick count is around 10^6.
//
// They are separate types now so the compiler refuses the confusion. Crossing
// between them is symbol_spec's job and only symbol_spec's, because that is the
// one place that knows the listing's tick and lot sizes and can reject a value
// that does not fit rather than truncate it.

namespace exchange::market_data {

/**
 * @brief A price as the feed states it, scaled by 10^price_decimals.
 *
 * Signed, and 64-bit, for two independent reasons. Signed because
 * @c parser::parse_fixed_point produces a signed result and can legitimately
 * return a negative one - casting that into an unsigned type turned a bad number
 * into a huge valid-looking price. 64-bit because the scale is chosen at run
 * time (@c --price-decimals), and at scale 8 a five-figure price already needs
 * ~40 bits.
 */
using scaled_price_t = std::int64_t;

/**
 * @brief A size as the feed states it, scaled by 10^qty_decimals.
 *
 * Signed because an L2 diff expresses a level's removal as a non-positive size,
 * which @c l2_book::set_level reads as "erase this level".
 */
using scaled_qty_t = std::int64_t;

} // namespace exchange::market_data
