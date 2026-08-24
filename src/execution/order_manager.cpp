#include "order_manager.hpp"

#include "order_book/order_state.hpp"
#include "order_book/reject_reason.hpp"
#include "orders/order.hpp"
#include "orders/types.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <expected>
#include <utility>

namespace exchange::engine::execution {

namespace {

/// @brief The reserved id the engine spends on anonymous liquidity, and which a
///        managed order therefore never carries. @see order_book
constexpr order_id_t ANONYMOUS = 0;

} // namespace

order_record order_manager::vacant() noexcept {
	// The state is a placeholder, not a claim: order_state has no
	// representation for a zero-quantity order, and every field of a vacant
	// record is dead anyway - id == 0 is what says so, and get() checks it
	// before handing the record out.
	return order_record{
		.id        = ANONYMOUS,
		.timestamp = 0,
		.state     = order_state{1},
		.symbol    = 0,
		.account   = 0,
		.price     = 0,
		.side      = side_t::bid,
		.type      = orders::order_type::LIMIT,
		.tif       = orders::time_in_force_instruction::GOOD_TILL_CANCELLED,
		.reason    = reject_reason::NONE,
		.flags     = {}};
}

order_manager::order_manager(std::uint32_t capacity)
	: capacity_(capacity > 0 ? capacity : DEFAULT_CAPACITY) {
	// All three allocations happen here and nowhere else. The slot table is
	// value-initialised rather than merely reserved, so the pages are faulted
	// in now instead of on whichever order first reached an untouched slot.
	slots_.assign(
		capacity_,
		detail::order_slot{.record = vacant(), .generation = 0, .padding = {}});
	retired_.assign(capacity_, 0);
	// Live plus retired can never exceed the slot count, so reserving that many
	// entries is what makes "never rehashes" a guarantee rather than a hope.
	index_.reserve(capacity_);
}

std::expected<order_handle, reject_reason>
order_manager::admit(const orders::order &incoming, account_id_t account) {
	if (incoming.id == ANONYMOUS)
		return std::unexpected(reject_reason::RESERVED_ORDER_ID);

	// The same boundary the book enforces, for the same reason: there is no
	// representable order_state for a non-positive order.
	if (incoming.qty <= 0)
		return std::unexpected(reject_reason::NON_POSITIVE_QUANTITY);

	// Stricter than the book's duplicate check, and deliberately so. The book
	// forgets an order the instant it fills, so it would take the id again and
	// give one client two lifecycles under one name; here the record outlives
	// the order and the id stays spent for as long as it is remembered.
	if (index_.contains(incoming.id))
		return std::unexpected(reject_reason::DUPLICATE_ORDER_ID);

	const std::uint32_t index = acquire_slot();
	if (index == order_handle::NO_SLOT) [[unlikely]]
		return std::unexpected(reject_reason::BOOK_AT_CAPACITY);

	detail::order_slot &taken = slots_[index];
	taken.record              = order_record{.id        = incoming.id,
											 .timestamp = incoming.timestamp,
											 .state     = order_state{incoming.qty},
											 .symbol    = incoming.symbol_id,
											 .account   = account,
											 .price     = incoming.price,
											 .side      = incoming.side,
											 .type      = incoming.type,
											 .tif       = incoming.tif,
											 .reason    = reject_reason::NONE,
											 .flags     = {}};
	index_[incoming.id]       = index;

	++live_;
	high_water_ = std::max(live_, high_water_);
	return order_handle{.slot = index, .generation = taken.generation};
}

void order_manager::apply_fill(order_handle handle, quantity_t lots) noexcept {
	order_record *record = live_record(handle);
	assert(record != nullptr && "apply_fill(): handle names no live order");
	if (record == nullptr) [[unlikely]]
		return;

	// The precondition on lots (positive, no overfill) is order_state's, and it
	// asserts rather than clamps - an overfill here would mean the book and the
	// manager disagree about what executed, which is not a case to paper over.
	record->state.apply_fill(lots);
	if (!record->state.is_active()) retire(handle.slot);
}

void order_manager::cancel(order_handle handle, reject_reason why) noexcept {
	order_record *record = live_record(handle);
	assert(record != nullptr && "cancel(): handle names no live order");
	if (record == nullptr) [[unlikely]]
		return;

	// Freezes the executed quantity rather than discarding it: a cancellation
	// withdraws the remainder, it does not undo the fills.
	record->state.cancel();
	record->reason = why;
	retire(handle.slot);
}

void order_manager::reject(order_handle handle, reject_reason why) noexcept {
	order_record *record = live_record(handle);
	assert(record != nullptr && "reject(): handle names no live order");
	if (record == nullptr) [[unlikely]]
		return;
	assert(record->state.traded() == 0 &&
		   "reject(): an order that executed entered the book - cancel() it");

	record->flags.set(record_flags{record_flag::REJECTED});
	record->reason = why;
	// Terminal in the quantities too, so the record retires by the same rule as
	// every other one. status() reads REJECTED off the bit above rather than
	// out of the quantities, which cannot tell a rejection from a cancel that
	// never filled.
	record->state.cancel();
	retire(handle.slot);
}

std::uint32_t order_manager::resolve(order_handle handle) const noexcept {
	if (!is_valid(handle) || handle.slot >= capacity_)
		return order_handle::NO_SLOT;
	const detail::order_slot &named = slots_[handle.slot];
	// The generation check is the whole point of the handle: a slot recycled
	// since this handle was issued now holds a different client's order, and
	// answering with it would be worse than answering with nothing.
	if (named.generation != handle.generation) return order_handle::NO_SLOT;
	// A slot the bump pointer has not reached yet, reachable only by guessing
	// an index. Id 0 is what says nothing lives here.
	if (named.record.id == ANONYMOUS) return order_handle::NO_SLOT;
	return handle.slot;
}

order_record *order_manager::get(order_handle handle) noexcept {
	// The lookup itself is const and lives in resolve(); both overloads differ
	// only in the constness of the reference they hand back, which is why
	// neither needs a const_cast to share it.
	const std::uint32_t index = resolve(handle);
	if (index == order_handle::NO_SLOT) return nullptr;
	return &slots_[index].record;
}

const order_record *order_manager::get(order_handle handle) const noexcept {
	const std::uint32_t index = resolve(handle);
	if (index == order_handle::NO_SLOT) return nullptr;
	return &slots_[index].record;
}

order_handle order_manager::find(order_id_t id) const noexcept {
	const auto found = index_.find(id);
	if (found == index_.end()) return {};
	return order_handle{.slot       = found->second,
						.generation = slots_[found->second].generation};
}

const order_record *order_manager::find_record(order_id_t id) const noexcept {
	const auto found = index_.find(id);
	if (found == index_.end()) return nullptr;
	return &slots_[found->second].record;
}

bool order_manager::contains(order_id_t id) const noexcept {
	return index_.contains(id);
}

reject_reason order_manager::cancellable(order_id_t id) const noexcept {
	const order_record *record = find_record(id);
	// No record: never placed, or placed so long ago its slot has been
	// recycled. Those two really are indistinguishable, which is what
	// UNKNOWN_ORDER says - and what every terminal order used to get from the
	// book.
	if (record == nullptr) return reject_reason::UNKNOWN_ORDER;

	switch (status(*record)) {
	case OrderStatus::LIVE:
	case OrderStatus::PARTIALLY_FILLED: return reject_reason::NONE;
	case OrderStatus::FILLED: return reject_reason::ORDER_ALREADY_FILLED;
	case OrderStatus::CANCELLED: return reject_reason::ORDER_ALREADY_CANCELLED;
	case OrderStatus::REJECTED: return reject_reason::ORDER_ALREADY_REJECTED;
	case OrderStatus::NEW: break;
	}
	// NEW is not reachable: a record exists only once admit() has built its
	// order_state, and order_state never derives NEW.
	assert(false && "cancellable(): a retained record cannot be NEW");
	return reject_reason::UNKNOWN_ORDER;
}

void order_manager::clear() noexcept {
	// Only the slots that were handed out. The bump pointer never went past
	// next_unused_, so everything beyond it is already vacant at generation 0
	// and writing it would make clearing a barely-used manager cost its whole
	// capacity - 2 MB of stores for five orders, on a call a session boundary
	// makes on the consumer thread.
	//
	// Bump the generation before the slot goes back into circulation, so a
	// handle held across the clear resolves to nullptr rather than to whichever
	// order lands in its slot next.
	for (std::uint32_t index = 0; index < next_unused_; ++index) {
		++slots_[index].generation;
		slots_[index].record = vacant();
	}
	index_.clear();
	next_unused_   = 0;
	retired_head_  = 0;
	retired_count_ = 0;
	live_          = 0;
	// high_water_ and evicted_ survive: they are lifetime capacity-planning
	// readings, and a benchmark that clears between iterations still wants to
	// know the worst case it reached.
}

std::uint32_t order_manager::acquire_slot() noexcept {
	// Untouched slots first, so history is only given up once there is
	// genuinely nowhere else to put a new order.
	if (next_unused_ < capacity_) return next_unused_++;

	// Everything is live: refuse rather than evict. Recycling a live record
	// would leave the book holding an order the venue has no record of.
	if (retired_count_ == 0) [[unlikely]]
		return order_handle::NO_SLOT;

	const std::uint32_t index = retired_[retired_head_];
	retired_head_             = (retired_head_ + 1) % capacity_;
	--retired_count_;

	// It stops answering lookups here, and not a moment earlier - that is the
	// whole retention policy, and the generation bump is what makes any handle
	// still naming it read as stale rather than as its replacement.
	index_.erase(slots_[index].record.id);
	++slots_[index].generation;
	++evicted_;
	return index;
}

void order_manager::retire(std::uint32_t index) noexcept {
	assert(live_ > 0 && "retire(): more records retired than admitted");
	assert(retired_count_ < capacity_ && "retire(): the retired ring is full");
	// live + retired never exceeds the slot count, so the ring sized to
	// capacity cannot overrun and the modulo is the only bound it needs.
	retired_[(retired_head_ + retired_count_) % capacity_] = index;
	++retired_count_;
	--live_;
}

order_record *order_manager::live_record(order_handle handle) noexcept {
	order_record *record = get(handle);
	if (record == nullptr) return nullptr;
	// A retired record is terminal, and a terminal status never changes - so
	// the mutators may not have it. Live and active are the same set here,
	// because a record retires in the same step it stops being active.
	return is_active(*record) ? record : nullptr;
}

} // namespace exchange::engine::execution
