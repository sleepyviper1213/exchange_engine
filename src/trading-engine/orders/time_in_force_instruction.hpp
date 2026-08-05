#pragma once

#include "core/util/enum_string.hpp"
#include "fwd.hpp"

#include <cstdint>

namespace exchange::engine::orders {

#define TIME_IN_FORCE_INSTRUCTION_LIST(X)                                      \
	X(GOOD_TILL_CANCELLED, "rest the unfilled remainder indefinitely")         \
	X(FILL_OR_KILL, "execute fully and immediately, or not at all")            \
	X(IMMEDIATE_OR_CANCEL, "execute what crosses now, drop the remainder")     \
	X(ALL_OR_NONE,                                                             \
	  "order must be filled in its entirety and stays on the book until it "   \
	  "is finished or cancelled")

/**
 * @brief How long an order may live — the duration half of its instructions.
 *
 * Decides only what becomes of the quantity that did not cross:
 * @c GOOD_TILL_CANCELLED rests it, @c IMMEDIATE_OR_CANCEL withdraws it as a
 * CANCELLED outcome carrying @c TIME_IN_FORCE, and @c FILL_OR_KILL refuses the
 * whole order up front unless the book can fill it entirely, so it never has a
 * remainder to decide about.
 */
enum class time_in_force_instruction : std::uint8_t {
	EXCHANGE_ENUM_VALUES(TIME_IN_FORCE_INSTRUCTION_LIST)
};

/// @brief The enumerator name of a @c time_in_force_instruction, e.g.
///        @c "FILL_OR_KILL" (empty view if out of range).
EXCHANGE_ENUM_NAME(time_in_force_instruction, to_string,
				   TIME_IN_FORCE_INSTRUCTION_LIST)
}
