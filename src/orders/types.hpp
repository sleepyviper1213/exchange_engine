#pragma once

#include "side.hpp" // IWYU pragma: export

#include <cstdint>
#include <type_traits>

namespace exchange {

/**
 * @brief Price in **ticks** - never a scaled decimal.
 *
 * The engine matches on the integer grid @c symbol_spec defines, so a value of
 * this type is a tick count and nothing else: @c "153.45" on a listing with a
 * 0.05 tick is 3069 here, not 15345. 32 bits is the whole tick domain of any
 * listing that could be traded - a $60,000 instrument on a $0.01 tick spans 6
 * million ticks, and this holds 4.29 billion - and it is what keeps a resting
 * order two to a cache line.
 *
 * @warning A **scaled** price (the text times 10^scale, which is what a venue
 *          feed carries and what @c market_data stores) does not fit here and
 *          must not be cast into it: at scale 8 a five-figure price is ~10^12.
 *          @c market_data::scaled_price_t is that type.
 *          @c symbol_spec::price_from_scaled is the only sanctioned crossing,
 *          and it range-checks rather than truncates.
 */
using price_t = std::uint32_t;

/**
 * @brief One order's quantity in **lots**, on the grid @c symbol_spec defines.
 *
 * Signed because the validation boundary is stated as @c qty <= 0 and because
 * an order's remaining quantity is arithmetic that wants a sign during
 * intermediate steps - not because a resting order may be negative. It may not.
 *
 * @warning This is a *per-order* quantity, bounded by
 *          @c symbol_spec::quantity_from_scaled's range check. Anything that
 *          sums quantities across orders or levels must use @c volume_t: a
 *          level's aggregate, or the liquidity a fill-or-kill adds up, can
 *          exceed one order's range without any single order doing so.
 */
using quantity_t = std::int32_t;

/**
 * @brief Aggregate quantity - a sum of @c quantity_t across orders or levels.
 *
 * Exists so narrowing an order's quantity cannot silently narrow the totals
 * built from it. A level holding 100,000 orders of a billion lots each is not a
 * realistic book, but the addition that would wrap is one line in the matching
 * loop and the loop is the wrong place to find out. 64 bits removes the
 * question rather than answering it.
 */
using volume_t = std::int64_t;

/// @brief Stable identifier for a client order. Stays 64-bit: it is assigned
///        outside the engine and carries no density contract, which is exactly
///        why cancel-by-id still goes through a hash map. @see order_book
using order_id_t = std::uint64_t;

/**
 * @brief Where a command sat in one partition's applied stream, stamped on
 *        every record that command produced.
 *
 * Not assigned by whoever built the command: it is the command's *ordinal*,
 * counted by the partition as it applies the batch, and therefore the index of
 * that same command in that partition's journal. Which is the point - a
 * producer-assigned number could disagree with the log, and a gap or a repeat
 * would then be indistinguishable from a command that never made it. Counting
 * on the way out makes both impossible by construction, and makes replay
 * verifiable: replaying a journal re-derives exactly the numbers the live run
 * stamped, so the two streams compare record for record.
 *
 * Monotonic and gapless within a partition, and meaningless across two - they
 * are independent streams over disjoint listings, and a total order between
 * them would be a synchronisation point on the one path that has none.
 *
 * @note Zero is "unsequenced": a record built outside a partition, such as a
 *       venue report crossing @c session::venue_bridge, or one produced by an
 *       @c order_book driven directly by a test or a benchmark.
 * @note @c engine_ and not a bare @c sequence_t, because
 *       @c market_data::sequence_t already means something else entirely - the
 *       update id a *venue* stamps on a depth diff, which arrives from outside
 *       and is checked for gaps rather than issued. The two would be an
 *       ambiguity in any scope that opened both namespaces, and a far worse
 *       confusion in prose. Same reason @c l2_book and @c order_book are named
 *       apart: different concepts do not share a name here.
 */
using engine_sequence_t = std::uint64_t;

/**
 * @brief A listing's execution number: 1 for its first trade, and up from
 *        there.
 *
 * Per listing rather than per venue, because a tape is per listing - a consumer
 * of one instrument's prints wants them numbered 1, 2, 3, and a venue-wide
 * counter would hand it an arbitrary subsequence with no way to tell a gap from
 * a message it dropped. @c (symbol_id, trade_id) is the venue-unique name, and
 * the symbol is already reattached by the time a trade leaves the partition.
 * @see event::engine_event
 *
 * Dense and gapless by construction: @c order_book assigns it where the trade
 * is created, so there is no path that prints an execution without numbering
 * it.
 *
 * @note Zero means unassigned, which is what a trade built by hand carries.
 */
using trade_id_t = std::uint64_t;

/// @brief A point in time, in nanoseconds since the Unix epoch. Zero means "not
///        stamped". The clock is read at the venue boundary and nowhere on the
///        matching path. @see engine::trade::timestamp
using timestamp_t = std::uint64_t;

/// @brief Dense identifier for a listing, assigned by the reference-data
/// source.
///        Dense because it indexes the book manager's per-symbol arrays.
using symbol_id_t = std::uint32_t;

/**
 * @brief Who an order belongs to - the participant the venue will bill and
 *        report to.
 *
 * Assigned at the gateway when a session authenticates, so it is trusted by the
 * time an order carries it and never comes off the wire. 32 bits because it
 * names a member of the venue rather than a client order: an order_book has
 * thousands of participants, not billions, and the narrower type is what keeps
 * an @c order_record inside its size budget.
 *
 * Zero means unattributed, which is what anonymous seeded liquidity carries.
 * Self-trade prevention is the reason this exists - two orders may not cross if
 * they name the same account - but nothing enforces that yet.
 */
using account_id_t = std::uint32_t;

static_assert(!std::is_floating_point_v<price_t>,
			  "Price must not be floating point");
static_assert(std::is_unsigned_v<price_t>, "Price must be unsigned");
static_assert(std::is_signed_v<quantity_t>,
			  "Quantity must be signed: the validation boundary is qty <= 0");
static_assert(
	sizeof(volume_t) >= 2 * sizeof(quantity_t),
	"volume_t must be wide enough that summing quantities cannot wrap");

/**
 * @brief The opposite side of @p s (bid <-> ask).
 * @param s A book side.
 * @return The opposing side.
 */
constexpr side_t opposed(side_t s) {
	return static_cast<side_t>(!static_cast<bool>(s));
}

} // namespace exchange
