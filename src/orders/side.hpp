#pragma once
// Which half of the book an order joins.
//
// Split from types.hpp, which is otherwise the scalar vocabulary - widths and
// the reasoning behind them. A side is not a width: it is the one enumerated
// type every layer in the tree switches on, and it comes with the fmt hook that
// prints it. That belongs in a file of its own, and types.hpp re-exports it so
// no call site has to care.

#include "core/util/enum_format.hpp"

namespace exchange {

using core::util::format_as;

/// @brief A book side. Prints as its enumerator name, e.g. @c "bid".
enum class side_t : bool {
	bid, ///< buy side; best price is the highest
	ask, ///< sell side; best price is the lowest
};

/**
 * @brief The opposite side of @p s (bid <-> ask).
 * @param s A book side.
 * @return The opposing side.
 */
constexpr side_t opposed(side_t s) {
	return static_cast<side_t>(!static_cast<bool>(s));
}

} // namespace exchange
