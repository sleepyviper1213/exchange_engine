#pragma once

#include "core/util/enum_format.hpp"

#include <cstdint>

namespace exchange::engine::event::lifecycle {

using core::util::format_as;

/**
 * @brief Why the session ended, and therefore how much of it to believe.
 *
 * Three outcomes and not a boolean, because a recovery has to treat them
 * differently. After CLEAN the books are exactly what the log says. After
 * HALTED they are too - the difference is that commands were still queued and
 * were never applied, so a client waiting on an ack will never get one and the
 * absence is not a bug. After FAULT the log is the *only* thing to trust: the
 * in-memory books at the moment of the fault are unreachable, and rebuilding
 * from the last snapshot forward is the only correct move.
 *
 * @note HALTED is not an error, which is why it is not FAULT. A circuit breaker
 *       tripping and an operator stopping a venue are the system working.
 */
enum class stop_reason : std::uint8_t {
	CLEAN,  ///< the queue was drained and the engine asked to stop
	HALTED, ///< stopped by an operator or a breaker with work still queued
	FAULT,  ///< an unrecoverable error; state is not to be trusted
};

} // namespace exchange::engine::event::lifecycle
