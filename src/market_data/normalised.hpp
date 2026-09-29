#pragma once
// Venue-neutral market data: the vocabulary every feed decoder normalises into.
//
// A venue's own types speak its wire vocabulary. Binance names the update-id
// bounds of a diff `U` and `u`, stamps events in milliseconds, delivers bids
// descending and asks ascending, and spells sizes as decimal strings. Another
// venue names, orders and scales all four differently. None of that should
// reach the code that sequences a feed or reconstructs a book, so it stops at
// the decoder: each venue adapter supplies a `normalise()` overload (see
// binance/normalise.hpp) producing the records in depth_event.hpp,
// book_snapshot.hpp and trade_print.hpp, and everything downstream knows only
// those.
//
// What normalisation fixes:
//   * sequencing - one inclusive @ref inclusive_range per event, whatever the
//     venue calls its bounds;
//   * time - nanoseconds since the Unix epoch, whatever resolution it
//     publishes;
//   * levels - already scaled to the book's integral Price/Volume, in whatever
//     order they arrived (@c l2_book::load imposes the ordering).
//
// This header holds only the two aliases all three records share. Include the
// record you need, not this - the aliases come with it.

#include "l2_book.hpp"

#include <chrono>

namespace exchange::market_data {

/// @brief A normalised aggregated level. Identical in shape to the book's own
///        cell, so a decoded side moves into an @c l2_book with no conversion.
using book_level = l2_book::price_level;

/// @brief Nanoseconds since the Unix epoch - the one time unit a normalised
///        feed speaks, whatever resolution the venue publishes.
using timestamp = std::chrono::nanoseconds;

} // namespace exchange::market_data
