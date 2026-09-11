#pragma once
#include "core/util/enum_string.hpp"

#include <cstdint>

namespace exchange::engine::event {

#define COMMAND_TYPE_LIST(X)                                                   \
	X(PLACE, "cross, then rest the remainder")                                 \
	X(CANCEL, "remove a resting order by id")                                  \
	X(ADD, "rest anonymous liquidity, no matching")                            \
	X(REDUCE, "drain qty at a price, FIFO-first")

enum class command_type : std::uint8_t {
	EXCHANGE_ENUM_VALUES(COMMAND_TYPE_LIST)
};

EXCHANGE_ENUM_NAME(command_type, to_string, COMMAND_TYPE_LIST)

#undef COMMAND_TYPE_LIST
} // namespace exchange::engine::event