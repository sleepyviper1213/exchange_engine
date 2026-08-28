#pragma once

#include <cstdint>

namespace exchange::market_data {

// No class here carries a dll interface, and that is deliberate. Exporting a
// non-polymorphic class wholesale makes MSVC treat its *inline* members as part
// of the ABI - they stop being inlined across the boundary - and it makes every
// static constexpr member an imported object that no translation unit defines,
// which MinGW reports as an unresolved `__imp_` reference. So the annotation
// goes on the out-of-line public members instead, in the header that declares
// them. @see the Qt wiki's binary-compatibility rules.
class l2_book;

using sequence_t = std::int64_t;

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

/// @brief Why a feed stopped producing messages.
enum class feed_stop : std::uint8_t;

struct feed_status;
struct feed_run;
class replay_feed;

} // namespace exchange::market_data
