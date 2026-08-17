#pragma once
// Forward declarations and the shared vocabulary for the lifecycle event
// submodule.

#include <cstdint>

namespace exchange::engine::event::lifecycle {

/**
 * @brief Identifies one run of the engine — a *session*.
 *
 * The name for something the tree already depends on everywhere and never
 * spelled. @c order_manager::clear, @c position_book::forget,
 * @c working_ledger::clear, @c rate_limiter::reset and @c counter::reset each
 * document themselves as "a session boundary", and @c order_id_t's contract is
 * that a client order id is unique *within* a session. That contract is what
 * makes the boundary load-bearing rather than administrative: replaying two
 * sessions' commands into one book without one collides their ids and every
 * collision comes back as DUPLICATE_ORDER_ID.
 *
 * So a session is the scope order ids are unique in, and this is its name.
 * Assigned by whatever starts the engine — a wall-clock stamp, a run counter, a
 * deployment id — and required only to differ from the previous one, since the
 * only question ever asked of it is "is this the same session as that record's".
 *
 * @note 64 bits, and monotonic if the assigner can manage it: a replay reading
 *       a log backwards can then stop at the first session older than the one
 *       it wants without decoding what follows.
 */
using session_id_t = std::uint64_t;

struct startup;
struct shutdown;
struct recovery;

} // namespace exchange::engine::event::lifecycle
