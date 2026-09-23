#pragma once
#include "../orders/amendment.hpp"
#include "../orders/order.hpp"
#include "../orders/types.hpp"
#include "command_type.hpp"
#include "event_export.hpp" // EVENT_EXPORT (generated)
#include "fwd.hpp"

#include <cassert>
#include <type_traits>

namespace exchange::engine::event {

// Command/level_change are execution input; they name order domain types
// (a downward dependency - Event sits above Orders in the layer graph).
using engine::orders::amendment;
using engine::orders::order;

/// @brief Side/price/qty payload shared by ADD and REDUCE.
struct level_change {
	side_t side;
	price_t price;
	quantity_t volume;
};

/**
 * @brief One unit of work handed to the matching engine over the SPSC queue: a
 *        tag plus the payload for exactly one book mutation.
 *
 * Deliberately a trivially copyable tagged union so @c spsc_queue<Command, N>
 * takes its @c memcpy batch path. There is no default constructor - build one
 * with a named factory (@c Command::place, @c Command::cancel, …) so the
 * union's active member always matches @c type.
 */
struct command {
	/// @brief Which book mutation a Command carries.

	command_type type;

	/**
	 * @brief Which listing this command is for - the routing key.
	 *
	 * On the command rather than in the union because @c dispatcher has to read
	 * it for every command without first switching on the tag, and all but one
	 * payload has nowhere to put it: a @c level_change is a side, a price and a
	 * size, a CANCEL is an id, and an @c amendment names an order rather than a
	 * listing. Only PLACE carried a symbol, inside its @c order, which made
	 * exactly one command type routable.
	 *
	 * @note Free, as it happens. The tag is one byte followed by seven of
	 *       padding, because the union aligns to eight; the symbol lands in
	 * that padding and @c sizeof(command) does not move.
	 * @note Zero is "unspecified", matching @c order::symbol_id - the honest
	 *       value for a single-book deployment that never routes.
	 */
	symbol_id_t symbol;

	/**
	 * @brief The order a PLACE carries.
	 * @pre @c type is @c command_type::PLACE. Reading the wrong arm of the
	 * union is undefined behaviour, not a misread value, so this is checked
	 * rather than trusted - the assert survives an optimised build under
	 *      @c enable_hardening.
	 */
	[[nodiscard]] EVENT_EXPORT const order &as_place() const noexcept;

	/// @brief The order id a CANCEL names. @pre @c type is @c
	/// command_type::CANCEL.
	[[nodiscard]] EVENT_EXPORT order_id_t as_cancel() const noexcept;

	/// @brief The side/price/size an ADD or REDUCE carries.
	/// @pre @c type is @c command_type::ADD or @c command_type::REDUCE.
	[[nodiscard]] EVENT_EXPORT const level_change &as_level() const noexcept;

	/// @brief The id/price/quantity a MODIFY carries.
	/// @pre @c type is @c command_type::MODIFY.
	[[nodiscard]] EVENT_EXPORT const amendment &as_modify() const noexcept;

	/// @brief Place @p o. The symbol is taken from @c order::symbol_id, which
	/// is
	///        where a validated order already records it.
	EVENT_EXPORT static command place(const order &o) noexcept;
	EVENT_EXPORT static command cancel(symbol_id_t symbol,
									   order_id_t id) noexcept;
	EVENT_EXPORT static command add(symbol_id_t symbol, side_t side,
									price_t price, quantity_t volume) noexcept;
	EVENT_EXPORT static command reduce(symbol_id_t symbol, side_t side,
									   price_t price,
									   quantity_t volume) noexcept;

	/// @brief Amend the resting order @p change names.
	///
	/// The symbol is a parameter here and not on the payload, unlike PLACE: an
	/// amendment names an order id and nothing else about where that order
	/// lives, so there is no second copy of the listing for the two to disagree
	/// about. @see amendment
	EVENT_EXPORT static command modify(symbol_id_t symbol,
									   const amendment &change) noexcept;

private:
	/**
	 * @brief Exactly one book mutation's payload, picked by @c type.
	 *
	 * Private, and reached only through @c as_place / @c as_cancel /
	 * @c as_level / @c as_modify. A union whose arms could be read from
	 * anywhere puts the tag-matches-payload obligation on every call site and
	 * gives undefined behaviour to whichever one forgets; confining it here
	 * leaves four checked readers and four writers, all in this file, and no
	 * way to ask the question wrongly.
	 *
	 * @note Still a union rather than a @c std::variant, which is what the
	 *       Core Guidelines would otherwise ask for. A variant carries its own
	 *       discriminant beside the @c type this already has, and - decisively
	 *       - @c spsc_queue's batch @c memcpy path needs the whole command
	 *       trivially copyable, which the @c static_assert below enforces.
	 */
	union {
		order order_;         ///< PLACE
		order_id_t cancel_id; ///< CANCEL
		level_change level;   ///< ADD / REDUCE
		amendment amend_;     ///< MODIFY
	};

	// Each ctor initialises exactly the union member that matches the tag, so
	// reading it back through the same tag is always the active member.
	explicit command(const order &o) noexcept;
	command(command_type t, symbol_id_t listing, order_id_t id) noexcept;
	command(command_type t, symbol_id_t listing, level_change lc) noexcept;
	command(command_type t, symbol_id_t listing, const amendment &a) noexcept;
};

static_assert(
	std::is_trivially_copyable_v<command>,
	"Command must stay trivially copyable for the lockfree's memcpy path");

/**
 * @brief The journal's stride, pinned.
 *
 * A command is what the journal is an array of, so its size *is* the on-disk
 * format. Adding a field here changes that format silently: every journal
 * written before the change still reads back, one record at a time, as
 * plausible nonsense. @c record_log now records the stride in its header and
 * refuses a file that disagrees - but it can only refuse at runtime, on a store
 * somebody is already trying to recover. This is the same fact stated at
 * compile time, so the change is caught by whoever makes it rather than by
 * whoever is on call.
 *
 * If you are here because this failed: the size moving is not itself a bug.
 * Update the number, and bump @c LOG_FORMAT_VERSION only if you also need
 * existing journals refused for a reason the stride check would miss.
 *
 * @note 48 rather than 45. The tag is one byte and the union aligns to eight,
 * so there are three bytes of padding after @c type, one inside @c order and
 *       four before its @c timestamp - eight bytes that cost nothing today and
 *       are where a defined-offset encoding would put a checksum and a version.
 * @note MODIFY did not move it, and that is what made the command cheap to add:
 *       an @c amendment is 24 bytes against a PLACE's 40, so it fits inside the
 *       widest arm with room to spare, and every field it needs already had an
 *       offset in the journal's layout. A command type whose payload was wider
 *       than an @c order would change both this number and the record stride.
 */
static_assert(sizeof(command) == 48,
			  "the journal's record stride changed - see the note above before "
			  "updating this number");

} // namespace exchange::engine::event
