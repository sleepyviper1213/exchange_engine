#pragma once
// Symbol -> partition routing.
//
// The one decision that turns N independent single-threaded matching engines
// into one engine: which partition owns a given listing. It is a pure function
// of the symbol id and the partition count, which is what makes the answer the
// same everywhere without anyone coordinating - a producer, a consumer and a
// recovery replay all compute it independently and agree.


#include "trading_engine_export.hpp" // TRADING_ENGINE_EXPORT (generated)
#include "fwd.hpp"
#include "trading-engine/event/command.hpp"
#include "trading-engine/orders/types.hpp"

#include <cassert>
#include <cstddef>

namespace exchange::engine::execution {

/**
 * @brief Maps a listing onto the partition that owns its book.
 *
 * Owns no book, no queue and nothing mutable - it holds the partition count and
 * answers questions about it. Copy it, share it, compute the same answer on
 * either side of a queue; there is nothing to keep in step.
 *
 * @par Why the symbol id is used directly and not scrambled
 * A hash here would be worse, not neutral. @c symbol_id_t is dense by contract,
 * so consecutive ids modulo the partition count deal the listings out
 * round-robin - perfectly even by construction, and adjacent ids (which is how
 * reference data numbers related instruments) deliberately land on *different*
 * partitions. Running them through an avalanche function would replace that
 * guarantee with the balance of a random assignment, which for the small
 * listing counts a venue actually has is measurably lumpier. Identity is the
 * better hash for a dense key.
 *
 * @warning Therefore not a stable mapping across a resize. Changing the
 *          partition count moves listings between partitions, and a listing
 *          whose book lives on the old partition must be drained before it
 *          moves - there is no consistent-hashing property here to lean on.
 *          Partition counts are fixed at startup.
 */
class dispatcher {
public:
	/**
	 * @brief Route across @p partition_count partitions.
	 * @param partition_count How many partitions exist. Must be positive; one
	 *        is legal and routes everything to partition 0.
	 */
	TRADING_ENGINE_EXPORT explicit dispatcher(
		std::size_t partition_count) noexcept;

	/// @brief The partition owning @p symbol.
	[[nodiscard]] TRADING_ENGINE_EXPORT std::size_t
	partition_for(symbol_id_t symbol) const noexcept;

	/**
	 * @brief The partition that must execute @p cmd.
	 *
	 * Reads @c command::symbol whatever the command's type, which is the reason
	 * that field sits outside the union: routing must not have to know whether
	 * it is looking at a PLACE, a CANCEL or a level change.
	 */
	[[nodiscard]] TRADING_ENGINE_EXPORT std::size_t
	partition_for(const event::command &cmd) const noexcept;

	/// @brief How many partitions this routes across.
	[[nodiscard]] TRADING_ENGINE_EXPORT std::size_t
	partition_count() const noexcept;

	/// @brief Whether @p partition is the one that owns @p symbol - the check a
	///        partition makes to reject a command that was misrouted to it.
	[[nodiscard]] TRADING_ENGINE_EXPORT bool
	owns(std::size_t partition, symbol_id_t symbol) const noexcept;

private:
	std::size_t count_;
	std::size_t mask_;  ///< count_ - 1 when that is meaningful, else unused
	bool power_of_two_; ///< whether mask_ is meaningful
};

} // namespace exchange::engine::execution
