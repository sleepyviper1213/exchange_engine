#pragma once

#include "core/util/enum_string.hpp"

#include <cstdint>

namespace exchange::engine::event::lifecycle {
#define START_MODE_LIST(X)                                                     \
	X(COLD, "empty books; the previous session's state was discarded")         \
	X(RECOVERED, "state rebuilt from a snapshot and journal; ids continue")

/**
 * @brief Whether the session inherited the previous one's state.
 *
 * The single most consequential thing a start-up record says, because it
 * decides what the order ids that follow mean. After a COLD start the id space
 * is empty and an id seen in the previous session may legitimately reappear
 * naming a different order; after a RECOVERED start the same id still names the
 * same resting order, and reusing it is the duplicate the book rejects.
 *
 * A reader that ignores this distinction and replays two COLD sessions into one
 * book gets DUPLICATE_ORDER_ID on every id that repeats - which is the failure
 * behaving correctly, but it is a failure that this one byte prevents.
 */
enum class start_mode : std::uint8_t { EXCHANGE_ENUM_VALUES(START_MODE_LIST) };

EXCHANGE_ENUM_NAME(start_mode, to_string, START_MODE_LIST)

#undef START_MODE_LIST
} // namespace exchange::engine::event::lifecycle