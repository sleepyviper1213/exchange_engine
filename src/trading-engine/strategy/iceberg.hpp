#pragma once
// Iceberg: show a slice, replenish it when it is taken, keep the rest hidden.

#include "command_writer.hpp"
#include "fwd.hpp"
#include "trading-engine/order_book/order_state.hpp"
#include "trading-engine/order_book/outcome.hpp"
#include "trading-engine/orders/order.hpp"
#include "trading-engine/orders/types.hpp"

#include <array>
#include <cstddef>
#include <optional>

namespace exchange::engine::strategy {

/**
 * @brief Works a large order as a series of small visible slices.
 *
 * The parent quantity never reaches the book. One slice of @c peak lots rests at
 * a time under an id this strategy assigns; when the book reports that slice
 * FILLED, the next is placed, until the reserve runs out.
 *
 * @tparam MaxWorking Parent orders this instance can work at once. Slots are an
 *         inline array — no allocation, and a lookup is a linear scan over
 *         40-byte cells, which beats a hash probe at any count a single
 *         participant plausibly works.
 *
 * @par Why this is a strategy and not a book feature
 * A book that hid quantity would have to keep the hidden part in the FIFO in
 * order to refill it in place, and then it is not hidden, it is merely
 * unprinted. Doing it out here keeps the book an order-by-order matcher with
 * nothing special in it, and makes the replenishment a genuine new order —
 * which is what makes the price the strategy pays visible rather than pretend.
 *
 * @par The price it pays, stated plainly
 * A replenished slice joins the **back** of the queue at its price. That is not
 * an artefact of doing it here — it is what an iceberg costs on any venue that
 * does not grant hidden quantity priority, and it is why an iceberg is a
 * concession to size rather than a free lunch. Between the fill and the
 * replenishment landing, the strategy shows nothing at all: @c submit is
 * asynchronous, so the gap is a queue round-trip wide and cannot be closed from
 * this side.
 *
 * @par Child ids
 * Assigned from a counter seeded at construction, so a given seed and a given
 * event stream always produce the same ids — the determinism replay depends on.
 * The seed comes from the caller's id space and must not collide with ids it
 * uses elsewhere; a strategy has no more claim on that space than any other
 * client.
 *
 * @note One outcome names one order, so at most one slot can react to it and at
 *       most one replenishment can follow. Hence a bound of 1.
 */
template <std::size_t MaxWorking = 8>
class iceberg {
public:
	static constexpr std::size_t MAX_COMMANDS_PER_EVENT = 1;
	static constexpr std::size_t MAX_WORKING            = MaxWorking;

	/// @brief What a caller can see of a working parent. Observability, not a
	///        handle — the strategy owns the state and mutates it on outcomes.
	struct parent_view {
		order_id_t child;   ///< id of the slice currently resting
		price_t price;      ///< the price every slice rests at
		quantity_t peak;    ///< the most that is ever shown at once
		quantity_t showing; ///< the resting slice's size
		quantity_t reserve; ///< quantity still hidden, not yet shown
		side_t side;
	};

	/// @brief Seed the child-id counter. @see the class note on ids.
	explicit constexpr iceberg(order_id_t child_id_seed) noexcept
		: next_child_(child_id_seed) {}

	/**
	 * @brief Start working @p total lots at @p price, showing @p peak at a time.
	 *
	 * Emits the first slice immediately — an iceberg that showed nothing until
	 * something else happened would never start.
	 *
	 * @param parent_id The caller's handle for this iceberg. Never sent to the
	 *        book; the slices carry generated ids instead.
	 * @param side Which side the slices join.
	 * @param price Limit price, in ticks, shared by every slice.
	 * @param total Full quantity, in lots. Must be positive.
	 * @param peak Visible slice size, in lots. Must be positive; a peak at or
	 *        above @p total is just a plain order and is accepted as one.
	 * @param out Where the first slice is written. Needs one free slot.
	 * @return @c false if @p parent_id is zero or already working, the
	 *         quantities are not positive, or every slot is taken. Nothing is
	 *         emitted and nothing is recorded in that case.
	 */
	bool arm(order_id_t parent_id, side_t side, price_t price, quantity_t total,
			 quantity_t peak, command_writer &out) noexcept {
		if (parent_id == 0 || total <= 0 || peak <= 0) return false;
		if (find_by_parent(parent_id) != nullptr) return false;

		working_parent *slot = free_slot();
		if (slot == nullptr) return false;

		*slot = working_parent{.parent  = parent_id,
							   .child   = 0,
							   .price   = price,
							   .peak    = peak,
							   .showing = 0,
							   .reserve = total,
							   .side    = side,
							   .active  = true};
		++working_;
		show_next(*slot, out);
		return true;
	}

	/**
	 * @brief Stop working @p parent_id and withdraw whatever it is showing.
	 *
	 * @return @c false if no such parent is working. On @c true a CANCEL for the
	 *         resting slice has been written and the slot released — the hidden
	 *         reserve is simply forgotten, since none of it ever reached a book.
	 * @note The cancel can still lose its race with a fill, in which case the
	 *       book answers CANCEL_REJECTED for a slice this strategy has already
	 *       forgotten. Ignoring that outcome is correct: the slot is gone, so no
	 *       replenishment follows, which is what stopping means.
	 */
	bool cancel(order_id_t parent_id, command_writer &out) noexcept {
		working_parent *slot = find_by_parent(parent_id);
		if (slot == nullptr) return false;
		const order_id_t child = slot->child;
		retire(*slot);
		out.cancel(child);
		return true;
	}

	/**
	 * @brief React to the book's verdict on a slice.
	 *
	 * Only a slice that filled *completely* replenishes: a partial fill leaves
	 * the slice resting with quantity still in front of the market, and showing
	 * more then would be showing more than the peak.
	 */
	void on_outcome(const OrderOutcome &o, command_writer &out) noexcept {
		working_parent *slot = find_by_child(o.id);
		if (slot == nullptr) return;

		switch (o.type) {
		case OutcomeType::FILL:
			if (o.status == OrderStatus::FILLED) show_next(*slot, out);
			return;
		case OutcomeType::CANCELLED:
		case OutcomeType::REJECTED:
			// The slice will not fill and cannot be waited on. Replenishing
			// over a rejection would spin: whatever refused this slice — a
			// duplicate id, a book at capacity — refuses the next one too.
			retire(*slot);
			return;
		case OutcomeType::ACCEPTED:
		case OutcomeType::CANCEL_REJECTED:
			return;
		}
	}

	/// @brief Parents currently being worked.
	[[nodiscard]] constexpr std::size_t working() const noexcept {
		return working_;
	}

	/// @brief The state of @p parent_id, or nothing if it is not being worked.
	[[nodiscard]] std::optional<parent_view>
	parent(order_id_t parent_id) const noexcept {
		const working_parent *slot = find_by_parent(parent_id);
		if (slot == nullptr) return std::nullopt;
		return parent_view{.child   = slot->child,
						   .price   = slot->price,
						   .peak    = slot->peak,
						   .showing = slot->showing,
						   .reserve = slot->reserve,
						   .side    = slot->side};
	}

	/// @brief The id of the slice @p parent_id currently has resting.
	[[nodiscard]] std::optional<order_id_t>
	showing(order_id_t parent_id) const noexcept {
		const working_parent *slot = find_by_parent(parent_id);
		if (slot == nullptr) return std::nullopt;
		return slot->child;
	}

private:
	/// @note 40 bytes, so a full scan of the default eight slots touches five
	///       cache lines and no pointer.
	struct working_parent {
		order_id_t parent;
		order_id_t child;
		price_t price;
		quantity_t peak;
		quantity_t showing;
		quantity_t reserve;
		side_t side;
		bool active;
	};

	/// @brief Place the next slice, or retire the parent if the reserve is out.
	void show_next(working_parent &slot, command_writer &out) noexcept {
		if (slot.reserve <= 0) {
			retire(slot);
			return;
		}
		const quantity_t slice =
			slot.reserve < slot.peak ? slot.reserve : slot.peak;
		slot.reserve -= slice;
		slot.showing = slice;
		slot.child   = next_child_++;
		out.place(orders::order{.id    = slot.child,
								.side  = slot.side,
								.price = slot.price,
								.qty   = slice});
	}

	void retire(working_parent &slot) noexcept {
		slot.active  = false;
		slot.showing = 0;
		slot.reserve = 0;
		--working_;
	}

	[[nodiscard]] working_parent *free_slot() noexcept {
		for (working_parent &slot : slots_)
			if (!slot.active) return &slot;
		return nullptr;
	}

	[[nodiscard]] working_parent *find_by_parent(order_id_t id) noexcept {
		for (working_parent &slot : slots_)
			if (slot.active && slot.parent == id) return &slot;
		return nullptr;
	}

	[[nodiscard]] const working_parent *
	find_by_parent(order_id_t id) const noexcept {
		for (const working_parent &slot : slots_)
			if (slot.active && slot.parent == id) return &slot;
		return nullptr;
	}

	[[nodiscard]] working_parent *find_by_child(order_id_t id) noexcept {
		// Id zero is order_book's anonymous sentinel and produces no outcomes,
		// so it can only name a slot that has not placed anything yet.
		if (id == 0) return nullptr;
		for (working_parent &slot : slots_)
			if (slot.active && slot.child == id) return &slot;
		return nullptr;
	}

	std::array<working_parent, MaxWorking> slots_{};
	order_id_t next_child_;
	std::size_t working_ = 0;
};

} // namespace exchange::engine::strategy
