#pragma once
// The Binance -> venue-neutral adapter: where `U`, `u`, `lastUpdateId` and
// milliseconds stop being anyone else's problem.
//
// This is the only place that knows how Binance spells the things
// normalised.hpp names generically. Everything downstream — sequencing, gap
// detection, reconstruction — takes the neutral types, so a second venue is a
// second file like this one and no change anywhere else.
// @see
// https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams

#include "market_data_export.hpp" // MARKET_DATA_EXPORT (generated)
#include "binance_depth.hpp"
#include "core/util/inclusive_range.hpp"
#include "fwd.hpp"
#include "market-data/normalised.hpp"

namespace exchange::market_data::binance {

/**
 * @brief The sequence range a diff event covers: Binance's @c U and @c u.
 *
 * The cheap half of normalisation, and the only half the sequencer needs — so
 * a caller decoding straight into a book with @c DepthParser::apply_update can
 * still gap-check the frame from the @c DepthUpdateMeta it gets back, without
 * materialising a @c depth_event.
 * @param meta The bookkeeping fields of a decoded @c depthUpdate.
 * @return The inclusive range @c [U, u].
 */
[[nodiscard]] MARKET_DATA_EXPORT core::util::inclusive_range<sequence_t>
sequence_of(const DepthUpdateMeta &meta) noexcept;

/// @copydoc sequence_of(const DepthUpdateMeta &)
[[nodiscard]] MARKET_DATA_EXPORT core::util::inclusive_range<sequence_t>
sequence_of(const DepthUpdate &update) noexcept;

/**
 * @brief Normalise a decoded @c depthUpdate into a venue-neutral event.
 *
 * Maps @c U / @c u onto @ref core::util::inclusive_range, converts @c E from
 * milliseconds to the neutral nanosecond epoch, and copies the already-scaled
 * levels across.
 * @param update The decoded diff event.
 * @return The neutral event, ready for @c depth_sequencer / @c
 *         depth_reconstructor.
 * @note The level copy is the price of retaining an event across the sequencing
 *       decision, which is unavoidable while a snapshot is outstanding: the
 *       frame's buffer is long gone by the time the event is replayed. On the
 *       steady in-sequence path, prefer @c DepthParser::apply_update with @c
 *       sequence_of — no event is retained there, so none needs building.
 */
[[nodiscard]] MARKET_DATA_EXPORT depth_event
normalise(const DepthUpdate &update);

/**
 * @brief Normalise a decoded REST depth payload into a neutral snapshot.
 *
 * @c lastUpdateId becomes @c book_snapshot::sequence — the last id the snapshot
 * already includes, so the first diff applied on top must cover it plus one.
 * Binance has no event time on this payload, so @c event_time stays zero.
 * @param snapshot The decoded @c /api/v3/depth payload.
 * @return The neutral snapshot, ready to seed a book.
 */
[[nodiscard]] MARKET_DATA_EXPORT book_snapshot
normalise(const DepthSnapshot &snapshot);

} // namespace exchange::market_data::binance
