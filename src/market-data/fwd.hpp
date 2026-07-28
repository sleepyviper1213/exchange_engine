#pragma once
// Forward declarations for the venue-neutral market-data types. Prefer this
// over the full headers wherever a declaration suffices.
#include "market_data_export.hpp"

#include <cstdint>

namespace exchange::market_data {

class l2_book;

struct sequence_range;
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
