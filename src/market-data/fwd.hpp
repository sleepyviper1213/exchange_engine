#pragma once


// inclusive_range is a general closed interval over an integral type and lives
// in core/util, not here — nothing about it is market data. Included rather than
// forward-declared because a using-declaration needs the real name, and because
// it is a header-only template whose only dependency is <concepts>.
#include "core/util/inclusive_range.hpp"

#include <cstdint>

namespace exchange::market_data {

class l2_book;

/**
 * @brief The integer venue sequence numbers are carried in.
 *
 * @c inclusive_range is a template because the width is a venue's choice, not
 * this module's — but every type that crosses the module boundary
 * (@c depth_event, @c depth_sequencer, @c binance::sequence_of) has to agree on
 * one, or the comparisons inside @c depth_sequencer::observe are mixed-sign.
 * This alias is that agreement, and the single place to change it.
 *
 * @note Signed, which is the narrower of the two plausible choices: Binance's
 *       @c U / @c u are @c uint64_t on the wire, so @c binance::sequence_of
 *       casts down and a venue exceeding 2^63 ids would alias. None is close —
 *       Binance's update ids are ~10^9 — and signed keeps the differences the
 *       sequencer computes from wrapping at zero, which unsigned would do
 *       silently on the first out-of-order frame.
 */
using sequence_t = std::int64_t;

/// @brief Re-exported so this module's types can be spelled
///        @c market_data::inclusive_range<sequence_t>, which is what they mean:
///        a span of venue sequence numbers. Same type as
///        @c core::util::inclusive_range, not a distinct one.
using core::util::inclusive_range;

struct depth_event;
struct book_snapshot;

/// @brief What the sequencer says to do with an event.
enum class sequence_action : std::uint8_t;
/// @brief Whether the local book is seeded and in sequence.
enum class sync_state : std::uint8_t;

struct sequencer_stats;
class depth_sequencer;

struct reconstructor_options;
class depth_reconstructor;

} // namespace exchange::market_data
