#include "order_book.hpp"
#include "branchless_binary_search.hpp"

#include <algorithm>
#include <cassert>
#include <functional>

OrderBook::OrderBook(std::size_t capacity) : pool_(capacity) {}

std::vector<Trade> OrderBook::place_order(const Order& order) {
    const auto& [id, side, price, volume, type] = order;

    std::vector<Trade> trades;
    if (volume <= 0) return trades;

    auto& opposite = side_levels(opposed(side));

    // Fill-or-kill: only proceed if the whole order can fill right now.
    if (type == OrderType::FILL_OR_KILL
        && !can_fully_fill(opposite, side, price, volume)) {
        return trades;
    }

    const auto new_volume = match(id, side, price, volume, opposite, trades);

    const bool rest_remainder = new_volume > 0
                                && type != OrderType::IMMEDIATE_OR_CANCEL
                                && type != OrderType::FILL_OR_KILL;
    if (rest_remainder) rest(id, side, price, new_volume);
    return trades;
}

void OrderBook::cancel_order(OrderId id) {
    const auto found = index_.find(id);
    if (found == index_.end()) return;

    const auto [side, price, node] = found->second;
    auto& levels = side_levels(side);
    const auto it = find_level(levels, side, price);
    assert(it != levels.end() && it->price == price);

    const Volume remaining = pool_.get(node).value.volume;
    unlink(*it, node);
    it->total_volume -= remaining;
    if (it->head == kNull) levels.erase(it);
    pool_.deallocate(node);
    index_.erase(found);
}

void OrderBook::add_order(Side side, Price price, Volume volume) {
    if (volume <= 0) return;
    rest(kAnonymous, side, price, volume);
}

void OrderBook::delete_order(Side side, Price price, Volume volume) {
    auto& levels = side_levels(side);
    const auto it = find_level(levels, side, price);
    if (it == levels.end() || it->price != price) return;

    while (volume > 0 && it->head != kNull) {
        auto& [id, current_volume] = pool_.get(it->head).value;
        const Volume take = std::min(volume, current_volume);
        current_volume -= take;
        it->total_volume -= take;
        volume -= take;
        if (current_volume == 0) pop_front(*it);
    }
    if (it->head == kNull) levels.erase(it);
}

void OrderBook::set_level(Side side, Price price, Volume volume) {
    auto& levels = side_levels(side);
    auto it = find_level(levels, side, price);
    const bool exists = it != levels.end() && it->price == price;

    if (volume <= 0) {
        // absolute size 0 (or negative) means "remove this price"
        if (exists) {
            while (it->head != kNull) pop_front(*it);
            levels.erase(it);
        }
        return;
    }

    if (!exists) {
        // new price level: one anonymous node carries the aggregate
        it = levels.emplace(it, Level{price});
        const NodeIndex node = pool_.allocate(kAnonymous, volume);
        it->head = it->tail = node;
        it->total_volume = volume;
        return;
    }

    // Existing level: collapse to the single head node and overwrite its size.
    // Levels touched only via set_level already hold exactly one node, so the
    // trailing-node drain is a no-op fast path; it also repairs a level that
    // was seeded with multiple orders (e.g. add_order) before diffs took over.
    for (NodeIndex n = pool_.get(it->head).next; n != kNull;) {
        const NodeIndex next = pool_.get(n).next;
        pool_.deallocate(n);
        n = next;
    }
    auto& head = pool_.get(it->head);
    head.next = kNull;
    it->tail = it->head;
    head.value.volume = volume;
    it->total_volume = volume;
}

Volume OrderBook::volume_at_price(Price price, Side side) const {
    const auto& levels = side_levels(side);
    const auto it = find_level(levels, side, price);
    return it != levels.end() && it->price == price ? it->total_volume : 0;
}

std::optional<Price> OrderBook::best_bid() const {
    if (bid_levels_.empty()) return std::nullopt;
    return bid_levels_.front().price;
}

std::optional<Price> OrderBook::best_ask() const {
    if (ask_levels_.empty()) return std::nullopt;
    return ask_levels_.front().price;
}

bool OrderBook::crosses(Side side, Price price, Price book_price) {
    return side == Side::BID ? price >= book_price : price <= book_price;
}

std::vector<OrderBook::Level>& OrderBook::side_levels(Side s) {
    return s == Side::BID ? bid_levels_ : ask_levels_;
}

const std::vector<OrderBook::Level>& OrderBook::side_levels(Side s) const {
    return s == Side::BID ? bid_levels_ : ask_levels_;
}

std::vector<OrderBook::Level>::const_iterator
OrderBook::find_level(const std::vector<Level>& levels, Side side,
                      Price price) {
    return side == Side::BID
               ? branchless_lower_bound(levels, price, std::greater<Price>{},
                                        &Level::price)
               : branchless_lower_bound(levels, price, std::less<Price>{},
                                        &Level::price);
}

// Non-const overload: run the const search, then lift the result to a mutable
// iterator by offset (same container, so the index is identical).
std::vector<OrderBook::Level>::iterator
OrderBook::find_level(std::vector<Level>& levels, Side side, Price price) {
    const std::vector<Level>& clevels = levels;
    const auto cit = find_level(clevels, side, price);
    return levels.begin() + (cit - clevels.begin());
}

Volume OrderBook::match(OrderId id, Side side, Price price, Volume volume,
                        std::vector<Level>& opposite,
                        std::vector<Trade>& trades) {
    while (volume > 0 && !opposite.empty()
           && crosses(side, price, opposite.front().price)) {
        Level& lvl = opposite.front();

        while (volume > 0 && lvl.head != kNull) {
            RestingOrder& r = pool_.get(lvl.head).value;
            const Volume fill = std::min(volume, r.volume);

            trades.emplace_back(id, r.id, lvl.price, fill);
            volume -= fill;
            r.volume -= fill;
            lvl.total_volume -= fill;

            if (r.volume == 0) pop_front(lvl);
        }
        if (lvl.head == kNull) opposite.erase(opposite.begin());
    }
    return volume;
}

void OrderBook::rest(OrderId id, Side side, Price price, Volume volume) {
    auto& levels = side_levels(side);
    auto it = find_level(levels, side, price);
    if (it == levels.end() || it->price != price)
        it = levels.emplace(it, Level{price});

    const NodeIndex node = pool_.allocate(id, volume);
    if (it->tail == kNull) {
        it->head = it->tail = node;
    } else {
        pool_.get(it->tail).next = node;
        pool_.get(node).prev = it->tail;
        it->tail = node;
    }
    it->total_volume += volume;
    if (id != kAnonymous) index_[id] = Location{side, price, node};
}

void OrderBook::pop_front(Level& lvl) {
    const NodeIndex node = lvl.head;
    const RestingOrder& r = pool_.get(node).value;
    if (r.id != kAnonymous) index_.erase(r.id);

    lvl.head = pool_.get(node).next;
    if (lvl.head != kNull) pool_.get(lvl.head).prev = kNull;
    else lvl.tail = kNull;
    pool_.deallocate(node);
}

void OrderBook::unlink(Level& lvl, NodeIndex node) {
    const NodeIndex prev = pool_.get(node).prev;
    const NodeIndex next = pool_.get(node).next;
    if (prev != kNull) pool_.get(prev).next = next;
    else lvl.head = next;
    if (next != kNull) pool_.get(next).prev = prev;
    else lvl.tail = prev;
}

bool OrderBook::can_fully_fill(const std::vector<Level>& opposite, Side side,
                               Price price, Volume volume) const {
    Volume available = 0;
    for (const Level& lvl : opposite) {
        if (!crosses(side, price, lvl.price)) break;
        available += lvl.total_volume;
        if (available >= volume) return true;
    }
    return false;
}
