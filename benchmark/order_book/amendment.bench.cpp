#include "orders/amendment.hpp"

#include "order_book/order_book.hpp"
#include "order_book/outcome.hpp"
#include "order_book/trade.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <vector>

// What an amendment costs, against the cancel-and-replace it exists to be
// cheaper than.
//
// The claim being measured is not "modify is fast" - it is that the three
// shapes of amendment cost three different things, and that the cheapest of
// them is the one a quoter sends most. A downsize at the same price touches one
// node and one aggregate; an increase at the same price adds an unlink and a
// relink on the level's FIFO; a reprice pays a level lookup, a pool release, a
// crossing walk and a pool acquire - which is a cancel and a place with the
// index work done once instead of twice, and BM_CancelAndReplace is the
// baseline that says by how much.
//
// Every case drives the same resting order in the same book, so the difference
// between the numbers is the amendment shape and nothing else.

using exchange::order_id_t;
using exchange::price_t;
using exchange::quantity_t;
using exchange::side_t;
using exchange::engine::order_book;
using exchange::engine::order_outcome;
using exchange::engine::trade;
using exchange::engine::orders::amendment;
using exchange::engine::orders::order;

namespace {

/// @brief Lots the amended order starts at. Well clear of zero, so a downsize
///        never reaches the traded quantity and turns into a cancel, and well
///        clear of @c quantity_t's maximum, so the increase case can climb for
///        as many iterations as Google Benchmark cares to run.
constexpr quantity_t AMEND_BENCH_LOTS = 1'000'000;

/// @brief Orders resting at each price these cases touch.
///
/// Not one: a level that held only the amended order would be erased and
/// recreated on every reprice, and the number would then be measuring ladder
/// churn as much as the amendment. Depth also gives the increase case a real
/// FIFO to be sent to the back of.
constexpr std::size_t AMEND_BENCH_DEPTH = 64;

constexpr price_t AMEND_BENCH_PRICE = 100'000;
constexpr price_t AMEND_BENCH_NEAR  = AMEND_BENCH_PRICE - 1;
constexpr price_t AMEND_BENCH_FAR   = AMEND_BENCH_PRICE - 2;

constexpr order_id_t AMEND_BENCH_ID = 1;

/// @brief A bid side with depth at the three prices these cases move between,
///        and @c AMEND_BENCH_ID at the head of the queue at @c
///        AMEND_BENCH_PRICE.
///
/// Built once per benchmark rather than per iteration: constructing a book
/// takes the order pool's block and both ladders' level cells up front, which
/// is orders of magnitude more than the operation being timed. @see
/// order_book.bench.cpp, which makes the same argument at length.
void amend_bench_seed(order_book &book) {
	std::vector<trade> trades;
	order_id_t next = AMEND_BENCH_ID;
	for (const price_t price :
		 {AMEND_BENCH_PRICE, AMEND_BENCH_NEAR, AMEND_BENCH_FAR})
		for (std::size_t i = 0; i < AMEND_BENCH_DEPTH; ++i)
			book.place_order(order{.id    = next++,
								   .side  = side_t::bid,
								   .price = price,
								   .qty   = AMEND_BENCH_LOTS},
							 trades);
}

/// @brief Buffers reused across iterations, so the allocator is not in the
///        measurement. These cases produce one outcome each and no trades - the
///        ask side is empty, so nothing crosses - but both are cleared anyway
///        to keep the reuse honest.
struct amend_bench_sink {
	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;

	void reset() noexcept {
		trades.clear();
		outcomes.clear();
	}
};

} // namespace

// The cheap one, and the one the priority rules exist to make cheap: the node
// is resized where it stands and keeps its place in the queue. No pool traffic,
// no relinking, one aggregate update.
//
// Every iteration asks for the same quantity, which after the first is a resize
// to the value the order already has. That is the same branch and the same work
// - modify_order takes `resize` for anything that is not an increase - so the
// number is a downsize per iteration and not a first one followed by no-ops.
void BM_AmendDownAtSamePrice(benchmark::State &state) {
	order_book book;
	amend_bench_seed(book);
	amend_bench_sink sink;

	for (auto _ : state) {
		book.modify_order(amendment{.id       = AMEND_BENCH_ID,
									.price    = AMEND_BENCH_PRICE,
									.quantity = AMEND_BENCH_LOTS - 1},
						  sink.trades,
						  sink.outcomes);
		benchmark::DoNotOptimize(&book);
		sink.reset();
	}
	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_AmendDownAtSamePrice);

// The same level, but the order gives up its queue position - so this is the
// resize plus an unlink and a push_back on the level's intrusive list. The pool
// cell does not move, which is what keeps it well short of a reprice.
//
// Strictly increasing, so every iteration really does requeue. The quantity
// climbs by one per iteration from a starting point six orders of magnitude
// below quantity_t's maximum, which is more headroom than any run needs.
void BM_AmendUpAtSamePrice(benchmark::State &state) {
	order_book book;
	amend_bench_seed(book);
	amend_bench_sink sink;

	quantity_t qty = AMEND_BENCH_LOTS;
	for (auto _ : state) {
		book.modify_order(amendment{.id       = AMEND_BENCH_ID,
									.price    = AMEND_BENCH_PRICE,
									.quantity = ++qty},
						  sink.trades,
						  sink.outcomes);
		benchmark::DoNotOptimize(&book);
		sink.reset();
	}
	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_AmendUpAtSamePrice);

// The expensive one: a level lookup, a node released to the pool, a crossing
// walk that finds nothing, a level found at the new price, and a node taken
// back out of the pool - plus one index erase and one index insert.
//
// The order moves between two prices that both hold depth of their own, so no
// level is created or destroyed and the number is the amendment rather than the
// ladder's insert path. The first iteration leaves AMEND_BENCH_PRICE for good;
// every one after it alternates between the other two.
void BM_AmendPrice(benchmark::State &state) {
	order_book book;
	amend_bench_seed(book);
	amend_bench_sink sink;

	price_t price = AMEND_BENCH_NEAR;
	for (auto _ : state) {
		price = price == AMEND_BENCH_NEAR ? AMEND_BENCH_FAR : AMEND_BENCH_NEAR;
		book.modify_order(amendment{.id       = AMEND_BENCH_ID,
									.price    = price,
									.quantity = AMEND_BENCH_LOTS},
						  sink.trades,
						  sink.outcomes);
		benchmark::DoNotOptimize(&book);
		sink.reset();
	}
	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_AmendPrice);

// The baseline BM_AmendPrice is measured against: the same move expressed as
// the two commands a venue without MODIFY needs. Everything a reprice does
// happens here too, plus a second index probe, a second outcome, and the
// admission checks an arriving order is put through.
//
// It is also the case that shows what a client gives up: the replacement is a
// new order as far as the book is concerned, so its executed quantity starts
// again at zero. Nothing trades here, so the two are comparable - on a
// partially filled order they would not be.
void BM_CancelAndReplace(benchmark::State &state) {
	order_book book;
	amend_bench_seed(book);
	amend_bench_sink sink;

	price_t price = AMEND_BENCH_NEAR;
	for (auto _ : state) {
		price = price == AMEND_BENCH_NEAR ? AMEND_BENCH_FAR : AMEND_BENCH_NEAR;
		book.cancel_order(AMEND_BENCH_ID, sink.outcomes);
		book.place_order(order{.id    = AMEND_BENCH_ID,
							   .side  = side_t::bid,
							   .price = price,
							   .qty   = AMEND_BENCH_LOTS},
						 sink.trades,
						 sink.outcomes);
		benchmark::DoNotOptimize(&book);
		sink.reset();
	}
	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_CancelAndReplace);
