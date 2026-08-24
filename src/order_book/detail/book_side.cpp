#include "book_side.hpp"

#include <cassert>
#include <utility>

namespace exchange::engine::detail {

book_side::book_side(side_t side, order_pool &pool, std::size_t level_capacity)
	: side_(side), pool_(pool), levels_(level_capacity),
	  ordered_(level_price_order{side}) {
	by_price_.reserve(level_capacity);
}

book_side::~book_side() { clear(); }

void book_side::clear() noexcept {
	// Intrusive containers link cells, they do not own them: dropping the
	// ladder without disposing would leak every level and every order on it
	// back to nowhere - the pool blocks go, but the levels' orders were never
	// unlinked, which safe_link hooks assert about on the way down.
	ordered_.clear_and_dispose([this](price_level *level) noexcept {
		level->release_orders(pool_);
		levels_.release(level);
	});
	// Keeps its buckets, so the side is empty without having given up the
	// storage it will want back on the next insert.
	by_price_.clear();
}

bool book_side::empty() const noexcept { return ordered_.empty(); }

std::optional<price_t> book_side::best_price() const {
	if (ordered_.empty()) return std::nullopt;
	return ordered_.begin()->price;
}

price_level &book_side::best() {
	assert(!ordered_.empty() && "best() on an empty side");
	return *ordered_.begin();
}

const price_level &book_side::best() const {
	assert(!ordered_.empty() && "best() on an empty side");
	return *ordered_.begin();
}

price_level *book_side::find(price_t price) {
	const auto found = by_price_.find(price);
	return found != by_price_.end() ? found->second : nullptr;
}

const price_level *book_side::find(price_t price) const {
	const auto found = by_price_.find(price);
	return found != by_price_.end() ? found->second : nullptr;
}

price_level *book_side::level_at(price_t price) {
	if (price_level *existing = find(price); existing != nullptr) return existing;

	// Value-initialised, then priced: a level is an aggregate of scalars and
	// two empty hooks, so there is nothing to build beyond zeroing the cell.
	price_level *level = levels_.acquire();
	if (level == nullptr) [[unlikely]] return nullptr;
	level->price = price;
	ordered_.insert(*level);
	by_price_.emplace(price, level);
	return level;
}

price_level *book_side::insert(const orders::order &incoming) {
	price_level *level = level_at(incoming.price);
	if (level == nullptr) [[unlikely]] return nullptr;
	if (level->add_order(pool_, incoming) == nullptr) [[unlikely]]
		return rewind(*level);
	return level;
}

price_level *book_side::insert(order_id_t id, price_t price,
						 const order_state &state) {
	price_level *level = level_at(price);
	if (level == nullptr) [[unlikely]] return nullptr;
	if (level->add_order(pool_, id, state) == nullptr) [[unlikely]]
		return rewind(*level);
	return level;
}

price_level *book_side::rewind(price_level &level) noexcept {
	// A level created for an order that then could not be rested would be an
	// empty level in the ladder - a price the book quotes with nothing behind
	// it. One that already held orders was not created here, so it stays.
	if (level.has_empty_orders()) destroy(level);
	return nullptr;
}

void book_side::remove_best_level_if_empty() {
	assert(!ordered_.empty() && "remove_best_level_if_empty() on an empty side");
	price_level &top = best();
	if (top.has_empty_orders()) destroy(top);
}

void book_side::erase(price_t price) {
	if (price_level *level = find(price); level != nullptr) destroy(*level);
}

void book_side::destroy(price_level &level) noexcept {
	// s_iterator_to rather than a search by price: the level carries its own
	// tree links, so leaving the ladder is a relink of its neighbours.
	ordered_.erase(ladder::s_iterator_to(level));
	by_price_.erase(level.price);
	// The level holds pool cells, not memory: dropping it without draining its
	// FIFO first would strand every node still on it.
	level.release_orders(pool_);
	levels_.release(&level);
}

volume_t book_side::volume_at_price(price_t price) const {
	const price_level *level = find(price);
	return level != nullptr ? level->total_volume() : 0;
}

ladder::const_iterator book_side::begin() const noexcept {
	return ordered_.begin();
}

ladder::const_iterator book_side::end() const noexcept {
	return ordered_.end();
}

} // namespace exchange::engine::detail
