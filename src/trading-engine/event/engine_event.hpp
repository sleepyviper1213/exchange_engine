#pragma once
// The record the return path carries: one published event, stamped with the
// listing it belongs to.
//
// `command` travels producer -> consumer; this travels consumer -> producer, and
// it exists for the same reason `command::symbol` does. A drain emits bare
// `trade`s and `order_outcome`s, neither of which names a listing, because inside
// a book the listing is implied by which book you are looking at. The moment
// those leave the partition that context is gone - and every consumer on the far
// side (`strategy_engine`, `risk_gate`) is built per listing - so the routing key
// has to be reattached before the events cross the queue, exactly where
// `command::symbol` reattaches it going the other way.
//
// It sits beside `command` rather than in `execution/` for that reason: the two
// are the same idea pointed in opposite directions, and this module is
// communication, not execution.

#include "core/util/enum_string.hpp"
#include "fwd.hpp"
#include "trading-engine/order_book/outcome.hpp"
#include "trading-engine/order_book/trade.hpp"
#include "trading-engine/orders/types.hpp"
#include "trading_engine_export.hpp" // TRADING_ENGINE_EXPORT (generated)

#include <cassert>
#include <cstdint>
#include <type_traits>

namespace exchange::engine::event {

#define ENGINE_EVENT_KIND_LIST(X)                                              \
	X(TRADE, "an execution; engine::trade")                                    \
	X(OUTCOME, "a lifecycle record; engine::order_outcome")

/**
 * @brief Which of the two published streams an @c engine_event came off.
 *
 * Two kinds and not more: these are the only things a partition publishes, and
 * the day a third stream appears the tag is where it goes rather than a second
 * queue, so the total order across streams survives.
 */
enum class EventKind : std::uint8_t {
	EXCHANGE_ENUM_VALUES(ENGINE_EVENT_KIND_LIST)
};

EXCHANGE_ENUM_NAME(EventKind, to_string, ENGINE_EVENT_KIND_LIST)

#undef ENGINE_EVENT_KIND_LIST

/**
 * @brief One event on its way back to the producer thread: a listing, a tag,
 *        and the payload the tag names.
 *
 * @par Why one tagged record and not two queues
 * A trade and the outcome that explains it are the same event seen twice, and
 * @c engine_partition::flush publishes them in that order deliberately. Two
 * rings would throw that away: a receiver draining the trade ring to empty
 * before touching the outcome ring can see batch *n+1*'s executions before
 * batch *n*'s order states, which is precisely the reordering the flush order
 * was chosen to prevent. One ring of one type keeps the total order the engine
 * produced, and costs one acquire load per pump instead of two.
 *
 * @par Layout
 * A trivially copyable tagged union, for the same reason @c command is:
 * @c spsc_queue takes its batch @c memcpy path only for a trivially copyable
 * element. The tag and the symbol sit outside the union - the symbol because
 * routing must read it without first switching on the tag (the dispatcher's
 * inner loop does nothing else), the tag because the union has nowhere to put
 * it.
 *
 * @note 8 bytes of header in front of a 24-byte payload. The alternative - a
 *       per-run header record threaded through the same ring - would save those
 *       8 bytes per event and cost the receiver a state machine spanning
 *       dequeues. At one cache line per event either way, that is not a trade
 *       worth making.
 */
class engine_event {
public:
	/**
	 * @brief An empty TRADE for a listing that does not exist.
	 *
	 * @c command deliberately has no default constructor, so that the active
	 * union member always matches the tag. The same reasoning gives this one
	 * *permission* to exist rather than taking it away: a receive buffer is an
	 * array of these, so a default is needed, and zeroing it leaves the tag and
	 * the active member agreeing. Nothing reads a default-constructed event -
	 * the dispatcher only ever looks at the prefix a dequeue filled.
	 */
	constexpr engine_event() noexcept
		: symbol(0), kind(EventKind::TRADE), execution_{} {}

	symbol_id_t symbol; ///< the listing this event belongs to; the routing key
	EventKind kind;     ///< which arm of the union is live

	/// @brief The execution a TRADE carries.
	/// @pre @c kind is @c EventKind::TRADE. Reading the wrong arm of a union is
	///      undefined behaviour rather than a wrong value, so this is checked
	///      and not trusted; the assert survives @c enable_hardening.
	[[nodiscard]] const engine::trade &as_trade() const noexcept {
		assert(kind == EventKind::TRADE);
		return execution_; // NOLINT(cppcoreguidelines-pro-type-union-access)
	}

	/// @brief The lifecycle record an OUTCOME carries.
	/// @pre @c kind is @c EventKind::OUTCOME.
	[[nodiscard]] const engine::order_outcome &as_outcome() const noexcept {
		assert(kind == EventKind::OUTCOME);
		return lifecycle_; // NOLINT(cppcoreguidelines-pro-type-union-access)
	}

	/// @brief Stamp @p execution as belonging to @p symbol.
	[[nodiscard]] TRADING_ENGINE_EXPORT static engine_event
	of(symbol_id_t symbol, const engine::trade &execution) noexcept;

	/// @brief Stamp @p record as belonging to @p symbol.
	[[nodiscard]] TRADING_ENGINE_EXPORT static engine_event
	of(symbol_id_t symbol, const engine::order_outcome &record) noexcept;

	TRADING_ENGINE_EXPORT bool
	operator==(const engine_event &other) const noexcept;

private:
	/// @brief Exactly one published payload, picked by @c kind. Private, so the
	///        tag-matches-payload obligation lives in this file and not at
	///        every call site. @see command's union for the same argument.
	union {
		engine::trade execution_;         ///< TRADE
		engine::order_outcome lifecycle_; ///< OUTCOME
	};

	// Each constructor initialises exactly the member its tag names.
	engine_event(symbol_id_t listing, const engine::trade &execution) noexcept;
	engine_event(symbol_id_t listing,
				 const engine::order_outcome &record) noexcept;
};

static_assert(std::is_trivially_copyable_v<engine_event>,
			  "engine_event must stay trivially copyable for the queue's "
			  "memcpy batch path");

/**
 * @brief Where one listing's slice of a drain's output ends.
 *
 * A drain applies whatever was queued, which is generally commands for several
 * listings, so its @c trades() and @c outcomes() buffers are a concatenation of
 * per-listing slices rather than one listing's stream. This is the cut list: run
 * @c i covers <code>[run[i-1].trade_end, run[i].trade_end)</code> of the trade
 * buffer and the matching half-open range of the outcome buffer, with the first
 * run starting at zero.
 *
 * @par Why end offsets and not (begin, count)
 * Because runs are contiguous and exhaustive by construction, so a begin would
 * be the previous end restated - a second copy of the same number that a bug
 * could make disagree. It is also what makes coalescing free: a second command
 * for the listing already at the back of the list moves two integers instead of
 * appending a record, which is the common case in a partition whose flow is
 * concentrated in a few names.
 *
 * @note Offsets are 32-bit. A single drain producing four billion events would
 *       have exhausted the queue's capacity many times over first.
 */
struct symbol_run {
	symbol_id_t symbol;        ///< the listing that produced this slice
	std::uint32_t trade_end;   ///< one past this slice's last trade
	std::uint32_t outcome_end; ///< one past this slice's last outcome

	bool operator==(const symbol_run &) const noexcept = default;
};

static_assert(std::is_trivially_copyable_v<symbol_run>);

} // namespace exchange::engine::event
