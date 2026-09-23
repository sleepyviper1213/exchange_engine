#pragma once
#include "core/util/enum_string.hpp"

#include <cstdint>

namespace exchange::engine::event {

#define COMMAND_TYPE_LIST(X)                                                   \
	X(PLACE, "cross, then rest the remainder")                                 \
	X(CANCEL, "remove a resting order by id")                                  \
	X(ADD, "rest anonymous liquidity, no matching")                            \
	X(REDUCE, "drain qty at a price, FIFO-first")                              \
	X(MODIFY, "amend a resting order's price or quantity")

/**
 * @brief Which book mutation a command carries.
 *
 * @warning The values are the journal's tags, written to disk one byte per
 *          record. Enumerators are appended and never reordered: renumbering
 *          them would make every journal written before the change read back as
 *          a different, entirely plausible stream of commands.
 *          @see journal_record
 */
enum class command_type : std::uint8_t {
	EXCHANGE_ENUM_VALUES(COMMAND_TYPE_LIST)
};

EXCHANGE_ENUM_NAME(command_type, to_string, COMMAND_TYPE_LIST)

#undef COMMAND_TYPE_LIST
} // namespace exchange::engine::event