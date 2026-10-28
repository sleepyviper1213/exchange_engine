#pragma once

#include "core/util/enum_format.hpp"
#include "fwd.hpp"

#include <cstdint>

namespace exchange::engine::orders {

using core::util::format_as;

/**
 * @brief How long an order may live - the duration half of its
 * instructions.
 *
 * Decides only what becomes of the quantity that did not cross:
 * @c GOOD_TILL_CANCELLED rests it, @c IMMEDIATE_OR_CANCEL withdraws it as a
 * CANCELLED outcome carrying @c TIME_IN_FORCE, and @c FILL_OR_KILL refuses
 * the whole order up front unless the book can fill it entirely, so it
 * never has a remainder to decide about.
 *
 * @warning @c ALL_OR_NONE states the instruction this vocabulary will grow
 * into and is **not matched yet**: @c order_book refuses it at admission
 *          with @c UNSUPPORTED_TIME_IN_FORCE. It is the one instruction
 * whose meaning is entirely in what happens after it rests, and a resting
 *          order in this book carries no time-in-force to honour it by - so
 * the alternative to refusing it is accepting it and filling it in part,
 *          which is the single thing it exists to forbid. The enumerator
 * stays because the label is the specification the book will be held to;
 *          @see order_book's note on what a resting order does not carry.
 */
enum class time_in_force_instruction : std::uint8_t {
	GOOD_TILL_CANCELLED, ///< rest the unfilled remainder indefinitely
	FILL_OR_KILL,        ///< execute fully and immediately, or not at all
	IMMEDIATE_OR_CANCEL, ///< execute what crosses now, drop the remainder
	ALL_OR_NONE,         ///< filled in its entirety, resting until finished or
						 ///< cancelled
};

} // namespace exchange::engine::orders