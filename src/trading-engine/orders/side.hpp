#pragma once
// Which half of the book an order joins.
//
// Split from types.hpp, which is otherwise the scalar vocabulary - widths and
// the reasoning behind them. A side is not a width: it is the one enumerated
// type every layer in the tree switches on, and it comes with the X-macro list
// that generates its names and the fmt hook that prints it. That belongs in a
// file of its own, and types.hpp re-exports it so no call site has to care.

#include "core/util/enum_string.hpp"

namespace exchange {

#define EXCHANGE_SIDE_LIST(X)                                                  \
	X(bid, "buy side; best price is the highest")                              \
	X(ask, "sell side; best price is the lowest")

enum class side_t : bool { EXCHANGE_ENUM_VALUES(EXCHANGE_SIDE_LIST) };

EXCHANGE_ENUM_NAME(side_t, to_string, EXCHANGE_SIDE_LIST)

#undef EXCHANGE_SIDE_LIST
} // namespace exchange
