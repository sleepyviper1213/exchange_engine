#pragma once
// The two enums the sequencer speaks in: what to do with an event, and whether
// the local book is a live replica.
//
// Split from sequencer.hpp because they are the *answers* the sequencer gives,
// and a caller that only switches on one — a reconstructor, a feed-health
// dashboard, a formatter — has no business also reading the state machine that
// produces them. The X-macro list that generates an enum's names is part of the
// enum and travels with it.

#include "core/util/enum_string.hpp"
#include "fwd.hpp"

#include <cstdint>

namespace exchange::market_data {

/**
 * @brief What the sequencer says to do with an event.
 *
 * The enumerators and their descriptions are generated from one list via the
 * shared X-macro helpers (see @c core/util/enum_string.hpp).
 */
#define MARKET_DATA_SEQUENCE_ACTION_LIST(X)                                    \
	X(buffer, "no snapshot yet; retain the event")                             \
	X(discard, "already covered by the snapshot; drop it")                     \
	X(apply, "resumes the sequence; apply it to the book")                     \
	X(gap, "sequence discontinuity; the book is stale")

enum class sequence_action : std::uint8_t {
	EXCHANGE_ENUM_VALUES(MARKET_DATA_SEQUENCE_ACTION_LIST)
};

/// @brief What a @c sequence_action means, and with it the fmt hook that prints
///        the action as that description.
EXCHANGE_ENUM_LABEL(sequence_action, describe, MARKET_DATA_SEQUENCE_ACTION_LIST)

/// @brief Whether the local book is a live replica.
#define MARKET_DATA_SYNC_STATE_LIST(X)                                         \
	X(awaiting_snapshot, "unsynced; events must be buffered")                  \
	X(streaming, "seeded and in sequence; events apply directly")

enum class sync_state : std::uint8_t {
	EXCHANGE_ENUM_VALUES(MARKET_DATA_SYNC_STATE_LIST)
};

/// @brief What a @c sync_state means, plus the fmt hook that prints it.
EXCHANGE_ENUM_LABEL(sync_state, describe, MARKET_DATA_SYNC_STATE_LIST)


#undef MARKET_DATA_SEQUENCE_ACTION_LIST
#undef MARKET_DATA_SYNC_STATE_LIST
} // namespace exchange::market_data
