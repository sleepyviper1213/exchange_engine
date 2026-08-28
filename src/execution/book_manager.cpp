#include "book_manager.hpp"

#include "order_book/order_book.hpp"

#include <cassert>


namespace exchange::engine::execution {

book_manager::book_manager(std::size_t default_book_capacity,
						   allocation_policy policy) noexcept
	: default_book_capacity_(default_book_capacity), default_policy_(policy) {}

order_book &book_manager::create(symbol_id_t symbol, std::size_t capacity,
								 allocation_policy policy) {
	const auto slot = static_cast<std::size_t>(symbol);
	// Grow to fit rather than reserve up front: a partition learns its listings
	// from reference data at startup, so this runs a handful of times and never
	// on the command path.
	if (slot >= books_.size()) books_.resize(slot + 1);

	std::unique_ptr<order_book> &held = books_[slot];
	// Idempotent: replacing a live book here would drop every order resting on
	// it with nothing emitted to say so.
	if (held == nullptr) {
		held = std::make_unique<order_book>(capacity, policy);
		++live_;
	}
	return *held;
}

order_book &book_manager::create(symbol_id_t symbol, std::size_t capacity) {
	return create(symbol, capacity, default_policy_);
}

order_book &book_manager::create(symbol_id_t symbol) {
	return create(symbol, default_book_capacity_, default_policy_);
}

order_book *book_manager::lookup(symbol_id_t symbol) noexcept {
	const auto slot = static_cast<std::size_t>(symbol);
	if (slot >= books_.size()) return nullptr;
	return books_[slot].get();
}

const order_book *book_manager::lookup(symbol_id_t symbol) const noexcept {
	const auto slot = static_cast<std::size_t>(symbol);
	if (slot >= books_.size()) return nullptr;
	return books_[slot].get();
}

bool book_manager::contains(symbol_id_t symbol) const noexcept {
	return lookup(symbol) != nullptr;
}

bool book_manager::remove(symbol_id_t symbol) noexcept {
	const auto slot = static_cast<std::size_t>(symbol);
	if (slot >= books_.size() || books_[slot] == nullptr) return false;
	books_[slot].reset();
	assert(live_ > 0 &&
		   "a slot was occupied, so the live count cannot be zero");
	--live_;
	// The slot itself stays. Shrinking would renumber nothing - the index *is*
	// the symbol id - so a vector that only ever grows to the highest live id
	// is already the smallest one that answers lookup in a single load.
	return true;
}

std::size_t book_manager::size() const noexcept { return live_; }

bool book_manager::empty() const noexcept { return live_ == 0; }

void book_manager::clear() noexcept {
	for (std::unique_ptr<order_book> &held : books_) held.reset();
	live_ = 0;
}

void book_manager::for_each_listing(
	core::util::function_ref<void(symbol_id_t, order_book &) const> visit) {
	for (std::size_t symbol = 0; symbol < books_.size(); ++symbol)
		if (books_[symbol] != nullptr)
			visit(static_cast<symbol_id_t>(symbol), *books_[symbol]);
}

void book_manager::for_each_listing(
	core::util::function_ref<void(symbol_id_t, const order_book &) const> visit)
	const {
	for (std::size_t symbol = 0; symbol < books_.size(); ++symbol)
		if (books_[symbol] != nullptr)
			visit(static_cast<symbol_id_t>(symbol), *books_[symbol]);
}

} // namespace exchange::engine::execution
