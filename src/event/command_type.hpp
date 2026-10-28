#pragma once
#include "core/util/enum_format.hpp"

#include <cstdint>

namespace exchange::engine::event {

using core::util::format_as;

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
	PLACE,  ///< cross, then rest the remainder
	CANCEL, ///< remove a resting order by id
	ADD,    ///< rest anonymous liquidity, no matching
	REDUCE, ///< drain qty at a price, FIFO-first
	MODIFY, ///< amend a resting order's price or quantity
};

} // namespace exchange::engine::event
