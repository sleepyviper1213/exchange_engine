#pragma once
// The one level type a Binance payload decodes into.

#include "market_data/l2_book.hpp" // the reconstruction target

namespace exchange::market_data::binance {

/**
 * @brief One aggregated price level from a Binance depth payload.
 *
 * Prices and sizes are integers scaled by 10^decimals (no floating point), so
 * they drop straight into @c l2_book - and this is literally that type, not a
 * struct shaped like it.
 *
 * @par Why an alias and not its own struct
 * It was its own struct with exactly these two fields - two types of identical
 * layout, unrelated to the compiler purely because they were spelled twice. The
 * separation was supposed to keep the venue-neutral layer independent of a
 * venue decoder, but the decoder already includes @c l2_book.hpp (the
 * reconstruction target is the whole point of it), so the dependency it was
 * protecting did not exist.
 *
 * @note Merging them did @b not speed anything up, which was the original
 *       motivation and was wrong. @c binance::normalise still copies level by
 *       level, because collapsing that to a whole-vector copy - which the
 * shared type now permits - measured ~15% @em slower on
 *       @c BM_Reconstructor_SteadyState. See the note on @c to_levels in
 *       normalise.cpp for the measurement and the likely reason. What the alias
 *       actually bought was one type instead of two, one fmt formatter instead
 *       of two, and one less forward declaration.
 *
 * The name stays because a Binance frame reads better with it, and because a
 * venue whose levels ever carry more than a price and a size gets its own
 * struct back at that point - the alias is what makes that a local change.
 */
using PriceLevel = l2_book::price_level;

} // namespace exchange::market_data::binance
