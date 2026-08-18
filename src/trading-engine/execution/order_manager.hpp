#pragma once
// Order records: who placed what, and what became of it.
//
// The book knows about *resting* orders and nothing else - an order that fills
// or is cancelled leaves it entirely, taking its history with it. That is the
// right shape for a matching structure and the wrong one for a venue, which has
// to answer "what happened to order 42" after the fact, refuse an id a client
// has already used, and tell a late cancel that its order filled rather than
// that it never existed. This is where those answers live.
//
// Above the book, never inside it: nothing here is on the fill loop, and the
// book links no pointer into these records.

#include "trading_engine_export.hpp" // TRADING_ENGINE_EXPORT (generated)
#include "core/util/flag.hpp"
#include "fwd.hpp"
#include "record_flag.hpp" // IWYU pragma: export
#include "trading-engine/order_book/order_state.hpp"
#include "trading-engine/order_book/reject_reason.hpp"
#include "trading-engine/orders/order.hpp"
#include "trading-engine/orders/order_type.hpp"
#include "trading-engine/orders/time_in_force_instruction.hpp"
#include "trading-engine/orders/types.hpp"

#include <boost/unordered/unordered_flat_map.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <new>
#include <type_traits>
#include <vector>

namespace exchange::engine::execution {

/**
 * @brief A generation-tagged reference to one order's record.
 *
 * A bare slot index would be an ABA bug waiting to happen: slots are recycled,
 * so an index held across a recycle would silently name a different client's
 * order. The generation counter is what turns that from a wrong answer into a
 * @c nullptr - @c order_manager::get compares it and refuses a stale handle.
 *
 * Eight bytes and trivially copyable, so it costs the same as the pointer it
 * replaces and can be stored in anything a @c command can.
 */
struct order_handle {
	/// @brief The slot value no live record ever has.
	static constexpr std::uint32_t NO_SLOT = ~std::uint32_t{0};

	std::uint32_t slot       = NO_SLOT; ///< index into the record table
	std::uint32_t generation = 0;       ///< how many times that slot was reused

	/// @brief Whether this names a slot at all. Says nothing about staleness -
	///        only @c order_manager::get can decide that.
	[[nodiscard]] constexpr bool valid() const noexcept {
		return slot != NO_SLOT;
	}

	constexpr bool operator==(const order_handle &) const noexcept = default;
};

static_assert(std::is_trivially_copyable_v<order_handle>);

/**
 * @brief Everything the venue knows about one order, for as long as it keeps
 * it.
 *
 * The static half (@c symbol, @c account, @c price, @c side, @c type, @c tif)
 * is copied from the validated @c orders::order once and never changes. The
 * moving half is @c state, which is the same @c order_state the book runs on a
 * resting node - the same type, so the quantity invariants hold here for the
 * same reasons and there is no second state machine to keep in step.
 *
 * @par Why status is not a field
 * @c order_state derives its status from the quantities precisely so no
 * representable state can disagree with them, and storing one here would throw
 * that away. The one status the quantities genuinely cannot express is
 * REJECTED - an order that never entered the book has traded 0 and a withdrawn
 * remainder, which is indistinguishable from a cancel that never filled. That
 * is what the single @c rejected bit is for, and it is the same trick
 * @c order_state plays with its own cancellation bit.
 */
struct order_record {
	order_id_t id;           ///< client's identifier; 0 marks a vacant slot
	std::uint64_t timestamp; ///< venue receipt time, ns since the Unix epoch
	order_state state;       ///< quantity/status machine, shared with the book
	symbol_id_t symbol;      ///< the listing this order is for
	account_id_t account;    ///< the participant who placed it
	price_t price;           ///< limit price in ticks
	side_t side;             ///< which side of the book it joined
	orders::order_type type; ///< LIMIT / MARKET / STOP
	orders::time_in_force_instruction tif; ///< GTC / IOC / FOK / AON
	/// @brief Why the order ended. NONE while it is still live, and NONE for an
	///        ordinary client cancel - a cancel needs no excuse.
	reject_reason reason;
	/// @brief Venue-level facts the quantities cannot carry. @see record_flag
	record_flags flags;

	/// @brief Where the order sits in its lifecycle. @see the class note.
	[[nodiscard]] OrderStatus status() const noexcept {
		return flags.test(record_flag::REJECTED) ? OrderStatus::REJECTED
												 : state.status();
	}

	/// @brief Can still fill or be cancelled.
	[[nodiscard]] bool is_active() const noexcept {
		return flags.none_of(record_flag::REJECTED) && state.is_active();
	}

	bool operator==(const order_record &) const noexcept = default;
};

static_assert(std::is_trivially_copyable_v<order_record>,
			  "records are copied out to clients and journalled by value");
static_assert(sizeof(order_record) == 48,
			  "an order record is the unit the slot table is sized in - see "
			  "order_manager's capacity note");

/// @brief How @c order_manager stores a record, which is its own business.
///
/// A namespace rather than a `detail/` header of its own, unlike the rest of
/// this tree's private types: a slot *contains* an @c order_record, and that
/// record is defined above in this same header. A subfolder header would have
/// to include this one to see it, and this one would have to include that to
/// declare the table - so the type stays here and the namespace does the
/// saying.
namespace detail {

/// @brief The stride a slot is padded to. Constructive, not destructive: the
///        question here is "does one record fit on one line", not "do two
///        writers share one" - the manager has a single owner and no false
///        sharing to avoid.
inline constexpr std::size_t SLOT_STRIDE =
	std::hardware_constructive_interference_size;

/**
 * @brief One table entry: the record, the counter that dates it, and the
 *        padding that keeps the two on one cache line.
 *
 * The padding is bought deliberately, and spelled out rather than left to
 * @c alignas so it is visible in the layout and costs no C4324. Resolving a
 * handle is a random access into a table far larger than L1, so what a lookup
 * pays is the miss - and a 52-byte stride would put one entry in eight across
 * two lines and make that miss two. Giving up 19% of the table to make every
 * lookup exactly one line is the right side of that trade for a structure whose
 * only hot operation *is* the lookup.
 */
struct alignas(SLOT_STRIDE) order_slot {
	order_record record;
	/// @brief Bumped every time this slot is recycled, so a handle issued
	///        before the recycle no longer matches.
	std::uint32_t generation;
	/// @brief Explicit filler to the stride. Sized from the members above, so a
	///        field added to @c order_record takes its cost out of here and
	///        trips the assertion below rather than silently doubling the
	///        table's line footprint.
	std::array<std::byte,
			   SLOT_STRIDE - sizeof(order_record) - sizeof(std::uint32_t)>
		padding;
};

static_assert(sizeof(order_slot) == SLOT_STRIDE,
			  "a slot must be exactly one cache line - see the padding note");

} // namespace detail

/**
 * @brief Fixed-capacity store of order records, drawn from storage taken up
 *        front.
 *
 * @par The three populations
 * ```
 * [ unused ]   never handed out; a bump pointer walks forward through them
 * [ live   ]   an active order - the book may still fill or cancel it
 * [ retired]   terminal, but still answering lookups; a FIFO of slot indices
 * ```
 * A slot is taken from the unused run while one is left, and only then from the
 * head of the retired FIFO - oldest terminal record first. That ordering is the
 * whole retention policy: history is kept for as long as there is room for it
 * and given up in the order it stopped mattering.
 *
 * @par Why a live order is never evicted
 * Recycling a live record would leave the book holding an order the venue has
 * no record of, which is the one state this exists to rule out. So @c admit
 * refuses with @c BOOK_AT_CAPACITY when every slot is live, and the refusal
 * reaches the client rather than corrupting the store. Capacity is therefore a
 * bound on *simultaneously live* orders first and a history depth second.
 *
 * @par Allocation
 * Everything is taken in the constructor: the slot table, the retired ring, and
 * the id index reserved to capacity. The index never holds more than @c
 * capacity entries - live plus retired is exactly the number of slots - so it
 * never rehashes and never allocates again. Nothing here calls the allocator
 * after construction.
 *
 * @par Why the index is a hash map and not an array
 * @c order_id_t is assigned outside the engine and carries no density contract,
 * unlike @c symbol_id_t. There is no array to index with it. What the pool buys
 * is that the *records* are dense and pre-allocated even though their keys are
 * not, so the map holds a 4-byte slot index rather than a 48-byte record and a
 * probe touches one line.
 *
 * @warning Not thread-safe, deliberately. One partition, one thread, one
 *          manager - the same single-owner rule the books rest on.
 */
class order_manager {
public:
	/// @brief Records taken when no capacity is named. Matches the book's own
	///        default resting-order hint, so a manager sized alongside a book
	///        can hold every order that book can rest and nothing is left over
	///        for history.
	static constexpr std::uint32_t DEFAULT_CAPACITY = 1U << 15;

	/**
	 * @brief Take @p capacity records now.
	 * @param capacity Simultaneously live orders, plus however much terminal
	 *        history fits in what is left. Sized to worst-case live orders; use
	 *        @c high_water() to find out whether you sized it right.
	 */
	TRADING_ENGINE_EXPORT explicit order_manager(
		std::uint32_t capacity = DEFAULT_CAPACITY);

	// Non-copyable, non-movable: handles name slots in *this* table.
	order_manager(const order_manager &)            = delete;
	order_manager &operator=(const order_manager &) = delete;
	order_manager(order_manager &&)                 = delete;
	order_manager &operator=(order_manager &&)      = delete;
	~order_manager()                                = default;

	/**
	 * @brief Take a record for @p incoming, or say why it cannot be taken.
	 *
	 * Runs before the book sees the order, and its refusals are the ones the
	 * book cannot make on its own:
	 * - @c RESERVED_ORDER_ID - id 0 is the anonymous sentinel. Anonymous
	 *   liquidity belongs to nobody and is not managed here; it goes straight
	 * to
	 *   @c order_book::add_order.
	 * - @c NON_POSITIVE_QUANTITY - there is no representable @c order_state for
	 *   one, the same boundary the book enforces.
	 * - @c DUPLICATE_ORDER_ID - an id still resting *or still remembered*. This
	 *   is the stricter half: the book forgets an order the moment it fills, so
	 *   it would accept the id again and hand the client two lifecycles under
	 *   one name.
	 * - @c BOOK_AT_CAPACITY - every slot holds a live order.
	 *
	 * @param incoming A validated order, already on the engine's integer grid.
	 * @param account The participant placing it.
	 * @return A handle to the new record, LIVE with nothing executed.
	 */
	[[nodiscard]] TRADING_ENGINE_EXPORT
		std::expected<order_handle, reject_reason>
		admit(const orders::order &incoming, account_id_t account = 0);

	/**
	 * @brief Execute @p lots against the order @p h names.
	 *
	 * Retires the record when the fill completes the order - it stops being
	 * live and starts being history in the same step, which is what keeps @c
	 * live() equal to the number of orders the book could still act on.
	 *
	 * @pre @p h is live and @c 0 < lots <= remaining. An overfill is a caller
	 *      bug: @c order_state refuses it rather than clamping.
	 */
	TRADING_ENGINE_EXPORT void apply_fill(order_handle h,
										  quantity_t lots) noexcept;

	/**
	 * @brief Withdraw the unexecuted remainder of @p h, and retire it.
	 * @param why NONE for a client cancel - a cancel needs no excuse - or the
	 *        cause when the engine withdrew it on the client's behalf, e.g.
	 *        @c TIME_IN_FORCE for a dropped IOC remainder.
	 * @pre @p h is live.
	 */
	TRADING_ENGINE_EXPORT void
	cancel(order_handle h, reject_reason why = reject_reason::NONE) noexcept;

	/**
	 * @brief Record that @p h never entered the book, and retire it.
	 *
	 * Distinct from @c cancel because the two are different facts about the
	 * order and a client acts on them differently: a rejection means nothing
	 * happened and the order may be re-sent, a cancellation means it was live
	 * and may have executed first.
	 *
	 * @pre @p h is live and has executed nothing. An order that traded cannot
	 * be rejected - it entered the book by definition.
	 */
	TRADING_ENGINE_EXPORT void reject(order_handle h,
									  reject_reason why) noexcept;

	/// @brief The record @p h names, or @c nullptr if the handle is stale or
	///        null. Live and retired records both answer; only recycling ends
	///        it.
	[[nodiscard]] TRADING_ENGINE_EXPORT order_record *
	get(order_handle h) noexcept;

	/// @brief @copydoc get(order_handle)
	[[nodiscard]] TRADING_ENGINE_EXPORT const order_record *
	get(order_handle h) const noexcept;

	/// @brief A handle for @p id, or a null handle if no record is retained.
	[[nodiscard]] TRADING_ENGINE_EXPORT order_handle
	find(order_id_t id) const noexcept;

	/// @brief The record for @p id, or @c nullptr. @see find
	[[nodiscard]] TRADING_ENGINE_EXPORT const order_record *
	find_record(order_id_t id) const noexcept;

	/// @brief Whether a record for @p id is retained, live or terminal.
	[[nodiscard]] TRADING_ENGINE_EXPORT bool
	contains(order_id_t id) const noexcept;

	/**
	 * @brief Whether a cancel naming @p id can be applied, and if not, why.
	 *
	 * The answer the book cannot give. @c order_book::cancel_order probes an
	 * index that holds only resting orders, so "filled a microsecond ago",
	 * "already cancelled" and "never placed" all come back as the same empty
	 * probe and the same @c UNKNOWN_ORDER. Here they are three different
	 * records, and a client chasing a fill it did not receive is told which one
	 * it is.
	 *
	 * @return @c NONE when @p id is live and cancellable; otherwise
	 *         @c ORDER_ALREADY_FILLED, @c ORDER_ALREADY_CANCELLED,
	 *         @c ORDER_ALREADY_REJECTED, or @c UNKNOWN_ORDER when no record is
	 *         retained - which now means genuinely unknown *or* aged out of
	 *         history, and those two really are indistinguishable.
	 */
	[[nodiscard]] TRADING_ENGINE_EXPORT reject_reason
	cancellable(order_id_t id) const noexcept;

	/// @brief Records the table holds. Fixed for the manager's life.
	[[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }

	/// @brief Orders that can still fill or be cancelled.
	[[nodiscard]] std::uint32_t live() const noexcept { return live_; }

	/// @brief Terminal records still answering lookups.
	[[nodiscard]] std::uint32_t retained() const noexcept {
		return retired_count_;
	}

	/// @brief Records resolvable by id - live plus retained.
	[[nodiscard]] std::uint32_t size() const noexcept {
		return live_ + retired_count_;
	}

	/**
	 * @brief The most orders ever live at once.
	 *
	 * The capacity-planning number, and the only one that survives the book
	 * emptying. A manager sized right reports a high water below @c capacity();
	 * one at capacity has refused orders, and the reading says by how much to
	 * raise the hint.
	 */
	[[nodiscard]] std::uint32_t high_water() const noexcept {
		return high_water_;
	}

	/// @brief Terminal records dropped to make room for a new order. Nonzero
	///        means history is shorter than the window a client might ask
	///        about.
	[[nodiscard]] std::uint64_t evicted() const noexcept { return evicted_; }

	/**
	 * @brief Forget every order, keeping the storage.
	 *
	 * @warning Not a mass cancel: no outcome is emitted and a client with a
	 * live order learns nothing. This is a session boundary or a replay reset,
	 * where there is nobody to report to - the same contract as
	 *          @c order_book::clear, and the two are cleared together or not at
	 *          all.
	 */
	TRADING_ENGINE_EXPORT void clear() noexcept;

private:
	/// @brief A record for a slot that holds no order. Id 0 is what marks it -
	///        the anonymous sentinel is never a client's id, so it costs no
	///        representable state to spend it here.
	[[nodiscard]] static order_record vacant() noexcept;

	/// @brief Take a slot for a new order: unused run first, then the oldest
	///        retired record. @c order_handle::NO_SLOT when every slot is live.
	[[nodiscard]] std::uint32_t acquire_slot() noexcept;

	/// @brief The slot @p h names, or @c order_handle::NO_SLOT if it is null,
	///        out of range, stale, or points at a slot never handed out. The
	///        shared body of both @c get overloads - the lookup is const either
	///        way, and only the reference handed back is not.
	[[nodiscard]] std::uint32_t resolve(order_handle h) const noexcept;

	/// @brief Move a slot from live to retired. Its record stays resolvable
	///        until @c acquire_slot comes back around to it.
	void retire(std::uint32_t index) noexcept;

	/// @brief The live record @p h names, or @c nullptr. Used by the mutators,
	///        which may not touch a retired record - a terminal status never
	///        changes.
	[[nodiscard]] order_record *live_record(order_handle h) noexcept;

	std::vector<detail::order_slot> slots_; ///< the table, taken at construction
	std::vector<std::uint32_t> retired_; ///< ring of terminal slot indices
	boost::unordered_flat_map<order_id_t, std::uint32_t> index_; ///< id -> slot

	std::uint32_t capacity_;
	std::uint32_t next_unused_   = 0; ///< bump pointer into the unused run
	std::uint32_t retired_head_  = 0; ///< oldest entry in the retired ring
	std::uint32_t retired_count_ = 0; ///< entries in the retired ring
	std::uint32_t live_          = 0;
	std::uint32_t high_water_    = 0;
	std::uint64_t evicted_       = 0;
};

} // namespace exchange::engine::execution
